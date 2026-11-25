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
    fontFamily: 'ui-monospace, SFMono-Regular, Consolas, "Liberation Mono", monospace'
    fontSize: "18px"
    fontWeight: 650
    lineHeight: 1.3
  body:
    fontFamily: 'ui-monospace, SFMono-Regular, Consolas, "Liberation Mono", monospace'
    fontSize: "14px"
  label:
    fontFamily: 'ui-monospace, SFMono-Regular, Consolas, "Liberation Mono", monospace'
    fontSize: "14px"
    fontWeight: 500
    lineHeight: 1.5
    letterSpacing: ".07em"
  measurement:
    fontFamily: 'ui-monospace, SFMono-Regular, Consolas, "Liberation Mono", monospace'
    fontSize: "clamp(23px, 2.2vw, 32px)"
    fontWeight: 400
    lineHeight: 1.6
rounded:
  panel: "5px"
  control: "6px"
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
    padding: "14px 12px 12px"
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
    padding: "14px 12px 12px"
    width: "100%"
    height: "100%"
  chart-control:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.ink}"
    rounded: "{rounded.control}"
    height: "24px"
    padding: "0 6px"
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

Teal identifies the first plot series, summary qualifiers, selection and keyboard focus. Green marks current summary values and the healthy sampling dot; it is distinct from the series palette.

### Secondary

Violet identifies the second plot series and supporting summary values. Gray and ochre identify the third and fourth CPU lines; rose, blue, olive and mauve identify the fifth through eighth. Further cores receive deterministic HSL colors: hue `(k * 137.508) % 360`, rounded to one decimal place, saturation (55%), and lightness (38%) in light theme or (68%) in dark theme, where `k` is the zero-based series index. Legend labels retain actual detected CPU IDs; all cores appear together in the first chart.

### Neutral

Background, panel, ink, muted and grid tokens provide the page, surfaces, primary text, secondary text and plot scaffolding. Dark tokens replace their corresponding light roles when the theme changes.

**The Series Identity Rule.** Keep the series color order consistent between legends, lines, bands and peak marks.

## Typography

Use the local monospace stack in the token layer; values use tabular numerals. All UI text, including canvas axes and tooltips, is at least 14px as requested by the user. The project title uses the title role. Uppercase section labels use the label role; qualifiers restore normal casing and letter spacing. Status, controls, legends, axes and footer use a minimum text size of 14px.

Measurements use the measurement role, centered in summaries. At the middle breakpoint they use (28px); at the narrow breakpoint they use (27px). Network uses the same green measurement color and sizes its combined rates to its tile: `clamp(14px, 7.8cqi, 32px)`, allowing wrapping on narrow tiles. Capacity values carry explicit binary units; compact network rates use decimal units with their meaning exposed in the control title.

## Layout

The page has a minimum width (280px), outer padding (14px) and panel gutters (14px). Desktop compact summaries use six equal columns. The six history charts and Disks use three equal columns with no spanning tile. At widths up to (1280px), summaries use three columns and charts use two. At widths up to (680px), summaries use two columns and charts use one, with outer padding and grid gaps (10px). Network occupies a regular summary column. Header controls wrap, and on narrow screens move below the title/status. Preserve source order across widths.

Charts have panel padding (12px), with horizontal padding reduced to (10px) on narrow screens. Canvas height is `clamp(180px, 28vh, 290px)` on wider screens and (210px) at the narrow breakpoint. Above (1900px), the page centers within a maximum width (2200px). The footer stacks on narrow screens. At widths up to 400px, memory, swap and temperature values use 18px so their units fit. Canvas plots reserve 60px at the left and 32px at the bottom for 14px axis labels; time labels are measured to keep them inside the plot.

## Elevation & Depth

Panels use tonal separation without shadows or default borders. Floating chart tooltips use the panel fill, a thin grid-colored border and a subtle shadow (`0 3px 12px #00000016`). Hover outlines and keyboard focus supply state feedback without movement or transitions.

## Shapes

Panels have gently rounded corners using the panel radius; native controls use the control radius. Legend markers and the healthy status dot are circles (7px); inspection markers are circles (6px). Controls are compact, borderless rectangles. Floating chart tooltips have a small corner radius (3px).

## Components

### Dashboard controls

The native range selector displays 1h, 1d, 7d, 30d and 90d, with 1h initially selected. A Graph/Bars selector sits at the top-right of the per-core tile, beside its heading, with no visible label. Its accessible label is Per-core CPU view; the graph legend wraps on the row beneath. The remaining charts retain the selected history range. An icon button switches themes; its accessible label describes the next theme. Controls use compact monospace text and panel fill. Hover adds a muted outline (1px); keyboard focus adds a teal outline (2px) offset by (3px). Theme choice is retained locally when storage is available and otherwise starts from the system preference.

Interactive summaries are full-panel buttons with the same panel geometry and internal spacing. Their focus outline has no offset. The RAM/swap chart control is a small text button (24px high, 14px type); its pressed state and accessible label identify the displayed history. Network interface cycling is disabled when fewer than two interfaces are available, without a hover outline.

### Summaries

Each summary places an uppercase label and teal qualifier above a centered green current value, followed by a right-aligned violet secondary value when applicable. Interactive headings use a shared flex row so long interface names truncate within the heading. Missing values appear as an em dash. Labels retain units or measurement context. Visible click-to-toggle/cycle hints are omitted; buttons retain their accessible next-action labels.

The six compact summaries are whole-machine CPU usage, CPU time, memory, swap, temperature and network. Whole-machine CPU usage comes from aggregate counters and names the logical CPU count below it. CPU time toggles user/system; memory toggles estimated used/free capacity with percentage and total beneath it. Memory free means available memory including reclaimable cache, and capacities are estimated from stored percentage readings. Swap shows used capacity, percentage and total, or `off` with explanatory text.

The network summary cycles interfaces and displays compact upload/download rates as `U4.2K|D2.5M`, without spaces, with decimal B/K/M/G units and a per-second caption. The selected interface name truncates with an ellipsis if needed and also identifies the network history.

### Disks

The Disks panel occupies one regular chart tile, with a body matching canvas height. Its focusable scrolling list uses one entry per mounted filesystem. A 16px source/mount label sits above a thick usage meter. Compact used/total text at 14px sits directly below the meter; matching units are written once, for example 280/362 GiB. Available capacity sits to the right of the meter at 16px with a 14px avail caption beneath. Explicit B/KiB/MiB/GiB/TiB units remain visible. The full source and mount are retained in the label's title. Labels truncate with an ellipsis. Entries use 12px column and entry gaps, with 4px gaps inside each entry.

The disk meter is 18px high, with a grid-colored track, teal fill and 2px corners; CPU meters remain 8px high. Its value is used/total; an accessible label and percentage text describe it. Unavailable capacities use an empty decorative track and explicit unavailable text. The panel reports mounted count, partial or unavailable data, update age and refresh failures in text.

### Services

When services are selected, a Services panel follows Disks in the same responsive chart grid.
It matches the chart body height and contains a plain semantic table with Name, Mem and Uptime
columns at 14px. Service names wrap rather than disappear; memory and uptime align right with
tabular numerals. Muted column headers remain visible while the body scrolls, with quiet grid
rules separating rows and teal memory values. Capacities use explicit binary units. Inactive
or failed states occupy the uptime cell, and unavailable memory is an em dash. The tile is
hidden without configured services; there are no service charts, toggles or extra controls.

### History charts

The first plot shows all detected per-core CPU percentages together. Its header and right-aligned legend wrap; each CPU ID stays attached to its color marker. In Bars view, the legend and canvas hide and a list of current readings appears instead: actual CPU ID, native usage meter and 0.1% value per row. Colors match the plot series. Two columns fill downward (16 cores yield eight rows); up to 420px viewport width, rows use one column. Large lists scroll within the same plot height. Missing readings show an em dash and a muted empty track. Nodes are created only when Bars is first shown or the CPU topology changes; hidden bars are not updated and the hidden CPU graph is not drawn.

The second plot shows aggregate CPU user and system percentages. The third shows a single RAM-used line, switchable to swap-used history. The fourth shows CPU frequency in GHz, with one series per shared-clock policy, labeled by its leading CPU and identified as hardware- or driver-reported. Temperature and selected-interface upload/download history complete the graph set. Chart headers wrap and legends repeat the corresponding series colors.

Average lines use stroke width (1.2px), minimum/maximum bands use opacity (0.12), and peak marks use opacity (0.5) with stroke width (0.8px). Grid strokes use (0.6px). Missing samples and long gaps break the lines. Absent sensors and empty histories show text within the plot.

Vertical axes fit the selected history's minimum and maximum across visible series, including spikes. Bounds receive padding and rounding; percentage plots stay within (0–100), network and frequency scales remain nonnegative, and temperature may extend below zero. Tick precision follows the displayed span. When no series has readings, percentage plots fall back to (0–100) and other plots to (0–1). Frequency and swap charts explicitly name unavailable frequency readings or unconfigured swap.

**The Measurement Context Rule.** Preserve average, minimum/maximum and peak-time distinctions when displaying historical windows.

### Details and status

Pointer inspection or chart focus synchronizes a vertical cursor and colored average-value dots across every chart at the selected historical window. The cursor is muted, dotted (1px) and partially transparent (0.5). Only the selected chart shows a floating tooltip with the window date/time and per-series mean values. Tooltip rows pair a color dot, label and value; more than eight series use two columns.

Tooltips stay within the plot with a boundary allowance (6px), padding (8px 10px), minimum width (128px), maximum width and height `calc(100% - 12px)`, and internal scrolling for long lists. Dates and labels wrap; values stay on one line. Tooltips float without expanding the panel. Detailed ranges and peak times remain in screen-reader live outputs.

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
