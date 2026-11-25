# Upgrade quectop

The probe and the hub must use the same protocol version. Upgrade the hub and all probes at the same time.

## Replace the binaries

1. Build or download the new release. See [Build from source](build-from-source.md).
2. On each machine, replace the binary:

	```sh
	sudo install -m 755 quectop /usr/local/bin/quectop
	```

	`install` removes the old file before it writes the new one, so the running process keeps its copy. Do not use `cp` here. `cp` fails with `Text file busy` while the binary runs.

3. Restart the services:

	```sh
	sudo systemctl restart quectop-hub   # hub machine
	sudo systemctl restart quectop       # each device
	```

4. Reload any open dashboard.

When the hub starts, it installs the new dashboard files into its data directory.

## When data formats change

New data formats use new file names, such as `live.v2.bin`. The probe starts new files in the same data directory and leaves the old files in place. To free the space, delete the old files after the upgrade.

## Change options that the history depends on

`--interval-ms`, `--points`, and the set of tracked CPUs and interfaces are fixed for a set of history files. To change them, start the probe with a new `--data-dir`. The probe refuses to start on a directory whose history does not match.
