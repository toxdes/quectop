# quectop

quectop shows how your Linux machines are doing: CPU, memory, temperature, network, disks, and services, with history for up to 90 days. It is small enough to run on single-board computers and other low-memory machines without slowing them down.

- **Small.** Each program is one static file under 100 KB, with no libraries and no database.
- **Your data stays on the device first.** Each device keeps its own history. A hub collects copies for the dashboard.
- **Encrypted.** Devices and the hub prove to each other that they share a key before any data moves.
- **Runs on** Linux 5.1 or later, on 64-bit x86, 64-bit ARM, and 32-bit ARM (ARMv6 and later).

## How it works

quectop has two programs:

- **`quectop`**, the probe, runs on each machine that you want to watch. It samples the machine every 5 seconds and keeps the history on that machine.
- **`quectop-hub`** runs on one machine and collects data from every probe. It also holds the dashboard that you open in a browser.

```text
quectop on machine A ─┐
quectop on machine B ─┼──▶ quectop-hub ──▶ dashboard in your browser
quectop on machine C ─┘
```

## Try it on one machine

1. Build both programs. You need a C compiler, `make`, Python 3, and [Bun](https://bun.sh):

	```sh
	make
	```

2. Start the hub with its dashboard on port 7080:

	```sh
	./quectop-hub --http 127.0.0.1:7080
	```

3. In a second terminal, start the probe and point it at the hub:

	```sh
	./quectop --hub 127.0.0.1:7070
	```

4. Open <http://127.0.0.1:7080/>. The dashboard shows your machine. The charts fill in as the probe collects samples.

Stop either program with Ctrl+C. Each program keeps its data in `~/.local/state`, and both continue where they stopped when you start them again.

## Next steps

| To do this | Read |
| --- | --- |
| Watch one machine from another machine | [Monitor another machine](docs/how-to/monitor-another-machine.md) |
| Open the dashboard safely over the internet | [Serve the dashboard with nginx](docs/how-to/serve-dashboard-with-nginx.md) |
| Start quectop when the machine boots | [Run quectop at boot](docs/how-to/run-at-boot.md) |
| Build for ARM or another machine | [Build from source](docs/how-to/build-from-source.md) |
| Save wear on flash storage | [Reduce storage writes](docs/how-to/reduce-storage-writes.md) |
| See how much memory nginx or your app uses | [Track service memory](docs/how-to/track-service-memory.md) |
| Install a new version | [Upgrade quectop](docs/how-to/upgrade.md) |
| Check a build | [Run the tests](docs/how-to/run-the-tests.md) |

## Documentation

- **[How-to guides](docs/how-to/README.md)** give the steps for one task.
- **[Reference](docs/reference/README.md)** lists every option, file, and protocol message.
- **[Architecture](docs/architecture.md)** explains how quectop is built and why.

## Good to know

- The built-in dashboard server (`--http`) has no password. Keep it on `127.0.0.1`, or put it behind nginx with a password and HTTPS.
- quectop does not call `fsync`, to save wear on flash storage. If the power fails, the device loses the history that it has not yet written.
- quectop has no configuration file. All settings are command-line options. Run `quectop --help` or `quectop-hub --help` for the list.
- quectop does not support IPv6 yet.
