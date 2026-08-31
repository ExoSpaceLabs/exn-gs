#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:?usage: hil_smoke.sh BUILD_DIR}"
SIM_BIN="${BUILD_DIR}/sim/stm32_sim"
DAEMON_BIN="${BUILD_DIR}/daemon/exn_gsd"
CTL_BIN="${BUILD_DIR}/tools/gsdctl/exn_gsdctl"
IPC_PORT=17777
SIM_PORT=9000
TMP_DIR="$(mktemp -d)"
SIM_PID=""
DAEMON_PID=""

cleanup() {
  [[ -n "$DAEMON_PID" ]] && kill "$DAEMON_PID" >/dev/null 2>&1 || true
  [[ -n "$SIM_PID" ]] && kill "$SIM_PID" >/dev/null 2>&1 || true
  [[ -n "$DAEMON_PID" ]] && wait "$DAEMON_PID" >/dev/null 2>&1 || true
  [[ -n "$SIM_PID" ]] && wait "$SIM_PID" >/dev/null 2>&1 || true
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

port_listening() {
  local port="$1"
  if command -v ss >/dev/null 2>&1; then
    ss -ltnH | awk '{print $4}' | grep -Eq ":${port}$"
  else
    (exec 3<>"/dev/tcp/127.0.0.1/${port}") >/dev/null 2>&1
  fi
}

wait_for_port() {
  local port="$1"
  for _ in {1..100}; do
    port_listening "$port" && return 0
    sleep 0.1
  done
  return 1
}

wait_for_connected() {
  local output
  for _ in {1..50}; do
    output="$(timeout 3 "$CTL_BIN" --port "$IPC_PORT" status 2>&1 || true)"
    if grep -q "Device link: CONNECTED" <<<"$output"; then
      printf '%s\n' "$output"
      return 0
    fi
    sleep 0.1
  done
  printf '%s\n' "$output" >&2
  return 1
}

"$SIM_BIN" >"$TMP_DIR/sim.log" 2>&1 &
SIM_PID=$!
wait_for_port "$SIM_PORT" || {
  cat "$TMP_DIR/sim.log" >&2
  exit 1
}

"$DAEMON_BIN" \
  --listen "127.0.0.1:${IPC_PORT}" \
  --port "tcp://127.0.0.1:${SIM_PORT}" \
  --logdir "$TMP_DIR/logs" \
  >"$TMP_DIR/daemon.log" 2>&1 &
DAEMON_PID=$!
wait_for_port "$IPC_PORT" || {
  cat "$TMP_DIR/daemon.log" >&2
  exit 1
}

wait_for_connected

timeout 3 "$CTL_BIN" --port "$IPC_PORT" ping | grep -q "PONG"
timeout 3 "$CTL_BIN" --port "$IPC_PORT" stats | grep -q "RX packets="
timeout 5 "$CTL_BIN" --port "$IPC_PORT" reconnect | grep -q "Device link: CONNECTED"
timeout 3 "$CTL_BIN" --port "$IPC_PORT" disconnect | grep -q "Device link: DISCONNECTED"
timeout 5 "$CTL_BIN" --port "$IPC_PORT" connect | grep -q "Device link: CONNECTED"

printf 'EXN-GS HIL smoke test passed\n'
