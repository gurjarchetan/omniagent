# OmniAgent — multi-stage Docker image
#
# The container needs these host access flags at runtime:
#   --pid host                              (read /proc/<PID> for process metrics)
#   --network host                          (accurate NIC stats + push to Loki)
#   -v /var/run/docker.sock:/var/run/docker.sock   (Docker container metrics)
#   -v /var/log:/var/log:ro                 (inotify log watching)
#   -v /sys/fs/cgroup:/sys/fs/cgroup:ro    (optional cgroup data)
#
# Usage (standalone):
#   docker build -t omniagent .
#   docker run -d --name omniagent \
#     --pid host --network host \
#     -v /var/run/docker.sock:/var/run/docker.sock \
#     -v /var/log:/var/log:ro \
#     omniagent -L localhost -Q 3100 -m 9100 -i 15

# ── Stage 1: Build ────────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS builder

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential \
      gcc \
      make \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy sources
COPY Makefile ./
COPY include/  include/
COPY src/      src/

# Build optimised binary.
# -march=native is fine here: Docker on Linux uses the host CPU directly,
# so the binary will run on the same machine it was built on.
RUN mkdir -p build && make -j"$(nproc)"

# ── Stage 2: Runtime ──────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS runtime

# The binary is pure C and only links libc — nothing extra to install.
COPY --from=builder /src/omniagent /usr/local/bin/omniagent

# Non-root user for reduced attack surface.
# We still need to be in the "docker" group (or map GID) for the socket.
RUN groupadd --gid 999 omniagent \
 && useradd  --uid 999 --gid 999 --no-create-home --shell /sbin/nologin omniagent

# Run as root so we can read /proc/<pid>/... for all processes and
# connect to /var/run/docker.sock. Override with --user if your
# docker group GID is mapped correctly.
USER root

EXPOSE 9100

ENTRYPOINT ["/usr/local/bin/omniagent"]

# Defaults: push logs to Loki on localhost:3100, expose metrics on :9100
CMD ["-L", "localhost", "-Q", "3100", "-m", "9100", "-i", "15"]
