# Copyright (c) 2022-2026, PostgreSQL Global Development Group

backend_sources += files(
  'alignedalloc.c',
  'aset.c',
  # jemalloc_leak.c is added conditionally by the top-level meson.build
  # when -Dwith_jemalloc=true is specified.
  'bump.c',
  'dsa.c',
  'freepage.c',
  'generation.c',
  'mcxt.c',
  'memdebug.c',
  'portalmem.c',
  'slab.c',
)
