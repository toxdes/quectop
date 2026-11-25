# quectop

quectop is a resource monitor for Linux. It records CPU, memory, temperature, network, disk, and service usage, and keeps up to 90 days of history. It is designed for machines with limited memory and storage, such as single-board computers.

## Features

- Each program is a single static binary of less than 100 KB. No libraries or database are required.
- Each device stores its own history. A hub collects copies of the history and serves the dashboard.
- Connections between devices and the hub are encrypted and authenticated with a per-device key.
- Supported systems: Linux 5.1 or later on x86-64, 64-bit ARM, and 32-bit ARM (ARMv6 or later).

## Overview

quectop consists of two programs:

| Program | Role |
| --- | --- |
| `quectop` (the probe) | Runs on each monitored machine. It samples the machine every 5 seconds and stores the history on that machine. |
| `quectop-hub` (the hub) | Runs on one machine. It receives data from all probes and provides the dashboard for a web browser. |

```text
quectop on machine A ─┐
quectop on machine B ─┼──▶ quectop-hub ──▶ dashboard in a web browser
quectop on machine C ─┘
```

## Quick start

These steps run the probe and the hub on the same machine.

1. Build both programs. The build requires a C compiler, `make`, Python 3, and [Bun](https://bun.sh):

	```sh
	make
	```

2. Start the hub, with the dashboard on port 7080:

	```sh
	./quectop-hub --http 127.0.0.1:7080
	```

3. In a second terminal, start the probe and connect it to the hub:

	```sh
	./quectop --hub 127.0.0.1:7070
	```

4. Open <http://127.0.0.1:7080/>. The dashboard shows the machine, and the charts fill in as the probe collects samples.

To stop either program, press Ctrl+C. Both programs keep their data in `~/.local/state` and resume from it when they start again.

## Guides

| Task | Guide |
| --- | --- |
| Monitor a machine from another machine | [Monitor another machine](docs/how-to/monitor-another-machine.md) |
| Serve the dashboard over HTTPS with a password | [Serve the dashboard with nginx](docs/how-to/serve-dashboard-with-nginx.md) |
| Start quectop at boot | [Run quectop at boot](docs/how-to/run-at-boot.md) |
| Build for ARM | [Build from source](docs/how-to/build-from-source.md) |
| Reduce writes to flash storage | [Reduce storage writes](docs/how-to/reduce-storage-writes.md) |
| Report the memory use of system services | [Track service memory](docs/how-to/track-service-memory.md) |
| Install a new version | [Upgrade quectop](docs/how-to/upgrade.md) |
| Test a build | [Run the tests](docs/how-to/run-the-tests.md) |

## Documentation

| Document | Content |
| --- | --- |
| [How-to guides](docs/how-to/README.md) | Procedures for specific tasks |
| [Reference](docs/reference/README.md) | Options, files, and protocol messages |
| [Architecture](docs/architecture.md) | Design of quectop and the reasons for it |

## Limitations

- The built-in dashboard server (`--http`) has no authentication. Bind it to `127.0.0.1`, or serve the dashboard through a web server with authentication and HTTPS.
- quectop does not call `fsync`, to reduce flash wear. On power loss, history that has not yet been written is lost.
- quectop has no configuration file. All settings are command-line options. For the list, run `quectop --help` or `quectop-hub --help`.
- IPv6 is not supported.
