# quectop

<!-- impeccable:product-schema 1 -->

## Platform
web

## Stack
C probe (`quectop`) and hub (`quectop-hub`) with raw Linux syscalls; independent static HTML, CSS and browser JavaScript. User chose building directly in code. The probe samples and keeps its own history in the selected data directory (explicit CLI path or $HOME/.local/state/quectop, created with missing parents at startup; selected path printed to stdout) and carries no dashboard. With `--hub HOST:PORT` (IPv4 or a name, looked up via /etc/hosts and resolv.conf nameservers) it also streams its files to the hub over a minimal custom TCP protocol, backfilling from its rings after disconnects. The hub keeps its data directory a static site (dashboard, hosts.txt index, one replica per host) that any static web server can serve; an optional built-in server (--http) maps the same paths. Both are standalone static PIE binaries (ASLR, RELRO, stack protector via boot.c) for AMD64, ARM64 and ARMv6. Streams are encrypted with Noise_NNpsk0_25519_ChaChaPoly_SHA512 over vendored Monocypher, each device key (`quectop-hub --add-device`) being the PSK, so both ends authenticate; crypto is checked against Wycheproof, cacophony and the noiseprotocol package (test/crypto.py); no IPv6.

## Product Purpose
Monitor severely constrained Linux machines, such as ARM64 single-board computers, with minimal CPU and memory overhead. Also support AMD64 Linux and a separate ARMv6 little-endian EABI build for compatible 32-bit ARM Linux systems (kernel 5.1+). Architecture selection is compile-time only; ARM32 helpers are absent from the 64-bit executables. The desired collection run time is below 100 ms, subject to measurement on the device.

## Capabilities and Constraints
Detect online CPU IDs, shared-clock frequency policies and device-backed network interfaces at startup. Collect per-core CPU percentages; machine-wide overall/user/system/iowait; PSI `some` stall shares for CPU, memory and I/O; memory and swap percentages; temperature; 1 MHz CPU frequency; separate receive/transmit rates per interface. Percentages use 0.1%, temperature 0.1°C and network three significant digits. Frequency wire width comes from the maximum reported CPU frequency and is stored once. Five fixed-size packed histories cover 1h and 1/7/30/90 days, retaining means, maxima, validity and peak times; minima only for memory, swap, temperature and frequency. CPU/device metadata is packed and fixed for a history layout; changing it under the same schema requires a new data directory. Data filenames include their schema version, so format upgrades automatically select fresh files in the same directory and leave old versions untouched. Mounted filesystem capacities have no history: discover local mounts on kernel change notifications checked during normal publication, deduplicate filesystems, and refresh capacities every ten minutes by default. Size capacity fields from each filesystem's total KiB. Reuse a startup arena and shared scratch buffer; grow filesystem metadata storage only if changed mount data needs more room. Batch 1h writes in about 1 KiB for up to a minute; keep pending data visible. Publish live and capacity snapshots separately, optionally on RAM storage to reduce SD writes. Power-loss data loss is accepted; no fsync/checkpoints. No libc, SQLite or threads; the probe has no HTTP server. The independent static UI decodes only the development schema (v1). The development schema stays at v1; from the first release onward, wire-layout changes bump the schema version, with compatibility considered from then onward. Application version 0.1.0; version/help work without a required data-directory argument; configuration uses CLI flags.

The dashboard shows all detected CPU cores together, CPU user/system and memory used/free tile toggles, calculated binary memory capacities, swap usage, and interface cycling tied to network history. Toggles retain accessible action labels without visible click-to-toggle/cycle helper text. Seven charts show per-core CPU, user/system/iowait, CPU/memory/I/O pressure, one RAM-or-swap series, policy CPU frequencies in GHz, temperature and selected-interface network traffic. An unlabeled Graph/Bars picker at the top-right of the per-core tile switches only that tile between its history graph and current per-core usage meters: two columns (16 cores form two columns of eight rows), one column on narrow screens, and scrolling for large CPU counts. Bars reuse existing latest readings; they add no probe metrics or requests. Disks occupies one chart-sized tile in the history grid, listing mounted-filesystem capacities with its name above an 18px usage meter, compact used/total beneath the meter at 14px, and available space to the right. Network rates use the space-free format U405.0K|D4.1M. Network shares summary colors and heading alignment, with rate text sized to its available width. All byte values render with one decimal place and K/M/G/T/P letters: memory, swap, disk and service capacities use binary units (K = 1024 bytes), network rates decimal units (K = 1000 bytes). Axes fit selected-history extrema, with percentage bounds of 0–100. Inspection synchronizes dotted cursors/dots and a compact floating tooltip across visible charts. It is served from the hub's data directory, with a host picker when hosts.txt lists more than one host.

Optional `--services` detects systemd or OpenRC and removes duplicate names; only systemd
names gain `.service`. Missing backends or data never abort collection. A chart-sized Services
panel defaults to PSS, allows RSS/Private/Cgroup selection, and expands each name to show RSS,
PSS, USS and PSS anonymous/file/shared-memory components. Capacities render with one decimal place
and K/M/G letters.
Retain cgroup memory separately. Sum mapped memory across every process in the service cgroup
and descendants using smaps_rollup by default; status provides RSS fallback. No full process
scan or full smaps fallback. Invalid/partial totals remain unavailable. Deduplicate at most 256
PIDs and traverse at most 16 levels/256 groups. Reuse existing scratch, no retained process arrays.
Systemd uptime uses ActiveEnterTimestampMonotonic; OpenRC main-process age is labeled separately.
Queries run only at publication under a cooperative 100 ms budget, rotating first service;
a single kernel PSS read can exceed the budget. No helpers, librc/libdbus, accounting changes or
service control. Pack exact KiB RSS/PSS varints and four-significant-digit component values with a
KiB/MiB/GiB/TiB unit in the existing v1 live snapshot, no history or extra files. Reserve 30 extra
live bytes per service.

## Brand Commitments

Visual direction: compact monochrome measurement typography, light gray background, near-white panels, teal and violet graph lines, summary metrics above the seven charts. Preserve high information density with responsive layouts. The user requires a minimum 14px UI font, including axes, legends, controls and tooltips. Use the project name quectop.

## Evidence on Hand
No on-device performance measurements yet.
