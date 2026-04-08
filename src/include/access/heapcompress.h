/*-------------------------------------------------------------------------
 *
 * heapcompress.h
 *	  Definitions for heap page compression.
 *
 * Each heap page can be compressed by the hourly_heap_compressor background
 * worker.  When compressed, all live tuple data portions (bytes after t_hoff)
 * are concatenated, compressed with pglz, and stored in a CompressedBlock
 * placed at the end of the page (pd_special points to its start).  The tuple
 * headers remain at their original offsets; HEAP_COMPRESSED_DATA is set in
 * t_infomask2 to indicate the data lives in the CompressedBlock.
 *
 * Page layout after compression:
 *
 *   | PageHeader | ItemIds | <free> | TupleHeaders... | CompressedBlock |
 *                                                       ^ pd_special
 *
 * The PD_PAGE_COMPRESSED flag in pd_flags indicates the page is compressed.
 *
 * src/include/access/heapcompress.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAPCOMPRESS_H
#define HEAPCOMPRESS_H

#include "access/htup.h"
#include "storage/buf.h"
#include "storage/bufpage.h"
#include "storage/itemid.h"
#include "utils/relcache.h"

/* Magic number stored in CompressedBlockHeader.cb_magic */
#define COMPRESSED_BLOCK_MAGIC	((uint32) 0xCB10C001)

/*
 * CompressedBlockHeader
 *
 * Stored at offset pd_special within a compressed heap page.
 * Immediately following this struct:
 *   TupleCompressInfo  tci[cb_ntups]   -- per-tuple offset/length info
 *   uint8              data[cb_comp_size] -- pglz-compressed data blob
 */
typedef struct CompressedBlockHeader
{
	uint32		cb_magic;		/* COMPRESSED_BLOCK_MAGIC */
	uint16		cb_ntups;		/* number of tuples whose data is here */
	uint32		cb_orig_size;	/* total size before compression */
	uint32		cb_comp_size;	/* size of compressed data blob */
} CompressedBlockHeader;

/*
 * TupleCompressInfo
 *
 * Per-tuple entry in the array that follows CompressedBlockHeader.
 * Describes where in the decompressed stream this tuple's data lives.
 */
typedef struct TupleCompressInfo
{
	OffsetNumber tci_offnum;	/* ItemId offset number on the page */
	uint16		tci_pad;		/* padding for alignment */
	uint32		tci_offset;		/* byte offset within decompressed stream */
	uint32		tci_length;		/* byte length of data in stream */
} TupleCompressInfo;

/* Accessor macros */
#define PageIsCompressed(page) \
	(((PageHeader)(page))->pd_flags & PD_PAGE_COMPRESSED)

/*
 * The CompressedBlock is stored at pd_lower (= end of ItemId array).
 * pd_lower is NOT advanced after writing the block; it still points to the
 * start of the CompressedBlock.  PD_PAGE_FULL prevents new tuples from
 * overwriting it.
 */
#define CompressedBlockGetHeader(page) \
	((CompressedBlockHeader *)((char *)(page) + ((PageHeader)(page))->pd_lower))

#define CompressedBlockGetTCI(cbh) \
	((TupleCompressInfo *)((char *)(cbh) + sizeof(CompressedBlockHeader)))

#define CompressedBlockGetData(cbh) \
	((char *)(cbh) + sizeof(CompressedBlockHeader) + \
	 (cbh)->cb_ntups * sizeof(TupleCompressInfo))

/* Minimum compression benefit required to actually store a compressed page */
#define COMPRESS_MIN_RATIO	90	/* must compress to < 90% of original */

/*
 * Public API
 */

/* Compress a single heap page in-place; caller holds exclusive buffer lock */
extern bool compress_heap_page(Relation rel, Buffer buf);

/*
 * Given a compressed page and an offset number, reconstruct the full tuple
 * (header + data) into a palloc'd HeapTuple.  Returns NULL if offnum is not
 * found in the CompressedBlock (should not happen for a valid live tuple).
 */
extern HeapTuple heap_decompress_tuple(Page page, OffsetNumber offnum);

/* Background worker entry point */
extern void HourlyHeapCompressorMain(Datum main_arg);

/* Called from postmaster to register the background worker */
extern void HourlyHeapCompressorRegister(void);

#endif							/* HEAPCOMPRESS_H */
