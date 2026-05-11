# ##########   #######   ############
FROM hub.tess.io/sds/sds_base:8.x-latest
LABEL description="billet: block-device benchmark with parametric app-shaped workload profiles"

ARG SislVer=unknown
ARG LiburingVer=unknown
ARG HdrHistogramVer=unknown
ARG BilletCommitId=unknown

LABEL SislVer=$SislVer
LABEL LiburingVer=$LiburingVer
LABEL HdrHistogramVer=$HdrHistogramVer
LABEL BilletCommitId=$BilletCommitId

# Base image's entrypoint dispatches to $BILLET_START_CMD. The
# runner script reads the scenario config (DEVICE_LABEL, TARGET_PVC,
# DURATION_S, etc.) from env supplied by the k8s Job manifest.
ENV BILLET_START_CMD "/usr/local/bin/billet-runner.sh"
ENV LOG_PATH "/logs/billet.log"

RUN set -eux; \
    apt-get update; \
    apt-get upgrade -y; \
    apt-get install -y --no-install-recommends \
      iotop \
      libasan6 \
      libubsan1 \
      strace \
      tmux \
    ; \
    rm -rf /var/lib/apt/lists/*; \
    mkdir /logs; \
    mkdir /output;

COPY billet /usr/local/bin/
COPY billet-runner.sh /usr/local/bin/

RUN chmod +x /usr/local/bin/billet

EXPOSE 9777
# ##########   #######   ############
