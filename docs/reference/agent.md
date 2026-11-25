# quectop agent reference

`quectop` samples one Linux host and keeps that host's history in a data directory. It has no HTTP server and no dashboard. To see the data, stream it to [`quectop-hub`](hub.md).

## Synopsis

```sh
quectop [options]
```

The agent runs in the foreground until it is killed. It needs no root unless the data directory or a sensor requires it.

## Options

Every option except `--help` and `--version` takes one value.

| Option | Default | Range | Description |
| --- | --- | --- | --- |
| `--data-dir DIR` | `$HOME/.local/state/quectop` | | Data directory. Created with missing parents, mode `0755` before umask. |
| `-o DIR`, `--output DIR` | | | Aliases for `--data-dir`. |
| `--live-dir DIR` | the data directory | | Directory for `live.v1.bin` and `disks.v1.bin`. Must exist. Use a RAM-backed directory to save SD card writes. |
| `--interval-ms N` | `5000` | 100–3600000 | Sampling period. |
| `--publish-ms N` | `30000` | 100–3600000 | Live snapshot period. Service queries and mount checks also run at this period. |
| `--points N` | `1000` | 16–10000 | Windows per history range. |
| `--disk-refresh-ms N` | `600000` | 1000–3600000 | Filesystem capacity refresh period. |
| `--disk PATH` | all local filesystems | | Report capacity for this mount point only. |
| `--thermal PATH` | `/sys/class/thermal/thermal_zone0/temp` | | Temperature sensor file, in millidegrees Celsius. |
| `--interface IF` | device-backed interfaces | | Track only interface `IF`. |
| `--services LIST` | none | | Comma-separated system services to report. See [Services](#services). |
| `--hub HOST:PORT` | off | | Stream to `quectop-hub`. `HOST` is an IPv4 address or a name. |
| `--key-file FILE` | none | | Device key line from `quectop-hub --add-device`. Requires `--hub`. |
| `--version`, `-v` | | | Print `quectop 0.1.0` and exit. |
| `--help` | | | Print usage and exit. |

There is no configuration file. Put the options in your service manager's command line.

## Startup

At startup, the agent does the following steps in order:

1. Parses options. An unknown option or a bad value exits with status 1 and a message on stderr.
2. Creates the data directory, then prints its path to stdout.
3. Takes an exclusive lock on the data directory, and on `--live-dir` when that is a different directory. A second agent on the same directory exits with `another quectop owns this data directory`.
4. Reads the host name from `uname`.
5. Detects online CPUs, CPU frequency policies, and network interfaces.
6. Opens the five history files, or creates them when they are missing.

The default data directory requires an absolute `$HOME`. The agent does not read `XDG_STATE_HOME`.

## Files

The agent writes these files.

| File | Directory | Written |
| --- | --- | --- |
| `1h.v1.bin` | data | Every completed 1h window, batched in about 1 KiB writes, at most 60 s apart. |
| `1d.v1.bin`, `7d.v1.bin`, `30d.v1.bin`, `90d.v1.bin` | data | When each window closes. |
| `live.v1.bin` | live | Every `--publish-ms`. Replaced atomically. |
| `disks.v1.bin` | live | After each capacity refresh or mount change. Replaced atomically. |

The [data formats reference](data-formats.md) describes their layout.

The device layout is fixed for a set of history files. It covers the CPU IDs, the frequency policies, the frequency source, the frequency bit width, and the interfaces. `--interval-ms` and `--points` are fixed in the same way. A change to any of them makes startup fail with a configuration mismatch. To change them, use a new data directory.

The agent never calls `fsync`. On power loss, it can lose the summaries in memory, the 1h records that are not yet flushed, and recent writes.

## Hub connection

With `--hub`, the agent also streams its files to the hub. The agent's own files stay the source of truth, and the hub keeps a replica.

- The agent resolves `HOST` again before every connection. It reads `/etc/hosts` first. It then sends an A query over UDP to each IPv4 `nameserver` in `/etc/resolv.conf`, with a 3 s timeout for each one. With no nameserver listed, it uses `127.0.0.1`. The agent does not support search domains, mDNS, or IPv6.
- `--hub` rejects a URL. Write `hub.example.com:7070`, not `http://hub.example.com:7070`.
- After a failed or lost connection, the agent retries after 1 s. Each failure doubles the delay, up to 60 s.
- After each connection, the agent sends the history buckets that the hub is missing, oldest first.
- Data waits in a 32 KiB outbox. When the outbox is full, the agent drops the connection, and the next connection resumes the stream. A snapshot that does not fit in the outbox is skipped until the next one.
- The [hub protocol reference](protocol.md) describes the wire format.

**Host ID.** The hub files each host under an ID:

- With `--key-file`, the ID is the name on the key line.
- Without `--key-file`, the ID is the contents of `/etc/machine-id`.
- When `/etc/machine-id` is missing, the ID is the host name.

**Key file.** The key file holds one line, `NAME HEX`, where `HEX` is 64 hex digits (32 bytes).

- The agent refuses a key file that other users can read. Fix it with `chmod 600 FILE`.
- Without `--key-file`, the agent uses an all-zero key. Only a hub that listens on `127.0.0.0/8` and has no keys file accepts that key.

## Services

`--services` takes one name or a comma-separated list, such as `nginx,postgresql.service,my-app`. Use one `--services` option only.

**Service manager.** The agent picks the manager once, at startup:

- It uses systemd when `/run/systemd/system` exists.
- Otherwise, it uses OpenRC when `/run/openrc` exists.

**Names.** The agent cleans up the list before it uses it:

- With systemd, a name without `.service` gets the suffix.
- With OpenRC, names are used as written.
- Duplicate names are removed.
- An empty or invalid name stops startup.

**Missing data.** A missing manager, service, or permission makes that value unavailable. It never stops sampling.

The agent reports these values for each service:

| Value | Source |
| --- | --- |
| State | systemd `ActiveState`, or the OpenRC state directory. |
| Uptime | systemd `ActiveEnterTimestampMonotonic`, or the age of the OpenRC main process. |
| Cgroup memory | systemd `MemoryCurrent`, or the cgroup's `memory.current` (v2) or memory usage (v1). |
| RSS, PSS, USS, and PSS by anonymous, file, and shared memory | Sum of `/proc/PID/smaps_rollup` over every process in the service cgroup and its descendants. |

**Limits.**

- The agent counts at most 256 processes and walks at most 16 cgroup levels for each service.
- All service queries in one publication share a 100 ms budget, and the first service rotates between publications.
- One `smaps_rollup` read in the kernel can exceed the budget.

The agent starts no helper processes and changes no accounting settings. It never controls services.

## Metrics

| Metric | Source | Precision |
| --- | --- | --- |
| Per-core CPU busy | `/proc/stat` | 0.1% |
| CPU overall, user, system, and iowait | `/proc/stat` | 0.1% |
| CPU, memory, and I/O pressure | `/proc/pressure/{cpu,memory,io}`, the `some` line | 0.1% |
| Memory used | `MemTotal` and `MemAvailable` in `/proc/meminfo` | 0.1% |
| Swap used | `SwapTotal` and `SwapFree` in `/proc/meminfo` | 0.1% |
| Temperature | `--thermal` file | 0.1 °C, from −204.8 to 204.7 |
| CPU frequency, for each policy | `cpuinfo_cur_freq`, falling back to `scaling_cur_freq` | 1 MHz |
| Receive and transmit rate, for each interface | `/proc/net/dev` | 3 significant digits |
| Filesystem capacity | `statfs` on each local mount | 1 KiB |

The CPU definitions follow `/proc/stat`:

- Busy excludes idle and iowait.
- User includes nice.
- System includes IRQ and softIRQ.
- Steal time counts as busy, but not as user or system.

**Pressure.** Each pressure value is the share of the interval in which at least one task stalled. The agent computes it from the cumulative microsecond counter, not from the kernel's averages. A kernel without PSI leaves gaps in the history.

**Interfaces.** By default, the agent tracks interfaces where `/sys/class/net/IF/device` exists. Bridges, `veth` devices, VPN tunnels, and loopback have no such entry, so the agent skips them.

**Filesystems.** The agent reports block-device filesystems (sources under `/dev/`) and ZFS pools. It removes duplicate filesystems. "Available" is the space that a user without root privileges can use.

**Missing readings.** A missing reading is stored as a gap, not as zero. The agent ignores a new interface until it restarts. A new CPU ID stops the agent with an error.
