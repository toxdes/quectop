# quectop

<!-- impeccable:product-schema 1 -->

## Platform
web

## Stack
C collector with raw Linux syscalls; independent static HTML, CSS and browser JavaScript. User chose building directly in code. Ship one executable with dashboard assets appended outside the ELF loadable pages. Startup compares a 16-character SHA-256 asset revision and installs missing or changed assets into the required data directory, then monitors. No separate initialization command, asset mapping, runtime SHA implementation, or HTTP server. Extraction reuses the existing 4 KiB scratch buffer; unchanged assets require only startup file checks.

## Product Purpose
Monitor a severely constrained Pi Zero 2 W running ARM64 Debian/DietPi with minimal CPU and memory overhead. Also support AMD64 Linux and a separate ARMv6 little-endian EABI build for compatible 32-bit ARM Linux systems (kernel 5.1+). Architecture selection is compile-time only; ARM32 helpers are absent from the 64-bit executables. The desired collection run time is below 100 ms, subject to measurement on the device.

## Capabilities and Constraints
Detect online CPU IDs, shared-clock frequency policies and network interfaces at startup. Collect per-core CPU percentages; machine-wide overall/user/system; memory and swap percentages; temperature; 1 MHz CPU frequency; separate receive/transmit rates per interface. Percentages use 0.1%, temperature 0.1°C and network three significant digits. Frequency wire width comes from the maximum reported CPU frequency and is stored once. Five fixed-size packed histories cover 1h and 1/7/30/90 days, retaining means, extrema, validity and peak times. CPU/device metadata is packed and fixed for a history layout; changing it under the same schema requires a new data directory. Data filenames include their schema version, so format upgrades automatically select fresh files in the same directory and leave old versions untouched. Mounted filesystem capacities have no history: discover local mounts on kernel change notifications checked during normal publication, deduplicate filesystems, and refresh capacities every ten minutes by default. Size capacity fields from each filesystem's total KiB. Reuse a startup arena and shared scratch buffer; grow filesystem metadata storage only if changed mount data needs more room. Batch 1h writes in about 1 KiB for up to a minute; keep pending data visible. Publish live and capacity snapshots separately, optionally on RAM storage to reduce SD writes. Power-loss data loss is accepted; no fsync/checkpoints. No libc, SQLite, threads or HTTP server. The independent static UI decodes only the current development schema (v6). Finalized releases will start their schema numbering at v1, with compatibility considered from then onward. Application version 0.1.0; version/help work without a required data-directory argument; configuration uses CLI flags.

The dashboard shows all detected CPU cores together, CPU user/system and memory used/free tile toggles, calculated binary memory capacities, swap usage, and interface cycling tied to network history. Toggles retain accessible action labels without visible click-to-toggle/cycle helper text. Six charts show per-core CPU, user/system, one RAM-or-swap series, policy CPU frequencies in GHz, temperature and selected-interface network traffic. An unlabeled Graph/Bars picker at the top-right of the per-core tile switches only that tile between its history graph and current per-core usage meters: two columns (16 cores form two columns of eight rows), one column on narrow screens, and scrolling for large CPU counts. Bars reuse existing latest readings; they add no collector metrics or requests. Disks occupies one chart-sized tile in the history grid, listing mounted-filesystem capacities with its name above an 18px usage meter, compact used/total beneath the meter at 14px, and available space to the right. Network rates use the space-free format U4.2K|D2.5M. Network shares summary colors and heading alignment, with rate text sized to its available width. Capacity units explicitly use KiB/MiB/GiB; network rates use decimal units. Axes fit selected-history extrema, with percentage bounds of 0–100. Inspection synchronizes dotted cursors/dots and a compact floating tooltip across visible charts. HTTP serving remains external.

Optional `--services` accepts comma-separated system service names, normalizes `.service` and
removes duplicates. A chart-sized Services panel contains a simple Name/Mem/Uptime table, hidden
without opt-in services. Memory is systemd MemoryCurrent; uptime uses ActiveEnterTimestampMonotonic
while active/reloading, with inactive/failure states shown in the uptime column. Unavailable
values remain explicit. Query the system manager directly over one nonblocking D-Bus connection
only at publication, with a shared 100 ms deadline and the existing scratch buffer. No helper
processes, libdbus, process scans, accounting changes or service control. Pack current stats in
the existing v6 live snapshot with variable-length counters; no service history or extra files.
Service metadata is allocated once in the startup arena only when selected.

## Brand Commitments

Follow the supplied screenshot: compact monochrome measurement typography, light gray background, near-white panels, teal and violet graph lines, summary metrics above the six charts. Preserve its information density with responsive layouts. The user requires a minimum 14px UI font, including axes, legends, controls and tooltips. Use the project name quectop.

## Evidence on Hand
Layout reference: `/home/bets/Pictures/screenshots/2026-10-23_08-34-01_516686969.png`. Graph interaction reference: `/home/bets/Pictures/screenshots/2026-10-23T10:23:36,614237482+05:30.png`. No on-device performance measurements yet.
