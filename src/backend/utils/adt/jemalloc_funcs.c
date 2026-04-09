/*-------------------------------------------------------------------------
 *
 * jemalloc_funcs.c
 *	  SQL-callable interface for jemalloc leak detection.
 *
 * Always compiled.  When USE_JEMALLOC is defined the real implementation
 * is used; otherwise an error is returned explaining the missing support.
 *
 * src/backend/utils/adt/jemalloc_funcs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/builtins.h"

#ifdef USE_JEMALLOC
#include "utils/jemalloc_leak.h"
#endif

/*
 * pg_jemalloc_leak_report([path text]) → text
 *
 * Generates a jemalloc heap-profile HTML leak report.
 * The optional argument overrides the jemalloc_leak_dump_dir GUC.
 * Returns the path of the generated HTML file.
 */
PG_FUNCTION_INFO_V1(pg_jemalloc_leak_report);

Datum
pg_jemalloc_leak_report(PG_FUNCTION_ARGS)
{
#ifdef USE_JEMALLOC
	return jemalloc_leak_report_internal(fcinfo);
#else
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("jemalloc support is not compiled in"),
			 errhint("Rebuild PostgreSQL with --with-jemalloc=<prefix>.")));
	PG_RETURN_NULL();
#endif
}
