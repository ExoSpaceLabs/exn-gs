#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"
RUN_TESTS=1
CLEAN=0
JOBS="${JOBS:-}"
GENERATOR="${GENERATOR:-}"

usage() {
  cat <<'EOF'
Usage: ./scripts/build_simulation.sh [options]

Build the complete EXN-GS simulation/HIL stack:
  - stm32_sim     OBC/MCU simulator
  - exn_gsd       transport/router daemon
  - exn_gsui      operator UI
  - exn_gsdctl    daemon CLI/automation client
  - shared libraries and external dependencies (CCSDSPack, HardRT, FTXUI)
  - regression/HIL tests by default

Options:
  --clean               Remove the build directory before configuring.
  --no-tests            Build only the runtime simulation stack.
  --build-dir DIR       Override the build directory.
  --build-type TYPE     CMake build type (default: RelWithDebInfo).
  --jobs N              Parallel build jobs.
  --generator NAME      CMake generator for a new build tree.
  -h, --help            Show this help.

Environment equivalents:
  BUILD_DIR, CMAKE_BUILD_TYPE, JOBS, GENERATOR
EOF
}

log() { printf '[build-sim] %s\n' "$*"; }
die() { printf '[build-sim] ERROR: %s\n' "$*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --clean)
      CLEAN=1
      shift
      ;;
    --no-tests)
      RUN_TESTS=0
      shift
      ;;
    --build-dir)
      [[ $# -ge 2 ]] || die "--build-dir requires a value"
      BUILD_DIR="$2"
      shift 2
      ;;
    --build-type)
      [[ $# -ge 2 ]] || die "--build-type requires a value"
      BUILD_TYPE="$2"
      shift 2
      ;;
    --jobs)
      [[ $# -ge 2 ]] || die "--jobs requires a value"
      JOBS="$2"
      shift 2
      ;;
    --generator)
      [[ $# -ge 2 ]] || die "--generator requires a value"
      GENERATOR="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown option: $1"
      ;;
  esac
done

command -v cmake >/dev/null 2>&1 || die "cmake is required"

if [[ -z "$JOBS" ]]; then
  if command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
  else
    JOBS=2
  fi
fi

if [[ "$CLEAN" -eq 1 ]]; then
  log "removing build tree: ${BUILD_DIR}"
  rm -rf -- "$BUILD_DIR"
fi

configure_args=(
  -S "$ROOT_DIR"
  -B "$BUILD_DIR"
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
  -DBUILD_TESTING="$([[ "$RUN_TESTS" -eq 1 ]] && printf ON || printf OFF)"
)

# Reuse the generator recorded by an existing build tree. For a fresh tree,
# prefer an explicit generator, then Ninja when available, otherwise CMake's default.
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  if [[ -n "$GENERATOR" ]]; then
    configure_args+=( -G "$GENERATOR" )
  elif command -v ninja >/dev/null 2>&1; then
    configure_args+=( -G Ninja )
  fi
fi

log "configuring ${BUILD_TYPE} simulation build in ${BUILD_DIR}"
cmake "${configure_args[@]}"

targets=(stm32_sim exn_gsd exn_gsui exn_gsdctl)
if [[ "$RUN_TESTS" -eq 1 ]]; then
  targets+=(gs_test)
fi

log "building simulation stack (${targets[*]}) with ${JOBS} jobs"
cmake --build "$BUILD_DIR" --parallel "$JOBS" --target "${targets[@]}"

if [[ "$RUN_TESTS" -eq 1 ]]; then
  log "running packet regression and simulator/daemon HIL smoke tests"
  ctest --test-dir "$BUILD_DIR" \
    --output-on-failure \
    -R '^exn_gs_(packet_regression|hil_smoke)$'
fi

cat <<EOF

[build-sim] build complete
  simulator : ${BUILD_DIR}/sim/stm32_sim
  daemon    : ${BUILD_DIR}/daemon/exn_gsd
  UI        : ${BUILD_DIR}/ui/exn_gsui
  CLI       : ${BUILD_DIR}/tools/gsdctl/exn_gsdctl

Launch the stack with:
  ${ROOT_DIR}/scripts/quick_test.sh
EOF
