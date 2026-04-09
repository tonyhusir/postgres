/*-------------------------------------------------------------------------
 *
 * jemalloc_leak.h
 *	  jemalloc-based memory leak detection for PostgreSQL.
 *
 * src/include/utils/jemalloc_leak.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef JEMALLOC_LEAK_H
#define JEMALLOC_LEAK_H

#ifdef USE_JEMALLOC

#include "fmgr.h"

/* GUC variables */
extern PGDLLIMPORT bool jemalloc_leak_detection;
extern PGDLLIMPORT char *jemalloc_leak_dump_dir;

/* GUC assign hook */
extern void assign_jemalloc_leak_detection(bool newval, void *extra);

/* Internal entry point called by jemalloc_funcs.c */
extern Datum jemalloc_leak_report_internal(FunctionCallInfo fcinfo);

#endif							/* USE_JEMALLOC */

#endif							/* JEMALLOC_LEAK_H */
