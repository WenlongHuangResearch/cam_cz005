#!/usr/bin/env python3
"""探测相机在当前链路下能否取流: 从低分辨率往高试, 看每档能读到几帧。"""
import sys
import time

import cv2

DEV = int(sys.argv[1]) if len(sys.argv) > 1 else 0

COMBOS = [
    ("MJPG", 640, 480, 30),
    ("MJPG", 1280, 720, 30),
    ("MJPG", 1920, 1200, 30),
    ("MJPG", 2000, 1200, 60),
    ("MJPG", 4000, 1200, 30),
    ("MJPG", 4000, 1200, 60),
    ("YUYV", 640, 480, 30),
]


def fourcc_str(v):
    iv = int(v)
    return "".join(chr((iv >> (8 * i)) & 0xFF) for i in range(4))


print(f"探测 /dev/video{DEV} ...")
for fourcc, w, h, fps in COMBOS:
    cap = cv2.VideoCapture(DEV, cv2.CAP_V4L2)
    if not cap.isOpened():
        print(f"  {fourcc} {w}x{h}@{fps}: 打不开设备")
        continue
    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*fourcc))
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, w)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, h)
    cap.set(cv2.CAP_PROP_FPS, fps)
    aw = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    ah = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    af = fourcc_str(cap.get(cv2.CAP_PROP_FOURCC))
    afps = cap.get(cv2.CAP_PROP_FPS)

    n = 0
    t0 = time.time()
    while time.time() - t0 < 3.0 and n < 15:
        ok, fr = cap.read()
        if ok and fr is not None:
            n += 1
    dt = time.time() - t0
    rate = n / dt if dt > 0 else 0
    tag = "OK" if n > 0 else "TIMEOUT"
    print(f"  请求 {fourcc} {w}x{h}@{fps} -> 实际 {af} {aw}x{ah}@{afps:.0f} "
          f"| 读到 {n} 帧 / {dt:.1f}s (~{rate:.1f} fps) [{tag}]")
    cap.release()
print("完成。")
