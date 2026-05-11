# billet

A C++23 block-device benchmark tool. Drives raw block devices (O_DIRECT, native liburing) with parametric app-shaped workload profiles. Compares candidate block drivers, virtual storage targets, md-raid, and raw devices through the same app-shaped workload lens.

The full design lives in `PLAN.md` at the repo root. `PLAN.md` is gitignored; treat it as a working document, not committed truth.

## Coding conventions

### No em dashes in committed files
Do not use em dashes (U+2014) in any file that gets committed. Use `--` (double hyphen), `:`, `;`, parentheses, or rewrite. Planning and scratch files (e.g. `PLAN.md`, which is gitignored) are exempt; this file is committed and follows the rule.

### Language
C++23. Match the repo's existing style and toolchain (CMake, Conan, sanitizers, GTest). Reuse shared `cmake/` helpers where applicable.

### East `const`
Place `const` to the right of the thing being made `const`: `int const x`, `T const&`, `T const*`, `T* const`. Never west-const (`const int x`, `const T&`). Reads consistently right-to-left and pairs naturally with pointer-to-const vs const-pointer.

### Yoda comparisons
When one side of a comparison is a literal, constant, or `const`-qualified value, write it on the **left**: `if (5 == x)`, `if (k_max <= n)`, `if (nullptr != p)`. A typo'd `=` then fails to compile (`if (5 = x)`) instead of silently assigning.

### Metrics
Use `sisl::metrics` directly. Do not port, mirror, or duplicate its bucket constants, registry, or HTTP exposition. The `/metrics` endpoint is served by sisl's existing HTTP module.

### No runtime shell-outs
All I/O is internal to the binary. The tool never invokes `fio`, `dbench`, `dd`, or similar at runtime. `fio` is permitted only as a one-time developer validation step (documented in `example/`), never wired into normal operation.

### Raw block device only
Always open the device with `O_DIRECT`. No filesystem layer. Buffer alignment is the device physical block size (probed via `BLKPBSZGET`), not assumed 4K.

### Coordinated-omission-correct latency
Record latency as `completion_ts - intended_ts_ns`, never `completion_ts - submit_ts`. Open-loop workloads schedule via a Poisson arrival process; closed-loop QD-bounded workloads set `intended_ts_ns = issue_ts`.

### Destructive ops are gated
Any workload that issues `Write`, `Discard`, `Fsync`, or `WriteZeroes` MUST check `SISL_OPTIONS["allow-destructive"].as<bool>()` at construction and refuse to run if false. The default is safe (read-only) so users can point billet at a device hosting a live filesystem without footgun risk. Read-only workloads (random reads, etc.) ignore the flag and always open the device `O_RDONLY`.

## Architecture seams

- `Workload` interface (`include/billet/workload/workload.hpp`) is the only thing the engine consumes. Future trace-replay drivers implement this same interface; do not introduce engine-side branches for "is this a profile or a trace?"
- Engine code never includes profile-specific headers.
- Stats has zero I/O dependencies; report has zero engine dependencies.

## Layout

```
src/{engine,workload,stats,report,cli}/   per-module sources
include/billet/                            public headers per module
test/                                      GTest unit tests
example/                                   loopback demo, fio sanity-check writeup
```

## Build & test

To be filled in once Step 1 of `PLAN.md` lands a working CMake scaffold.
