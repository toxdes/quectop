# Dashboard reference

The dashboard is `index.html`, `style.css`, and `app.js` in the hub's data directory. It runs in the browser and decodes the agent's files with `web/codec.js`.

## Data loading

The dashboard reads these files, in order:

1. `hosts.txt`, to list the hosts. A host picker appears when the file lists more than one host. The URL hash holds the selected host ID.
2. `h/ID/live.v1.bin`, every 15 s while the tab is visible. Polling stops while the tab is hidden.
3. The history file of the selected range, when the live snapshot reports a new committed window.
4. `h/ID/disks.v1.bin`, when the live snapshot reports a new capacity snapshot.

Every request uses `cache: 'no-cache'` and times out after 10 s. The dashboard decodes only schema v1. Any other version shows an error.

## Summary tiles

| Tile | Shows | Click or press Enter |
| --- | --- | --- |
| CPU | Overall busy, with user or system time | Switch between user and system. |
| Memory | Used or available RAM, in binary units (K = 1024 bytes) | Switch between used and free. |
| Swap | Swap used, or `off` when swap is disabled | |
| Temperature | Latest reading, in °C | |
| Network | Upload and download rates, such as `U405.0K\|D4.1M`, in decimal units (K = 1000 bytes) | Show the next interface in the tile and in its chart. |

"Free" memory is `MemAvailable`, which includes cache that the kernel can reclaim.

## Charts

| Chart | Series |
| --- | --- |
| Per-core CPU | Every tracked logical CPU. The **Graph** and **Bars** picker switches this panel between history and the latest per-core values. |
| CPU usage | User, system, and iowait |
| Pressure | CPU, memory, and I/O stall share (PSI `some`) |
| Memory | RAM or swap used. The panel's button switches between them. |
| Frequency | Each CPU frequency policy, in GHz |
| Temperature | °C |
| Network | Receive and transmit rate of the selected interface |

**Ranges.** The range picker selects 1h, 1d, 7d, 30d, or 90d. The default is 1h.

**Theme.** The theme button in the header switches between the light and dark themes.

**Lines and bands.** Each line is the window mean. The band behind a line spans minimum to maximum where the minimum is stored, and mean to maximum elsewhere.

**Axes.** Each vertical axis fits the extremes of the selected range, with padding. Percentage axes stay within 0–100%. Charts use different scales, so read the axes when you compare charts.

**Gaps.** A missing window is a gap, not zero.

**Frequency source.** A frequency series from `scaling_cur_freq` is labeled as driver-reported. Neither frequency nor temperature alone proves throttling.

**Bars mode.** Bars use two columns, such as eight rows for 16 cores, and one column on narrow screens. The panel scrolls when there are many CPUs. Bars reuse the latest sample and fetch nothing extra.

### Inspect a chart

Hover over a chart or focus it. Every visible chart then shows a dotted cursor at the same time, with dots on each series. A tooltip shows the window's mean values.

| Key | Action |
| --- | --- |
| Left, Right | Move one window. |
| Home, End | Go to the oldest or the latest window. |
| Escape | Clear the cursor. |
| Page Up, Page Down | Scroll a long tooltip. |

Screen readers get the window range, the extremes, and the peak times.

## Disks panel

The **Disks** panel lists each filesystem from `disks.v1.bin`:

- The name sits above a usage meter.
- Used and total space sit below the meter.
- Available space sits on the right.

Available space can be smaller than total minus used, because filesystems reserve space for root. A disk that is not mounted has no row.

## Services panel

The **Services** panel appears only when the agent runs with `--services`. It shows PSS by default. The selector switches to **RSS**, **Private**, or **Cgroup**. **Breakdown** under each name shows every figure. A missing figure shows `—`. See [Track service memory](../how-to/track-service-memory.md).
