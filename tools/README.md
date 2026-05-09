# tools

billet: function generator and oscilloscope for block devices.

Standalone utilities. Not part of the build, not linked into the binary.

## `compare.py`

Renders a side-by-side comparison report from N billet.run/1 JSON files.
Three panel types in one HTML document, pure stdlib, no external Python
dependencies. Output is a single self-contained file with inline SVG
charts -- right-click any chart to save the SVG individually for slide
decks.

```sh
tools/compare.py baseline.json md0-raid0.json candidate-raid0-hw2.json \
    -o compare.html --title "Stack comparison @ qd=32, 120s"
```

Each scenario's label comes from `device.label` in the JSON (set via
billet's `--device-label`); falls back to the JSON filename stem when the
label is empty. Scenarios are colored by run position, so duplicate
labels still get distinct colors. Cells with `count == 0` (op kind the
profile didn't emit) render as `n/a`, never as zero -- zero on a log
scale or in a ratio cell would falsely look like a huge win.

### Panels

**Headlines** (one panel per metric, one bar per scenario). Aggregate
IOPS, throughput, then one p99 latency panel per cell discovered in
the runs (driven from `by_component` keys, not a hard-coded list, so a
random_read_4k-only report shows one reader.Read panel rather than
five mostly-empty PG rows). Latency panels render on a log10 scale so
microseconds and seconds stay readable side by side. Direction
("higher is better" / "lower is better") is in each panel header.

**Tail shape** (one panel per cell). For each component cell, one
polyline per scenario across `p50 | p99 | p99.9 | p99.99 | max` on a log
Y axis. Reads "where does the tail diverge?" If two scenarios' lines run
parallel but offset, the layer adds a constant per-percentile cost. If
one scenario's line bends sharply between p99 and p99.9, the layer has
a bursty tail that doesn't show up in p99 alone.

**Device capabilities** (single per-scenario table). Per-row: whether
the device claims FUA, discard, write_zeroes, rotational, plus its
logical / physical block sizes (from billet's probe). Sits *above*
the heat-grid because device capabilities are useful context when
interpreting storage-stack differences. These are advertised
capabilities, not proof that a specific operation path used or honored
them; billet submits `wal.Fsync` with fsync, while `fua` only says
whether the device advertises FUA writes.

**p99 vs reference** (single heat-grid). Rows are scenarios excluding
the reference, columns are component cells, each cell shows
`scenario_p99 / reference_p99`. Reference is the first run whose label
lowercases to `baseline`, else the first positional argument with an
amber warning in the panel header so readers don't mistake a happenstance
reference for an intentional one. Cell colors: green for improvement
(< 1x), red for regression (> 1x), saturation by `log10(ratio)`. Read
top-to-bottom for which scenarios cost the most; left-to-right for
which cells the cost lives in. The reference isn't necessarily a pure
layer-overhead control -- if you use raw NVMe as the reference, ratios
fold in topology and RAID effects along with layer overhead, so don't
oversell what the numbers mean.

### Sanity row

The report ends with a "sanity" disclosure showing per-run `errors` and
`component_drops`. Any non-zero value invalidates the comparison for
that run.
