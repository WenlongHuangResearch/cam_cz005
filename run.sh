#!/usr/bin/env bash
# One-command recorder entrypoint.
# Usage:
#   ./run.sh [seconds] [output_root] [extra stereo_record args...]
#
# Examples:
#   ./run.sh                         # record until Ctrl+C
#   ./run.sh 10                      # record 10 seconds
#   ./run.sh 30 artifacts/test       # record under artifacts/test/
#   ./run.sh 0 "" --bitrate 30000    # pass extra args to stereo_record
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$SCRIPT_DIR/apps/stereo-recorder"
BIN_DIR="$APP_DIR/build/bin"

export MSYSTEM="${MSYSTEM:-MINGW64}"
export PATH="/mingw64/bin:/usr/bin:$PATH"

SECS="${1:-0}"
if [[ $# -gt 0 ]]; then
  shift
fi

ROOT="${1:-$SCRIPT_DIR/artifacts/stereo-recorder/recordings_run}"
if [[ $# -gt 0 ]]; then
  shift
fi
if [[ -z "$ROOT" ]]; then
  ROOT="$SCRIPT_DIR/artifacts/stereo-recorder/recordings_run"
fi

EXEEXT=""
case "$(uname -s 2>/dev/null || true)" in
  MINGW*|MSYS*|CYGWIN*) EXEEXT=".exe" ;;
esac

REC="$BIN_DIR/stereo_record$EXEEXT"
MUX="$BIN_DIR/hevc2mp4$EXEEXT"

has_device_arg() {
  for arg in "$@"; do
    if [[ "$arg" == "--device" ]]; then
      return 0
    fi
  done
  return 1
}

detect_linux_v4l2_device() {
  command -v v4l2-ctl >/dev/null 2>&1 || return 1

  local dev fmt
  while IFS= read -r dev; do
    [[ -n "$dev" ]] || continue
    fmt="$(v4l2-ctl -d "$dev" --list-formats-ext 2>/dev/null || true)"
    if [[ "$fmt" == *"'MJPG'"* && "$fmt" == *"4000x1200"* && "$fmt" == *"60.000 fps"* ]]; then
      printf '%s\n' "$dev"
      return 0
    fi
  done < <(
    v4l2-ctl --list-devices 2>/dev/null |
      awk '
        /DECXIN Camera/ { in_dev = 1; next }
        /^[^[:space:]]/ { in_dev = 0 }
        in_dev && /\/dev\/video[0-9]+/ { print $1 }
      '
  )

  return 1
}

if [[ ! -x "$REC" || ! -x "$MUX" ]]; then
  echo "== Clean build recorder =="
  make -C "$APP_DIR" clean all
else
  echo "== Build recorder =="
  make -C "$APP_DIR" all
fi

TS="$(date +%Y%m%d_%H%M%S)"
DIR="$ROOT/rec_${TS}_4000x1200_60fps"
mkdir -p "$DIR"

echo "== Start recording =="
echo "Output: $DIR"
if [[ "$SECS" == "0" || "$SECS" == "0.0" ]]; then
  echo "Duration: until Ctrl+C"
else
  echo "Duration: ${SECS}s"
fi

REC_ARGS=(--seconds "$SECS" --out "$DIR")
if [[ -z "$EXEEXT" ]] && ! has_device_arg "$@"; then
  if DEV="$(detect_linux_v4l2_device)"; then
    echo "Device: $DEV (auto-detected DECXIN 4000x1200 MJPG@60)"
    REC_ARGS+=(--device "$DEV")
  else
    echo "Device: recorder default (/dev/video0)"
  fi
fi
REC_ARGS+=("$@")

rec_pid=""
interrupted=0
on_int() {
  interrupted=1
  echo
  echo "== Stop recording =="
  if [[ -n "$rec_pid" ]] && kill -0 "$rec_pid" 2>/dev/null; then
    kill -INT "-$rec_pid" 2>/dev/null || kill -INT "$rec_pid" 2>/dev/null || true
  fi
}

trap on_int INT
setsid "$REC" "${REC_ARGS[@]}" &
rec_pid=$!
set +e
while true; do
  wait "$rec_pid"
  rec_status=$?
  if [[ "$interrupted" -eq 1 ]] && kill -0 "$rec_pid" 2>/dev/null; then
    continue
  fi
  break
done
set -e
trap - INT

if [[ "$rec_status" -ne 0 && "$interrupted" -eq 0 ]]; then
  exit "$rec_status"
fi

echo "== Convert to MP4 =="
if [[ -s "$DIR/left.hevc" ]]; then
  "$MUX" "$DIR/left.hevc" "$DIR/left.mp4" 60
else
  echo "Skip left.mp4: left.hevc is missing or empty"
fi
if [[ -s "$DIR/right.hevc" ]]; then
  "$MUX" "$DIR/right.hevc" "$DIR/right.mp4" 60
else
  echo "Skip right.mp4: right.hevc is missing or empty"
fi

echo "== Done =="
ls -lh "$DIR"
echo "DIR=$DIR"
