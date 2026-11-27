# Dashboard reference

The dashboard is `index.html`, `style.css`, and `app.js` in the hub's data directory. It runs in the browser and decodes the probe's files with `web/codec.js`.

## Data loading

The dashboard reads these files, in order:

1. `hosts.txt`, on every poll, to list the hosts and their connection state.
2. `h/ID/live.v1.bin` of each device in the current view, every 15 s while the tab is visible. Polling stops while the tab is hidden.
3. The history file of the selected range, for each device with a chart in the view, when its live snapshot reports a new committed window.
4. `h/ID/disks.v1.bin`, for each device with a Disks tile in the view, when its live snapshot reports a new capacity snapshot.

Every request uses `cache: 'no-cache'` and times out after 10 s. The dashboard decodes only schema v1. Any other version shows an error.

## Views

The view picker in the header lists each device and each saved board.

| View | Shows | URL |
| --- | --- | --- |
| Device | Every tile of one device, in a fixed order. The Services tile appears only when the device tracks services. | `?host=ID` |
| Board | A named list of tiles. Each tile shows one metric of one device. | `?board=NAME&tiles=ID~METRIC,…`, with `~COL.ROW` after a pinned tile |

The URL holds the whole view, so a copied link opens the same view in another browser. The link button next to the title copies the URL.

### Boards

1. Select **New board** in the view picker. The new board is named `board1`, `board2`, and so on, and opens in edit mode.
2. Select a device and a metric in the last tile, then select **Add tile**. **Cancel** closes the form; the last tile then offers **Add tile** to reopen it and **Save and exit**, which saves like the ✓ button.
3. To move a tile, drag its handle (⠿), or focus the handle and press an arrow key. While you drag, a dashed outline marks where the tile will land, and the other tiles slide aside. Tiles fill gaps in list order. On screens wider than 1280 px, dropping a tile on an empty space where it fits pins it there; dropping it on another tile unpins it. Narrower screens ignore pins. To remove a tile, select ×.
4. To rename the board, edit the name field.
5. Select the save button (✓). The board is stored in the browser's `localStorage` and appears in the view picker.

The edit button (pencil) reopens edit mode. The delete button removes a saved board from the browser. Boards live in the current browser. A board opened from a link is unsaved. Saving it creates a separate board and adds a number to its name if the name is already in use.

If a save fails, the editor keeps your draft. Copy the board link to keep the draft or retry the save. If a delete fails, the board stays in the picker.

Metric keys in the URL: `usage`, `time`, `memory`, `swap`, `temp`, `network` (summaries); `cores`, `cpu`, `pressure`, `ram`, `frequency`, `temperature`, `traffic` (charts); `disks`, `services`.

## Status line

In a device view, the status line under the title shows the device uptime and one of these states:

| State | Condition |
| --- | --- |
| `connected` | `hosts.txt` marks the device as connected. |
| `offline` | `hosts.txt` marks the device as not connected. |
| `no new data for` a duration | The device is connected, but the live snapshot has not changed for longer than the greater of 90 s, 3 × the sampling interval, and 3 × the publication interval. |

The duration is measured with the browser clock only, so a wrong device clock does not affect the state.

On a board, the status line counts offline and stale devices and devices that failed to refresh. Tiles label the affected devices. If a refresh fails, the dashboard identifies measurements from the last successful refresh as cached.

## Summary tiles

| Tile | Shows | Click or press Enter |
| --- | --- | --- |
| CPU usage | Overall busy share | |
| CPU time | User or system time, with the other below it | Switch between user and system. |
| Memory | Used or available RAM, in binary units (K = 1024 bytes) | Switch between used and free. |
| Swap | Swap used, or `off` when swap is disabled | |
| Temperature | Latest reading, in °C | |
| Network | Upload and download rates, such as `↑405.0K ↓4.1M`, in decimal units (K = 1000 bytes) | Show the next interface in every tile of that device. |

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

### Ranges

The range picker selects 1h, 1d, 7d, 30d, or 90d. The default is 1h.

### Theme and font

The theme button in the header switches between the light and dark themes. The font picker switches between the system monospace font (**Mono**, the default) and the system sans-serif font (**Sans**). The browser keeps both choices.

### Lines and bands

Each line is the window mean. The band behind a line spans minimum to maximum where the minimum is stored, and mean to maximum elsewhere.

### Axes

Each vertical axis fits the extremes of the selected range, with padding. Percentage axes stay within 0–100%. Charts use different scales, so read the axes when you compare charts.

The time axis ends at the latest sample. While the stored history is shorter than the selected range, the axis starts at the oldest stored window instead, and covers at least 12 windows.

### Gaps

A missing window is a gap, not zero.

### Frequency source

A frequency series from `scaling_cur_freq` is labeled as driver-reported. Neither frequency nor temperature alone proves throttling.

### Bars mode

Bars use two columns, such as eight rows for 16 cores, and one column on narrow screens. The panel scrolls when there are many CPUs. Bars reuse the latest sample and fetch nothing extra.

### Inspect a chart

Hover over a chart or focus it. Every visible chart, including charts of other devices on a board, then shows a dotted cursor at the same time, with dots on each series. A tooltip shows the window's mean values.

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

In a device view, the **Services** panel appears only when the probe runs with `--services`. It shows PSS by default. The selector switches to **RSS**, **Private**, or **Cgroup**. **Breakdown** under each name shows every figure. A missing figure shows `—`. See [Track service memory](../how-to/track-service-memory.md).
