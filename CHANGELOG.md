# Changelog

## v10.0 (2026-07-14)

### Added
- VFS-level kprobe (`vfs_read`) + `vfs_fadvise(WILLNEED)` prefetch
- Markov, Bigram, FreqRecency, Stride predictors
- Sequential bypass with hysteresis
- Adaptive prefetch span (per-strategy multiplier)
- Per-file accuracy gate
- Auto-threshold with HDD-specific limits
- Death spiral recovery mechanism
- Process-aware hashing (optional)
- Waste tracking (optional)

### Fixed
- HDD regression: Reduced span 512KB→4KB, threshold 200→700, region 16KB→64KB
- Stride predictor bypassing sequential bypass (`strat != NSD_STRAT_NONE` check)
- Auto-tune hardcoded HDD values → defines
- Autothresh death spiral causing threshold oscillation
- kprobe auto-enabled in init

### Performance
- SSD sequential 64K: +20.5% (390→470 MB/s)
- HDD sequential 64K: −4% to 0% (regression from −70.7% fixed)

## v9.x — Pre-release (not tagged)

- v9.x: WIP, never stabilized

## v8.0 — Pre-release

- VFS-read kprobe + page-cache prefetch (base architecture)

## v7.1 — Pre-release

- Synaptic region architecture + adaptive strategy selection
- Block-level kprobe (`submit_bio_noacct`) + raw bio prefetch

## v6.0 — Pre-release

- Multi-destination Markov chains (top-4)
- xarray-based storage
- Lock-free pending prefetch hash set
- Confidence system with pruning
