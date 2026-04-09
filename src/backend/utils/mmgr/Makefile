#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/mmgr
#
# IDENTIFICATION
#    src/backend/utils/mmgr/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/mmgr
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

JEDIR ?= $(top_srcdir)/jemalloc
ifdef with_jemalloc
override CPPFLAGS += -DUSE_JEMALLOC -D_GNU_SOURCE -I$(JEDIR)/include
endif

OBJS = \
	alignedalloc.o \
	aset.o \
	$(if $(with_jemalloc),jemalloc_leak.o) \
	bump.o \
	dsa.o \
	freepage.o \
	generation.o \
	mcxt.o \
	memdebug.o \
	portalmem.o \
	slab.o

include $(top_srcdir)/src/backend/common.mk
