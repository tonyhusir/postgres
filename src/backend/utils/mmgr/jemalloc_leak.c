/*-------------------------------------------------------------------------
 *
 * jemalloc_leak.c
 *	  Memory leak detection using jemalloc heap profiling.
 *
 * When jemalloc_leak_detection is set to ON, jemalloc's heap profiler is
 * activated for the current backend.  Calling pg_jemalloc_leak_report()
 * dumps a heap profile and converts it to a self-contained HTML report
 * listing every call stack that still has live (unfreed) allocations,
 * sorted by live bytes descending.
 *
 * Build requirements:
 *   - jemalloc built with --enable-prof
 *   - PostgreSQL built with --with-jemalloc=<prefix>
 *     (adds -DUSE_JEMALLOC -I<prefix>/include -L<prefix>/lib -ljemalloc)
 *
 * src/backend/utils/mmgr/jemalloc_leak.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_JEMALLOC

#include <dlfcn.h>
#include <time.h>
#include <unistd.h>

#include <jemalloc/jemalloc.h>

#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/jemalloc_leak.h"
#include "utils/memutils.h"

/* GUC variables */
bool		jemalloc_leak_detection = false;
char	   *jemalloc_leak_dump_dir = NULL;

/* Maximum call stack depth to display per leak site */
#define JE_MAX_FRAMES		16
/* Maximum number of leak sites kept in memory for sorting */
#define JE_MAX_LEAK_SITES	4096
/* Minimum live bytes to appear in the report */
#define JE_MIN_REPORT_BYTES	1024

/*
 * One parsed record from the .heap file.
 */
typedef struct LeakSite
{
	uint64		live_bytes;
	uint64		live_count;
	uint64		total_bytes;
	uint64		total_count;
	uintptr_t	addrs[JE_MAX_FRAMES];
	int			naddrs;
	char		frames[JE_MAX_FRAMES][128]; /* resolved symbol names */
} LeakSite;

/* ----------------------------------------------------------------
 * assign_jemalloc_leak_detection
 *
 * GUC assign hook.  Activates/deactivates jemalloc profiling for
 * the current backend when the GUC value changes.
 * ----------------------------------------------------------------
 */
void
assign_jemalloc_leak_detection(bool newval, void *extra)
{
	int			rc;

	/* Activate/deactivate global profiling */
	rc = mallctl("prof.active",
				 NULL, NULL,
				 &newval, sizeof(bool));
	if (rc != 0)
		ereport(WARNING,
				(errmsg("jemalloc mallctl(prof.active) failed: %s",
						strerror(rc))));

	/*
	 * Also activate thread-level profiling.  After fork(), jemalloc's
	 * per-thread profiling state may default to inactive even when the
	 * global prof.active is true.  Setting thread.prof.active ensures
	 * allocations in this backend thread are actually sampled.
	 */
	rc = mallctl("thread.prof.active",
				 NULL, NULL,
				 &newval, sizeof(bool));
	if (rc != 0)
		ereport(WARNING,
				(errmsg("jemalloc mallctl(thread.prof.active) failed: %s",
						strerror(rc))));
}

/* ----------------------------------------------------------------
 * resolve_addr
 *
 * Convert a code address to "function_name (file:line)" using
 * dladdr().  Falls back to raw hex if symbols aren't available.
 * ----------------------------------------------------------------
 */
static void
resolve_addr(uintptr_t addr, char *buf, size_t bufsz)
{
	Dl_info		info;

	if (dladdr((void *) addr, &info) && info.dli_sname)
	{
		uintptr_t	offset = addr - (uintptr_t) info.dli_saddr;

		snprintf(buf, bufsz, "%s+0x%lx", info.dli_sname,
				 (unsigned long) offset);
	}
	else
		snprintf(buf, bufsz, "0x%lx", (unsigned long) addr);
}

/* ----------------------------------------------------------------
 * parse_heap_file
 *
 * Parse a jemalloc heap profile file.  Returns array of LeakSite,
 * with *nsites set to the count.  Returns NULL on error.
 *
 * Heap profile format (heap_v2):
 *   heap_v2/<lg_sample>
 *   @<addr1> <addr2> ...        <- call stack line
 *     t*: <live_n>: <live_b> [<tot_n>: <tot_b>]
 *     t<tid>: ...
 * ----------------------------------------------------------------
 */
static LeakSite *
parse_heap_file(const char *path, int *nsites)
{
	FILE	   *f;
	char		line[4096];
	LeakSite   *sites;
	int			cap = 256;
	int			n = 0;
	LeakSite	cur;
	bool		in_site = false;

	f = fopen(path, "r");
	if (!f)
		return NULL;

	sites = (LeakSite *) palloc(cap * sizeof(LeakSite));
	memset(&cur, 0, sizeof(cur));

	while (fgets(line, sizeof(line), f))
	{
		/* Call stack line starts with '@' */
		if (line[0] == '@')
		{
			/* Save previous site if valid */
			if (in_site && cur.live_bytes >= JE_MIN_REPORT_BYTES)
			{
				if (n >= cap)
				{
					cap *= 2;
					sites = repalloc(sites, cap * sizeof(LeakSite));
				}
				sites[n++] = cur;
			}
			memset(&cur, 0, sizeof(cur));
			in_site = true;

			/* Parse addresses: @addr1 addr2 ... */
			{
				char	   *p = line + 1;
				char	   *tok;

				while ((tok = strtok(p, " \t\r\n")) != NULL && cur.naddrs < JE_MAX_FRAMES)
				{
					cur.addrs[cur.naddrs++] = (uintptr_t) strtoull(tok, NULL, 16);
					p = NULL;
				}
			}
		}
		/* Aggregate stats line: "  t*: <live_n>: <live_b> [<tot_n>: <tot_b>]" */
		else if (in_site && strncmp(line, "  t*:", 5) == 0)
		{
			uint64		ln,
						lb,
						tn,
						tb;

			if (sscanf(line, "  t*: %llu: %llu [%llu: %llu]",
					   (unsigned long long *) &ln,
					   (unsigned long long *) &lb,
					   (unsigned long long *) &tn,
					   (unsigned long long *) &tb) == 4)
			{
				cur.live_count = ln;
				cur.live_bytes = lb;
				cur.total_count = tn;
				cur.total_bytes = tb;
			}
		}
	}

	/* Don't forget the last site */
	if (in_site && cur.live_bytes >= JE_MIN_REPORT_BYTES)
	{
		if (n >= cap)
		{
			cap *= 2;
			sites = repalloc(sites, cap * sizeof(LeakSite));
		}
		sites[n++] = cur;
	}

	fclose(f);

	/* Resolve symbols */
	for (int i = 0; i < n; i++)
	{
		for (int j = 0; j < sites[i].naddrs; j++)
			resolve_addr(sites[i].addrs[j],
						 sites[i].frames[j],
						 sizeof(sites[i].frames[j]));
	}

	/* Sort by live_bytes descending (simple insertion sort, n is usually small) */
	for (int i = 1; i < n; i++)
	{
		LeakSite	tmp = sites[i];
		int			j = i - 1;

		while (j >= 0 && sites[j].live_bytes < tmp.live_bytes)
		{
			sites[j + 1] = sites[j];
			j--;
		}
		sites[j + 1] = tmp;
	}

	*nsites = n;
	return sites;
}

/* ----------------------------------------------------------------
 * format_bytes
 *
 * Format a byte count as a human-readable string.
 * ----------------------------------------------------------------
 */
static void
format_bytes(uint64 bytes, char *buf, size_t bufsz)
{
	if (bytes >= 1024 * 1024 * 1024)
		snprintf(buf, bufsz, "%.2f GB", (double) bytes / (1024 * 1024 * 1024));
	else if (bytes >= 1024 * 1024)
		snprintf(buf, bufsz, "%.2f MB", (double) bytes / (1024 * 1024));
	else if (bytes >= 1024)
		snprintf(buf, bufsz, "%.2f KB", (double) bytes / 1024);
	else
		snprintf(buf, bufsz, "%llu B", (unsigned long long) bytes);
}

/* ----------------------------------------------------------------
 * html_escape
 *
 * Write HTML-escaped version of src into buf.
 * ----------------------------------------------------------------
 */
static void
html_escape(const char *src, char *buf, size_t bufsz)
{
	size_t		pos = 0;

	while (*src && pos + 8 < bufsz)
	{
		switch (*src)
		{
			case '<':
				memcpy(buf + pos, "&lt;", 4);
				pos += 4;
				break;
			case '>':
				memcpy(buf + pos, "&gt;", 4);
				pos += 4;
				break;
			case '&':
				memcpy(buf + pos, "&amp;", 5);
				pos += 5;
				break;
			default:
				buf[pos++] = *src;
		}
		src++;
	}
	buf[pos] = '\0';
}

/* ----------------------------------------------------------------
 * generate_html_report
 *
 * Write a self-contained HTML file to outpath.
 * ----------------------------------------------------------------
 */
static void
generate_html_report(const char *outpath,
					 const LeakSite *sites, int nsites,
					 uint64 total_live_bytes)
{
	FILE	   *f;
	char		ts_buf[64];
	char		bytes_buf[32];
	time_t		now = time(NULL);
	struct tm	tm_info;

	localtime_r(&now, &tm_info);
	strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

	f = fopen(outpath, "w");
	if (!f)
		ereport(ERROR,
				(errcode(ERRCODE_IO_ERROR),
				 errmsg("could not create leak report file \"%s\": %m",
						outpath)));

	format_bytes(total_live_bytes, bytes_buf, sizeof(bytes_buf));

	/* ---- HTML header ---- */
	fprintf(f,
			"<!DOCTYPE html>\n"
			"<html lang=\"en\">\n"
			"<head>\n"
			"<meta charset=\"UTF-8\">\n"
			"<title>PG Memory Leak Report — PID %d</title>\n"
			"<style>\n"
			"body{font-family:monospace;background:#1e1e1e;color:#d4d4d4;margin:0;padding:20px}\n"
			"h1{color:#4ec9b0;margin-bottom:4px}\n"
			".meta{color:#858585;margin-bottom:24px;font-size:13px}\n"
			".summary{background:#252526;border:1px solid #3c3c3c;border-radius:6px;"
			"padding:16px 24px;margin-bottom:24px;display:inline-block}\n"
			".summary .val{color:#ce9178;font-size:1.4em;font-weight:bold}\n"
			".summary .lbl{color:#858585;font-size:12px}\n"
			"table{width:100%%;border-collapse:collapse;background:#252526;"
			"border-radius:6px;overflow:hidden}\n"
			"th{background:#2d2d30;color:#9cdcfe;padding:10px 14px;text-align:left;"
			"border-bottom:1px solid #3c3c3c;font-size:13px}\n"
			"td{padding:8px 14px;border-bottom:1px solid #2d2d30;font-size:13px;"
			"vertical-align:top}\n"
			"tr:hover td{background:#2a2d2e}\n"
			".rank{color:#858585;text-align:right;width:50px}\n"
			".live-bytes{color:#ce9178;font-weight:bold;white-space:nowrap}\n"
			".live-count{color:#b5cea8}\n"
			".stack{color:#d4d4d4}\n"
			".frame{display:block;padding:1px 0}\n"
			".frame:first-child{color:#4ec9b0;font-weight:bold}\n"
			".frame:not(:first-child){color:#858585;font-size:12px;padding-left:16px}\n"
			".bar-wrap{background:#1e1e1e;border-radius:3px;height:8px;margin-top:4px}\n"
			".bar{background:#4ec9b0;height:8px;border-radius:3px}\n"
			".pct{color:#858585;font-size:11px}\n"
			"</style>\n"
			"</head>\n"
			"<body>\n",
			(int) getpid());

	/* ---- Summary card ---- */
	fprintf(f,
			"<h1>Memory Leak Report</h1>\n"
			"<div class=\"meta\">PID %d &nbsp;|&nbsp; %s</div>\n"
			"<div class=\"summary\">\n"
			"  <span style=\"margin-right:32px\">"
			"<div class=\"lbl\">Total live (unfreed)</div>"
			"<div class=\"val\">%s</div></span>\n"
			"  <span>"
			"<div class=\"lbl\">Leak sites found</div>"
			"<div class=\"val\">%d</div></span>\n"
			"</div>\n\n",
			(int) getpid(), ts_buf, bytes_buf, nsites);

	if (nsites == 0)
	{
		fprintf(f,
				"<p style=\"color:#6a9955\">No leak sites detected "
				"(all sampled allocations have been freed).</p>\n");
		fprintf(f, "</body></html>\n");
		fclose(f);
		return;
	}

	/* ---- Leak sites table ---- */
	fprintf(f,
			"<table>\n"
			"<thead><tr>\n"
			"  <th class=\"rank\">#</th>\n"
			"  <th>Live Bytes</th>\n"
			"  <th>Live Count</th>\n"
			"  <th>%% of Total</th>\n"
			"  <th>Call Stack</th>\n"
			"</tr></thead>\n"
			"<tbody>\n");

	for (int i = 0; i < nsites; i++)
	{
		const LeakSite *s = &sites[i];
		char		sb[32];
		double		pct = total_live_bytes > 0
			? (double) s->live_bytes / total_live_bytes * 100.0 : 0.0;
		int			bar_w = (int) pct;
		char		esc[256];

		format_bytes(s->live_bytes, sb, sizeof(sb));

		fprintf(f,
				"<tr>\n"
				"  <td class=\"rank\">%d</td>\n"
				"  <td class=\"live-bytes\">%s</td>\n"
				"  <td class=\"live-count\">%llu</td>\n"
				"  <td>\n"
				"    <div class=\"bar-wrap\">"
				"<div class=\"bar\" style=\"width:%d%%\"></div></div>\n"
				"    <div class=\"pct\">%.1f%%</div>\n"
				"  </td>\n"
				"  <td class=\"stack\">\n",
				i + 1,
				sb,
				(unsigned long long) s->live_count,
				bar_w, pct);

		for (int j = 0; j < s->naddrs; j++)
		{
			html_escape(s->frames[j], esc, sizeof(esc));
			fprintf(f, "    <span class=\"frame\">%s</span>\n", esc);
		}

		fprintf(f, "  </td>\n</tr>\n");
	}

	fprintf(f,
			"</tbody></table>\n"
			"</body></html>\n");

	fclose(f);
}

/* ----------------------------------------------------------------
 * pg_jemalloc_leak_report
 *
 * SQL function: pg_jemalloc_leak_report(path text DEFAULT NULL)
 *   → text  (path of the generated HTML file)
 *
 * 1. Dumps the current heap profile to a temp .heap file.
 * 2. Parses it.
 * 3. Generates an HTML report.
 * 4. Returns the HTML file path.
 * ----------------------------------------------------------------
 */
/*
 * jemalloc_leak_report_internal is called by jemalloc_funcs.c which
 * holds the PG_FUNCTION_INFO_V1 registration visible to the catalog.
 */
Datum
jemalloc_leak_report_internal(FunctionCallInfo fcinfo)
{
	char		heap_path[MAXPGPATH];
	char		html_path[MAXPGPATH];
	char		ts_buf[32];
	const char *dump_dir;
	time_t		now;
	struct tm	tm_info;
	LeakSite   *sites;
	int			nsites = 0;
	uint64		total_live = 0;
	int			rc;
	const char *heap_ptr = heap_path;

	/* Resolve output directory */
	if (PG_NARGS() > 0 && !PG_ARGISNULL(0))
		dump_dir = text_to_cstring(PG_GETARG_TEXT_PP(0));
	else if (jemalloc_leak_dump_dir && jemalloc_leak_dump_dir[0])
		dump_dir = jemalloc_leak_dump_dir;
	else
		dump_dir = "/tmp";

	/* Build file paths */
	now = time(NULL);
	localtime_r(&now, &tm_info);
	strftime(ts_buf, sizeof(ts_buf), "%Y%m%dT%H%M%S", &tm_info);

	snprintf(heap_path, MAXPGPATH, "%s/pg_leak_%d_%s.heap",
			 dump_dir, (int) getpid(), ts_buf);
	snprintf(html_path, MAXPGPATH, "%s/pg_leak_%d_%s.html",
			 dump_dir, (int) getpid(), ts_buf);

	/* Dump heap profile */
	rc = mallctl("prof.dump",
				 NULL, NULL,
				 &heap_ptr, sizeof(heap_ptr));
	if (rc != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("jemalloc prof.dump failed: %s", strerror(rc))));

	/* Parse .heap file */
	sites = parse_heap_file(heap_path, &nsites);
	if (!sites)
		ereport(ERROR,
				(errcode(ERRCODE_IO_ERROR),
				 errmsg("could not read heap profile \"%s\": %m", heap_path)));

	/* Compute total live bytes */
	for (int i = 0; i < nsites; i++)
		total_live += sites[i].live_bytes;

	/* Generate HTML */
	generate_html_report(html_path, sites, nsites, total_live);

	pfree(sites);

	/* Remove the raw .heap file (keep only the HTML) */
	unlink(heap_path);

	PG_RETURN_TEXT_P(cstring_to_text(html_path));
}

#endif							/* USE_JEMALLOC */
