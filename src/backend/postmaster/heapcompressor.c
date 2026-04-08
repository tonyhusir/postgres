/*-------------------------------------------------------------------------
 *
 * heapcompressor.c
 *	  Background worker that compresses heap pages once per hour.
 *
 * The worker wakes up every COMPRESS_INTERVAL_MS milliseconds, scans all
 * user tables in the current database, and calls compress_heap_page() on
 * each uncompressed heap page.
 *
 * Only one instance is registered (per cluster), connecting to the default
 * database ("postgres").  For a multi-database deployment the approach would
 * need to be extended — but for a prototype this is sufficient.
 *
 * src/backend/postmaster/heapcompressor.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/heapcompress.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "catalog/pg_class.h"
#include "utils/relcache.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/ps_status.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

/* Wake up every hour */
#define COMPRESS_INTERVAL_MS	(60 * 60 * 1000L)

/* Use a bulk-read buffer strategy to avoid polluting the shared buffer pool */
static BufferAccessStrategy compress_strategy = NULL;

/*
 * compress_relation
 *
 * Walk every block of the given relation and compress any uncompressed page
 * that shows compression benefit.  Each page is processed under its own
 * mini-transaction so we don't hold locks longer than necessary.
 */
static void
compress_relation(Oid reloid)
{
	Relation	rel;
	BlockNumber nblocks;
	BlockNumber blkno;

	/*
	 * Open with ShareUpdateExclusiveLock — this is the same lock level used
	 * by VACUUM, which prevents concurrent DDL but allows normal DML.
	 */
	rel = try_relation_open(reloid, ShareUpdateExclusiveLock);
	if (rel == NULL)
		return;					/* relation was dropped concurrently */

	/* Only compress plain heap relations */
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_TOASTVALUE)
	{
		relation_close(rel, ShareUpdateExclusiveLock);
		return;
	}

	nblocks = RelationGetNumberOfBlocks(rel);

	for (blkno = 0; blkno < nblocks; blkno++)
	{
		Buffer		buf;
		Page		page;
		bool		compressed;

		CHECK_FOR_INTERRUPTS();

		buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno,
								 RBM_NORMAL, compress_strategy);

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);

		/* Skip already-compressed or empty pages */
		if (PageIsCompressed(page) || PageIsEmpty(page))
		{
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buf);
			continue;
		}

		START_CRIT_SECTION();

		compressed = compress_heap_page(rel, buf);

		if (compressed)
		{
			MarkBufferDirty(buf);

			/*
			 * WAL-log the full page so crash recovery can reconstruct the
			 * compressed page correctly.
			 */
			if (RelationNeedsWAL(rel))
			{
				XLogRecPtr	recptr;

				recptr = log_newpage_buffer(buf, true);
				PageSetLSN(page, recptr);
			}
		}

		END_CRIT_SECTION();

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buf);
	}

	relation_close(rel, ShareUpdateExclusiveLock);
}

/*
 * compress_all_user_tables
 *
 * Collect OIDs of all user tables in the current database and compress them.
 */
static void
compress_all_user_tables(void)
{
	Relation	pg_class;
	TableScanDesc scan;
	HeapTuple	tup;
	List	   *reloids = NIL;
	ListCell   *lc;

	elog(LOG, "heap compressor: starting compression pass");

	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());

	pg_class = table_open(RelationRelationId, AccessShareLock);
	scan = table_beginscan_catalog(pg_class, 0, NULL);

	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_class pgc = (Form_pg_class) GETSTRUCT(tup);

		/* Only plain user tables and toast tables with physical storage */
		if ((pgc->relkind != RELKIND_RELATION &&
			 pgc->relkind != RELKIND_TOASTVALUE))
			continue;

		/* Skip shared system catalogs */
		if (pgc->relisshared)
			continue;

		/* Skip temp tables */
		if (pgc->relpersistence == RELPERSISTENCE_TEMP)
			continue;

		reloids = lappend_oid(reloids, pgc->oid);
	}

	table_endscan(scan);
	table_close(pg_class, AccessShareLock);

	PopActiveSnapshot();
	CommitTransactionCommand();

	/* Now compress each collected relation */
	foreach(lc, reloids)
	{
		Oid			reloid = lfirst_oid(lc);

		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());

		compress_relation(reloid);

		PopActiveSnapshot();
		CommitTransactionCommand();

		CHECK_FOR_INTERRUPTS();
	}

	list_free(reloids);

	elog(LOG, "heap compressor: compression pass complete");
}

/*
 * HourlyHeapCompressorMain
 *
 * Entry point for the background worker.
 */
void
HourlyHeapCompressorMain(Datum main_arg)
{
	/* Set up signal handlers the same way other bgworkers do */
	pqsignal(SIGTERM, die);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);

	BackgroundWorkerUnblockSignals();

	/* Connect to the "postgres" database */
	BackgroundWorkerInitializeConnection("postgres", NULL, 0);

	init_ps_display("heap compressor");

	compress_strategy = GetAccessStrategy(BAS_VACUUM);

	elog(LOG, "heap compressor background worker started");

	for (;;)
	{
		int			rc;

		/* Wait for COMPRESS_INTERVAL_MS or until signalled */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   COMPRESS_INTERVAL_MS,
					   WAIT_EVENT_AUTOVACUUM_MAIN);

		ResetLatch(MyLatch);

		if (rc & WL_EXIT_ON_PM_DEATH)
			break;

		CHECK_FOR_INTERRUPTS();

		compress_all_user_tables();
	}

	FreeAccessStrategy(compress_strategy);
}

/*
 * HourlyHeapCompressorRegister
 *
 * Called from postmaster startup to register the static background worker.
 */
void
HourlyHeapCompressorRegister(void)
{
	BackgroundWorker bgw;

	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, MAXPGPATH, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "HourlyHeapCompressorMain");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "hourly heap compressor");
	snprintf(bgw.bgw_type, BGW_MAXLEN, "hourly heap compressor");
	bgw.bgw_restart_time = 60;	/* restart after 1 minute on crash */
	bgw.bgw_notify_pid = 0;
	bgw.bgw_main_arg = (Datum) 0;

	RegisterBackgroundWorker(&bgw);
}
