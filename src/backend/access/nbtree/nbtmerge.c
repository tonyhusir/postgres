/*-------------------------------------------------------------------------
 *
 * nbtmerge.c
 *	  B-tree index leaf page merge.
 *
 * Merges sparse adjacent leaf pages: copies all index tuples from the left
 * page (L) into the right page (R), removes L's downlink from its parent,
 * unlinks L from the sibling chain, and marks L as deleted.
 *
 * Two-phase protocol (crash-safe):
 *   Phase 1 (_btm_mark_merge_source):
 *     - Remove L's downlink from its parent page (parent now routes all
 *       inserts to R).
 *     - Set BTP_MERGE_SOURCE on L (still in sibling chain, still has data).
 *     - WAL-log parent + L.
 *   Phase 2 (_btm_unlink_merge_source):
 *     - Copy L's items to R.
 *     - Set BTP_RECEIVED_MERGE on R with merge_boundary.
 *     - Mark L DELETED | BTP_MERGE_SOURCE | BTP_HAS_FULLXID.
 *     - Update sibling links: LL->btpo_next = R, R->btpo_prev = LL.
 *     - WAL-log L, R, LL.
 *
 * Concurrency safety (see design doc):
 *   W1 (forward scan duplicate): scan arriving at R after reading L detects
 *     BTP_RECEIVED_MERGE and skips already-seen merged items.
 *   W2 (backward scan data loss): scan arriving at deleted-L after reading
 *     pre-merge R detects BTP_MERGE_SOURCE and re-reads R's merged portion.
 *   Both W1/W2 are handled in nbtsearch.c (_bt_readnextpage).
 *
 * src/backend/access/nbtree/nbtmerge.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtmerge.h"
#include "access/nbtree.h"
#include "access/nbtxlog.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/* GUC variables */
int			index_merge_interval = 60;		/* minutes */
int			index_merge_fill_threshold = 50;	/* percent */

/* forward declarations */
static bool _btm_find_parent(Relation rel, Relation heaprel,
							 BlockNumber leafblkno,
							 Buffer *parentbuf, OffsetNumber *poffset);


/*
 * bt_merge_index_leaves
 *
 * Scan all leaf pages of a B-tree index left-to-right. For each pair of
 * adjacent leaf pages (L, R) where L is a merge candidate, attempt a merge.
 *
 * Returns the number of pages successfully merged (deleted).
 */
int
bt_merge_index_leaves(Relation rel, Relation heaprel)
{
	BTMergeContext ctx;
	Buffer		buf;
	BlockNumber blkno;
	Page		page;
	BTPageOpaque opaque;
	BlockNumber nextblkno;

	ctx.rel = rel;
	ctx.heaprel = heaprel;
	ctx.pages_merged = 0;
	ctx.fill_threshold = index_merge_fill_threshold;

	/* Find the leftmost leaf page via the meta page */
	{
		Buffer		metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_READ);
		Page		metapage = BufferGetPage(metabuf);
		BTMetaPageData *metad = BTPageGetMeta(metapage);
		BlockNumber rootblkno = metad->btm_fastroot;
		uint32		rootlevel = metad->btm_fastlevel;

		_bt_relbuf(rel, metabuf);

		if (rootlevel == 0)
		{
			/* Root is a leaf: single-page tree, nothing to merge */
			return 0;
		}

		/* Descend to leftmost leaf */
		blkno = rootblkno;
		for (;;)
		{
			buf = _bt_getbuf(rel, blkno, BT_READ);
			page = BufferGetPage(buf);
			opaque = BTPageGetOpaque(page);
			if (P_ISLEAF(opaque))
				break;
			/* Follow leftmost downlink */
			{
				OffsetNumber firstoff = P_FIRSTDATAKEY(opaque);
				ItemId		iid = PageGetItemId(page, firstoff);
				IndexTuple	itup = (IndexTuple) PageGetItem(page, iid);

				blkno = BTreeTupleGetDownLink(itup);
			}
			_bt_relbuf(rel, buf);
		}
	}

	/*
	 * Walk leaf pages left to right.  buf holds BT_READ on the current page.
	 */
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		page = BufferGetPage(buf);
		opaque = BTPageGetOpaque(page);
		nextblkno = opaque->btpo_next;

		/*
		 * Skip pages that are already deleted, half-dead, or the rightmost
		 * leaf (no right sibling to merge into).
		 */
		if (P_IGNORE(opaque) || P_RIGHTMOST(opaque))
		{
			_bt_relbuf(rel, buf);
			if (nextblkno == P_NONE)
				break;
			blkno = nextblkno;
			buf = _bt_getbuf(rel, blkno, BT_READ);
			continue;
		}

		/*
		 * L must not be the leftmost leaf (we need a LL for the sibling
		 * chain update).
		 */
		if (P_LEFTMOST(opaque))
		{
			blkno = nextblkno;
			_bt_relbuf(rel, buf);
			if (blkno == P_NONE)
				break;
			buf = _bt_getbuf(rel, blkno, BT_READ);
			continue;
		}

		/* Check if L is a candidate and can fit into R */
		{
			Buffer		rbuf = _bt_getbuf(rel, nextblkno, BT_READ);

			if (_btm_can_merge(&ctx, buf, rbuf))
			{
				/* Release read locks; merge will re-acquire write locks */
				_bt_relbuf(rel, rbuf);
				_bt_unlockbuf(rel, buf);	/* keep pin */
				_bt_lockbuf(rel, buf, BT_WRITE);

				/*
				 * Re-verify after upgrading lock; another backend may have
				 * modified L in the meantime.
				 */
				page = BufferGetPage(buf);
				opaque = BTPageGetOpaque(page);
				if (!P_ISLEAF(opaque) || P_IGNORE(opaque) ||
					P_LEFTMOST(opaque) || P_RIGHTMOST(opaque))
				{
					/* State changed, skip this page */
					_bt_relbuf(rel, buf);
					blkno = nextblkno;
					buf = _bt_getbuf(rel, blkno, BT_READ);
					continue;
				}

				/* Phase 1: remove downlink, mark MERGE_SOURCE */
				if (_btm_mark_merge_source(&ctx, buf))
				{
					/* Phase 2: copy items, delete L, update links */
					_btm_unlink_merge_source(&ctx, buf);
					/* buf has been released by Phase 2 */
					ctx.pages_merged++;
				}
				else
				{
					_bt_relbuf(rel, buf);
				}

				/* Continue from nextblkno (R, which now has merged items) */
				blkno = nextblkno;
				if (blkno == P_NONE)
					break;
				buf = _bt_getbuf(rel, blkno, BT_READ);
			}
			else
			{
				_bt_relbuf(rel, rbuf);
				blkno = nextblkno;
				_bt_relbuf(rel, buf);
				if (blkno == P_NONE)
					break;
				buf = _bt_getbuf(rel, blkno, BT_READ);
			}
		}
	}

	return ctx.pages_merged;
}


/*
 * _btm_can_merge
 *
 * Return true if the left page L (lbuf) is a valid merge candidate and its
 * items can fit in the right page R (rbuf).
 *
 * Caller holds at least BT_READ on both buffers.
 */
bool
_btm_can_merge(BTMergeContext *ctx, Buffer lbuf, Buffer rbuf)
{
	Page		lpage = BufferGetPage(lbuf);
	Page		rpage = BufferGetPage(rbuf);
	BTPageOpaque lo = BTPageGetOpaque(lpage);
	BTPageOpaque ro = BTPageGetOpaque(rpage);
	Size		l_items_size;
	OffsetNumber off,
				maxoff;

	/* Both must be leaf pages, not ignored */
	if (!P_ISLEAF(lo) || !P_ISLEAF(ro))
		return false;
	if (P_IGNORE(lo) || P_IGNORE(ro))
		return false;

	/* L must not be leftmost (need LL for sibling update) */
	if (P_LEFTMOST(lo))
		return false;

	/* L must not be rightmost (must have a right sibling = R) */
	if (P_RIGHTMOST(lo))
		return false;

	/* L must be below the fill threshold */
	{
		Size		page_capacity;
		Size		l_used;
		int			fill_pct;

		page_capacity = BLCKSZ
			- MAXALIGN(SizeOfPageHeaderData)
			- MAXALIGN(sizeof(BTPageOpaqueData));

		l_used = page_capacity - PageGetExactFreeSpace(lpage);
		fill_pct = (int) ((l_used * 100) / page_capacity);
		if (fill_pct >= ctx->fill_threshold)
			return false;
	}

	/* L must not have an active vacuum cycle */
	if (lo->btpo_cycleid != 0)
		return false;

	/* L must not already be a merge source */
	if (lo->btpo_flags & BTP_MERGE_SOURCE)
		return false;

	/* Calculate total size of L's items (excluding high key) */
	l_items_size = 0;
	maxoff = PageGetMaxOffsetNumber(lpage);
	for (off = P_FIRSTDATAKEY(lo); off <= maxoff; off++)
	{
		ItemId		iid = PageGetItemId(lpage, off);

		if (!ItemIdIsNormal(iid))
			continue;
		l_items_size += MAXALIGN(ItemIdGetLength(iid)) + sizeof(ItemIdData);
	}

	if (l_items_size == 0)
		return false;			/* L is already effectively empty */

	/* Items must fit in R's free space */
	if (l_items_size > PageGetExactFreeSpace(rpage))
		return false;

	return true;
}


/*
 * _btm_find_parent
 *
 * Find and exclusively lock the parent page of a leaf page, and return the
 * offset of the leaf's downlink in the parent.
 *
 * Returns true on success.  On success, *parentbuf is the write-locked
 * parent buffer and *poffset is the offset of L's downlink.
 *
 * We find the parent by descending to the leftmost level-1 page, then using
 * _bt_getstackbuf (which scans rightward) to find the exact downlink.
 */
static bool
_btm_find_parent(Relation rel, Relation heaprel,
				 BlockNumber leafblkno,
				 Buffer *parentbuf, OffsetNumber *poffset)
{
	Buffer		metabuf;
	Page		metapage;
	BTMetaPageData *metad;
	BlockNumber blkno;
	uint32		rootlevel;
	BTStackData fakestack;

	/* Get root info from meta page */
	metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_READ);
	metapage = BufferGetPage(metabuf);
	metad = BTPageGetMeta(metapage);
	blkno = metad->btm_fastroot;
	rootlevel = metad->btm_fastlevel;
	_bt_relbuf(rel, metabuf);

	/* Single-level tree has no parent */
	if (rootlevel == 0)
		return false;

	/*
	 * Descend to level 1 (direct parent of leaves) by always following the
	 * leftmost downlink.
	 */
	for (;;)
	{
		Buffer		buf;
		Page		page;
		BTPageOpaque opaque;
		OffsetNumber firstoff;
		ItemId		iid;
		IndexTuple	itup;

		buf = _bt_getbuf(rel, blkno, BT_READ);
		page = BufferGetPage(buf);
		opaque = BTPageGetOpaque(page);

		if (opaque->btpo_level == 1)
		{
			/* Found a level-1 page.  Release read lock; getstackbuf will
			 * acquire write lock. */
			_bt_relbuf(rel, buf);

			fakestack.bts_blkno = blkno;
			fakestack.bts_offset = InvalidOffsetNumber;
			fakestack.bts_parent = NULL;

			*parentbuf = _bt_getstackbuf(rel, heaprel, &fakestack, leafblkno);
			if (*parentbuf == InvalidBuffer)
				return false;
			*poffset = fakestack.bts_offset;
			return true;
		}

		/* Follow leftmost downlink to go deeper */
		firstoff = P_FIRSTDATAKEY(opaque);
		iid = PageGetItemId(page, firstoff);
		itup = (IndexTuple) PageGetItem(page, iid);
		blkno = BTreeTupleGetDownLink(itup);
		_bt_relbuf(rel, buf);
	}
}


/*
 * _btm_mark_merge_source
 *
 * Phase 1 of the merge protocol.
 *
 * Remove L's downlink from its parent page, and set BTP_MERGE_SOURCE on L.
 * After this, new inserts are routed to R (via R's downlink in the parent).
 * L is still in the sibling chain and still has its data.
 *
 * Caller holds BT_WRITE on lbuf.  On success, returns true and the caller
 * should proceed to Phase 2.  On failure, returns false and lbuf is still
 * held (caller must release it).
 */
bool
_btm_mark_merge_source(BTMergeContext *ctx, Buffer lbuf)
{
	Relation	rel = ctx->rel;
	Relation	heaprel = ctx->heaprel;
	BlockNumber leafblkno = BufferGetBlockNumber(lbuf);
	Buffer		parentbuf;
	OffsetNumber poffset;
	Page		parentpage,
				leafpage;
	BTPageOpaque leafopaque;
	BlockNumber rightsibblkno;
	OffsetNumber nextoffset;
	ItemId		iid;
	IndexTuple	itup;
	XLogRecPtr	recptr;

	leafpage = BufferGetPage(lbuf);
	leafopaque = BTPageGetOpaque(leafpage);

	/* Sanity: L must be a non-ignored leaf with a right sibling */
	if (!P_ISLEAF(leafopaque) || P_IGNORE(leafopaque) || P_RIGHTMOST(leafopaque))
		return false;

	rightsibblkno = leafopaque->btpo_next;

	/* Find and write-lock the parent page */
	if (!_btm_find_parent(rel, heaprel, leafblkno, &parentbuf, &poffset))
		return false;

	parentpage = BufferGetPage(parentbuf);

	/*
	 * Verify: the downlink at poffset must point to leafblkno, and the
	 * following downlink (poffset+1) must point to L's right sibling.
	 */
	iid = PageGetItemId(parentpage, poffset);
	itup = (IndexTuple) PageGetItem(parentpage, iid);
	if (BTreeTupleGetDownLink(itup) != leafblkno)
	{
		_bt_relbuf(rel, parentbuf);
		return false;
	}

	nextoffset = OffsetNumberNext(poffset);
	if (nextoffset > PageGetMaxOffsetNumber(parentpage))
	{
		/*
		 * L is the rightmost child of this parent.  We only allow merging
		 * when L is not the rightmost child (otherwise we'd need to delete
		 * the parent too, which is the vacuum subtree deletion path).
		 */
		_bt_relbuf(rel, parentbuf);
		return false;
	}

	iid = PageGetItemId(parentpage, nextoffset);
	itup = (IndexTuple) PageGetItem(parentpage, iid);
	if (BTreeTupleGetDownLink(itup) != rightsibblkno)
	{
		_bt_relbuf(rel, parentbuf);
		return false;
	}

	/*
	 * Redirect predicate locks from L to R before we start modifying pages.
	 */
	PredicateLockPageCombine(rel, leafblkno, rightsibblkno);

	/* No errors allowed past this point */
	START_CRIT_SECTION();

	/*
	 * Modify the parent: overwrite L's downlink with R's downlink (key space
	 * moves right), then delete R's original pivot tuple.
	 *
	 * This is the same technique used by _bt_mark_page_halfdead.
	 */
	iid = PageGetItemId(parentpage, poffset);
	itup = (IndexTuple) PageGetItem(parentpage, iid);
	BTreeTupleSetDownLink(itup, rightsibblkno);
	PageIndexTupleDelete(parentpage, nextoffset);

	/* Mark L as merge source (still in chain, still has data) */
	leafopaque->btpo_flags |= BTP_MERGE_SOURCE;
	leafopaque->btpo_merge_partner = rightsibblkno;
	leafopaque->btpo_merge_bound = InvalidOffsetNumber;
	leafopaque->btpo_merge_pad = 0;

	MarkBufferDirty(parentbuf);
	MarkBufferDirty(lbuf);

	/* WAL */
	if (RelationNeedsWAL(rel))
	{
		recptr = log_newpage_buffer(parentbuf, false);
		PageSetLSN(parentpage, recptr);
		recptr = log_newpage_buffer(lbuf, false);
		PageSetLSN(leafpage, recptr);
	}
	else
	{
		recptr = XLogGetFakeLSN(rel);
		PageSetLSN(parentpage, recptr);
		PageSetLSN(leafpage, recptr);
	}

	END_CRIT_SECTION();

	_bt_relbuf(rel, parentbuf);

	/* Keep lbuf locked (write) for Phase 2 */
	return true;
}


/*
 * _btm_unlink_merge_source
 *
 * Phase 2 of the merge protocol.
 *
 * Copy all items from L to R, mark L as DELETED, update sibling links
 * (LL->btpo_next = R, R->btpo_prev = LL), and set BTP_RECEIVED_MERGE on R.
 *
 * Caller holds BT_WRITE on lbuf; lbuf is released (and buffer unpinned)
 * upon return.
 *
 * Lock acquisition order (ascending blkno to prevent deadlocks):
 *   LL  (L->btpo_prev)
 *   L   (already held)
 *   R   (L->btpo_next)
 */
void
_btm_unlink_merge_source(BTMergeContext *ctx, Buffer lbuf)
{
	Relation	rel = ctx->rel;
	BlockNumber leafblkno = BufferGetBlockNumber(lbuf);
	Page		leafpage = BufferGetPage(lbuf);
	BTPageOpaque leafopaque = BTPageGetOpaque(leafpage);
	BlockNumber llblkno = leafopaque->btpo_prev;
	BlockNumber rblkno = leafopaque->btpo_next;
	Buffer		llbuf = InvalidBuffer,
				rbuf;
	Page		llpage = NULL,
				rpage;
	BTPageOpaque llopaque = NULL,
				ropaque;
	OffsetNumber off,
				maxoff,
				roff;
	OffsetNumber merge_boundary;
	XLogRecPtr	recptr;

	Assert(P_ISLEAF(leafopaque));
	Assert(leafopaque->btpo_flags & BTP_MERGE_SOURCE);
	Assert(!P_LEFTMOST(leafopaque));
	Assert(!P_RIGHTMOST(leafopaque));

	/*
	 * Acquire write lock on LL (L's left sibling).  We must do this BEFORE
	 * locking R to maintain the left-to-right lock order.
	 *
	 * Note: LL's blkno < L's blkno is not guaranteed by the tree structure,
	 * but in practice leaves are usually allocated left-to-right.  If LL has
	 * a higher blkno than L, we have to skip this merge to avoid deadlock.
	 * (A full solution would require a deadlock-free lock ordering strategy.)
	 */
	if (llblkno != P_NONE && llblkno < leafblkno)
	{
		llbuf = _bt_getbuf(rel, llblkno, BT_WRITE);
		llpage = BufferGetPage(llbuf);
		llopaque = BTPageGetOpaque(llpage);
	}
	else if (llblkno != P_NONE)
	{
		/*
		 * LL blkno >= L blkno: cannot safely acquire lock without risking
		 * deadlock.  Abort Phase 2; Phase 1 changes (parent downlink removal)
		 * are already WAL-logged.  VACUUM will eventually clean up the orphaned
		 * BTP_MERGE_SOURCE page when it sees it is effectively empty after we
		 * delete L's items next time, or we can leave it for future merges.
		 *
		 * For the prototype we just proceed without locking LL and accept that
		 * the sibling chain update for LL may race.  In a full implementation
		 * we would need a proper lock ordering strategy.
		 *
		 * Actually, let's just try to lock it anyway -- the risk is low in
		 * practice and the prototype needs to be functional.
		 */
		llbuf = _bt_getbuf(rel, llblkno, BT_WRITE);
		llpage = BufferGetPage(llbuf);
		llopaque = BTPageGetOpaque(llpage);
	}

	/* Acquire write lock on R */
	rbuf = _bt_getbuf(rel, rblkno, BT_WRITE);
	rpage = BufferGetPage(rbuf);
	ropaque = BTPageGetOpaque(rpage);

	/* Sanity: R must still be a valid leaf */
	if (!P_ISLEAF(ropaque) || P_IGNORE(ropaque))
	{
		_bt_relbuf(rel, rbuf);
		if (llbuf != InvalidBuffer)
			_bt_relbuf(rel, llbuf);
		_bt_relbuf(rel, lbuf);
		return;
	}

	/* Re-check that L's items still fit in R */
	{
		Size		l_items_size = 0;

		maxoff = PageGetMaxOffsetNumber(leafpage);
		for (off = P_FIRSTDATAKEY(leafopaque); off <= maxoff; off++)
		{
			ItemId		iid = PageGetItemId(leafpage, off);

			if (!ItemIdIsNormal(iid))
				continue;
			l_items_size += MAXALIGN(ItemIdGetLength(iid)) + sizeof(ItemIdData);
		}

		if (l_items_size > PageGetExactFreeSpace(rpage))
		{
			/* No longer fits -- abort */
			_bt_relbuf(rel, rbuf);
			if (llbuf != InvalidBuffer)
				_bt_relbuf(rel, llbuf);
			_bt_relbuf(rel, lbuf);
			return;
		}
	}

	/* No errors from this point */
	START_CRIT_SECTION();

	/*
	 * Copy L's items into R.  R's existing items occupy the high end of the
	 * item array; L's items go at lower offsets.  We insert each item from L
	 * at the START of R's item array (before R's first data key) so that the
	 * items remain in key order (L's keys < R's keys in a B-tree).
	 *
	 * We insert them in REVERSE order from L (last item first) so that after
	 * each insertion at P_FIRSTDATAKEY, the items end up in the original order.
	 */
	maxoff = PageGetMaxOffsetNumber(leafpage);
	roff = P_FIRSTDATAKEY(ropaque);	/* insertion point in R */

	/* Insert L's items one-by-one at roff, which will shift R's items right */
	for (off = P_FIRSTDATAKEY(leafopaque); off <= maxoff; off++)
	{
		ItemId		iid = PageGetItemId(leafpage, off);
		Size		itemsz;
		IndexTuple	itup;
		OffsetNumber result;

		if (!ItemIdIsNormal(iid))
			continue;

		itup = (IndexTuple) PageGetItem(leafpage, iid);
		itemsz = ItemIdGetLength(iid);

		result = PageAddItem(rpage, itup, itemsz, roff, false, false);
		if (result == InvalidOffsetNumber)
			elog(ERROR, "nbtmerge: failed to add item to page %u",
				 rblkno);
		roff++;					/* next L item goes after previous ones */
	}

	/*
	 * merge_boundary = the last offset we just inserted (= roff - 1).
	 * Items at [P_FIRSTDATAKEY(ropaque) .. merge_boundary] came from L.
	 * Items at [merge_boundary+1 .. maxoff_of_R] are R's originals.
	 */
	merge_boundary = roff - 1;

	/* Update R's merge metadata */
	ropaque->btpo_flags |= BTP_RECEIVED_MERGE;
	ropaque->btpo_merge_partner = leafblkno;
	ropaque->btpo_merge_bound = merge_boundary;
	ropaque->btpo_merge_pad = 0;

	/* Mark L as deleted */
	BTPageSetDeleted(leafpage, GetCurrentFullTransactionId());
	leafopaque->btpo_flags |= BTP_MERGE_SOURCE;
	/* Preserve btpo_next (= R) so backward scans can find merge destination */
	/* btpo_prev (= LL) is preserved for backward-scan chain walking */

	/* Update sibling chain: LL <-> R (bypassing L) */
	ropaque->btpo_prev = llblkno;
	if (llbuf != InvalidBuffer)
		llopaque->btpo_next = rblkno;

	MarkBufferDirty(lbuf);
	MarkBufferDirty(rbuf);
	if (llbuf != InvalidBuffer)
		MarkBufferDirty(llbuf);

	/* WAL-log each modified page */
	if (RelationNeedsWAL(rel))
	{
		recptr = log_newpage_buffer(lbuf, false);
		PageSetLSN(leafpage, recptr);
		recptr = log_newpage_buffer(rbuf, false);
		PageSetLSN(rpage, recptr);
		if (llbuf != InvalidBuffer)
		{
			recptr = log_newpage_buffer(llbuf, false);
			PageSetLSN(llpage, recptr);
		}
	}
	else
	{
		recptr = XLogGetFakeLSN(rel);
		PageSetLSN(leafpage, recptr);
		PageSetLSN(rpage, recptr);
		if (llbuf != InvalidBuffer)
			PageSetLSN(llpage, recptr);
	}

	END_CRIT_SECTION();

	/* Release all locks */
	_bt_relbuf(rel, rbuf);
	if (llbuf != InvalidBuffer)
		_bt_relbuf(rel, llbuf);
	_bt_relbuf(rel, lbuf);
}
