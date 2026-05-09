# Local smoke of the k8s runner

Validates that `example/k8s/entrypoint.sh` produces a valid
`metadata.json`, per-profile JSON files, and a `results.tar.gz`
*before* launching it in the cluster. Cheap insurance against env-var
typos or path mismatches that wouldn't surface until you go read
`kubectl logs` after a wasted scheduling attempt.

The goal is to confirm the **pipeline** works, not to generate
meaningful numbers. A loop device gives terrible perf data and is
fine for smoke.

## Prereqs

- Linux with io_uring (5.6+).
- A built billet binary: `cmake --build build/Release`.
- `sudo` to set up the loop device and to run billet with O_RDWR
  against a block device.

## Setup

Create a backing file + loop device:

```sh
sudo truncate -s 1G /tmp/billet-target.img
LOOP=$(sudo losetup -f --show /tmp/billet-target.img)
echo "loop device: $LOOP"
```

Set up an output directory the entrypoint can write to:

```sh
mkdir -p /tmp/billet-output
```

Make billet discoverable on PATH:

```sh
export PATH="$(pwd)/build/Release/src/cli:$PATH"
which billet            # should resolve under the build tree
billet --version        # sanity
```

## Run the entrypoint

Export the env vars the Job manifest would supply, then invoke the
script directly:

```sh
sudo -E \
    DEVICE_PATH="$LOOP" \
    DEVICE_LABEL=smoke \
    TARGET_PVC=local-loop \
    OUTPUT_DIR=/tmp/billet-output \
    DURATION_S=10 \
    QD=8 \
    METRICS_PORT=9777 \
    METRICS_DRAIN_S=5 \
    JOB_NAME=smoke-job \
    POD_NAME=smoke-pod \
    POD_NAMESPACE=default \
    NODE_NAME="$(hostname)" \
    PATH="$PATH" \
    example/k8s/entrypoint.sh
```

`sudo -E` preserves the env vars; explicit `PATH=$PATH` works around
sudo's secure-path scrubbing so the script finds `billet`. The
`--allow-destructive` flag is set inside the script for the
postgresql run, which is what needs O_RDWR.

A 10s `DURATION_S` keeps each profile short -- the whole smoke
finishes in ~30s including drain.

## Verify

```sh
ls /tmp/billet-output/
# Expect: metadata.json, billet-version.txt,
#         smoke-rr4k.json, smoke-pg.json, results.tar.gz
```

```sh
jq '.status, .scenarios_attempted, .kernel_version, .cpu_count_visible_to_pod' \
    /tmp/billet-output/metadata.json
# Expect: "ok"
#         ["random_read_4k", "postgresql"]
#         "<your kernel string>"
#         <integer>
```

```sh
jq '.profile.name, .results.summary.errors, .results.summary.component_drops' \
    /tmp/billet-output/smoke-pg.json
# Expect: "postgresql"
#         0
#         0
```

```sh
tar -tzf /tmp/billet-output/results.tar.gz
# Expect every output file present in the archive.
```

## Test the compare path end-to-end

`collect.sh` is k8s-specific (uses `kubectl cp` + `kubectl get pvc`),
so skip it locally. The downstream `compare-collected.sh` works on
any directory tree containing `*-rr4k.json` / `*-pg.json` files, so
unpack the archive into a sibling dir and feed it in:

```sh
mkdir -p /tmp/smoke-collected
tar -xzf /tmp/billet-output/results.tar.gz -C /tmp/smoke-collected/
example/k8s/compare-collected.sh /tmp/smoke-collected
# Expect:
#   wrote /tmp/smoke-collected/compare-rr4k.html (1 runs)
#   wrote /tmp/smoke-collected/compare-pg.html  (1 runs)
```

A single-scenario `compare.html` has one bar per panel; the heat-grid
is uninteresting with N=1. That's fine for smoke -- the goal is to
confirm the pipeline runs.

If you want to spot-check the Device capabilities section, open
either HTML in a browser and look for the `fua_supported` /
`discard_supported` / `write_zeroes_supported` row. Values reflect
whatever the loop device reports (typically `discard=yes`, `fua=no`).

## Cleanup

```sh
sudo losetup -d "$LOOP"
sudo rm /tmp/billet-target.img
rm -rf /tmp/billet-output /tmp/smoke-collected
```

## What this smoke does NOT validate

- `kubectl cp` from a Completed pod (manual k8s test on first apply).
- `kubectl get pvc` PV-name resolution (same).
- Prometheus / Beats scrape of the pod's `/metrics` endpoint.
- Whether your image build (Jenkins or otherwise) places the binary
  + entrypoint at the expected paths.
- Block-mode PVC binding with your CSI driver.

These all need a real cluster to validate. The smoke just confirms
the in-pod script doesn't have shell typos.
