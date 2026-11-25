# Reduce SD card writes

The agent replaces `live.v1.bin` every 30 s by default. On a Raspberry Pi, put that file in RAM to save SD card wear. History files stay on the card.

## Use a RAM directory

1. Find a RAM-backed directory. On most systems, `/run` and `/dev/shm` use `tmpfs`:

	```sh
	findmnt -no FSTYPE /run
	```

	The output is `tmpfs`.

2. Create a directory there for the agent. Files in `/run` disappear at reboot, so create the directory at every boot. With systemd, add this line to the agent's `[Service]` section:

	```ini
	RuntimeDirectory=quectop
	```

	systemd then creates `/run/quectop` for the service user.

3. Add `--live-dir` to the agent:

	```sh
	quectop --data-dir /var/lib/quectop --live-dir /run/quectop
	```

The agent writes `live.v1.bin` and `disks.v1.bin` to `/run/quectop`. `--live-dir` must exist when the agent starts.

## Write less often

These options also cut writes, at the cost of detail:

| Option | Effect |
| --- | --- |
| `--publish-ms 60000` | Replace the live snapshot every 60 s instead of 30 s. |
| `--interval-ms 10000` | Sample every 10 s. Changing it needs a new data directory. |
| `--points 500` | Keep half as many windows in each range. Changing it needs a new data directory. |

The 1h history batches its writes in about 1 KiB blocks, at most once a minute. The longer histories write once for each window. A small logical write can still cause a larger physical write on the card.
