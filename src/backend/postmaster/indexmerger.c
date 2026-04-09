/*-------------------------------------------------------------------------
 *
 * indexmerger.c
 *	  Background worker: periodic B-tree index leaf page merger.
 *
 * Wakes every index_merge_interval minutes, scans all user B-tree indexes,
 * and calls bt_merge_index_leaves() on each one.
 *
 * Uses an atomic flag (IndexMergerRunning) in shared memory to prevent
 * re-entrant runs when the previous round has not yet finished.
 *
 * src/backend/postmaster/indexmerger.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/nbtmerge.h"
#include "access/nbtree.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/pg_shmem.h"
#include "storage/shmem.h"
#include "storage/subsystems.h"
#include "tcop/tcopprot.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/ps_status.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

/* GUC: defined in nbtmerge.c */
extern int	index_merge_interval;
extern int	index_merge_fill_threshold;

/* Shared memory: one flag to prevent re-entrant runs */
typedef struct
{
	pg_atomic_uint32 running;	/* 0 = idle, 1 = running */
} IndexMergerShmem;

static IndexMergerShmem *imstate = NULL;

static void IndexLeafMergerShmemRequest(void *arg);
static void IndexLeafMergerShmemInit(void *arg);

const ShmemCallbacks IndexLeafMergerShmemCallbacks = {
	.request_fn = IndexLeafMergerShmemRequest,
	.init_fn = IndexLeafMergerShmemInit,
};

/* ----------
 * IndexLeafMergerShmemSize
 * ----------
 */
Size
IndexLeafMergerShmemSize(void)
{
	return MAXALIGN(sizeof(IndexMergerShmem));
}

static void
IndexLeafMergerShmemRequest(void *arg)
{
	ShmemRequestStruct(.name = "IndexLeafMerger",
					   .size = IndexLeafMergerShmemSize());
}

static void
IndexLeafMergerShmemInit(void *arg)
{
	bool		found;

	imstate = (IndexMergerShmem *)
		ShmemInitStruct("IndexLeafMerger", IndexLeafMergerShmemSize(), &found);
	if (!found)
		pg_atomic_init_u32(&imstate->running, 0);
}

/*
 * merge_one_index
 *
 * Open a single user B-tree index and call bt_merge_index_leaves().
 * Each call runs in its own transaction.
 */
static void
merge_one_index(Oid indexoid)
{
	Relation	indexrel;
	Relation	heaprel;
	int			nmerged;
	Oid			heapoid;

	/* Use ShareUpdateExclusiveLock: same as VACUUM, no DML conflict */
	indexrel = try_relation_open(indexoid, ShareUpdateExclusiveLock);
	if (indexrel == NULL)
		return;					/* dropped concurrently */

	/* Only process btree indexes */
	if (indexrel->rd_rel->relam != BTREE_AM_OID)
	{
		relation_close(indexrel, ShareUpdateExclusiveLock);
		return;
	}

	heapoid = IndexGetRelation(indexoid, true);
	if (!OidIsValid(heapoid))
	{
		relation_close(indexrel, ShareUpdateExclusiveLock);
		return;
	}

	heaprel = try_relation_open(heapoid, NoLock);
	if (heaprel == NULL)
	{
		relation_close(indexrel, ShareUpdateExclusiveLock);
		return;
	}

	nmerged = bt_merge_index_leaves(indexrel, heaprel);

	if (nmerged > 0)
		elog(LOG, "index leaf merger: merged %d pages in index \"%s\"",
			 nmerged, RelationGetRelationName(indexrel));

	relation_close(heaprel, NoLock);
	relation_close(indexrel, ShareUpdateExclusiveLock);
}

/*
 * merge_all_user_indexes
 *
 * Collect OIDs of all user B-tree indexes, then merge each one in a
 * separate mini-transaction.
 */
static void
merge_all_user_indexes(void)
{
	Relation	pg_index;
	Relation	pg_class;
	TableScanDesc iscan;
	HeapTuple	tup;
	List	   *indexoids = NIL;
	ListCell   *lc;

	elog(LOG, "index leaf merger: starting merge pass");

	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());

	pg_index = table_open(IndexRelationId, AccessShareLock);
	pg_class = table_open(RelationRelationId, AccessShareLock);
	iscan = table_beginscan_catalog(pg_index, 0, NULL);

	while (HeapTupleIsValid(tup = heap_getnext(iscan, ForwardScanDirection)))
	{
		Form_pg_index idxform = (Form_pg_index) GETSTRUCT(tup);
		Form_pg_class clsform;
		HeapTuple	classtup;

		/* Only live indexes */
		if (!idxform->indislive)
			continue;

		/* Look up the heap relation */
		classtup = SearchSysCacheCopy1(RELOID,
									   ObjectIdGetDatum(idxform->indrelid));
		if (!HeapTupleIsValid(classtup))
			continue;
		clsform = (Form_pg_class) GETSTRUCT(classtup);

		/* Skip shared system catalogs */
		if (clsform->relisshared)
		{
			heap_freetuple(classtup);
			continue;
		}

		/* Skip temp tables */
		if (clsform->relpersistence == RELPERSISTENCE_TEMP)
		{
			heap_freetuple(classtup);
			continue;
		}

		/* Skip non-persistent tables */
		if (clsform->relpersistence != RELPERSISTENCE_PERMANENT)
		{
			heap_freetuple(classtup);
			continue;
		}

		heap_freetuple(classtup);

		/* Check the index itself is permanent */
		{
			HeapTuple	idxclasstup = SearchSysCacheCopy1(
												RELOID,
												ObjectIdGetDatum(idxform->indexrelid));

			if (!HeapTupleIsValid(idxclasstup))
				continue;
			clsform = (Form_pg_class) GETSTRUCT(idxclasstup);
			if (clsform->relpersistence != RELPERSISTENCE_PERMANENT)
			{
				heap_freetuple(idxclasstup);
				continue;
			}
			/* Only B-tree */
			if (clsform->relam != BTREE_AM_OID)
			{
				heap_freetuple(idxclasstup);
				continue;
			}
			heap_freetuple(idxclasstup);
		}

		indexoids = lappend_oid(indexoids, idxform->indexrelid);
	}

	table_endscan(iscan);
	table_close(pg_class, AccessShareLock);
	table_close(pg_index, AccessShareLock);

	PopActiveSnapshot();
	CommitTransactionCommand();

	/* Process each index in its own transaction */
	foreach(lc, indexoids)
	{
		Oid			indexoid = lfirst_oid(lc);

		CHECK_FOR_INTERRUPTS();

		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());

		merge_one_index(indexoid);

		PopActiveSnapshot();
		CommitTransactionCommand();
	}

	list_free(indexoids);

	elog(LOG, "index leaf merger: merge pass complete");
}


/*
 * IndexLeafMergerMain
 *
 * Entry point for the background worker process.
 */
void
IndexLeafMergerMain(Datum main_arg)
{
	pqsignal(SIGTERM, die);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);

	BackgroundWorkerUnblockSignals();

	BackgroundWorkerInitializeConnection("postgres", NULL, 0);

	init_ps_display("index leaf merger");

	elog(LOG, "index leaf merger background worker started");

	for (;;)
	{
		int			rc;
		uint32		expected = 0;

		/* Wait for the configured interval */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   (long) index_merge_interval * 60 * 1000L,
					   WAIT_EVENT_AUTOVACUUM_MAIN);

		ResetLatch(MyLatch);

		if (rc & WL_EXIT_ON_PM_DEATH)
			break;

		CHECK_FOR_INTERRUPTS();

		/*
		 * CAS: atomically set running = 1 only if it was 0.
		 * If it was already 1, the previous round is still running; skip.
		 */
		if (!pg_atomic_compare_exchange_u32(&imstate->running, &expected, 1))
		{
			elog(LOG, "index leaf merger: previous round still running, skipping");
			continue;
		}

		PG_TRY();
		{
			merge_all_user_indexes();
		}
		PG_FINALLY();
		{
			pg_atomic_write_u32(&imstate->running, 0);
		}
		PG_END_TRY();
	}
}


/*
 * IndexLeafMergerRegister
 *
 * Called from postmaster at startup to register the static background worker.
 */
void
IndexLeafMergerRegister(void)
{
	BackgroundWorker bgw;

	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, MAXPGPATH, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "IndexLeafMergerMain");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "index leaf merger");
	snprintf(bgw.bgw_type, BGW_MAXLEN, "index leaf merger");
	bgw.bgw_restart_time = 60;
	bgw.bgw_notify_pid = 0;
	bgw.bgw_main_arg = (Datum) 0;

	RegisterBackgroundWorker(&bgw);
}
