#!/bin/sh

prefix=/home/h00613086/proj/pg/jemalloc
exec_prefix=/home/h00613086/proj/pg/jemalloc
libdir=${exec_prefix}/lib

LD_PRELOAD=${libdir}/libjemalloc.so.2
export LD_PRELOAD
exec "$@"
