# Architecture

This document describes the components of quectop, how data moves between them, and the reasons for the main design decisions. quectop targets machines with limited memory, CPU, and flash storage, so the monitor itself must use few resources. Most decisions below follow from that requirement.

## Components

```text
 device                          hub machine                     browser
┌──────────────────┐   TCP     ┌──────────────────┐   HTTP     ┌───────────┐
│ quectop (probe)  │ ────────▶ │ quectop-hub      │ ─────────▶ │ dashboard │
│ samples, keeps   │ encrypted │ keeps a replica  │  static    │ decodes   │
│ its own history  │           │ as a static site │  files     │ the files │
└──────────────────┘           └──────────────────┘            └───────────┘
```

| Component | Role |
| --- | --- |
| `quectop` (the probe) | Runs on each monitored device. It samples the device and writes its own history files. It does not listen on any port. |
| `quectop-hub` (the hub) | Accepts connections from probes. It keeps an exact copy of each device's files in a directory that is also a website. |
| Dashboard | HTML, CSS, and JavaScript. The browser downloads the binary data files and decodes them. |

The probe always opens the connection to the hub. The hub never connects to a device. As a result, a device needs no open port, and a device behind NAT needs no additional configuration.

## Data ownership

The probe writes its history to local storage first, whether or not a hub is configured. The hub holds a replica of those files.

This design limits the effect of outages. While the network or the hub is unavailable, the probe continues to write locally. When the connection returns, the hub reports the newest bucket that it holds for each range. The probe then sends the missing buckets from its files, oldest first. The hub needs no database and no merge logic, because each bucket is written to the same offset in the replica as in the original.

## Runtime environment

Both binaries call the Linux kernel directly through `linux.h` and do not use a C library. `start.S` and `boot.c` replace the C runtime startup code. `runtime.h` supplies the few helpers that libc would otherwise provide.

### Reasons for avoiding libc

Without libc, the binaries are fully static. They run on any system with Linux 5.1 or later and do not depend on installed library versions. They are also small: on AMD64, the probe is 58,528 bytes and the hub is 87,072 bytes. To measure the sizes again, run `make && ls -l quectop quectop-hub`.

The disadvantage is that each system call needs a number for each architecture, and the numbers differ between AMD64, ARM64, and ARMv6. `test/abi.py` checks every call on all three architectures. The architecture is selected at compile time, so the 64-bit binaries contain no ARM32 code.

### Memory

The probe determines the device layout once at startup, in `topology.h`, and then allocates one arena of the required size. Normal sampling performs no allocations. Reads from `/proc` and `/sys` share one scratch buffer, and file descriptors stay open between samples.

### Concurrency

Each program runs a single thread with one event loop:

- The probe sleeps until its next sample on an absolute monotonic timer. With `--hub`, it waits in `ppoll` on one nonblocking socket instead.
- The hub waits in one `ppoll` call on all probe and HTTP connections.

## Sampling

At every `--interval-ms`, `main.c` runs these steps:

1. `collect` (`sampling.h`, `proc.h`) reads the counters from `/proc` and `/sys` and converts them to fixed-precision values.
2. `update_tiers` (`history.h`) adds the sample to the open window of each of the five ranges: 1 hour, 1 day, 7 days, 30 days, and 90 days. Each window keeps the mean, the maximum, and the time of the maximum. Memory, swap, temperature, and frequency also keep the minimum. The raw sample is then discarded.
3. A window that closes becomes one fixed-size record. The record is written to the slot that its start time selects, so the file needs no write cursor. The 1h range batches its records into writes of about 1 KiB.
4. At every `--publish-ms`, the probe writes `live.v1.bin` atomically. The snapshot holds the latest sample, the open windows, the 1h records not yet written, and the service figures. In the same step, the probe checks for mount changes (`disks.h`) and queries services (`services.h`).

History files have a fixed size, set when they are created. A 1000-point history for a four-core machine takes about 390 KiB across the five files. The probe never reads history back into memory.

### Record encoding

A record stores each metric at the precision that it needs, such as 10 bits for a percentage in 0.1% steps. Small records reduce writes to flash storage. The [data formats reference](reference/data-formats.md) describes the layout.

### Durability

The probe does not call `fsync`. On power loss, it loses at most the open windows and the most recent writes. Each record has a checksum, so a reader skips a partially written record. Calling `fsync` on every write would increase flash wear for data that the next sample replaces.

## Hub

The hub (`hub/main.c`) runs these modules in one loop:

| Module | Function |
| --- | --- |
| `hub/intake.h` | Performs the handshake for each probe connection and validates every frame before it is written to a replica file. An invalid frame closes the connection. |
| `hub/hosts.h` | Maintains the host table and writes `hosts.txt`, which lists each host and its connection state. |
| `hub/keys.h` | Looks up each device's key in the keys file. The file is read on every new connection, so key changes apply without a restart. |
| `hub/http.h` | Optional static file server for `--http`. |

### Static site

The hub serves only static files from its data directory. Any static web server, such as nginx or Caddy, can therefore serve the same directory and add the authentication and TLS that the built-in server lacks. The built-in server is intended for local use.

### Dashboard assets

At build time, `scripts/assets.py` and `hub/assets.S` embed the bundled dashboard into `quectop-hub`. At startup, the hub writes these files to its data directory when their revision has changed. The installed dashboard therefore always matches the hub version.

## Security

### Authentication and encryption

Every connection uses the [Noise](https://noiseprotocol.org/) handshake `Noise_NNpsk0_25519_ChaChaPoly_SHA512`, with the device's 256-bit key as the pre-shared key. The probe sends no data until the hub has proven that it holds the same key, so an impostor hub learns only the device ID. Each connection uses new X25519 ephemeral keys, so a key that is compromised later cannot decrypt recorded traffic. The [protocol reference](reference/protocol.md) describes the details.

### Cryptographic implementation

`noise.h` contains about 150 lines that combine primitives from [Monocypher](https://monocypher.org/) 4.0.3. Monocypher is included unmodified in `vendor/monocypher/`. `test/crypto.py` checks the implementation against external test suites: Wycheproof, the cacophony Noise vector, and the `noiseprotocol` Python package.

### Hub protections

- The hub accepts only devices listed in the keys file, unless it listens on loopback only.
- Both programs refuse key files that other users can read.
- The hub has fixed limits: 64 probes, 32 HTTP clients, and 10 s to complete a handshake.
- The binaries are static position-independent executables, with address space layout randomization, read-only relocations, and stack protection.

### Known limitations

- The device ID is sent unencrypted, because the hub needs it to select the key.
- The built-in HTTP server has no authentication.
- A client without a key can occupy all 64 probe slots with slow connections.

## Design constraints

| Constraint | Effect |
| --- | --- |
| Fixed layouts | CPU IDs, interfaces, the sample interval, and the point count determine the record size. A change requires a new data directory, so the code never migrates existing records. |
| Versioned file names | Each file name includes its schema version, such as `live.v1.bin`. A format change creates new files and leaves old ones in place. Readers support only the current version. |
| Bounded work | Text lines are limited to 4095 bytes. Service queries share a 100 ms budget for each publication. Network operations never block the sample loop. When the probe reaches a limit, the affected value becomes unavailable, and sampling continues. |
| No runtime cost from refactoring | `AGENTS.md` requires that refactoring adds no CPU work, memory, or I/O. `test/overhead.py` compares the generated assembly. |

## Source files

| File | Content |
| --- | --- |
| `main.c` | Probe options and the sample loop |
| `topology.h` | Device discovery and the startup arena |
| `sampling.h`, `proc.h`, `metrics.h` | Reading and packing measurements |
| `history.h` | Ranges, history files, and the live snapshot |
| `disks.h` | Filesystem discovery and capacity snapshots |
| `services.h`, `service_dbus.h`, `service_memory.h` | Service state and memory |
| `uplink.h`, `resolve.h` | Hub connection and name lookup |
| `protocol.h`, `noise.h`, `auth.h` | Wire format, handshake, and key lines |
| `hub/` | The hub |
| `web/` | The dashboard. `web/codec.js` decodes the data files. |
| `linux.h`, `runtime.h`, `net.h`, `lines.h`, `datadir.h` | System calls and shared helpers |
| `start.S`, `boot.c`, `arm.c` | Process startup and ARMv6 integer helpers |
| `vendor/monocypher/` | Cryptographic library, unmodified |
| `test/` | Tests. See [Run the tests](how-to/run-the-tests.md). |
