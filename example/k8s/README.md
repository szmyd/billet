# Kubernetes deployment of billet

billet: function generator and oscilloscope for block devices.

Run billet as a `Job` against a block-mode PVC, scrape its `/metrics`
endpoint with your in-cluster prometheus (or Elastic Metricbeat),
then collect the run JSONs + audit metadata for offline comparison
via `tools/compare.py`.

This directory is a starting-point reference, not a finished helm
chart. The pieces are small and editable.

## Pieces

| File | Role |
| --- | --- |
| `entrypoint.sh` | Baked into your billet image; runs the scripted profile series, writes per-profile JSON + audit metadata, archives /output. |
| `billet-job.yaml` | Job template: block-mode PVC at `/dev/billet-target`, downward API env (job/pod/namespace/node), prometheus annotations, emptyDir output. |
| `collect.sh` | `kubectl cp` the archive out of a completed Job's pod; resolve PV from PVC via the cluster API. |
| `compare-collected.sh` | Drive `tools/compare.py` over collected results to emit comparison HTML per profile. |

## PVC requirement: `volumeMode: Block`

billet needs raw block access. A filesystem-mode PVC won't work --
you'd be running billet against a file inside a mounted directory,
which puts ext4/xfs in the I/O path and breaks O_DIRECT guarantees.
Confirm each test PVC is created with `volumeMode: Block` before
launching the Job. Most modern CSI drivers (md, LVM, local-pvc,
candidate userspace block targets) support block mode; check your
storage class's `volumeBindingMode` and CSI capabilities if in doubt.

## Image build

The reference build lives in `.jenkins/Dockerfile` + `.jenkins/Jenkinsfile`
at the repo root. The Jenkinsfile:

1. Runs `conan create` for each build matrix cell.
2. Stages the billet binary, dump_syms-generated sym file, and
   `example/k8s/entrypoint.sh` (as `billet-runner.sh`) into `.jenkins/`.
3. Builds the Docker image from `.jenkins/Dockerfile`.
4. Pushes the tagged image to the org's docker registry.

The image's behavior follows the org's standard pattern: the base
image's entrypoint dispatches to `$BILLET_START_CMD`, which defaults
to `/usr/local/bin/billet-runner.sh`. The runner script reads scenario
config (DEVICE_LABEL, TARGET_PVC, DURATION_S, ...) from env supplied
by the Job manifest.

To change what the runner does (different profiles, durations, qd
sweep, etc.), edit `example/k8s/entrypoint.sh` and rebuild the image.
A ConfigMap-mounted variant is straightforward later if iteration
speed matters; the script reads its parameters from env so the Job
manifest already controls everything except the *list* of profile
invocations.

For ad-hoc builds outside Jenkins, copy `example/k8s/entrypoint.sh`
to `.jenkins/billet-runner.sh`, place the built `billet` binary
alongside, then `docker build .jenkins/` with the matching build
args.

## Prometheus scrape

The Job pod has annotation-style scrape hints by default. Adjust
for your scraper:

| Scraper | Adjustment |
| --- | --- |
| Vanilla Prometheus or kube-prometheus-stack with annotation-based discovery | Keep the `prometheus.io/*` annotations as shipped. |
| Elastic Metricbeat with autodiscover | Comment out `prometheus.io/*`, uncomment the `co.elastic.metrics/*` block in `billet-job.yaml`. |
| prometheus-operator (`ServiceMonitor`) | Create a separate `Service` for port 9777 and a `ServiceMonitor` selecting `app: billet`; the annotations on the pod are harmless when ignored. |

The Job pod stays alive for `--duration` + `--metrics-drain-s`
(default 120 + 30 = 150 s). With a 30 s scrape interval that's
five tick windows; terminal counters land in at least one of them.

## Dashboard import

The dashboard JSON at `example/grafana/grafana/dashboards/billet.json`
imports cleanly into Grafana. Adjust the datasource UID to match the
prometheus datasource in your cluster's Grafana, then save. The
dashboard's `Run` variable is multi-select; pick the entity labels
of the runs you want to overlay.

## Running a scenario

Copy `billet-job.yaml` to a per-scenario filename, edit the four
scenario-specific fields (`metadata.name`, label `scenario`,
`env.DEVICE_LABEL`, `env.TARGET_PVC`, and the PVC `claimName`),
then:

```sh
kubectl apply -f billet-md-raid0.yaml

# Wait for the Job to finish.
kubectl wait --for=condition=complete --timeout=20m job/billet-md-raid0

# Collect the archive + augment metadata with PV name.
./collect.sh billet-md-raid0 ./collected/md-raid0

# Repeat for each scenario you care about (baseline, md-raid0,
# md-raid1, candidate-driver-raid0, candidate-driver-raid1, etc.).
# Use a distinct DEVICE_LABEL + Job name per scenario.

# Once all are collected, compare.
./compare-collected.sh ./collected
xdg-open ./collected/compare-pg.html
```

## Audit metadata

Each run lands a `metadata.json` next to its profile JSONs. Captured
in-pod (downward API + manifest env + pod-visible host facts):

- `device_label`, `device_path`
- `job_name`, `pod_name`, `pod_namespace`, `node_name`
- `pvc_name`
- `duration_s`, `qd`, `metrics_port`, `metrics_drain_s`
- `started_at` (ISO 8601) + `start_epoch_s` (integer, for sorting)
- `completed_at` (ISO 8601) + `end_epoch_s` (integer)
- `status` (`ok` / `partial` / `running`)
- `kernel_version` (`uname -r` from the pod's view)
- `cpu_model` (from `/proc/cpuinfo`; empty on archs that don't
  populate "model name")
- `cpu_count_visible_to_pod` (`nproc`; respects cgroup `cpu.max` on
  modern kernels so it reflects the pod's effective parallelism, not
  the host's total core count)
- `scenarios_attempted`

Augmented by `collect.sh` post-run:

- `pv_name` -- resolved from PVC via `kubectl get pvc`.

`billet --version` is captured to `billet-version.txt` alongside.
Both files are inside the `results.tar.gz` that `collect.sh`
extracts.

## Output collection model

Each Job's pod uses an `emptyDir` mounted at `/output`. The
entrypoint writes its files there, then archives them to
`results.tar.gz` for one-shot `kubectl cp`. The pod stays in
`Completed` state until the Job's `ttlSecondsAfterFinished` (default
24 h) elapses, so collection can happen any time within that window
from a workstation with kubeconfig + cluster-API access.

If your environment captures pod stdout (e.g., Elastic / Beats /
Sherlock), the entrypoint also emits the metadata JSON blob to
stdout, so audit metadata survives even if `kubectl cp` isn't part of
the workflow.
