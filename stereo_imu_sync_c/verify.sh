#!/usr/bin/env bash
export PATH="/mingw64/bin:/usr/bin:$PATH"
cd "$1"
echo "left.mp4  时长(s): $(ffprobe -v error -show_entries format=duration -of csv=p=0 left.mp4)"
echo "right.mp4 时长(s): $(ffprobe -v error -show_entries format=duration -of csv=p=0 right.mp4)"
echo "frames.csv 数据行(不含表头): $(($(grep -c '' frames.csv) - 1))"
echo "imu.csv    数据行(不含表头): $(($(grep -c '' imu.csv) - 1))"
