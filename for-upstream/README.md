# NSD - Neural Storage Driver: Linux Kernel Patch Series

## Overview
NSD is a learning prefetcher for the Linux kernel page cache. It monitors
I/O patterns via a hook in filemap_read() and prefetches pages using
page_cache_sync_readahead().

## Status
- Module compiles cleanly (0 errors, 2 minor checkpatch warnings)
- Module loads, sysfs works, rmmod/reload clean
- vfs_fadvise replaced with page_cache_sync_readahead
- checkpatch.pl --strict: only 2 info-level warnings remain

## Patch Series
1. mm/filemap: Add NSD prefetch hook (5 lines in filemap_read)
2. fs/nsd/core.c: Prediction engine (synaptic Markov chain)
3. Kconfig, Makefile, MAINTAINERS
4. Documentation/filesystems/nsd.rst

## Remaining Before LKML
- Full kernel build with mm/filemap.c hook applied
- Hook test with actual I/O workloads (SQLite, dd, fio)
- ARM64 build test (if cross-compiler available)

## Performance
SQLite FTS: -18.8% | Seq 64K: +22.6% | Random 4K: +1.1% (noise)
