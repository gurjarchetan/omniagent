#!/usr/bin/env bash
# =============================================================================
#  OmniAgent deploy.sh — build, install, and manage the OmniAgent daemon
#
#  USAGE
#    sudo ./deploy.sh <command> [options]
#
#  COMMANDS
#    start        Build, install, and start OmniAgent (idempotent)
#    stop         Stop the running daemon
#    restart      Restart the daemon
#    status       Show current service status and config
#    logs         Follow live logs  (Ctrl+C to exit)
#    uninstall    Stop daemon, remove binary + service + config
#    help         Show this help message
#
#  START OPTIONS
#    -L <host>     Loki push host               (default: localhost)
#    -Q <port>     Loki push port               (default: 3100)
#    -m <port>     Prometheus /metrics port     (default: 9100)
#    -l <dirs>     Extra log dirs (colon-separated) watched via inotify
#    -i <secs>     procfs scrape interval       (default: 5)
#    -d            Enable debug logging
#    --no-service  Build + install binary only, skip systemd
#
#  EXAMPLES
#    sudo ./deploy.sh start
#    sudo ./deploy.sh start -L loki.internal -Q 3100 -m 9100
#    sudo ./deploy.sh logs
#    sudo ./deploy.sh stop
#    sudo ./deploy.sh restart
#    sudo ./deploy.sh status
#    sudo ./deploy.sh uninstall
#
#  VISUALIZATION
#    cd viz/ && docker compose up -d
#    # Open http://localhost:3000  (admin / admin)
#
#  TESTED ON
#    Ubuntu 20.04+, Debian 11+, CentOS/RHEL 8+, Amazon Linux 2023,
#    Fedora 38+, Arch Linux, Alpine Linux
# =============================================================================

set -euo pipefail

# ── Constants ─────────────────────────────────────────────────────────────────
INSTALL_DIR="/usr/local/bin"
SERVICE_NAME="omniagent"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"
CONF_FILE="/etc/omniagent.conf"
SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── Colours (disabled when not a tty) ────────────────────────────────────────
if [[ -t 1 ]]; then
  R='\033[0;31m' G='\033[0;32m' Y='\033[1;33m'
  B='\033[0;34m' C='\033[0;36m' BOLD='\033[1m' DIM='\033[2m' X='\033[0m'
else
  R='' G='' Y='' B='' C='' BOLD='' DIM='' X=''
fi

# ── Logging helpers ───────────────────────────────────────────────────────────
info()    { echo -e "  ${G}✓${X}  $*"; }
warn()    { echo -e "  ${Y}⚠${X}  $*"; }
error()   { echo -e "  ${R}✗${X}  $*" >&2; }
step()    { echo -e "\n${BOLD}${C}▶${X}${BOLD} $*${X}"; }
divider() { echo -e "${DIM}────────────────────────────────────────────────────────────${X}"; }

banner() {
  echo -e "\n${BOLD}${B}"
  echo "   ┌──────────────────────────────────────────────────┐"
  echo "   │   OmniAgent  •  deploy.sh                        │"
  echo "   │   Ultra-lightweight observability daemon          │"
  echo "   │   Pure C11  •  72 KB binary  •  ~6.6 MB RSS      │"
  echo "   └──────────────────────────────────────────────────┘"
  echo -e "${X}"
}

# ── Guards ────────────────────────────────────────────────────────────────────
require_root() {
  if [[ $EUID -ne 0 ]]; then
    error "Must be run as root.  Try:  sudo ./deploy.sh $*"
    exit 1
  fi
}

require_systemd() {
  if ! command -v systemctl &>/dev/null; then
    error "systemd not available. Start OmniAgent manually:"
    error "  ${INSTALL_DIR}/omniagent $(build_exec_args)"
    exit 1
  fi
}

require_installed() {
  if [[ ! -f "$SERVICE_FILE" ]]; then
    error "OmniAgent not installed. Run:  sudo ./deploy.sh start"
    exit 1
  fi
}

# ── Distro detection ──────────────────────────────────────────────────────────
detect_distro() {
  [[ -f /etc/os-release ]] || { error "/etc/os-release not found."; exit 1; }
  # shellcheck disable=SC1091
  source /etc/os-release
  DISTRO="${ID:-unknown}"
  DISTRO_VERSION="${VERSION_ID:-0}"
  info "Distro: ${PRETTY_NAME:-$DISTRO}"
}

# ── Build dependencies ────────────────────────────────────────────────────────
install_build_deps() {
  step "Installing build dependencies"
  local pm
  case "${DISTRO:-}" in
    ubuntu|debian|raspbian|linuxmint|pop|kali) pm="apt" ;;
    centos|rhel|rocky|almalinux|ol)
      pm="dnf"; [[ "${DISTRO_VERSION%%.*}" -le 7 ]] 2>/dev/null && pm="yum" ;;
    fedora)                    pm="dnf" ;;
    amzn) pm="dnf"; [[ "${DISTRO_VERSION:-}" == "2" ]] && pm="yum" ;;
    arch|manjaro|endeavouros)  pm="pacman" ;;
    opensuse*|sles)            pm="zypper" ;;
    alpine)                    pm="apk" ;;
    *) warn "Unknown distro '${DISTRO:-}' — trying apt."; pm="apt" ;;
  esac

  case "$pm" in
    apt)
      apt-get update -qq
      apt-get install -y --no-install-recommends build-essential libzstd-dev ;;
    dnf)    dnf install -y gcc make libzstd-devel ;;
    yum)
      yum install -y gcc make
      rpm -q libzstd-devel &>/dev/null || {
        rpm -q epel-release &>/dev/null || yum install -y epel-release
        yum install -y libzstd-devel
      } ;;
    pacman) pacman -Sy --noconfirm gcc make zstd ;;
    zypper) zypper install -y gcc make libzstd-devel ;;
    apk)    apk add --no-cache build-base zstd-dev ;;
  esac

  for bin in gcc make; do
    command -v "$bin" &>/dev/null || { error "$bin not found after install."; exit 1; }
  done
  info "gcc $(gcc -dumpversion)  •  make $(make --version | head -1 | awk '{print $3}')"
}

# ── Build binary ──────────────────────────────────────────────────────────────
build_binary() {
  step "Building OmniAgent"
  if [[ -f "${SOURCE_DIR}/Makefile" ]]; then
    info "Source: ${SOURCE_DIR}"
    pushd "$SOURCE_DIR" >/dev/null
      make clean >/dev/null 2>&1 || true
      make -j"$(nproc)"
    popd >/dev/null
    BINARY="${SOURCE_DIR}/omniagent"
  else
    BINARY="${SOURCE_DIR}/omniagent"
    [[ -f "$BINARY" ]] || {
      error "No Makefile and no binary found next to deploy.sh."
      exit 1
    }
    warn "No Makefile — using pre-built binary."
  fi
  [[ -x "$BINARY" ]] || { error "Binary not executable: $BINARY"; exit 1; }
  info "Size: $(du -sh "$BINARY" | cut -f1)"
}

# ── Install binary ────────────────────────────────────────────────────────────
install_binary() {
  step "Installing binary to ${INSTALL_DIR}/omniagent"
  cp -f "$BINARY" "${INSTALL_DIR}/omniagent"
  chmod 755 "${INSTALL_DIR}/omniagent"
  strip "${INSTALL_DIR}/omniagent" 2>/dev/null || true
  info "Installed: ${INSTALL_DIR}/omniagent"
}

# ── Config ────────────────────────────────────────────────────────────────────
write_conf() {
  step "Writing config  →  ${CONF_FILE}"
  {
    echo "# OmniAgent config — generated $(date -u '+%Y-%m-%d %H:%M UTC')"
    echo "LOKI_HOST=${LOKI_HOST}"
    echo "LOKI_PORT=${LOKI_PORT}"
    echo "METRICS_PORT=${METRICS_PORT}"
    echo "EXTRA_LOG_DIRS=${EXTRA_LOG_DIRS}"
    echo "SCRAPE_INTERVAL=${SCRAPE_INTERVAL}"
    echo "DEBUG=${DEBUG_FLAG}"
  } > "$CONF_FILE"
  chmod 600 "$CONF_FILE"
  info "Config written"
}

load_conf() {
  [[ -f "$CONF_FILE" ]] || return 0
  while IFS='=' read -r key val; do
    [[ "$key" =~ ^# || -z "${key// }" ]] && continue
    case "$key" in
      LOKI_HOST)       LOKI_HOST="${val}" ;;
      LOKI_PORT)       LOKI_PORT="${val}" ;;
      METRICS_PORT)    METRICS_PORT="${val}" ;;
      EXTRA_LOG_DIRS)  EXTRA_LOG_DIRS="${val}" ;;
      SCRAPE_INTERVAL) SCRAPE_INTERVAL="${val}" ;;
      DEBUG)           DEBUG_FLAG="${val}" ;;
    esac
  done < "$CONF_FILE"
}

build_exec_args() {
  local a="-L ${LOKI_HOST} -Q ${LOKI_PORT} -m ${METRICS_PORT} -i ${SCRAPE_INTERVAL}"
  [[ -n "${EXTRA_LOG_DIRS:-}" ]] && a="${a} -l ${EXTRA_LOG_DIRS}"
  [[ -n "${DEBUG_FLAG:-}"     ]] && a="${a} ${DEBUG_FLAG}"
  echo "$a"
}

# ── Install systemd service ───────────────────────────────────────────────────
install_service() {
  local exec_args; exec_args="$(build_exec_args)"
  step "Installing systemd service"

  cat > "$SERVICE_FILE" <<UNIT
[Unit]
Description=OmniAgent observability daemon
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=${INSTALL_DIR}/omniagent ${exec_args}
Restart=on-failure
RestartSec=5s
User=root
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=read-only
ReadWritePaths=/tmp
StandardOutput=journal
StandardError=journal
SyslogIdentifier=omniagent

[Install]
WantedBy=multi-user.target
UNIT

  systemctl daemon-reload
  systemctl enable "$SERVICE_NAME"
  systemctl restart "$SERVICE_NAME"
  sleep 2
}

# =============================================================================
#  COMMANDS
# =============================================================================

# ── start ──────────────────────────────────────────────────────────────────────
cmd_start() {
  LOKI_HOST="localhost"; LOKI_PORT="3100"; METRICS_PORT="9100"
  EXTRA_LOG_DIRS=""; SCRAPE_INTERVAL="5"
  DEBUG_FLAG=""; NO_SERVICE=0

  load_conf   # seed from existing config; CLI flags override below

  while [[ $# -gt 0 ]]; do
    case "$1" in
      -L) LOKI_HOST="$2";        shift 2 ;;
      -Q) LOKI_PORT="$2";        shift 2 ;;
      -m) METRICS_PORT="$2";     shift 2 ;;
      -l) EXTRA_LOG_DIRS="$2";   shift 2 ;;
      -i) SCRAPE_INTERVAL="$2";  shift 2 ;;
      -d) DEBUG_FLAG="-d";       shift ;;
      --no-service) NO_SERVICE=1; shift ;;
      *) error "Unknown option: $1"; echo ""; cmd_help; exit 1 ;;
    esac
  done

  require_root start
  banner
  detect_distro
  install_build_deps
  build_binary
  install_binary
  write_conf

  if [[ $NO_SERVICE -eq 1 ]]; then
    divider
    warn "--no-service: systemd setup skipped."
    info "Run manually:  ${INSTALL_DIR}/omniagent $(build_exec_args)"
    echo ""
    exit 0
  fi

  require_systemd
  install_service

  if systemctl is-active --quiet "$SERVICE_NAME"; then
    local pid; pid=$(systemctl show -p MainPID --value "$SERVICE_NAME" 2>/dev/null || echo "?")
    echo ""
    divider
    echo -e "  ${BOLD}${G}✓  OmniAgent is running!${X}"
    divider
    printf "  ${BOLD}%-14s${X} %s\n" "Metrics"   ":${METRICS_PORT}/metrics"
    printf "  ${BOLD}%-14s${X} %s\n" "Loki"      "${LOKI_HOST}:${LOKI_PORT}"
    printf "  ${BOLD}%-14s${X} %s\n" "Binary"    "${INSTALL_DIR}/omniagent"
    printf "  ${BOLD}%-14s${X} %s\n" "Config"    "${CONF_FILE}"
    printf "  ${BOLD}%-14s${X} %s\n" "Service"   "${SERVICE_NAME}.service (enabled at boot)"
    printf "  ${BOLD}%-14s${X} %s\n" "PID"       "${pid}"
    divider
    echo ""
    echo -e "  ${DIM}Next steps:${X}"
    printf "    ${C}%-34s${X} %s\n" "sudo ./deploy.sh logs"             "— follow live logs"
    printf "    ${C}%-34s${X} %s\n" "sudo ./deploy.sh status"           "— detailed status"
    printf "    ${C}%-34s${X} %s\n" "sudo ./deploy.sh stop"             "— stop daemon"
    printf "    ${C}%-34s${X} %s\n" "sudo ./deploy.sh restart"          "— restart daemon"
    printf "    ${C}%-34s${X} %s\n" "cd viz/ && docker compose up -d"   "— Grafana on :3000"
    echo ""
  else
    error "Service failed to start."
    echo ""
    systemctl status "$SERVICE_NAME" --no-pager -l || true
    echo ""
    error "Full logs:  sudo ./deploy.sh logs"
    exit 1
  fi
}

# ── stop ───────────────────────────────────────────────────────────────────────
cmd_stop() {
  require_root stop
  require_systemd
  require_installed

  step "Stopping OmniAgent"
  if systemctl is-active --quiet "$SERVICE_NAME"; then
    systemctl stop "$SERVICE_NAME"
    info "Daemon stopped."
  else
    warn "OmniAgent was not running."
  fi
  systemctl disable "$SERVICE_NAME" 2>/dev/null || true
  info "Auto-start at boot: disabled."
  echo ""
  echo -e "  ${DIM}To start again:  sudo ./deploy.sh start${X}"
  echo ""
}

# ── restart ────────────────────────────────────────────────────────────────────
cmd_restart() {
  require_root restart
  require_systemd
  require_installed

  step "Restarting OmniAgent"
  systemctl daemon-reload
  systemctl restart "$SERVICE_NAME"
  sleep 1
  if systemctl is-active --quiet "$SERVICE_NAME"; then
    info "Restarted successfully."
    echo ""
    systemctl status "$SERVICE_NAME" --no-pager -l | head -20 || true
    echo ""
  else
    error "Restart failed."
    systemctl status "$SERVICE_NAME" --no-pager -l || true
    exit 1
  fi
}

# ── status ────────────────────────────────────────────────────────────────────
cmd_status() {
  require_systemd
  if [[ ! -f "$SERVICE_FILE" ]]; then
    echo -e "\n  ${Y}⚠${X}  OmniAgent not installed. Run:  sudo ./deploy.sh start\n"
    exit 0
  fi

  echo ""
  divider
  echo -e "  ${BOLD}Service status${X}"
  divider
  systemctl status "$SERVICE_NAME" --no-pager -l || true
  divider

  if [[ -f "$CONF_FILE" ]]; then
    echo ""
    echo -e "  ${BOLD}Configuration${X}  (${CONF_FILE})"
    divider
    while IFS='=' read -r key val; do
      [[ "$key" =~ ^# || -z "${key// }" ]] && continue
      [[ "$key" == "AUTH_TOKEN" && -n "$val" ]] && val="${val:0:6}…[masked]"
      printf "  ${C}%-20s${X} %s\n" "$key" "$val"
    done < "$CONF_FILE"
    divider
  fi
  echo ""
}

# ── logs ──────────────────────────────────────────────────────────────────────
cmd_logs() {
  require_systemd
  require_installed
  echo ""
  echo -e "  ${DIM}Following OmniAgent logs — press Ctrl+C to stop${X}"
  echo ""
  exec journalctl -u "$SERVICE_NAME" -f --no-pager "$@"
}

# ── uninstall ─────────────────────────────────────────────────────────────────
cmd_uninstall() {
  require_root uninstall
  banner
  echo -e "  ${BOLD}${R}This will permanently remove:${X}"
  echo -e "    • ${INSTALL_DIR}/omniagent"
  echo -e "    • ${SERVICE_FILE}"
  echo -e "    • ${CONF_FILE}"
  echo ""
  read -r -p "  Continue? [y/N] " confirm
  [[ "$confirm" =~ ^[Yy]$ ]] || { echo -e "\n  Aborted.\n"; exit 0; }
  echo ""

  step "Uninstalling OmniAgent"
  if command -v systemctl &>/dev/null; then
    systemctl is-active  --quiet "$SERVICE_NAME" 2>/dev/null && \
      { systemctl stop    "$SERVICE_NAME"; info "Service stopped."; } || true
    systemctl is-enabled --quiet "$SERVICE_NAME" 2>/dev/null && \
      { systemctl disable "$SERVICE_NAME"; info "Service disabled."; } || true
    [[ -f "$SERVICE_FILE" ]] && \
      { rm -f "$SERVICE_FILE"; systemctl daemon-reload; info "Unit file removed."; }
  fi
  [[ -f "${INSTALL_DIR}/omniagent" ]] && { rm -f "${INSTALL_DIR}/omniagent"; info "Binary removed."; }
  [[ -f "$CONF_FILE"               ]] && { rm -f "$CONF_FILE";               info "Config removed."; }
  echo ""
  info "OmniAgent fully uninstalled."
  echo ""
}

# ── help ──────────────────────────────────────────────────────────────────────
cmd_help() {
  banner
  divider
  echo -e "  ${BOLD}USAGE${X}"
  echo -e "    sudo ./deploy.sh ${C}<command>${X} [options]"
  echo ""
  echo -e "  ${BOLD}COMMANDS${X}"
  printf "    ${C}%-14s${X} %s\n" "start"     "Build, install, and start OmniAgent (idempotent)"
  printf "    ${C}%-14s${X} %s\n" "stop"      "Stop daemon and disable auto-start at boot"
  printf "    ${C}%-14s${X} %s\n" "restart"   "Restart the daemon"
  printf "    ${C}%-14s${X} %s\n" "status"    "Service status + current config"
  printf "    ${C}%-14s${X} %s\n" "logs"      "Follow live logs  (Ctrl+C to stop)"
  printf "    ${C}%-14s${X} %s\n" "uninstall" "Remove binary, service, and config"
  printf "    ${C}%-14s${X} %s\n" "help"      "Show this message"
  echo ""
  divider
  echo -e "  ${BOLD}START OPTIONS${X}"
  printf "    ${Y}%-18s${X} %s\n" "-L <host>"    "Loki push host             (default: localhost)"
  printf "    ${Y}%-18s${X} %s\n" "-Q <port>"    "Loki push port             (default: 3100)"
  printf "    ${Y}%-18s${X} %s\n" "-m <port>"    "Prometheus /metrics port   (default: 9100)"
  printf "    ${Y}%-18s${X} %s\n" "-l <dirs>"    "Extra log dirs (colon-separated)"
  printf "    ${Y}%-18s${X} %s\n" "-i <secs>"    "procfs scrape interval     (default: 5)"
  printf "    ${Y}%-18s${X} %s\n" "-d"           "Enable debug logging"
  printf "    ${Y}%-18s${X} %s\n" "--no-service" "Binary only, skip systemd"
  echo ""
  divider
  echo -e "  ${BOLD}EXAMPLES${X}"
  echo -e "    ${DIM}# Default (local OTel Collector)${X}"
  echo -e "    sudo ./deploy.sh start"
  echo ""
  echo -e "    ${DIM}# Grafana Cloud${X}"
  echo -e "    sudo ./deploy.sh start -e otlp-gateway-prod-us-central-0.grafana.net \\"
  echo -e "                           -p 443 -t \"<instance_id>:<api_token>\""
  echo ""
  echo -e "    ${DIM}# Datadog OTLP ingest${X}"
  echo -e "    sudo ./deploy.sh start -e <dd-agent-host> -p 4318 -t \"DD-API-KEY <key>\""
  echo ""
  echo -e "    ${DIM}# Follow logs after starting${X}"
  echo -e "    sudo ./deploy.sh logs"
  echo ""
  echo -e "    ${DIM}# Self-hosted Grafana visualization stack${X}"
  echo -e "    cd viz/ && docker compose up -d"
  echo -e "    ${DIM}Open http://localhost:3000  (admin / admin)${X}"
  echo ""
  divider
  echo ""
}

# =============================================================================
#  Dispatch
# =============================================================================
CMD="${1:-help}"; shift || true
case "$CMD" in
  start)          cmd_start     "$@" ;;
  stop)           cmd_stop      "$@" ;;
  restart)        cmd_restart   "$@" ;;
  status)         cmd_status    "$@" ;;
  logs)           cmd_logs      "$@" ;;
  uninstall)      cmd_uninstall "$@" ;;
  help|--help|-h) cmd_help ;;
  *)
    error "Unknown command: '${CMD}'"
    echo ""
    cmd_help
    exit 1
    ;;
esac
