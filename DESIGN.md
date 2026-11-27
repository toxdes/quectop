---
name: quectop
description: Compact resource measurements and history for small Linux machines.
colors:
  bg: "#edf1f0"
  panel: "#f8faf9"
  ink: "#172c31"
  muted: "#586b6e"
  grid: "#e3e9e7"
  teal: "#05829c"
  violet: "#7253c8"
  third: "#657078"
  fourth: "#a56516"
  core5: "#b1425a"
  core6: "#316ca9"
  core7: "#6b7817"
  core8: "#95508b"
  good: "#128260"
  dark-bg: "#151e21"
  dark-panel: "#1c292d"
  dark-ink: "#e1edeb"
  dark-muted: "#a1b6b8"
  dark-grid: "#2b3b40"
  dark-teal: "#51c2d6"
  dark-violet: "#b39bf4"
  dark-third: "#acb6bf"
  dark-fourth: "#e4b66b"
  dark-core5: "#e88a9b"
  dark-core6: "#8bb7ef"
  dark-core7: "#c1d36a"
  dark-core8: "#d49ccf"
  dark-good: "#66c7a3"
typography:
  title:
    fontFamily: 'ui-monospace, monospace'
    fontSize: "18px"
    fontWeight: 650
    lineHeight: 1.3
  body:
    fontFamily: 'ui-monospace, monospace'
    fontSize: "14px"
  label:
    fontFamily: 'ui-monospace, monospace'
    fontSize: "14px"
    fontWeight: 500
    lineHeight: 1.5
    letterSpacing: ".07em"
  measurement:
    fontFamily: 'ui-monospace, monospace'
    fontSize: "clamp(23px, 2.2vw, 32px)"
    fontWeight: 600
    lineHeight: 1.3
rounded:
  panel: "6px"
  control: "6px"
  small: "3px"
spacing:
  compact: "10px"
  panel: "12px"
  gutter: "14px"
  header: "16px"
components:
  summary:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.ink}"
    rounded: "{rounded.panel}"
    padding: "12px"
  chart:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.ink}"
    rounded: "{rounded.panel}"
    padding: "12px"
  theme-button:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.ink}"
    rounded: "{rounded.control}"
    height: "32px"
    width: "34px"
  range-select:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.ink}"
    rounded: "{rounded.control}"
    height: "32px"
    padding: "0 10px"
  summary-toggle:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.ink}"
    rounded: "{rounded.panel}"
    padding: "12px"
    width: "100%"
    height: "100%"
  chart-control:
    backgroundColor: "{colors.bg}"
    textColor: "{colors.ink}"
    rounded: "{rounded.small}"
    height: "24px"
    padding: "0 8px"
---

# Design System: quectop

## Overview

**Creative North Star: "Technical dashboard"**

The design is a compact measurement surface: a pale background, near-white panels, monospaced values and restrained chart colors. Six compact summaries sit above a grid of six history charts and one chart-sized Disks tile. Density serves comparison and reading measurements.

The source also provides a dark theme with the same hierarchy and geometry. System fonts, native controls and canvas plots keep the interface independent of external fonts, images and libraries. There are no decorative animations.

**Key Characteristics:**

- Compact monospaced measurements with tabular numerals.
- Flat panels with quiet tonal separation.
- Teal and violet series, with additional colors for all detected CPU cores.
- Responsive grids that preserve measurement order.

This document records source in `web/index.html`, `web/style.css` and `web/app.js`. Browser rendering and screenshot fidelity have not been verified in this environment.

## Colors

### Primary

Teal identifies the first plot series, summary qualifiers, selection and keyboard focus. Green marks only the healthy sampling dot; it is distinct from the series palette.

### Secondary

Violet identifies the second plot series. Gray and ochre identify the third and fourth CPU lines; rose, blue, olive and mauve identify the fifth through eighth. Further cores receive deterministic HSL colors: hue `(k * 137.508) % 360`, rounded to one decimal place, saturation (55%), and lightness (38%) in light theme or (68%) in dark theme, where `k` is the zero-based series index. Legend labels retain actual detected CPU IDs; all cores appear together in the first chart.

### Neutral

Background, panel, ink, muted and grid tokens provide the page, surfaces, primary text, secondary text and plot scaffolding. Dark tokens replace their corresponding light roles when the theme changes.

**The Series Identity Rule.** Keep the series color order consistent between legends, lines, bands and peak marks.

## Typography

Use the system monospace font (`ui-monospace`, then the browser's `monospace`) for page and canvas text alike; the font picker can switch both to the system sans-serif font (`system-ui`); values use tabular numerals. All UI text, including canvas axes and tooltips, is at least 14px as requested by the user. The project title uses the title role. Uppercase section labels use the label role; qualifiers restore normal casing and letter spacing. Status, controls, legends, axes and footer use a minimum text size of 14px.

Measurements use the measurement role in ink, left-aligned in summaries. At the middle breakpoint they use (28px), at the narrow breakpoint (24px), and up to 400px (20px). Network sizes its combined rates to its tile: `clamp(14px, 7.8cqi, 32px)`, allowing wrapping on narrow tiles. Units inside chart titles keep their case, so `GHz` and `K/s` are not uppercased. Capacity values carry explicit binary units; compact network rates use decimal units with their meaning exposed in the control title.

## Layout

The page has a minimum width (280px), outer padding (14px) and panel gutters (14px). Every view is one grid of six columns and short rows with dense placement. On desktop a summary spans one column and one row, and a chart, Disks or Services spans two columns and three rows; at widths up to (1280px), two and three columns; at widths up to (680px), one column, with outer padding and gaps (10px). A later tile that fits an earlier gap fills it, so summaries stack beside a chart and a new tile takes the first free space. The add-tile box is as wide as its contents. On boards wider than 1280px, a tile dropped on free cells that fit it is pinned to that column and row; pinned tiles are placed first and the rest pack around them. A device view lists its tiles in a fixed order, so its six summaries fill the first row. A summary keeps its own height when it shares a row with a chart. The header is one row: title, icon buttons and status on the left, controls on the right; on narrow screens the controls wrap below. Preserve source order across widths.

Charts have panel padding (12px), with horizontal padding reduced to (10px) on narrow screens. Plots have a minimum height of `clamp(180px, 28vh, 290px)` on wider screens and (210px) at the narrow breakpoint, and stretch so that charts in one row end at the same line. Above (1900px), the page centers within a maximum width (2200px). The footer stacks on narrow screens. Canvas plots reserve 60px at the left and 32px at the bottom for 14px axis labels; time labels are measured to keep them inside the plot.

## Elevation & Depth

Panels use tonal separation without shadows or default borders. Floating chart tooltips use the panel fill, a thin grid-colored border and a subtle shadow (`0 3px 12px #00000016`). Hover outlines and keyboard focus supply state feedback without movement or transitions.

## Shapes

Panels have gently rounded corners using the panel radius; native controls use the control radius. Legend markers and the healthy status dot are circles (7px); inspection markers are circles (6px). Controls are compact, borderless rectangles. Chart controls, meters and floating chart tooltips use the small radius (3px).

## Components

### Dashboard controls

The first header row holds the title, then icon buttons: edit (pencil, a check while editing) and delete on boards, and a link button that copies the current URL in every view. The link button turns green on success. The view picker lists devices and boards in two groups, plus New board, beside the range picker and the theme button. In edit mode, the title becomes a name field, each tile gains a drag handle and a remove button in a 24px row, and a dashed tile at the end adds a device and metric pair. Dragging lifts a tilted, shadowed copy of the tile under the pointer and leaves a dashed teal placeholder in its slot; the other tiles slide to their new places in 180 ms, and the tile glides into its slot on release. No movement animates when the system requests reduced motion. The add tile comes with Add tile and Cancel; once cancelled, it offers Add tile and Save and exit instead. Board tiles name their device above the heading in muted text, and in violet with `· offline` when the device is offline.

The native range selector displays 1h, 1d, 7d, 30d and 90d, with 1h initially selected. A Graph/Bars selector sits at the top-right of the per-core tile, beside its heading, with no visible label. Its accessible label is Per-core CPU view; the graph legend sits below the plot. The remaining charts retain the selected history range. An icon button switches themes; its accessible label describes the next theme. Controls use compact monospace text and panel fill. Hover adds a muted outline (1px); keyboard focus adds a teal outline (2px) offset by (3px). Theme choice is retained locally when storage is available and otherwise starts from the system preference.

Interactive summaries are full-panel buttons with the same panel geometry and internal spacing. Their focus outline has no offset. Chart controls (Graph/Bars, RAM/swap, service measurement) are 24px high with background fill so that they read as controls on the panel. The RAM/swap control's pressed state and accessible label identify the displayed history. Network interface cycling is disabled when fewer than two interfaces are available, without a hover outline.

### Summaries

Each summary places an uppercase label and teal qualifier on one row, then the current value and a muted secondary value, all left-aligned. Headings do not wrap; long qualifiers such as interface names truncate with an ellipsis. Missing values appear as an em dash. Labels retain units or measurement context. Visible click-to-toggle/cycle hints are omitted; buttons retain their accessible next-action labels.

The six compact summaries are whole-machine CPU usage, CPU time, memory, swap, temperature and network. Whole-machine CPU usage carries the qualifier total, comes from aggregate counters and names the logical CPU count below it. CPU time toggles user/system and shows the other value below it; memory toggles estimated used/free capacity with percentage and total beneath it. Memory free means available memory including reclaimable cache, and capacities are estimated from stored percentage readings. Swap shows used capacity, percentage and total, or `off` with explanatory text.

The network summary cycles interfaces and displays compact upload/download rates as `↑4.2K ↓2.5M`, with decimal B/K/M/G units and a per-second caption. The selected interface name truncates with an ellipsis if needed and also identifies the network history.

### Disks

The Disks panel occupies one regular chart tile, with a body matching canvas height. Its focusable scrolling list uses one entry per mounted filesystem. A source/mount label sits above the usage meter. Compact used/total text sits directly below the meter; matching units are written once, for example 280/362 GiB. Available capacity sits to the right of the meter with an avail caption beneath. Explicit B/KiB/MiB/GiB/TiB units remain visible. The full source and mount are retained in the label's title. Labels truncate with an ellipsis. Entries use 12px column and entry gaps, with 4px gaps inside each entry.

Disk and CPU meters are 8px high, with a grid-colored track, colored fill and the small radius. Its value is used/total; an accessible label and percentage text describe it. Unavailable capacities use an empty decorative track and explicit unavailable text. The panel reports mounted count, partial or unavailable data, update age and refresh failures in text.

### Services

When services are selected, a Services panel follows Disks in the same responsive chart grid.
It matches the chart body height and contains a plain semantic table with Name, Mem and Uptime
columns at 14px. Service names wrap rather than disappear; memory and uptime align right with
tabular numerals. Muted column headers remain visible while the body scrolls, with quiet grid
rules separating rows and teal memory values. Capacities use explicit binary units. Inactive
or failed states occupy the uptime cell, and unavailable memory is an em dash. The tile is
hidden without configured services; there are no service charts, toggles or extra controls.

### History charts

The first plot shows all detected per-core CPU percentages together. Its legend sits below the plot and wraps; each CPU ID stays attached to its color marker. In Bars view, the legend and canvas hide and a list of current readings appears instead: actual CPU ID, native usage meter and 0.1% value per row. Colors match the plot series. Two columns fill downward (16 cores yield eight rows); up to 420px viewport width, rows use one column. Large lists scroll within the same plot height. Missing readings show an em dash and a muted empty track. Nodes are created only when Bars is first shown or the CPU topology changes; hidden bars are not updated and the hidden CPU graph is not drawn.

The second plot shows aggregate CPU user and system percentages. The third shows a single RAM-used line, switchable to swap-used history. The fourth shows CPU frequency in GHz, with one series per shared-clock policy, labeled by its leading CPU and identified as hardware- or driver-reported. Temperature and selected-interface upload/download history complete the graph set; the network legend names the interface. Chart headers wrap and legends repeat the corresponding series colors.

Average lines use stroke width (1.2px), minimum/maximum bands use opacity (0.12), and peak marks use opacity (0.5) with stroke width (0.8px). Grid strokes use (0.6px). Missing samples and long gaps break the lines. Absent sensors and empty histories show text within the plot.

The time axis ends at the latest sample and starts at the selected range, or at the oldest visible window when the history is shorter, spanning at least 12 windows. Time labels show seconds up to a 10-minute span and dates beyond 2 days; their count is limited so that labels do not overlap. Vertical axes fit the selected history's minimum and maximum across visible series, including spikes. Bounds receive padding, then snap to a tick step of 1, 2 or 5 times a power of ten that gives about five intervals; percentage plots stay within (0–100), network and frequency scales remain nonnegative, and temperature may extend below zero. Tick precision follows the step, and network ticks change unit at 1000 (1M, not 1000K). When no series has readings, percentage plots fall back to (0–100) and other plots to (0–1). Frequency and swap charts explicitly name unavailable frequency readings or unconfigured swap.

**The Measurement Context Rule.** Preserve average, minimum/maximum and peak-time distinctions when displaying historical windows.

### Details and status

Pointer inspection or chart focus synchronizes a vertical cursor and colored average-value dots across every chart at the selected historical window. The cursor is muted, dotted (1px) and partially transparent (0.5). Only the selected chart shows a floating tooltip with the window date/time and per-series mean values. Tooltip rows pair a color dot, label and value; more than eight series use two columns.

Tooltips stay within the plot with a boundary allowance (6px), padding (8px 10px), minimum width (128px), maximum width and height `calc(100% - 12px)`, and internal scrolling for long lists. Dates and labels wrap; values stay on one line. Tooltips float without expanding the panel and let the pointer pass through to the chart, so the cursor keeps following the pointer over them. Detailed ranges and peak times remain in screen-reader live outputs.

Left/Right inspect windows; Home/End reach the oldest/latest window; PageUp/PageDown scroll long tooltips while focus remains on the canvas; Escape clears inspection. Chart labels describe latest values. Header status begins `uptime:` and includes sample age. Refresh failures preserve the previous snapshot with explicit status text; stale samples name their age.

## Do's and Don'ts

### Do:

- Do preserve the summary and chart order across responsive widths.
- Do keep series colors consistent across legends and plot marks.
- Do expose units, unavailable readings and stale data in text.
- Do retain keyboard access and visible focus for charts and controls.

### Don't:

- Don't add external fonts, images, chart libraries or decorative animation.
- Don't replace averaged history with an unqualified instantaneous line.
- Don't turn absent sensor values into zero readings.
