# Data formats reference

The agent writes these files, and the hub keeps byte-identical copies. The dashboard decodes them in `web/codec.js`. The C writers are in `history.h`, `metrics.h`, and `disks.h`.

## Conventions

- All integers are little-endian.
- Bit fields pack starting at the least significant bit.
- Every field is encoded byte by byte at a fixed offset. No C structure is written to a file directly.
- Checksums are FNV-1a 32-bit.

Each file name carries its schema version:

| File | Schema constant |
| --- | --- |
| `1h.v1.bin`, `1d.v1.bin`, `7d.v1.bin`, `30d.v1.bin`, `90d.v1.bin`, `live.v1.bin` | `QUECTOP_SCHEMA_VERSION` in `protocol.h`, `schemaVersion` in `web/codec.js` |
| `disks.v1.bin` | `QUECTOP_DISK_SCHEMA_VERSION` in `protocol.h`, `diskSchemaVersion` in `web/codec.js` |

After a schema change, the agent writes new file names in the same directory. Files with other versions are ignored and left in place.

**Schema policy.** During development, the schema stays at v1 and has no readers for older versions. From the first release, every layout change bumps the schema version. The application version, 0.1.0, is separate from the schema version.

## Metrics

Here, `N` is the CPU count, `F` the number of frequency policies, `I` the interface count, and `W` the frequency width in bits. There are `M = N + 10 + F + 2I` metrics, in this order:

| Order | Metrics | Bits each | Encoding |
| --- | --- | ---: | --- |
| 1 | `N` per-core CPU busy | 10 | 0.1% steps |
| 2 | Nine values: CPU overall, user, system, iowait, CPU pressure, memory pressure, I/O pressure, memory used, and swap used | 10 | 0.1% steps |
| 3 | Temperature | 12 | 0.1 °C steps, biased by 2048 |
| 4 | `F` frequencies | `W` | MHz. `W` is 1–16, from the highest `cpuinfo_max_freq`. |
| 5 | Receive, then transmit, for each interface | 14 | 10-bit mantissa (0–999), 2-bit decimal count (0–2), and 2-bit unit (B, KB, MB, or GB) |

A full value set takes `V = 10(N+9) + 12 + FW + 28I` bits. A rate above 999 GB/s stops the agent with an error.

The minimum set covers only memory, swap, temperature, and frequencies. It takes `L = 32 + FW` bits.

## Device metadata

Device metadata follows each file header. The agent compares it byte for byte on restart.

1. CPU IDs, each `id_bits` wide. `id_bits` is the smallest width that holds the highest ID.
2. `F` pairs of the policy's first CPU ID (`id_bits`) and a 1-bit source flag. The flag is `1` for `cpuinfo_cur_freq` and `0` for `scaling_cur_freq`.
3. Padding to a byte.
4. `I` interface names, each a `u8` length (1–15) and the name bytes.

## History files

Each history file has a 64-byte header, the device metadata, then `points − 1` fixed-size record slots.

| Offset | Field |
| ---: | --- |
| 0 | `QUECTOPH` |
| 8 | `u32` version, points, span, width, interval_ms, record_bytes, tier |
| 36 | `u32` CPU count, metric count, complete header bytes, CPU ID bits |
| 52 | `u32` interface count |
| 56 | `u16` frequency count, `u8` frequency bits, `u8` reserved (0) |
| 60 | FNV-1a over bytes 0–59 and the device metadata |
| 64 | Device metadata |

**Ranges.** The five tiers cover 1 hour, 1 day, 7 days, 30 days, and 90 days.

- A window is `ceil(span / (points − 1))` seconds wide.
- A 1h window is at least `ceil(interval_ms / 1000)` seconds.
- The start time of a record selects its slot, so the file has no write cursor.

A record holds these fields, in order:

1. `u32` window start, in Unix seconds.
2. `M` validity bits.
3. Mean set, `V` bits.
4. Minimum set, `L` bits.
5. Maximum set, `V` bits.
6. `M` peak offsets, each `bits_needed(width − 1)` bits: the seconds from the window start to each maximum.
7. Padding to a byte.
8. FNV-1a over the record.

Validity applies to a metric across all sets. A reader skips a record with a bad checksum.

**Example size.** The example host has four CPUs, one 10-bit frequency policy, interface `wlan0`, 1000 points, and 5 s samples:

| File | Window | Bytes |
| --- | ---: | ---: |
| `1h.v1.bin` | 5 s | 67005 |
| `1d.v1.bin` | 87 s | 75996 |
| `7d.v1.bin` | 606 s | 81990 |
| `30d.v1.bin` | 2595 s | 85986 |
| `90d.v1.bin` | 7784 s | 87984 |

## Live snapshot: `live.v1.bin`

| Offset | Field |
| ---: | --- |
| 0 | `QUECTOPL` |
| 8 | `u32` version, points, interval_ms, publish_ms |
| 24 | `u64` generated_ms, uptime_ms, total RAM KiB |
| 48 | `u32` latest sample Unix seconds, CPU count, CPU ID bits, metric count |
| 64 | Five `u32` window widths |
| 84 | Five `u32` committed window starts |
| 104 | `u32` pending 1h record count, interface count |
| 112 | `u64` total swap KiB |
| 120 | `u32` frequency count, frequency bits, metadata bytes, service count |
| 136 | `u64` disk snapshot generated_ms |
| 144 | Payload |

The payload holds these fields, in order:

1. Device metadata.
2. The latest sample: `M` validity bits, then `V` value bits.
3. The active record for each of the five tiers.
4. The pending 1h records.
5. The host name: a `u8` length (0–64), then the `uname` node name.
6. Service entries.
7. FNV-1a over the whole file.

### Service entries

| Field | Encoding |
| --- | --- |
| Name length | `u8` |
| Flags | `u8`, see below |
| Name | Bytes, no terminator |
| Cgroup memory | Bytes, varint, when flag bit 3 is set |
| Uptime | Seconds, varint, when flag bit 4 is set |
| Detail | When flag bit 5 is set, see below |

A varint is unsigned base-128. The lowest 7 bits come first, and a set high bit means that more bytes follow.

| Flag bits | Meaning |
| --- | --- |
| 0–2 | State: 0 unknown, 1 inactive, 2 active, 3 failed, 4 activating, 5 deactivating, 6 reloading, 7 other |
| 3 | Cgroup memory present |
| 4 | Uptime present. Only for active and reloading services. |
| 5 | Mapped-memory detail present |
| 6 | OpenRC |
| 7 | Uptime is the age of the main process. Requires bit 4. |

The detail starts with a nonzero `u8` mask. Bits 0–5 select RSS, PSS, USS, PSS anonymous, PSS file, and PSS shared memory. The fields that follow depend on the mask:

1. RSS, then PSS, each in KiB as a varint, when present.
2. USS, anonymous, file, and shared memory, when present. Each value is 17 bits: a 14-bit mantissa (0–9999), a decimal-point bit, and a 2-bit unit (KiB, MiB, GiB, or TiB).
3. Zero bits that pad the detail to a byte.

The components (bits 3–5) require PSS. A missing value takes no space.

## Capacity snapshot: `disks.v1.bin`

| Offset | Field |
| ---: | --- |
| 0 | `QUECTOPD` |
| 8 | `u32` schema version |
| 12 | `u64` generated_ms |
| 20 | `u32` row count |
| 24 | `u32` partial flags |
| 28 | Rows |

Each row has these fields:

1. `u16` source length, then a `u16` mount point length.
2. One byte: bit 0 is validity, and bits 1–6 hold `capacity_bits − 1`. Bit 7 is reserved.
3. Total, used, and available KiB, each `capacity_bits` wide.
4. Padding to a byte.
5. Source bytes, then mount point bytes.

The file ends with FNV-1a over all earlier bytes. The capacity width comes from the total size, so a 32 GiB filesystem uses 26 bits for each value.
