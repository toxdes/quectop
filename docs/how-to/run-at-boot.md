# Run quectop at boot

quectop has no service files of its own. This guide adds systemd units for the probe and the hub. Both run as a system user named `quectop`.

## Install the binaries

On each machine, copy the binaries to `/usr/local/bin`:

```sh
sudo install -m 755 quectop /usr/local/bin/
sudo install -m 755 quectop-hub /usr/local/bin/   # hub machine only
```

## Create the user

```sh
sudo useradd --system --no-create-home --shell /usr/sbin/nologin quectop
```

## Run the hub

1. Create the keys file and add a key for each device:

	```sh
	sudo install -d -o quectop -m 700 /etc/quectop-hub
	sudo -u quectop quectop-hub --keys /etc/quectop-hub/keys --add-device web1
	```

2. Create `/etc/systemd/system/quectop-hub.service`:

	```ini
	[Unit]
	Description=quectop hub

	[Service]
	User=quectop
	ExecStart=/usr/local/bin/quectop-hub --data-dir /var/lib/quectop-hub/site --keys /etc/quectop-hub/keys --probes 0.0.0.0:7070
	StateDirectory=quectop-hub
	Restart=always
	RestartSec=5

	[Install]
	WantedBy=multi-user.target
	```

	`StateDirectory` makes systemd create `/var/lib/quectop-hub` for the `quectop` user. The keys file stays outside the data directory, so a web server never serves it.

3. Start the hub:

	```sh
	sudo systemctl daemon-reload
	sudo systemctl enable --now quectop-hub
	```

## Run the probe

1. Save the device's key line, so that only the `quectop` user can read it:

	```sh
	sudo install -d -o quectop -m 700 /etc/quectop
	sudo install -o quectop -m 600 /dev/null /etc/quectop/key
	sudo nano /etc/quectop/key
	```

2. Create `/etc/systemd/system/quectop.service`:

	```ini
	[Unit]
	Description=quectop probe

	[Service]
	User=quectop
	ExecStart=/usr/local/bin/quectop --data-dir /var/lib/quectop --hub hub.example.com:7070 --key-file /etc/quectop/key
	StateDirectory=quectop
	Restart=always
	RestartSec=5

	[Install]
	WantedBy=multi-user.target
	```

	The probe retries the hub on its own, so the unit does not wait for the network.

3. Start the probe:

	```sh
	sudo systemctl daemon-reload
	sudo systemctl enable --now quectop
	```

## Check the services

```sh
systemctl status quectop quectop-hub
journalctl -u quectop-hub -f
```

The hub logs rejected devices to the journal.

## Related guides

- To track service memory, the probe may need more access. See [Track service memory](track-service-memory.md).
- To change options later, edit `ExecStart`, then run `sudo systemctl daemon-reload` and `sudo systemctl restart quectop`.
