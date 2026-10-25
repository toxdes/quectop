# quectop

A small C resource collector for ARMv6, ARM64 and AMD64 Linux, with an independent static dashboard.
Application version **0.1.0** (`quectop --version` or `quectop -v`). No libc, threads, database,
HTTP server, chart library, external fonts, or runtime libraries. Optional service tracking
reads the systemd system manager through the system D-Bus socket.

## Build and release

```sh
make
make release TARGET=linux-arm64
```

The release archive contains one executable, `quectop`, with the static dashboard appended as
an unmapped payload. Building uses Python 3 (standard library only), Bun to minify JavaScript,
and a C compiler/linker; deployment needs none of these tools.
The default target name follows the compiler, including a 32-bit compiler on a 64-bit kernel.
On AMD64, use `make -B release TARGET=linux-amd64`.
Force a rebuild when switching compilers or architectures. Cross compilation with Clang/LLD
needs no target sysroot:

```sh
make -B CC='clang --target=aarch64-linux-gnu --ld-path=/path/to/ld.lld' release TARGET=linux-arm64
```

For 32-bit ARM, use an ARM EABI compiler. The ARMv6 release target supplies
`-march=armv6 -marm -mfloat-abi=soft`, without VFP, NEON or hardware-division requirements:

```sh
make -B CC='clang --target=arm-linux-gnueabi --ld-path=/path/to/ld.lld' release TARGET=linux-armv6
```

The ARMv6 binary targets little-endian ARM Linux EABI, including compatible ARMv7 and
ARMv8 machines with 32-bit execution enabled. It requires **Linux 5.1 or newer** with
the time64 syscalls, avoiding 2038-limited clock structures and runtime fallbacks. ARMv5,
Thumb-only microcontrollers, big-endian ARM32 and legacy OABI are outside this target.
There are no libc or floating-point-library dependencies. Small integer-division helpers
are compiled only into ARM32; startup and syscall selection happen at compile time.
Existing 64-bit binaries contain no ARM32 dispatch or helpers. Wire layouts remain identical.

## Run

Prepare the data directory, then run:

```sh
./quectop --data-dir /path/to/data --interval-ms 5000 --publish-ms 30000 --points 1000
```

`--data-dir DIR` is required and the directory must exist. `-o DIR` and the original
`--output DIR` are aliases. The collector never defaults to the current directory.
On startup it installs or updates `index.html`, `style.css` and `app.js` in that directory.
There is no separate initialization command. Serve that directory with your existing HTTP server.
A single sleeping process uses an absolute monotonic timer and skips missed deadlines.
Use your existing supervisor and static HTTP server; neither is implemented here.

### Dashboard installation and updates

Each build records a 16-character SHA-256 prefix for its bundled dashboard in
`assets.revision`. The hash covers each asset's name, little-endian byte length and exact
served bytes, in CSS/JavaScript/HTML order. It identifies asset content independently of
the application or data-schema version; it is not a security signature.

At startup, a matching revision and existing asset files skip extraction without opening
the executable payload. Asset contents are not reread or rehashed. Missing assets or a
missing/different revision trigger installation: copy through the existing 4 KiB proc scratch
buffer, stage all three files, rename CSS and JavaScript before HTML, then atomically replace
the revision marker last. Interrupted installation is retried on restart. These are individual
file replacements, not an atomic transaction across all files. As with history, there is no fsync
or power-loss durability guarantee. Collector directory locks cover installation as well as sampling.

To update, replace the executable atomically (for example, copy to a temporary sibling and
rename it), then restart the monitor. It installs matching assets automatically; history files
are untouched by asset installation. Managed UI files are overwritten on revision changes.
Configure the external HTTP server to revalidate cached HTML/CSS/JavaScript after updates;
already-open dashboards may need a browser reload.

The payload sits beyond every file-backed ELF loadable page: 4 KiB alignment on AMD64 and
64 KiB on ARM, accommodating ARM64 kernels with 4/16/64 KiB pages. Padding increases disk
size but compresses well. The assets are never mapped into the collector's address space;
there is no asset mapping to unmap afterward, and no additional allocation or sampling work.
Linux may cache executable reads through read-ahead; that cache is reclaimable. The small
installation routine and manifest remain part of the executable's ordinary code/read-only data.
Only installation opens `/proc/self/exe`; version/help do not check or install assets.

### Verification

Build all three releases, leaving the native AMD64 build as `./quectop`. With Clang, LLD,
Bun, Python 3, `qemu-arm` and `qemu-aarch64` installed, run:

```sh
python3 test/abi.py --target native --linker /path/to/ld.lld
python3 test/abi.py --target arm64 --linker /path/to/ld.lld
python3 test/abi.py --target armv6 --linker /path/to/ld.lld
python3 test/verify.py --arm-binary dist/quectop-linux-arm64/quectop --arm32-binary dist/quectop-linux-armv6/quectop
python3 test/verify.py --fixture-target arm64 --fixtures-only --linker /path/to/ld.lld
python3 test/verify.py --fixture-target armv6 --fixtures-only --linker /path/to/ld.lld
```

QEMU user-mode execution needs no guest distribution because the collectors are static
and libc-free. ARM32 runs on emulated ARM1176 (ARMv6) and Cortex-A7 (ARMv7) CPUs. ABI checks
cover division/remainders, sparse-file offsets beyond 4 GiB, filesystem structures, clocks,
polling and high-address mappings. Fixture checks cover 1/2/4/8/16/256 CPUs, offline/returning
cores, packed histories, extrema, browser decoding and dashboard behavior. Execution checks
cover sampling, directory locks, restart, asset extraction, matching-revision write avoidance,
revision updates and missing assets. QEMU uses the host kernel; this is not a full guest-kernel
boot or a measurement of real ARM hardware performance.

For SD cards, publish live snapshots in a separate existing RAM-backed directory:

```sh
./quectop -o /path/to/persistent/data --live-dir /path/to/ram/data
```

Serve the web assets, the five history files, and RAM-backed `live.v7.bin` and `disks.v2.bin` at the
same public URL level. Without `--live-dir`, snapshots go to the data directory. No mounts,
server configuration or service files are managed by quectop.

| Option | Default | Purpose |
| --- | --- | --- |
| `--interval-ms` | 5000 | Sampling period, 100–3600000 ms |
| `--publish-ms` | 30000 | Live publication period, 100–3600000 ms |
| `--points` | 1000 | Maximum windows per range, 16–10000 |
| `--disk-refresh-ms` | 600000 | Filesystem capacity period, 1000–3600000 ms |
| `--interface IF` | All except loopback | Restrict tracked network interfaces |
| `--disk PATH` | All mounted local filesystems | Restrict capacity reporting to one mount point |
| `--thermal PATH` | `/sys/class/thermal/thermal_zone0/temp` | Millidegree temperature sensor |
| `--services LIST` | None | Comma-separated system services; `.service` suffix optional |

There is no config-file parser. Keep settings in the supervisor's startup arguments.

### System service stats

```sh
./quectop --data-dir /path/to/data --services nginx,postgresql.service,my-app
./quectop --data-dir /path/to/data --services my-app
```

One option accepts either one name or a comma-separated list. Names are normalized to
`.service`, duplicates are removed, and empty entries or invalid names are rejected at startup.
Use instantiated names for template services, for example `worker@one`. These are **system**
services; user session managers are not queried. Changing the service list requires a restart
but does not change history configuration.

The **Services** tile matches a graph panel's dimensions and contains just **Name, Mem, Uptime**.
It is hidden when no services are selected. Mem is systemd's `MemoryCurrent`, including the
service cgroup rather than only the main process; unavailable accounting is shown as `—`.
Uptime measures time since the unit entered the active state, using systemd's monotonic timestamp
so wall-clock adjustments cannot change it. Inactive, failed and transitional units show their
state in the uptime column. Unloaded/missing units or denied queries show `unavailable`.
These values match the memory and active-since concepts used by `usage.py`; tasks and restart
counts are not collected. Nothing enables accounting or starts, stops or loads a service.

Queries run only at live publication (30 seconds by default), not every sample. One persistent,
nonblocking D-Bus connection sends `GetUnit` and individual read-only property requests, without
spawning `systemctl`, linking libdbus, subscribing to signals or scanning processes/cgroups.
The existing 4 KiB scratch buffer holds requests and replies. All service I/O shares a 100 ms
deadline per publication; transport failures are retried on the next publication. With many
services or an overloaded manager, entries beyond that budget can be unavailable. System-bus
permissions and systemd memory accounting determine which values can be read.

Only current values are stored, packed into the existing atomic live snapshot: no service
history, additional file or extra publication write. Each record uses its name, two metadata
bytes, and variable-length memory bytes/uptime seconds. Service names and worst-case live space
are reserved once in the existing arena; no service allocation, connection or query occurs when
the option is absent. The embedded executable includes the service code in either case.

## Measurements and dashboard

* All online CPU IDs are discovered at startup, including nonconsecutive IDs. The first graph
  displays all tracked logical CPUs together. The default history range is **1h**. The picker in this tile’s top-right corner switches only this tile between
  **Graph** history and **Bars** showing the latest per-core percentages. Bars use two columns
  (eight rows for 16 cores), one column on narrow screens, and scrolling for large CPU counts.
  They reuse existing samples without additional collector work or fetches.
  Overall/user/system percentages use aggregate
  counters; overall usage is not an average of rounded per-core values.
* CPU frequency is sampled once per shared-clock policy, using its leading online CPU.
  The stored value is rounded to **1 MHz**. The maximum `cpuinfo_max_freq` across detected
  policies determines the shared packed width: 1000 MHz needs 10 bits; 4300 MHz needs 13.
  `cpuinfo_cur_freq` is preferred; `scaling_cur_freq` is the fallback and is labeled
  driver-reported. Driver-reported clocks may differ from actual instantaneous clocks.
  Missing or unreadable readings are gaps. MHz values are displayed as GHz in the graph.
* Memory uses `MemTotal` and `MemAvailable`; "free" means available memory including reclaimable
  cache. The tile estimates bytes from total RAM and the stored 0.1% value. Click to switch
  used/free; capacity units are explicitly KiB/MiB/GiB/TiB. CPU time similarly switches
  user/system. Both support keyboard activation.
* Swap total/free come from the existing `/proc/meminfo` read. Swap percentages retain history;
  the memory chart's button switches its single series between RAM and swap. Disabled swap
  displays `off` and creates missing swap readings, not artificial zero samples.
* Each tracked interface has separate receive/transmit histories. Click the network tile to
  cycle both its current rates and graph. The tile uses `U4.2K|D2.5M`; its caption identifies
  per-second rates and its title explains K/M/G as decimal KB/MB/GB. Graphs/tooltips use explicit
  B/s, KB/s, MB/s or GB/s. A first, reset or returning interface needs a fresh counter baseline.
* The **Disks** tile is a current capacity list in one chart-sized panel, with no disk history. Mounted block-device filesystems
  (`/dev/...` sources) and ZFS pools are included; RAM, pseudo and network filesystems are
  excluded. Duplicate filesystem IDs or sources are collapsed. Each capacity entry shows its name above an 18px usage meter, compact used/total
  beneath it, and space available to an unprivileged user on the right; filesystem reservations can make available
  smaller than total minus used. Unmounted physical disks have no filesystem capacity row.

CPU IDs, frequency policies, frequency source, and interface metadata are fixed for a history
layout. Restart with a **new data directory** after changing that set or packed frequency width
under the same schema; format upgrades use new versioned filenames automatically.
Tracked CPUs/interfaces that disappear become unavailable; returning counters establish a fresh
baseline. New interfaces are ignored until restart; a new CPU ID causes an explicit error.
Frequency values beyond the packed width fail explicitly rather than silently truncate.

Filesystem discovery is independent: a nonblocking mount-change check runs during existing live
publication, without a new timer wakeup. The mount list is reparsed only on changes. Capacities
refresh every **10 minutes** by default and immediately after discovery changes. Device metadata
uses the startup arena initially; a larger replacement is mapped only when changed mount data
outgrows that space. Old replacement mappings are released. Capacity queries can still be delayed
by a failing local filesystem. Proc rows longer than the fixed 4095-byte line limit are skipped
outside CPU parsing; skipped mount rows make the capacity snapshot visibly partial.

Hover or focus a chart for a synchronized dotted cursor, matching series dots and a floating
mean-value tooltip. Historical means, min/max bands and peak times are retained. Left/Right
step windows; Home/End select oldest/latest; Escape clears; PageUp/PageDown scroll long tooltips.
Detailed ranges and peak times remain accessible to screen readers. Hover updates overlays
without redrawing the graph lines. Each vertical axis fits the selected history's extrema with
padding; percentages remain within 0–100%. Read the axes when comparing differently scaled plots.

CPU busy excludes idle/iowait; user includes nice, system includes IRQ/softIRQ. Steal time is
included in busy but not user/system. Neither CPU frequency nor temperature alone proves
throttling. Throttling flags, memory pressure and disk I/O rates are not collected.

## Storage and writes

Five fixed-size rings cover **1h, 1d, 7d, 30d and 90d**, with 999 completed slots by default plus
one active window. Widths are `ceil(span/(points-1))`, rounded to seconds; 1h width is also at
least `ceil(interval_ms/1000)`. Five-second sampling provides about 720 actual samples per hour.
Each sample feeds all summaries directly; raw samples are discarded. Missing windows stay gaps.
Means, minima, maxima, validity and each maximum's sample time are stored.

Example: four CPUs 0–3, one 10-bit frequency policy, interface `wlan0`, 1000 points, 5s samples:

| File | Window | Bytes |
| --- | ---: | ---: |
| `1h.v7.bin` | 5s | 67005 |
| `1d.v7.bin` | 87s | 73998 |
| `7d.v7.bin` | 606s | 78993 |
| `30d.v7.bin` | 2595s | 81990 |
| `90d.v7.bin` | 7784s | 83988 |

The five histories total **385974 bytes**, about 377 KiB. Sizes scale with actual CPU/policy/
interface counts; history is not loaded into collector RAM. One startup arena supplies working
arrays. Normal sampling performs no memory allocations. Open sensor and proc descriptors are reused.

History files are pre-sized. Completed records overwrite their timestamp-selected slots using
positioned writes, without a mutable cursor header. The 1h ring batches about 1 KiB of records
(or one larger record) and flushes on capacity, wrap, or the next sample after a 60s deadline.
The longer rings write when their windows close. Pending 1h records remain visible in `live.v7.bin`.

`live.v7.bin` is atomically published every 30 seconds by default. `disks.v2.bin` is atomically
published on capacity refresh or mount changes, not on every sample. The browser polls every
15 seconds while visible, fetches only the selected history when its committed marker changes,
and fetches capacities only when their publication marker changes. Polling pauses in hidden tabs.

There is no fsync, durability checkpoint or shutdown flush. RAM summaries, pending 1h records and
recent filesystem writes may be lost on power loss/exit. Checksums let readers skip torn history
records. Small logical writes do not guarantee equally small physical SD writes. CPU time,
resident memory and SD behavior still need measurement on the target Pi.

**Data filenames include the schema version.** Current histories are `1h.v7.bin`, `1d.v7.bin`,
`7d.v7.bin`, `30d.v7.bin` and `90d.v7.bin`; the live snapshot is `live.v7.bin`. Capacities have
an independent schema and use `disks.v2.bin`. A schema upgrade automatically selects a fresh
set of names in the same directory. Previous versions and legacy unversioned files are ignored
and left untouched. No reset flag, migration or deletion is needed for schema upgrades.

The application version (currently 0.1.0) is independent of the data schema. When changing a wire
layout, bump `QUECTOP_SCHEMA_VERSION` in `main.c` and `schemaVersion` in `web/app.js` together; for
capacity-only changes, bump their disk-schema constants instead. Deploy matching collector and
web assets. Version-derived names, headers and reader checks then follow those constants.

Changing the interval, point count or tracked devices under the **same schema** remains a
configuration mismatch; those changes still need a separate data directory. Compatible histories
resume normally. Existing files are never silently reset or migrated.

## Schema policy during development

Development supports only the current data schema; there are no backward readers or migrations.
Old versioned files are ignored and left untouched. Keep incrementing the development schema
when its layout changes. Once the format is finalized, reset the released schema number to v1;
compatibility guarantees apply from that finalized release onward. Application version and
schema version remain independent.

## Packed format v7

Integers and bit fields are little endian; fields pack least significant bit first. Headers,
records and checksums are explicitly encoded as bytes at fixed offsets, independent of host
byte order, structure padding or alignment. No native C structures are written to files.
The current development formats are v7 history/live and v2 capacity snapshots. `N` is CPU
count, `F` frequency-policy count, `I` interface count, and `W` frequency width in bits.
There are `M = N + 6 + F + 2I` metrics in this order:

1. N per-core percentages.
2. Overall, user, system, memory-used and swap-used percentages: 10 bits each, 0.1% steps.
3. Temperature: 12 bits, tenths °C biased by 2048 (−204.8…204.7°C).
4. F frequencies: W bits each, integer MHz; W is 1–16, chosen at startup.
5. Each interface's receive then transmit rate: 14 bits each (10-bit mantissa 0–999,
   2-bit decimal count 0–2, 2-bit decimal B/KB/MB/GB unit). Rates above 999 GB/s fail.

A value set needs `V = 10(N+5) + 12 + FW + 28I` bits. Network rates retain three significant
figures; internal sums retain greater precision until encoding. A latest sample uses M validity
bits then V value bits. Each history record uses a 32-bit epoch-second start, M validity bits,
three value sets (mean/min/max), M peak-second offsets of `bits_needed(width-1)` bits, byte
padding, then FNV-1a32. Validity belongs to the metric across all three sets.

### Device metadata

CPU IDs use the minimum `id_bits` needed for their maximum. Packed CPU IDs are followed without
intermediate padding by F pairs of (leading CPU ID, 1-bit hardware-source flag). Pad to a byte,
then encode I names as (1-byte length, 1–15 name bytes). The complete metadata is checksummed and
compared byte-for-byte on restart; no maximum-sized CPU/interface arrays are reserved.

### History header: 64 bytes, then device metadata

| Offset | Field |
| ---: | --- |
| 0 | `QUECTOPH` |
| 8 | u32 version=7, points, span, width, interval_ms, record_bytes, tier |
| 36 | u32 CPU count, metric count, complete header bytes, CPU ID bits |
| 52 | u32 interface count |
| 56 | u16 frequency count; u8 frequency bits; u8 reserved=0 |
| 60 | FNV-1a32 over bytes 0–59 followed by device metadata |
| 64 | Device metadata, then `points-1` fixed-size record slots |

### Live header: 144 bytes, then payload

| Offset | Field |
| ---: | --- |
| 0 | `QUECTOPL` |
| 8 | u32 version=7, points, interval_ms, publish_ms |
| 24 | u64 generated_ms, uptime_ms, total RAM KiB |
| 48 | u32 latest epoch seconds, CPUs, CPU ID bits, metrics |
| 64 | Five u32 window widths |
| 84 | Five u32 committed start seconds |
| 104 | u32 pending 1h count, interface count |
| 112 | u64 total swap KiB |
| 120 | u32 frequency count, frequency bits, metadata bytes, service count |
| 136 | u64 disk snapshot generated_ms |
| 144 | Metadata, latest sample, five active records, pending 1h records, service entries, final FNV-1a32 |

Service entries are current stats only and do not alter history metrics. Each contains a u8
name length, u8 flags, that many ASCII name bytes (including `.service`, no NUL), then optional
unsigned base-128 varints for memory in bytes and uptime in seconds, in that order. Varints
store the least significant 7-bit group first; the high bit means more bytes follow, up to ten
bytes per u64. Flags bits 0–2 encode unknown/inactive/active/failed/activating/deactivating/
reloading/other (0–7); bit 3 means memory is present; bit 4 means uptime is present; bits 5–7
are zero. Uptime is present only for active/reloading units. Missing values have no counter
bytes and are not encoded as zero. The live checksum covers these entries too.

### Capacity snapshot v2: `disks.v2.bin`

A 28-byte header contains `QUECTOPD` at offset 0, u32 schema version at 8, u64 generated_ms at 12,
u32 row count at 20 and u32 partial flags at 24.
Each row starts with u16 source length, u16 mount-point length, then a byte holding validity in
bit 0 and `(capacity_bits-1)` in bits 1–6; bit 7 is reserved. Pack total/used/available KiB with
that shared width, chosen from total capacity, pad to a byte, then append source and mount bytes.
The file ends with FNV-1a32 over all prior bytes. Thus a 32 GiB filesystem needs 26 bits per
capacity value, not three fixed 64-bit integers. Sub-KiB remainders are discarded; displayed
capacities use three significant figures. Filesystem names are rendered as text, not HTML.
