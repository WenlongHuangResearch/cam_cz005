#!/usr/bin/env bash
set -e
export MSYSTEM=MINGW64
export PATH="/mingw64/bin:/usr/bin:$PATH"

echo "===== 安装工具链 ====="
pacman -S --needed --noconfirm \
    mingw-w64-x86_64-gcc \
    mingw-w64-x86_64-ffmpeg \
    mingw-w64-x86_64-pkgconf \
    make

echo "===== 工具链版本 ====="
gcc --version | head -1
pkg-config --modversion libavcodec libavformat libavdevice libswscale

echo "===== hevc 编码器 ====="
ffmpeg -hide_banner -encoders 2>/dev/null | grep -i hevc || true

echo "===== 编译 ====="
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$APP_DIR"
make clean
make

echo "===== 产物 ====="
ls -l build/bin
