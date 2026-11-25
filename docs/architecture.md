# Architecture

quectop is built for a Raspberry Pi Zero 2 W, which has 512 MiB of RAM and an SD card. A monitor on such a machine must not become the load that it reports. Most design choices below follow from that goal.

## Two programs, one direction

```text
 device                          hub machine                     browser
┌──────────────────┐   TCP     ┌──────────────────┐   HTTP     ┌───────────┐
│ quectop (agent)  │ ────────▶ │ quectop-hub      │ ─────────▶ │ dashboard │
│ samples, keeps   │ encrypted │ keeps a replica  │  static    │ decodes   │
│ its own history  │           │ as a static site │  files     │ the files │
└──────────────────┘           └──────────────────┘            └───────────┘
```

- **`quectop`**, the agent, runs on each device. It samples the device and writes its own history files. It does not listen on any port.
- **`quectop-hub`** accepts connections from agents. It keeps an exact copy of each device's files in a directory that is also a website.
- **The dashboard** is HTML, CSS, and JavaScript. The browser downloads the binary files and decodes them itself.

Data moves in one direction. The agent connects out to the hub, and the hub never connects to a device. So a device needs no open port, and a device behind NAT works without extra setup.

## The agent is the source of truth

The agent writes its history to its own disk first, with or without a hub. The hub holds a replica.

This choice makes outages cheap. When the network or the hub goes down, the agent keeps writing locally. When the connection returns, the hub reports the newest bucket that it holds for each range. The agent then sends the missing buckets, oldest first, from its files. The hub needs no database and no merge logic. The same bytes go to the same offsets.

## No libc, no threads, no allocator

Both binaries call the Linux kernel directly through `linux.h`, with no C library. `start.S` and `boot.c` replace the C runtime's startup code. `runtime.h` supplies the small set of helpers that libc would otherwise provide.

**Why no libc.** Without libc, the binaries are fully static. They run on any Linux 5.1 or later system without matching library versions. The binaries are small: the AMD64 agent is 58,528 bytes, and the hub is 87,072 bytes. Startup does almost nothing. To measure the sizes again, run `make && ls -l quectop quectop-hub`.

**The cost.** Each system call needs a number for each architecture, and AMD64, ARM64, and ARMv6 all differ. `test/abi.py` checks every call on all three. The architecture is chosen at compile time, so the 64-bit binaries carry no ARM32 code.

**Memory.** The agent works out the device layout once at startup, in `topology.h`. It then allocates one arena of the right size. Normal sampling allocates nothing. Proc and sysfs reads share one scratch buffer, and file descriptors stay open between samples.

**One thread.** Each program runs one loop:

- The agent sleeps until its next sample on an absolute monotonic timer. With `--hub`, it waits in `ppoll` on one nonblocking socket instead.
- The hub waits in one `ppoll` call on every agent and HTTP connection.

## The agent's sample loop

Every `--interval-ms`, `main.c` runs these steps:

1. **`collect`** (`sampling.h`, `proc.h`) reads the counters from `/proc` and `/sys`. It turns them into values with fixed precision.
2. **`update_tiers`** (`history.h`) adds the sample to the open window of all five ranges: 1 hour, 1 day, 7 days, 30 days, and 90 days. A range keeps the mean, the maximum, and the time of the maximum. Memory, swap, temperature, and frequency also keep the minimum. The raw sample is then discarded.
3. **Write closed windows.** A window that closes becomes one fixed-size record. The record is written at the slot that its start time selects, so the file needs no write cursor. The 1h range batches its records into about 1 KiB writes.
4. **Publish the live snapshot.** Every `--publish-ms`, the agent writes `live.v1.bin` atomically. The snapshot holds the latest sample, the open windows, the 1h records not yet written, and the service figures. In the same step, the agent checks for mount changes (`disks.h`) and queries services (`services.h`).

History files have a fixed size, set when they are created. A 1000-point history for a four-core Pi takes about 390 KiB across the five files. The agent never reads history back into memory.

**Bit-packed records.** A record packs each metric at the precision that it needs, such as 10 bits for a percentage at 0.1% steps. Records are small, so the agent writes less to the SD card. The [data formats reference](reference/data-formats.md) gives the layout.

**No `fsync`.** On power loss, the agent loses at most the open windows and recent writes. Each record has a checksum, so a reader skips a torn record. An `fsync` on every write would wear the SD card for data that the next sample replaces.

## The hub's replica

The hub (`hub/main.c`) runs these parts in one loop:

- **`hub/intake.h`** takes each agent connection through the handshake. It then checks every frame before the frame touches a replica file. Any invalid frame closes the connection.
- **`hub/hosts.h`** keeps the host table and writes `hosts.txt`, which lists each host and whether it is connected.
- **`hub/keys.h`** looks up each device's key in the keys file. It reads the file on every new connection, so key changes apply without a restart.
- **`hub/http.h`** is the optional static file server for `--http`.

**The data directory is the website.** The hub serves nothing that a static web server could not serve. So nginx, Caddy, or any other server can serve the same directory, with the login and TLS that the hub does not have. The built-in server is a convenience for local use.

**The dashboard ships inside the hub.** At build time, `scripts/assets.py` and `hub/assets.S` embed the bundled dashboard into `quectop-hub`. At startup, the hub writes the files into its data directory when their revision changes. The hub binary therefore always matches its dashboard.

## Security

**The agent starts every connection, and the hub must prove who it is.** Every connection uses the [Noise](https://noiseprotocol.org/) handshake `Noise_NNpsk0_25519_ChaChaPoly_SHA512`. The device's 256-bit key is the pre-shared key. The agent sends no data until the hub proves that it holds the same key. A fake hub learns only the device ID. A new X25519 key for each connection means that a key stolen later cannot decrypt recorded traffic. The [protocol reference](reference/protocol.md) gives the details.

**The cryptography is borrowed.** `noise.h` is about 150 lines that combine primitives from [Monocypher](https://monocypher.org/) 4.0.3. Monocypher is copied unmodified into `vendor/monocypher/`. `test/crypto.py` checks the result against suites that this project did not write: Wycheproof, the cacophony Noise vector, and the `noiseprotocol` Python package.

**Defenses on the hub side.**

- The hub accepts only devices in the keys file, unless it listens on loopback only.
- Both programs refuse key files that other users can read.
- The hub has fixed limits: 64 agents, 32 HTTP clients, and 10 s to complete a handshake.
- The binaries are static PIE, with address space layout randomization, read-only relocations, and stack protection.

**What stays open.**

- The device ID travels unencrypted, because the hub needs it to pick the key.
- The built-in HTTP server has no login.
- A client without a key can still hold all 64 agent slots with slow connections.

## Constraints that shape the code

- **Fixed layouts.** CPU IDs, interfaces, the sample interval, and the point count fix the record size. A change needs a new data directory, so the code never migrates old records.
- **Versioned file names.** Each file name carries its schema version, such as `live.v1.bin`. A format change writes new files and leaves old ones in place. Readers support only the current version.
- **Bounded work.** Text lines are at most 4095 bytes. Service queries share a 100 ms budget for each publication. Agent sockets never block the sample loop. A limit that the agent reaches makes a value unavailable. It never stops sampling.
- **No new runtime cost for cleaner code.** `AGENTS.md` requires that refactors add no CPU work, memory, or I/O. `test/overhead.py` compares the generated assembly.

## Source map

| File | Part |
| --- | --- |
| `main.c` | Agent options and the sample loop |
| `topology.h` | Device discovery and the startup arena |
| `sampling.h`, `proc.h`, `metrics.h` | Reading and packing measurements |
| `history.h` | Ranges, history files, and the live snapshot |
| `disks.h` | Filesystem discovery and capacity snapshots |
| `services.h`, `service_dbus.h`, `service_memory.h` | Service state and memory |
| `uplink.h`, `resolve.h` | Hub connection and name lookup |
| `protocol.h`, `noise.h`, `auth.h` | Shared wire format, handshake, and key lines |
| `hub/` | The hub |
| `web/` | The dashboard. `web/codec.js` decodes the files. |
| `linux.h`, `runtime.h`, `net.h`, `lines.h`, `datadir.h` | System calls and shared helpers |
| `start.S`, `boot.c`, `arm.c` | Process start and ARMv6 integer helpers |
| `vendor/monocypher/` | Cryptography, unmodified |
| `test/` | Tests. See [Run the tests](how-to/run-the-tests.md). |
