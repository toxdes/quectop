# Track service memory

The probe can report the state, uptime, and memory of system services. The dashboard shows them in the **Services** panel.

## Choose the services

Add `--services` with a comma-separated list:

```sh
quectop --services nginx,postgresql,my-app
```

With systemd, `.service` is optional. With OpenRC, use the names in `/etc/init.d`.

To change the list, restart the probe. The history files stay valid.

## Give the probe access

Each memory figure needs different access.

| Figure | Needs |
| --- | --- |
| State, uptime, cgroup memory | Read access to the system bus or the cgroup files. Any user has it by default. |
| RSS, PSS, USS | Read access to `/proc/PID/smaps_rollup` of every process in the service. |

Only the process owner and root can read `smaps_rollup`. Use one of these options:

- Run the probe as root. For example, remove `User=` from the unit in [Run quectop at boot](run-at-boot.md).
- Run the probe as the service's user. Use this option only when that user owns every process in the service.

Without access, RSS, PSS, and USS show `—`. The other figures still appear.

## Interpret the figures

On the dashboard, the **Services** panel shows PSS by default. Use the selector to show **RSS**, **Private**, or **Cgroup**. Expand **Breakdown** to see every figure.

| Figure | Meaning |
| --- | --- |
| **Cgroup** | Memory charged to the service's cgroup, including page cache and kernel memory. |
| **RSS** | Resident memory of all processes in the service. Shared pages count once per process, so the sum can exceed physical RAM. |
| **PSS** | RSS with each shared page split among the processes that map it. |
| **Private (USS)** | Pages that only one process maps. |
| PSS anonymous, file, and shared memory | PSS split by mapping type. File-backed PSS includes shared libraries. |

No figure predicts exactly how much memory you free when you stop a service.

## Performance cost

The probe queries services only when it publishes, every 30 s by default. All queries in one publication share a 100 ms budget. On a busy service, one kernel `smaps_rollup` read can take longer than that.
