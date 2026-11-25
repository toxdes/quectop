# Monitor another machine

This guide connects a device to a hub that runs on another machine. The device runs `quectop`. The hub machine runs `quectop-hub`.

The device must reach the hub's probe port over TCP. The hub does not connect to devices.

## Give the device a key

1. On the hub machine, add a key for the device. Use a short name, such as `web1`:

	```sh
	quectop-hub --add-device web1
	```

	The command adds a line to `~/.config/quectop-hub/keys` and prints it:

	```text
	web1 3f9a...e1
	```

2. On the device, save that line in a file that only your user can read:

	```sh
	mkdir -p ~/.config/quectop
	nano ~/.config/quectop/key
	chmod 600 ~/.config/quectop/key
	```

	Treat the line like a password. Anyone who holds it can stream data as this device.

**Copy over SSH instead.** If you reach the hub over SSH, you can do both steps with one command on the device:

```sh
(umask 077 && ssh you@hub quectop-hub --add-device web1 > ~/.config/quectop/key)
```

## Start the hub

On the hub machine, listen for probes on an address that the device can reach:

```sh
quectop-hub --probes 0.0.0.0:7070
```

The hub does not start with a non-loopback `--probes` address unless the keys file exists. Open TCP port 7070 in the hub's firewall.

## Start the probe

On the device, point the probe at the hub:

```sh
quectop --hub hub.example.com:7070 --key-file ~/.config/quectop/key
```

`--hub` takes an IPv4 address or a host name, without `http://`.

## Check the connection

The device appears in the hub's `hosts.txt` under the key's name. A `1` in the second column means that the device is connected:

```sh
cat ~/.local/state/quectop-hub/hosts.txt
```

If the device does not appear, read the hub's stderr. The hub logs every rejected device.

| Message | Fix |
| --- | --- |
| `rejected device web1: not in the keys file, or wrong key` | Check that the hub's keys file has a `web1` line. If it does, copy that line to the device again. |
| `rejected device` followed by 32 hex digits | That ID is a machine ID, so the probe runs without `--key-file`. Add `--key-file`. |
| `keys file is readable by other users; run chmod 600 on it` | Run `chmod 600` on the keys file. |

## Remove a device

Delete the device's line from the keys file. The next connection from that device fails. You do not need to restart the hub.

## Next steps

- To view the dashboard from a browser, see [Serve the dashboard with nginx](serve-dashboard-with-nginx.md).
- To start both programs at boot, see [Run quectop at boot](run-at-boot.md).
