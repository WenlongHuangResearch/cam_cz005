#!/usr/bin/env bash
# 在 MSYS2 MINGW64 环境下构建。用法 (从 PowerShell):
#   C:\msys64\usr\bin\bash.exe -lc "MSYSTEM=MINGW64 /c/code/cam_cz005/stereo_imu_sync_c/build.sh"
set -e
export MSYSTEM=MINGW64
export PATH="/mingw64/bin:$PATH"
cd "$(dirname "$0")"

echo "== 工具链 =="
gcc --version | head -1
pkg-config --modversion libavcodec libavformat libavdevice || true

echo "== hevc 编码器 =="
ffmpeg -hide_banner -encoders 2>/dev/null | grep -i hevc || true

echo "== make =="
make clean
make
echo "== 产物 =="
ls -l stereo_record.exe hevc2mp4.exe
