/*-------------------------------------------------------------------------
 *
 * nbtmerge.h
 *	  Header for B-tree index leaf page merge support.
 *
 * src/include/access/nbtmerge.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NBTMERGE_H
#define NBTMERGE_H

#include "access/nbtree.h"
#include "storage/buf.h"
#include "storage/subsystems.h"
#include "utils/relcache.h"

/* GUC parameters */
extern PGDLLIMPORT int index_merge_interval;		/* minutes between runs */
extern PGDLLIMPORT int index_merge_fill_threshold;	/* page fill % to trigger merge */

/*
 * Context passed through a single merge scan of one index.
 */
typedef struct BTMergeContext
{
	Relation	rel;			/* index relation */
	Relation	heaprel;		/* heap relation */
	int			pages_merged;	/* pages merged so far this pass */
	int			fill_threshold; /* GUC copy: pages below this % are candidates */
} BTMergeContext;

/*
 * Public API
 */

/* Merge sparse leaf pages in one B-tree index; returns number of merges done */
extern int	bt_merge_index_leaves(Relation rel, Relation heaprel);

/* Check whether adjacent leaf pages L and R can be merged */
extern bool _btm_can_merge(BTMergeContext *ctx, Buffer lbuf, Buffer rbuf);

/*
 * Phase 1: remove L's downlink from its parent page, mark L as BTP_MERGE_SOURCE.
 * Caller holds BT_WRITE on lbuf.  Returns true on success.
 */
extern bool _btm_mark_merge_source(BTMergeContext *ctx, Buffer lbuf);

/*
 * Phase 2: copy L's items to R, mark L deleted, update sibling links.
 * Caller holds BT_WRITE on lbuf (from Phase 1).  lbuf is released on exit.
 */
extern void _btm_unlink_merge_source(BTMergeContext *ctx, Buffer lbuf);

/* Shared memory size and callbacks */
extern Size IndexLeafMergerShmemSize(void);
extern const ShmemCallbacks IndexLeafMergerShmemCallbacks;

/* Background worker entry point and registration */
extern void IndexLeafMergerMain(Datum main_arg);
extern void IndexLeafMergerRegister(void);

#endif							/* NBTMERGE_H */
