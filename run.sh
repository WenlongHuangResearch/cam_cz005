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
setsid "$REC" --seconds "$SECS" --out "$DIR" "$@" &
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
