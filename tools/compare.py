#!/usr/bin/env python3
"""billet-compare: render a side-by-side comparison report from billet.run/1 JSONs.

Takes N JSON output files (whatever billet writes via --output) and emits a
single self-contained HTML file. Three panel types:

  * Headline bars: one bar-chart panel per metric, one bar per scenario.
  * Tail-shape spectrums: per cell, one polyline per scenario across
    p50, p99, p99.9, p99.99, max on a log Y axis.
  * p99 vs reference heat-grid: ratio of each scenario's per-cell p99
    against a reference run (label "baseline" or first positional arg).

No external Python dependencies; uses stdlib only. Open the output in any
browser to view; right-click any chart to save the inline SVG individually
for slide decks.

Usage:
    tools/compare.py baseline.json md0-raid0.json md0-raid1.json \\
        candidate-raid0-hw1.json candidate-raid0-hw2.json \\
        candidate-raid1-hw1.json candidate-raid1-hw2.json \\
        -o compare.html

Each run's scenario label comes from `device.label` in the JSON (set via
billet's --device-label). Falls back to the JSON filename stem if the
label is empty.
"""

from __future__ import annotations

import argparse
import html as html_mod
import json
import math
from pathlib import Path
from typing import Callable, NamedTuple


# Stable color rotation. Works for up to ~10 scenarios in one comparison.
# Indexed by run position rather than label so duplicate labels still get
# distinct colors.
PALETTE = [
    "#2563eb",  # blue
    "#16a34a",  # green
    "#d97706",  # amber
    "#dc2626",  # red
    "#7c3aed",  # purple
    "#0891b2",  # cyan
    "#db2777",  # pink
    "#ca8a04",  # yellow
    "#475569",  # slate
    "#65a30d",  # lime
]


class Run(NamedTuple):
    label: str
    iops: float
    throughput_mibs: float
    errors: int
    drops: int
    cells: dict  # cell_name -> {count, p50_us, p99_us, p99_9_us, p99_99_us, max_us, bytes}
    # Device capabilities as reported by the kernel at probe time. These
    # are device-claimed (e.g. fua_supported=true just means the device
    # advertises FUA; it does not prove a flush is durably persisted).
    # Surfaced in the report as context for interpreting storage-stack
    # differences; they do not classify billet's fsync path by themselves.
    fua_supported: bool
    discard_supported: bool
    write_zeroes_supported: bool
    rotational: bool
    logical_block: int
    physical_block: int


# Percentile keys carried through `cells`. Ordered for the spectrum X axis;
# the same order is used for column / row layout where percentiles appear.
PERCENTILE_KEYS = ["p50_us", "p99_us", "p99_9_us", "p99_99_us", "max_us"]
PERCENTILE_LABELS = ["p50", "p99", "p99.9", "p99.99", "max"]


def parse_run(path: Path) -> Run:
    with path.open() as f:
        d = json.load(f)
    device = d.get("device", {})
    label = device.get("label") or path.stem
    summary = d["results"]["summary"]
    by_component = d["results"].get("by_component", {})
    cells = {
        name: {
            "count": v.get("count", 0),
            "p50_us": v.get("p50_us", 0),
            "p99_us": v.get("p99_us", 0),
            "p99_9_us": v.get("p99_9_us", 0),
            "p99_99_us": v.get("p99_99_us", 0),
            "max_us": v.get("max_us", 0),
            "bytes": v.get("bytes", 0),
        }
        for name, v in by_component.items()
    }
    return Run(
        label=label,
        iops=float(summary.get("iops_mean", 0)),
        throughput_mibs=float(summary.get("throughput_mibs", 0)),
        errors=int(summary.get("errors", 0)),
        drops=int(summary.get("component_drops", 0)),
        cells=cells,
        fua_supported=bool(device.get("fua_supported", False)),
        discard_supported=bool(device.get("discard_supported", False)),
        write_zeroes_supported=bool(device.get("write_zeroes_supported", False)),
        rotational=bool(device.get("rotational", False)),
        logical_block=int(device.get("logical_block", 0)),
        physical_block=int(device.get("physical_block", 0)),
    )


class MetricSpec(NamedTuple):
    title: str
    unit: str
    direction: str  # "higher" or "lower"
    log_scale: bool
    extract: Callable[[Run], float]


def cell_p99_ms(name: str) -> Callable[[Run], float]:
    def _e(r: Run) -> float:
        c = r.cells.get(name)
        if not c or c.get("count", 0) == 0:
            return 0.0
        return c["p99_us"] / 1000.0
    return _e


AGGREGATE_METRICS: list[MetricSpec] = [
    MetricSpec("Aggregate IOPS", "ops/s", "higher", False, lambda r: r.iops),
    MetricSpec("Throughput",     "MiB/s", "higher", False, lambda r: r.throughput_mibs),
]


def build_cell_p99_metrics(cells: list[str]) -> list[MetricSpec]:
    """Per-cell p99 latency MetricSpecs derived from the cells actually
    present in the report. Earlier versions hard-coded the PG set, which
    rendered noisy n/a rows for non-PG profiles (random_read_4k etc.).
    """
    return [
        MetricSpec(f"{cell} p99", "ms", "lower", True, cell_p99_ms(cell))
        for cell in cells
    ]


# Cells we know about, in the order we want them to appear in spectrum
# panels and heat-grid columns. Unknown cells (future profiles) get
# appended alphabetically.
KNOWN_CELL_ORDER = [
    "reader.Read",
    "rand_writer.Write",
    "wal.Write",
    "wal.Fsync",
    "ckpt.Write",
]


def discover_cells(runs: list[Run]) -> list[str]:
    """Union of cells across all runs, ordered by KNOWN_CELL_ORDER then alpha."""
    seen: set[str] = set()
    for r in runs:
        seen.update(r.cells.keys())
    ordered: list[str] = [c for c in KNOWN_CELL_ORDER if c in seen]
    extras = sorted(c for c in seen if c not in KNOWN_CELL_ORDER)
    return ordered + extras


def format_value(v: float, unit: str) -> str:
    if unit == "ops/s":
        return f"{v:,.0f}"
    if unit == "MiB/s":
        return f"{v:.1f}"
    if unit == "ms":
        if v <= 0:
            return "0"
        if v < 1:
            return f"{v * 1000:.0f} µs"
        if v < 100:
            return f"{v:.2f} ms"
        if v < 1000:
            return f"{v:.0f} ms"
        return f"{v / 1000:.2f} s"
    return f"{v:.2f}"


def format_us(v: float) -> str:
    """Compact latency annotation for spectrum tick labels."""
    if v <= 0:
        return "0"
    if v < 1000:
        return f"{v:.0f}µs"
    if v < 100_000:
        return f"{v / 1000:.2f}ms"
    if v < 1_000_000:
        return f"{v / 1000:.0f}ms"
    return f"{v / 1_000_000:.2f}s"


# -------- panel: headline bars (existing) ----------------------------------

def render_bar_panel(metric: MetricSpec, runs: list[Run], colors: list[str]) -> str:
    """Emit one SVG containing horizontal bars for `metric` across `runs`."""

    width = 880
    bar_height = 30
    gap = 8
    margin_left = 240
    margin_right = 130
    margin_top = 60
    margin_bottom = 28
    n = len(runs)
    plot_h = n * (bar_height + gap)
    height = margin_top + plot_h + margin_bottom
    plot_width = width - margin_left - margin_right

    values = [metric.extract(r) for r in runs]

    if metric.log_scale:
        # log10(v + offset) where offset is chosen so the smallest nonzero
        # value still gets a visible bar. v <= 0 floors at 0 (suppressed).
        nonzero = [v for v in values if v > 0]
        offset = (min(nonzero) / 4.0) if nonzero else 1.0
        def transform(v: float) -> float:
            if v <= 0:
                return 0.0
            return math.log10(v + offset) - math.log10(offset)
        plot_values = [transform(v) for v in values]
    else:
        plot_values = list(values)
    plot_max = max(plot_values + [1.0])

    parts: list[str] = []
    parts.append(
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
        f'width="{width}" height="{height}" font-family="system-ui, sans-serif" font-size="14">'
    )

    direction = "higher is better" if metric.direction == "higher" else "lower is better"
    scale_note = " (log scale)" if metric.log_scale else ""
    parts.append(
        f'<text x="{margin_left}" y="24" font-weight="600" font-size="17">'
        f'{html_mod.escape(metric.title)}</text>'
    )
    parts.append(
        f'<text x="{margin_left}" y="44" fill="#64748b" font-size="12">'
        f'{metric.unit} &middot; {direction}{scale_note}</text>'
    )

    for i, (run, val, plot_v) in enumerate(zip(runs, values, plot_values)):
        y = margin_top + i * (bar_height + gap)
        bar_w = (plot_v / plot_max) * plot_width if plot_max > 0 else 0
        bar_w = max(0, bar_w)
        color = colors[i]
        parts.append(
            f'<text x="{margin_left - 10}" y="{y + bar_height / 2 + 5}" text-anchor="end" '
            f'font-family="ui-monospace, monospace" font-size="13" fill="#0f172a">'
            f'{html_mod.escape(run.label)}</text>'
        )
        parts.append(
            f'<rect x="{margin_left}" y="{y}" width="{bar_w:.1f}" '
            f'height="{bar_height}" fill="{color}" />'
        )
        annotation = format_value(val, metric.unit) if val > 0 else "n/a"
        parts.append(
            f'<text x="{margin_left + bar_w + 8:.1f}" y="{y + bar_height / 2 + 5}" '
            f'font-family="ui-monospace, monospace" font-size="13" fill="#0f172a">'
            f'{html_mod.escape(annotation)}</text>'
        )

    parts.append("</svg>")
    return "\n".join(parts)


# -------- panel: percentile spectrum (new) ---------------------------------

def render_percentile_spectrum(cell: str, runs: list[Run], colors: list[str]) -> str:
    """One panel for `cell`: lines connecting p50 .. max for each scenario.

    Scenarios with count == 0 in this cell get a legend entry suffixed
    "(n/a)" and no polyline. Drawing a line through 0 on log scale would
    falsely imply huge improvement, so we skip absent data entirely.
    """

    width = 880
    margin_left = 80
    margin_right = 240  # space for legend
    margin_top = 60
    margin_bottom = 50
    plot_height = 280
    height = margin_top + plot_height + margin_bottom
    plot_width = width - margin_left - margin_right

    # Collect (run_index, [percentile_us]) for runs with data.
    present: list[tuple[int, Run, list[float]]] = []
    absent: list[tuple[int, Run]] = []
    for i, r in enumerate(runs):
        c = r.cells.get(cell)
        if not c or c.get("count", 0) == 0:
            absent.append((i, r))
            continue
        values = [float(c.get(k, 0)) for k in PERCENTILE_KEYS]
        # If every percentile is 0 the cell technically had ops but the
        # histogram is empty -- treat as absent too.
        if max(values) <= 0:
            absent.append((i, r))
            continue
        present.append((i, r, values))

    # Y axis range across present data
    all_y = [v for _, _, vs in present for v in vs if v > 0]
    y_min = min(all_y) if all_y else 1.0
    y_max = max(all_y) if all_y else 1.0
    # Pad log range slightly for headroom
    log_min = math.floor(math.log10(max(y_min, 1.0))) - 0.2
    log_max = math.ceil(math.log10(max(y_max, 10.0))) + 0.2
    if log_max <= log_min:
        log_max = log_min + 1.0

    def y_pixel(v: float) -> float:
        if v <= 0:
            return margin_top + plot_height
        lv = math.log10(v)
        frac = (lv - log_min) / (log_max - log_min)
        return margin_top + plot_height * (1 - frac)

    # X positions: equally-spaced categorical
    n_pcts = len(PERCENTILE_LABELS)
    def x_pixel(idx: int) -> float:
        if n_pcts == 1:
            return margin_left + plot_width / 2
        return margin_left + (plot_width * idx) / (n_pcts - 1)

    parts: list[str] = []
    parts.append(
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
        f'width="{width}" height="{height}" font-family="system-ui, sans-serif" font-size="14">'
    )
    parts.append(
        f'<text x="{margin_left}" y="24" font-weight="600" font-size="17">'
        f'{html_mod.escape(cell)} latency spectrum</text>'
    )
    parts.append(
        f'<text x="{margin_left}" y="44" fill="#64748b" font-size="12">'
        f'µs &middot; lower is better (log scale)</text>'
    )

    # Gridlines + Y axis labels at integer log decades
    decade_lo = math.ceil(log_min)
    decade_hi = math.floor(log_max)
    for decade in range(int(decade_lo), int(decade_hi) + 1):
        y = y_pixel(10 ** decade)
        parts.append(
            f'<line x1="{margin_left}" y1="{y:.1f}" '
            f'x2="{margin_left + plot_width}" y2="{y:.1f}" '
            f'stroke="#e2e8f0" stroke-width="1" />'
        )
        parts.append(
            f'<text x="{margin_left - 8}" y="{y + 4:.1f}" text-anchor="end" '
            f'fill="#64748b" font-size="11">{format_us(10 ** decade)}</text>'
        )

    # X axis labels
    for j, label in enumerate(PERCENTILE_LABELS):
        x = x_pixel(j)
        parts.append(
            f'<line x1="{x:.1f}" y1="{margin_top}" '
            f'x2="{x:.1f}" y2="{margin_top + plot_height}" '
            f'stroke="#f1f5f9" stroke-width="1" />'
        )
        parts.append(
            f'<text x="{x:.1f}" y="{margin_top + plot_height + 18:.1f}" '
            f'text-anchor="middle" fill="#64748b" font-size="12">'
            f'{html_mod.escape(label)}</text>'
        )

    # Polylines + circles
    for i, r, vs in present:
        color = colors[i]
        pts = " ".join(f"{x_pixel(j):.1f},{y_pixel(v):.1f}" for j, v in enumerate(vs))
        parts.append(
            f'<polyline fill="none" stroke="{color}" stroke-width="2" points="{pts}" />'
        )
        for j, v in enumerate(vs):
            cx, cy = x_pixel(j), y_pixel(v)
            parts.append(
                f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="3.5" fill="{color}" />'
            )

    # Legend (top-right)
    legend_x = margin_left + plot_width + 16
    legend_y = margin_top
    parts.append(
        f'<text x="{legend_x}" y="{legend_y + 12}" font-size="12" font-weight="600" '
        f'fill="#475569">scenarios</text>'
    )
    row_y = legend_y + 30
    for i, r, _vs in present:
        color = colors[i]
        parts.append(
            f'<rect x="{legend_x}" y="{row_y - 8}" width="14" height="3" fill="{color}" />'
        )
        parts.append(
            f'<text x="{legend_x + 20}" y="{row_y}" font-size="11" '
            f'font-family="ui-monospace, monospace" fill="#0f172a">'
            f'{html_mod.escape(r.label)}</text>'
        )
        row_y += 16
    for i, r in absent:
        parts.append(
            f'<text x="{legend_x + 20}" y="{row_y}" font-size="11" '
            f'font-family="ui-monospace, monospace" fill="#94a3b8">'
            f'{html_mod.escape(r.label)} (n/a)</text>'
        )
        row_y += 16

    parts.append("</svg>")
    return "\n".join(parts)


# -------- panel: p99-vs-reference heat-grid (new) --------------------------

def pick_reference(runs: list[Run]) -> tuple[int, bool]:
    """Returns (index, was_baseline_match). First run with label.lower() ==
    'baseline' wins; otherwise the first positional arg is used and the
    caller surfaces the fallback in the panel header so readers don't
    assume it was an intentional reference choice.
    """
    for i, r in enumerate(runs):
        if r.label.lower() == "baseline":
            return i, True
    return 0, False


def _heat_color(ratio: float) -> str:
    """Green for ratio < 1 (improvement), white near 1, red for > 1.

    Saturation scales with |log10(ratio)|, capped so 1000x maps to fully
    red. Values within ~0.95-1.05 read as white-ish to avoid noise.
    """
    if ratio <= 0:
        return "#f1f5f9"  # neutral slate
    lr = math.log10(ratio)
    if abs(lr) < 0.02:
        return "#ffffff"
    if lr < 0:
        sat = min(1.0, abs(lr))  # 0.1x -> sat 1.0
        # mix toward green #16a34a
        return _blend("#ffffff", "#16a34a", sat)
    sat = min(1.0, lr / 3.0)  # 1000x -> sat 1.0
    return _blend("#ffffff", "#dc2626", sat)


def _blend(c1: str, c2: str, t: float) -> str:
    """Linear interpolate two hex colors. t in [0,1]."""
    def parse(c: str) -> tuple[int, int, int]:
        return int(c[1:3], 16), int(c[3:5], 16), int(c[5:7], 16)
    r1, g1, b1 = parse(c1)
    r2, g2, b2 = parse(c2)
    r = round(r1 + (r2 - r1) * t)
    g = round(g1 + (g2 - g1) * t)
    b = round(b1 + (b2 - b1) * t)
    return f"#{r:02x}{g:02x}{b:02x}"


def render_p99_vs_reference(runs: list[Run]) -> str:
    """Heat-grid: rows = scenarios (excluding reference), cols = cells."""

    ref_idx, baseline_match = pick_reference(runs)
    ref = runs[ref_idx]
    others = [r for i, r in enumerate(runs) if i != ref_idx]
    cells = discover_cells(runs)

    cell_w = 130
    cell_h = 38
    margin_left = 240
    margin_top = 80
    margin_right = 20
    margin_bottom = 24
    width = margin_left + cell_w * len(cells) + margin_right
    height = margin_top + cell_h * len(others) + margin_bottom

    parts: list[str] = []
    parts.append(
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
        f'width="{width}" height="{height}" font-family="system-ui, sans-serif" font-size="14">'
    )
    parts.append(
        f'<text x="{margin_left}" y="24" font-weight="600" font-size="17">'
        f'p99 latency vs reference</text>'
    )
    ref_note = (
        "no run labeled 'baseline'; using first positional arg as reference"
        if not baseline_match
        else ""
    )
    parts.append(
        f'<text x="{margin_left}" y="44" fill="#64748b" font-size="12">'
        f'ratio &middot; lower is better &middot; reference: '
        f'<tspan font-family="ui-monospace, monospace" fill="#0f172a">'
        f'{html_mod.escape(ref.label)}</tspan>'
        + (f' <tspan fill="#b45309">({html_mod.escape(ref_note)})</tspan>' if ref_note else '')
        + '</text>'
    )

    # Column headers
    for j, cell in enumerate(cells):
        x = margin_left + cell_w * j + cell_w / 2
        parts.append(
            f'<text x="{x:.1f}" y="{margin_top - 8}" text-anchor="middle" '
            f'font-size="12" font-family="ui-monospace, monospace" fill="#475569">'
            f'{html_mod.escape(cell)}</text>'
        )

    # Rows
    for i, run in enumerate(others):
        y = margin_top + cell_h * i
        # Row label
        parts.append(
            f'<text x="{margin_left - 10}" y="{y + cell_h / 2 + 5:.1f}" text-anchor="end" '
            f'font-family="ui-monospace, monospace" font-size="13" fill="#0f172a">'
            f'{html_mod.escape(run.label)}</text>'
        )
        for j, cell in enumerate(cells):
            x = margin_left + cell_w * j
            rc = ref.cells.get(cell)
            sc = run.cells.get(cell)
            if (not rc or rc.get("count", 0) == 0
                    or not sc or sc.get("count", 0) == 0):
                # n/a hatched
                parts.append(
                    f'<rect x="{x:.1f}" y="{y:.1f}" width="{cell_w}" height="{cell_h}" '
                    f'fill="#f8fafc" stroke="#e2e8f0" />'
                )
                parts.append(
                    f'<text x="{x + cell_w / 2:.1f}" y="{y + cell_h / 2 + 5:.1f}" '
                    f'text-anchor="middle" fill="#94a3b8" font-size="12" '
                    f'font-family="ui-monospace, monospace">n/a</text>'
                )
                continue
            ratio = sc["p99_us"] / rc["p99_us"] if rc["p99_us"] > 0 else 0
            fill = _heat_color(ratio)
            parts.append(
                f'<rect x="{x:.1f}" y="{y:.1f}" width="{cell_w}" height="{cell_h}" '
                f'fill="{fill}" stroke="#e2e8f0" />'
            )
            if ratio == 0:
                txt = "n/a"
            elif ratio < 10:
                txt = f"{ratio:.2f}x"
            else:
                txt = f"{ratio:,.0f}x"
            parts.append(
                f'<text x="{x + cell_w / 2:.1f}" y="{y + cell_h / 2 + 5:.1f}" '
                f'text-anchor="middle" fill="#0f172a" font-size="13" '
                f'font-family="ui-monospace, monospace">{html_mod.escape(txt)}</text>'
            )

    parts.append("</svg>")
    return "\n".join(parts)


# -------- summary + html assembly ------------------------------------------

def render_summary_table(runs: list[Run]) -> str:
    """Errors/drops sanity row -- a non-zero value invalidates the comparison."""
    rows = []
    for r in runs:
        flag = "" if (r.errors == 0 and r.drops == 0) else " &#9888;"
        rows.append(
            f'<tr><td>{html_mod.escape(r.label)}</td>'
            f'<td>{r.errors}</td><td>{r.drops}</td><td>{flag}</td></tr>'
        )
    return (
        '<table>'
        '<thead><tr><th>scenario</th><th>errors</th><th>component_drops</th><th></th></tr></thead>'
        '<tbody>' + "".join(rows) + '</tbody></table>'
    )


def render_device_caps_table(runs: list[Run]) -> str:
    """Per-scenario device capability rows. Surfaced ABOVE the heat-grid
    as context for interpreting comparisons. FUA, discard, and
    write_zeroes are advertised capabilities, not proof that a specific
    workload path used or honored them.
    """
    def yes_no(v: bool) -> str:
        if v:
            return '<td style="color:#0f172a">yes</td>'
        # No -- amber so missing capabilities draw the eye. Not red:
        # absent FUA/discard/write_zeroes is a device property, not a
        # comparison error.
        return '<td style="color:#b45309">no</td>'

    rows = []
    for r in runs:
        block = f"{r.logical_block} / {r.physical_block}" if r.logical_block else "?"
        rows.append(
            f'<tr><td>{html_mod.escape(r.label)}</td>'
            + yes_no(r.fua_supported)
            + yes_no(r.discard_supported)
            + yes_no(r.write_zeroes_supported)
            + yes_no(r.rotational)
            + f'<td>{block}</td></tr>'
        )
    return (
        '<table>'
        '<thead><tr>'
        '<th>scenario</th>'
        '<th>fua</th>'
        '<th>discard</th>'
        '<th>write_zeroes</th>'
        '<th>rotational</th>'
        '<th>logical / physical block</th>'
        '</tr></thead>'
        '<tbody>' + "".join(rows) + '</tbody></table>'
    )


def render_html(runs: list[Run], title: str) -> str:
    # Colors keyed by run position so duplicate labels don't collide.
    colors = [PALETTE[i % len(PALETTE)] for i in range(len(runs))]
    cells = discover_cells(runs)

    # Headline panels are aggregate (IOPS, throughput) plus one p99 bar
    # per cell discovered in the runs. Building cell metrics from
    # discover_cells keeps the report profile-agnostic -- a
    # random_read_4k-only report renders one reader.Read p99 bar, not
    # five mostly-empty rows.
    metrics = AGGREGATE_METRICS + build_cell_p99_metrics(cells)
    bar_panels = [render_bar_panel(m, runs, colors) for m in metrics]
    spectrum_panels = [render_percentile_spectrum(c, runs, colors) for c in cells]
    heat = render_p99_vs_reference(runs)
    device_caps = render_device_caps_table(runs)

    def wrap_panel(svg: str) -> str:
        return f'<div class="panel">{svg}</div>'

    body_bars = "\n".join(wrap_panel(p) for p in bar_panels)
    body_spectrums = "\n".join(wrap_panel(p) for p in spectrum_panels)
    body_heat = wrap_panel(heat)
    body_device_caps = wrap_panel(device_caps)

    sanity = render_summary_table(runs)

    return f"""<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>{html_mod.escape(title)}</title>
<style>
  body {{ font-family: system-ui, sans-serif; margin: 0; padding: 24px;
          background: #f8fafc; color: #0f172a; }}
  h1 {{ margin: 0 0 8px 0; }}
  h2 {{ margin: 28px 0 12px 0; font-size: 18px; color: #334155;
        border-bottom: 1px solid #e2e8f0; padding-bottom: 4px; }}
  .meta {{ color: #64748b; font-size: 13px; margin: 0 0 24px 0; }}
  .panel {{ background: white; padding: 14px 22px; margin-bottom: 18px;
            border: 1px solid #e2e8f0; border-radius: 8px;
            box-shadow: 0 1px 2px rgba(15, 23, 42, 0.04); }}
  table {{ font-family: ui-monospace, monospace; font-size: 13px;
           border-collapse: collapse; }}
  th, td {{ padding: 4px 12px; text-align: left;
            border-bottom: 1px solid #e2e8f0; }}
  th {{ font-weight: 600; color: #64748b; }}
  details {{ margin-top: 24px; }}
  summary {{ cursor: pointer; color: #475569; font-size: 13px; }}
</style>
</head>
<body>
<h1>{html_mod.escape(title)}</h1>
<p class="meta">{len(runs)} runs &middot; right-click any chart to save the SVG</p>
<h2>Headlines</h2>
{body_bars}
<h2>Tail shape</h2>
{body_spectrums}
<h2>Device capabilities</h2>
<p class="meta">As reported by the kernel at probe time. These are
advertised device capabilities, not proof that a specific operation path used
or honored them. billet submits wal.Fsync with fsync; <code>fua</code> only
says whether the device advertises FUA writes.</p>
{body_device_caps}
<h2>p99 vs reference</h2>
{body_heat}
<details><summary>sanity: errors &amp; component_drops (zero on clean runs)</summary>
{sanity}
</details>
</body>
</html>
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("jsons", nargs="+", type=Path, help="billet.run/1 JSON files")
    parser.add_argument("-o", "--output", type=Path, default=Path("compare.html"),
                        help="Output HTML path (default: compare.html)")
    parser.add_argument("--title", default="billet comparison",
                        help="Report title (default: 'billet comparison')")
    args = parser.parse_args()

    runs = [parse_run(p) for p in args.jsons]
    if not runs:
        print("no runs to compare", file=__import__("sys").stderr)
        return 2
    html = render_html(runs, args.title)
    args.output.write_text(html)
    print(f"Wrote {args.output} ({len(runs)} runs)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
