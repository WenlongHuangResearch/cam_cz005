#!/usr/bin/env bash
# 采集一组数据到带时间戳的目录, 录完转 MP4。
# 用法: record_session.sh [秒数] [输出根目录]
set -e
export MSYSTEM=MINGW64
export PATH="/mingw64/bin:/usr/bin:$PATH"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$APP_DIR/../.." && pwd)"
BIN_DIR="$APP_DIR/build/bin"
cd "$APP_DIR"

SECS="${1:-60}"
ROOT="${2:-$REPO_ROOT/artifacts/stereo-recorder/recordings}"
TS="$(date +%Y%m%d_%H%M%S)"
DIR="${ROOT}/rec_${TS}_4000x1200_60fps"
mkdir -p "$DIR"
echo "输出目录: $DIR"

"$BIN_DIR/stereo_record.exe" --seconds "$SECS" --out "$DIR" 2>&1 | grep -vi "deprecated pixel"

echo "===== 转 MP4 ====="
"$BIN_DIR/hevc2mp4.exe" "$DIR/left.hevc"  "$DIR/left.mp4"  60
"$BIN_DIR/hevc2mp4.exe" "$DIR/right.hevc" "$DIR/right.mp4" 60

echo "===== 目录内容 ====="
ls -lh "$DIR"
echo "DIR=$DIR"
