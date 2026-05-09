# Grafana Dashboard

billet: function generator and oscilloscope for block devices.

Live dashboard for a running billet process. Brings up Prometheus
(scraping billet's `/metrics` endpoint) and Grafana (with the billet
dashboard auto-provisioned). billet itself runs on the host; Prometheus
reaches it via the `host.docker.internal` extra_host alias.

## Bring Up the Stack

```sh
cd example/grafana
docker compose up -d
```

Grafana is at <http://localhost:3030> (admin / admin). Prometheus is at
<http://localhost:9091>. Prometheus is configured to scrape
`host.docker.internal:9777`, which is where billet exposes `/metrics`
when run with `--metrics-port 9777`.

## Run billet With Metrics On

From the repo root (not from `example/grafana/`):

```sh
sudo build/Release/src/cli/billet \
    --device /dev/<your-disk> --profile postgresql \
    --workers 1 --qd 32 --duration 600 \
    --metrics-port 9777 --allow-destructive
```

`--metrics-drain-s` (default 30) keeps the endpoint up after the run so
a final scrape catches terminal counters before tear-down. The dashboard
uses short rate windows so 2-minute smoke runs still show checkpoint and
WAL detail instead of flattening everything into one smooth line.

## Comparing Runs

Add `--device-label <name>` to prefix the run's metric `entity` label, so
Grafana legends read `<name>.<ulid>` instead of just the ULID. Prometheus
retains 24 h by default, so several sequential runs all live in the same
history; the dashboard's `Run` variable is multi-select, so picking 2-5
labelled runs overlays them per panel for side-by-side comparison.

```sh
# Three scenarios, sequential. Drain is short so the next run starts
# without too much idle time between scrapes.
sudo build/Release/src/cli/billet --device /dev/nvme1n1 \
    --device-label bare-nvme --profile postgresql --duration 120 \
    --metrics-port 9777 --metrics-drain-s 5 --allow-destructive \
    --output bare-nvme.json

sudo build/Release/src/cli/billet --device /dev/md0 \
    --device-label raid0-md --profile postgresql --duration 120 \
    --metrics-port 9777 --metrics-drain-s 5 --allow-destructive \
    --output raid0-md.json

sudo build/Release/src/cli/billet --device /dev/<candidate-disk> \
    --device-label raid0-candidate --profile postgresql --duration 120 \
    --metrics-port 9777 --metrics-drain-s 5 --allow-destructive \
    --output raid0-candidate.json
```

In Grafana, open the dashboard, click the `Run` dropdown, tick the three
entries. Every timeseries panel now plots one series per entity (per
cell, per percentile, etc.); each series legend starts with the label.

## Tear It Down

```sh
docker compose down
```

Use `docker compose down -v` to wipe Prometheus data.

## Files

- `docker-compose.yml` -- Prometheus + Grafana, pinned versions
- `prometheus.yml` -- scrape config for `host.docker.internal:9777`
- `grafana/provisioning/datasources/prometheus.yml` -- Prometheus datasource
- `grafana/provisioning/dashboards/dashboards.yml` -- dashboard provider
- `grafana/dashboards/billet.json` -- provisioned dashboard JSON
