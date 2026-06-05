#!/usr/bin/env bash
# 采集一组数据到带时间戳的目录, 录完转 MP4。
# 用法: record_session.sh [秒数] [输出根目录]
set -e
export MSYSTEM=MINGW64
export PATH="/mingw64/bin:/usr/bin:$PATH"
cd "$(dirname "$0")"

SECS="${1:-60}"
ROOT="${2:-recordings}"
TS="$(date +%Y%m%d_%H%M%S)"
DIR="${ROOT}/rec_${TS}_4000x1200_60fps"
mkdir -p "$DIR"
echo "输出目录: $DIR"

./stereo_record.exe --seconds "$SECS" --out "$DIR" 2>&1 | grep -vi "deprecated pixel"

echo "===== 转 MP4 ====="
./hevc2mp4.exe "$DIR/left.hevc"  "$DIR/left.mp4"  60
./hevc2mp4.exe "$DIR/right.hevc" "$DIR/right.mp4" 60

echo "===== 目录内容 ====="
ls -lh "$DIR"
echo "DIR=$DIR"
