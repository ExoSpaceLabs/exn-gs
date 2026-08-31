#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"

SIM_BIN="${BUILD_DIR}/sim/stm32_sim"
DAEMON_BIN="${BUILD_DIR}/daemon/exn_gsd"
UI_BIN="${BUILD_DIR}/ui/exn_gsui"

SIM_PORT=9000
IPC_PORT=7777

log() { printf '[quick-test] %s\n' "$*"; }
die() { printf '[quick-test] ERROR: %s\n' "$*" >&2; exit 1; }

port_listening() {
  local port="$1"
  if command -v ss >/dev/null 2>&1; then
    ss -ltnH | awk '{print $4}' | grep -Eq ":${port}$"
  else
    (exec 3<>"/dev/tcp/127.0.0.1/${port}") >/dev/null 2>&1
  fi
}

wait_for_port() {
  local name="$1" port="$2"
  for _ in {1..100}; do
    if port_listening "$port"; then
      log "${name} ready on 127.0.0.1:${port}"
      return 0
    fi
    sleep 0.1
  done
  die "${name} did not start listening on port ${port}"
}

choose_terminal() {
  local candidate
  for candidate in gnome-terminal konsole kitty xterm; do
    if command -v "$candidate" >/dev/null 2>&1; then
      printf '%s' "$candidate"
      return 0
    fi
  done
  return 1
}

make_terminal_command() {
  local title="$1"
  shift
  local qroot qcmd
  printf -v qroot '%q' "$ROOT_DIR"
  printf -v qcmd '%q ' "$@"
  printf 'cd %s; echo "== %s =="; %s; rc=$?; echo; echo "Process exited with code $rc"; exec bash' \
    "$qroot" "$title" "$qcmd"
}

open_terminal() {
  local terminal="$1" title="$2"
  shift 2
  local cmd
  cmd="$(make_terminal_command "$title" "$@")"

  case "$terminal" in
    gnome-terminal) gnome-terminal --title="$title" -- bash -lc "$cmd" >/dev/null 2>&1 & ;;
    konsole)        konsole --new-tab -p "tabtitle=${title}" -e bash -lc "$cmd" >/dev/null 2>&1 & ;;
    kitty)          kitty --title "$title" bash -lc "$cmd" >/dev/null 2>&1 & ;;
    xterm)          xterm -T "$title" -e bash -lc "$cmd" >/dev/null 2>&1 & ;;
    *) die "unsupported terminal: ${terminal}" ;;
  esac
}

if [[ ! -x "$SIM_BIN" || ! -x "$DAEMON_BIN" || ! -x "$UI_BIN" ]]; then
  log "build artifacts missing; configuring ${BUILD_TYPE} build"
  cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
  cmake --build "$BUILD_DIR" --parallel
fi

[[ -x "$SIM_BIN" ]] || die "missing simulator binary: ${SIM_BIN}"
[[ -x "$DAEMON_BIN" ]] || die "missing daemon binary: ${DAEMON_BIN}"
[[ -x "$UI_BIN" ]] || die "missing UI binary: ${UI_BIN}"

port_listening "$SIM_PORT" && die "port ${SIM_PORT} is already in use"
port_listening "$IPC_PORT" && die "port ${IPC_PORT} is already in use"

TERMINAL="${TERMINAL:-$(choose_terminal || true)}"
[[ -n "$TERMINAL" ]] || die "no supported terminal found (gnome-terminal, konsole, kitty, xterm)"
log "using terminal: ${TERMINAL}"

log "starting OBC simulator"
open_terminal "$TERMINAL" "EXN OBC Simulator" "$SIM_BIN" --verbose
wait_for_port "OBC simulator" "$SIM_PORT"

log "starting GS daemon"
open_terminal "$TERMINAL" "EXN GS Daemon" "$DAEMON_BIN" \
  --listen "127.0.0.1:${IPC_PORT}" \
  --port "tcp://127.0.0.1:${SIM_PORT}" \
  --logdir "${ROOT_DIR}/logs" \
  --verbose
wait_for_port "GS daemon" "$IPC_PORT"

log "starting GS UI"
open_terminal "$TERMINAL" "EXN GS UI" "$UI_BIN" --connect "127.0.0.1:${IPC_PORT}"

log "started simulator -> daemon -> UI in separate terminals"
