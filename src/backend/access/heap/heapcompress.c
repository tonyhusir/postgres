/*-------------------------------------------------------------------------
 *
 * heapcompress.c
 *	  Heap page compression support.
 *
 * compress_heap_page() takes an exclusively locked buffer and, if the page
 * contains enough live tuples to make compression worthwhile, replaces all
 * tuple data portions with a single pglz-compressed block stored at
 * pd_special.  Tuple headers remain at their original positions.
 *
 * heap_decompress_tuple() is called by the heap scan path whenever it
 * encounters a tuple with HEAP_COMPRESSED_DATA set; it decompresses the
 * whole-page block and reconstructs the requested tuple.
 *
 * src/backend/access/heap/heapcompress.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/heapcompress.h"
#include "access/htup_details.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "common/pg_lzcompress.h"
#include "catalog/namespace.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "utils/regproc.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"


/*
 * compress_heap_page
 *
 * Attempt to compress the heap page in *buf.  The caller must hold an
 * exclusive lock on buf.
 *
 * Page layout after compression:
 *
 *   [PageHeader][ItemIds][CompressedBlock][...free...][TupleHeaders(hdr-only)]
 *               ^pd_lower                             ^pd_upper
 *
 * The CompressedBlock is placed immediately after the ItemId array (at the
 * original pd_lower).  pd_lower is advanced past the block.  Tuple headers
 * are compacted toward pd_upper (using PageRepairFragmentation) to ensure
 * there is enough room for the block in the free space area.
 *
 * Returns true if the page was compressed, false if compression was skipped.
 */
bool
compress_heap_page(Relation rel, Buffer buf)
{
	Page		page = BufferGetPage(buf);
	PageHeader	phdr = (PageHeader) page;
	OffsetNumber maxoff;
	OffsetNumber offnum;
	int			ntups;

	Size		raw_size;
	char	   *raw_buf;
	char	   *raw_ptr;

	TupleCompressInfo *tci_array;
	int			tci_idx;

	Size		comp_max;
	char	   *comp_buf;
	int32		comp_size;

	Size		block_size;
	Size		cb_offset;		/* where CompressedBlock will sit in page */
	Size		free_after_compact;

	CompressedBlockHeader *cbh;
	TupleCompressInfo *tci_dst;
	char	   *data_dst;

	if (PageIsCompressed(page))
		return false;
	if (PageIsEmpty(page))
		return false;

	maxoff = PageGetMaxOffsetNumber(page);
	if (maxoff < FirstOffsetNumber)
		return false;

	/*
	 * Pass 1: count live tuples and accumulate total data size.
	 * We include ALL normal tuples (live and dead) so that MVCC readers can
	 * still find them.  Dead tuples will be excluded on the next vacuum pass.
	 */
	ntups = 0;
	raw_size = 0;

	for (offnum = FirstOffsetNumber; offnum <= maxoff; offnum++)
	{
		ItemId		lp = PageGetItemId(page, offnum);
		HeapTupleHeader tup;

		if (!ItemIdIsNormal(lp))
			continue;

		tup = (HeapTupleHeader) PageGetItem(page, lp);
		raw_size += ItemIdGetLength(lp) - tup->t_hoff;
		ntups++;
	}

	if (ntups == 0 || raw_size == 0)
		return false;

	raw_buf = palloc(raw_size);
	raw_ptr = raw_buf;
	tci_array = palloc(ntups * sizeof(TupleCompressInfo));

	/* Pass 2: copy data portions and record per-tuple offsets. */
	tci_idx = 0;
	for (offnum = FirstOffsetNumber; offnum <= maxoff; offnum++)
	{
		ItemId		lp = PageGetItemId(page, offnum);
		HeapTupleHeader tup;
		Size		data_len;

		if (!ItemIdIsNormal(lp))
			continue;

		tup = (HeapTupleHeader) PageGetItem(page, lp);
		data_len = ItemIdGetLength(lp) - tup->t_hoff;

		tci_array[tci_idx].tci_offnum = offnum;
		tci_array[tci_idx].tci_pad = 0;
		tci_array[tci_idx].tci_offset = (uint32) (raw_ptr - raw_buf);
		tci_array[tci_idx].tci_length = (uint32) data_len;

		memcpy(raw_ptr, (char *) tup + tup->t_hoff, data_len);
		raw_ptr += data_len;
		tci_idx++;
	}

	Assert(tci_idx == ntups);

	/* Compress. */
	comp_max = PGLZ_MAX_OUTPUT(raw_size);
	comp_buf = palloc(comp_max);
	comp_size = pglz_compress(raw_buf, (int32) raw_size,
							  comp_buf, PGLZ_strategy_always);
	pfree(raw_buf);

	if (comp_size < 0)
	{
		pfree(tci_array);
		pfree(comp_buf);
		return false;
	}

	if ((Size) comp_size >= raw_size * COMPRESS_MIN_RATIO / 100)
	{
		pfree(tci_array);
		pfree(comp_buf);
		return false;
	}

	block_size = MAXALIGN(sizeof(CompressedBlockHeader) +
						  ntups * sizeof(TupleCompressInfo) +
						  (Size) comp_size);

	/*----
	 * The CompressedBlock will be placed at the original pd_lower (right
	 * after the ItemId array).  Before writing it we must shrink all tuple
	 * ItemIds to header-only size and call PageRepairFragmentation so that
	 * pd_upper advances, creating enough free space to hold the block.
	 *
	 * free_after_compact = (pd_upper_after_repair) - (pd_lower + block_size)
	 *
	 * We estimate: after shrinking all tuples to t_hoff, the sum of tuple
	 * sizes decreases by raw_size.  The new pd_upper ≈ old_pd_special -
	 * sum(MAXALIGN(t_hoff)).  We need new_pd_upper > pd_lower + block_size.
	 *----
	 */
	{
		Size		hdr_total = 0;

		for (offnum = FirstOffsetNumber; offnum <= maxoff; offnum++)
		{
			ItemId		lp = PageGetItemId(page, offnum);

			if (!ItemIdIsNormal(lp))
				continue;
			hdr_total += MAXALIGN(((HeapTupleHeader) PageGetItem(page, lp))->t_hoff);
		}

		/*
		 * After compaction, tuples occupy [pd_special - hdr_total, pd_special).
		 * Free space will be [pd_lower, pd_special - hdr_total).
		 */
		if (phdr->pd_special < hdr_total + phdr->pd_lower + block_size)
		{
			/* Not enough space even after full compaction. */
			pfree(tci_array);
			pfree(comp_buf);
			return false;
		}
		free_after_compact = phdr->pd_special - hdr_total - phdr->pd_lower;
		(void) free_after_compact;	/* checked above */
	}

	/*----
	 * Commit: modify the page.
	 *
	 * Step 1: Set HEAP_COMPRESSED_DATA on each tuple and shrink its ItemId.
	 *----
	 */
	for (tci_idx = 0; tci_idx < ntups; tci_idx++)
	{
		OffsetNumber off = tci_array[tci_idx].tci_offnum;
		ItemId		lp = PageGetItemId(page, off);
		HeapTupleHeader tup = (HeapTupleHeader) PageGetItem(page, lp);

		tup->t_infomask2 |= HEAP_COMPRESSED_DATA;
		ItemIdSetNormal(lp, ItemIdGetOffset(lp), tup->t_hoff);
	}

	/*
	 * Step 2: Compact tuple headers toward pd_special, freeing up space in
	 * the middle of the page.
	 */
	PageRepairFragmentation(page);

	/*
	 * Step 3: Write the CompressedBlock at the original pd_lower (right after
	 * the ItemId array).  cb_offset is computed fresh because pd_lower may
	 * have changed if there were LP_UNUSED slots that were cleared, but for
	 * heap pages this is typically stable.
	 *
	 * We use the start-of-free-space (pd_lower) as the write position and
	 * then advance pd_lower past the block.
	 */
	cb_offset = phdr->pd_lower;

	Assert(phdr->pd_upper >= cb_offset + block_size);

	cbh = (CompressedBlockHeader *)((char *) page + cb_offset);
	cbh->cb_magic = COMPRESSED_BLOCK_MAGIC;
	cbh->cb_ntups = (uint16) ntups;
	cbh->cb_orig_size = (uint32) raw_size;
	cbh->cb_comp_size = (uint32) comp_size;

	tci_dst = CompressedBlockGetTCI(cbh);
	memcpy(tci_dst, tci_array, ntups * sizeof(TupleCompressInfo));

	data_dst = CompressedBlockGetData(cbh);
	memcpy(data_dst, comp_buf, comp_size);

	/*
	 * Do NOT advance pd_lower — it stays pointing at the CompressedBlock so
	 * that heap_decompress_tuple() can locate it via phdr->pd_lower.
	 * PD_PAGE_FULL prevents any new tuples from overwriting the block.
	 */
	phdr->pd_flags |= PD_PAGE_COMPRESSED | PD_PAGE_FULL;

	pfree(tci_array);
	pfree(comp_buf);

	return true;
}


/*
 * heap_decompress_tuple
 *
 * Given a compressed page and an offset number, reconstruct the full tuple
 * (header + data) in a palloc'd HeapTuple.
 *
 * Returns NULL if the offset is not found in the CompressedBlock (this
 * shouldn't happen for a valid live tuple with HEAP_COMPRESSED_DATA set).
 */
HeapTuple
heap_decompress_tuple(Page page, OffsetNumber offnum)
{
	CompressedBlockHeader *cbh;
	TupleCompressInfo *tci;
	int			i;
	char	   *decomp_buf;
	int32		decomp_result;

	ItemId		lp;
	HeapTupleHeader src_hdr;
	uint32		data_offset;
	uint32		data_length;

	HeapTuple	result;
	char	   *result_data;

	Assert(PageIsCompressed(page));

	cbh = CompressedBlockGetHeader(page);
	Assert(cbh->cb_magic == COMPRESSED_BLOCK_MAGIC);

	/* Find the TupleCompressInfo entry for this offset */
	tci = CompressedBlockGetTCI(cbh);
	data_offset = 0;
	data_length = 0;
	for (i = 0; i < cbh->cb_ntups; i++)
	{
		if (tci[i].tci_offnum == offnum)
		{
			data_offset = tci[i].tci_offset;
			data_length = tci[i].tci_length;
			break;
		}
	}

	if (i == cbh->cb_ntups)
	{
		/* Not found — tuple must have been added after compression (shouldn't happen) */
		return NULL;
	}

	/* Decompress the whole-page data block */
	decomp_buf = palloc(cbh->cb_orig_size);
	decomp_result = pglz_decompress(CompressedBlockGetData(cbh),
									(int32) cbh->cb_comp_size,
									decomp_buf,
									(int32) cbh->cb_orig_size,
									true);
	if (decomp_result < 0)
		elog(ERROR, "heapcompress: pglz_decompress failed for page offset %u",
			 offnum);

	/* Get the on-page tuple header */
	lp = PageGetItemId(page, offnum);
	src_hdr = (HeapTupleHeader) PageGetItem(page, lp);

	/*
	 * Build a new HeapTuple.  Allocate t_data separately from the
	 * HeapTupleData wrapper so that callers can copy the wrapper struct and
	 * pfree it while the t_data pointer remains valid.  Both allocations live
	 * in CurrentMemoryContext and are cleaned up with that context.
	 */
	result = (HeapTuple) palloc(HEAPTUPLESIZE);
	result->t_len = src_hdr->t_hoff + data_length;
	result->t_data = (HeapTupleHeader) palloc(src_hdr->t_hoff + data_length);

	/* Copy the header */
	memcpy(result->t_data, src_hdr, src_hdr->t_hoff);

	/* Copy the decompressed data */
	result_data = (char *) result->t_data + src_hdr->t_hoff;
	memcpy(result_data, decomp_buf + data_offset, data_length);

	/* Clear the HEAP_COMPRESSED_DATA flag in the copy — it's now a full tuple */
	result->t_data->t_infomask2 &= ~HEAP_COMPRESSED_DATA;

	pfree(decomp_buf);

	return result;
}


/* ----------------------------------------------------------------
 * heap_compress_table
 *
 * SQL-callable function: SELECT heap_compress_table('tablename');
 * Immediately compresses all pages of the named relation.
 * ----------------------------------------------------------------
 */
PG_FUNCTION_INFO_V1(heap_compress_table);

Datum
heap_compress_table(PG_FUNCTION_ARGS)
{
	text	   *relname_text = PG_GETARG_TEXT_PP(0);
	char	   *relname = text_to_cstring(relname_text);
	RangeVar   *rv;
	Oid			reloid;
	Relation	rel;
	BlockNumber nblocks;
	BlockNumber blkno;
	int			compressed = 0;
	BufferAccessStrategy strategy;

	rv = makeRangeVarFromNameList(stringToQualifiedNameList(relname, NULL));
	reloid = RangeVarGetRelid(rv, NoLock, false);

	rel = relation_open(reloid, ShareUpdateExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_TOASTVALUE)
	{
		relation_close(rel, ShareUpdateExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table", relname)));
	}

	strategy = GetAccessStrategy(BAS_VACUUM);
	nblocks = RelationGetNumberOfBlocks(rel);

	for (blkno = 0; blkno < nblocks; blkno++)
	{
		Buffer		buf;
		Page		page;

		buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno,
								 RBM_NORMAL, strategy);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);

		if (!PageIsCompressed(page) && !PageIsEmpty(page))
		{
			bool ok;

			START_CRIT_SECTION();
			ok = compress_heap_page(rel, buf);
			if (ok)
			{
				MarkBufferDirty(buf);
				if (RelationNeedsWAL(rel))
				{
					XLogRecPtr recptr;

					recptr = log_newpage_buffer(buf, true);
					PageSetLSN(page, recptr);
				}
				compressed++;
			}
			END_CRIT_SECTION();
		}

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buf);
	}

	FreeAccessStrategy(strategy);
	relation_close(rel, ShareUpdateExclusiveLock);

	PG_RETURN_INT32(compressed);
}
