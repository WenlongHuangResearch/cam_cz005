#!/usr/bin/env python3
"""
capture_test.py

双目 (DECXIN / Nori 3D) UVC 相机测试程序:
  1) 以最高分辨率 4000x1200 + MJPG 打开相机, 实测真实 FPS;
  2) 同时逐帧解码图像底部编码区, 取出"帧曝光时间戳 + IMU 采样";
  3) 帧时间戳与 IMU 时间戳为相机同一时钟域 (us), 直接对齐, 输出 CSV。

相机为 UVC 免驱, Linux 下走 v4l2 即可, 无需厂商 SDK。

用法示例:
  # 列出可用相机
  python3 capture_test.py --list

  # 纯测最高分辨率最高帧率上限 (不解码图像, 最准):
  python3 capture_test.py -d /dev/video0 --mode rawfps --seconds 10

  # 取流 + 解码 IMU + 时间戳同步, 跑 10 秒并存 CSV:
  python3 capture_test.py -d /dev/video0 --mode sync --seconds 10 --out out

  # 想看 YUYV 30fps 对照:
  python3 capture_test.py -d /dev/video0 --fourcc YUYV --fps 30 --mode rawfps
"""

from __future__ import annotations

import argparse
import csv
import os
import statistics
import sys
import time
from typing import Optional

import cv2
import numpy as np

import tst_decode as dec


_BACKENDS = {
    "v4l2": cv2.CAP_V4L2,
    "msmf": cv2.CAP_MSMF,
    "dshow": cv2.CAP_DSHOW,
    "avfoundation": getattr(cv2, "CAP_AVFOUNDATION", cv2.CAP_ANY),
    "any": cv2.CAP_ANY,
}


def resolve_backend(name: str) -> int:
    """根据平台和 --backend 选项返回 OpenCV 后端常量。"""
    if name != "auto":
        return _BACKENDS[name]
    if sys.platform.startswith("linux"):
        return cv2.CAP_V4L2
    if sys.platform.startswith("win"):
        # 实测: MSMF 按索引取流不稳(fourcc 设不动、~13fps); DShow 能真正切到
        # MJPG 并跑满高帧率, 故 Windows 默认用 DShow。
        return cv2.CAP_DSHOW
    if sys.platform == "darwin":
        return _BACKENDS["avfoundation"]
    return cv2.CAP_ANY


def list_devices(backend: int) -> None:
    if sys.platform.startswith("linux"):
        print("扫描 /dev/video* ...")
        found = False
        for i in range(64):
            path = f"/dev/video{i}"
            if not os.path.exists(path):
                continue
            found = True
            cap = cv2.VideoCapture(path, backend)
            ok = cap.isOpened()
            w = cap.get(cv2.CAP_PROP_FRAME_WIDTH)
            h = cap.get(cv2.CAP_PROP_FRAME_HEIGHT)
            cap.release()
            print(f"  {path:14s} open={ok} default={int(w)}x{int(h)}")
        if not found:
            print("  没有发现 /dev/video* 设备。")
            print("  WSL2 需先把相机透传且内核带 UVC 驱动; 建议直接在 Windows 上跑 (见 README)。")
        return
    # Windows / Mac: 按索引枚举
    print("按设备索引枚举 (0..9) ...")
    any_ok = False
    for i in range(10):
        cap = cv2.VideoCapture(i, backend)
        if cap.isOpened():
            any_ok = True
            w = cap.get(cv2.CAP_PROP_FRAME_WIDTH)
            h = cap.get(cv2.CAP_PROP_FRAME_HEIGHT)
            print(f"  index {i}: open=True default={int(w)}x{int(h)}")
        cap.release()
    if not any_ok:
        print("  未找到相机, 确认已插好且未被其它软件占用。")


def open_camera(device, width, height, fps, fourcc,
                backend: int) -> cv2.VideoCapture:
    # 设备可以是 int 索引或 /dev/videoN 路径
    try:
        dev_arg = int(device)
    except (ValueError, TypeError):
        dev_arg = device
    cap = cv2.VideoCapture(dev_arg, backend)
    if not cap.isOpened():
        raise RuntimeError(f"无法打开相机 {device}")

    # 顺序很重要: 先 FOURCC, 再分辨率, 最后帧率。
    # Windows(DShow) 有个坑: 只设一次 FOURCC 常被忽略, 退回 YUY2 ->
    # 4000x1200 被 USB3 带宽限制只剩 ~15fps。所以分辨率设完后再设一次 FOURCC,
    # 强制切到 MJPG (实测这样 dshow 才会真正生效)。
    fcc = cv2.VideoWriter_fourcc(*fourcc)
    cap.set(cv2.CAP_PROP_FOURCC, fcc)
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
    cap.set(cv2.CAP_PROP_FPS, fps)
    cap.set(cv2.CAP_PROP_FOURCC, fcc)
    # 尽量减小内部缓冲, 让 FPS 反映真实到达率
    try:
        cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
    except Exception:
        pass
    return cap


def fourcc_to_str(v: float) -> str:
    iv = int(v)
    return "".join(chr((iv >> (8 * i)) & 0xFF) for i in range(4))


def report_actual(cap: cv2.VideoCapture) -> dict:
    info = {
        "width": int(cap.get(cv2.CAP_PROP_FRAME_WIDTH)),
        "height": int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT)),
        "fps": cap.get(cv2.CAP_PROP_FPS),
        "fourcc": fourcc_to_str(cap.get(cv2.CAP_PROP_FOURCC)),
    }
    print(f"[相机实际生效] {info['width']}x{info['height']} "
          f"@ {info['fps']:.1f}fps  fourcc={info['fourcc']}")
    return info


def detect_orientation(frame_bgr: np.ndarray) -> Optional[bool]:
    """探测编码区方向。返回是否需要竖直翻转 (flipud); None 表示两个方向都解不出。"""
    m = dec.decode_frame(frame_bgr)
    if m.ok and m.group_size > 0:
        return False
    m = dec.decode_frame(np.flipud(frame_bgr))
    if m.ok and m.group_size > 0:
        return True
    return None


class Unwrap32:
    """把 32bit us 时间戳累加成单调递增的 64bit (处理回绕)。"""
    def __init__(self):
        self.last = None
        self.base = 0

    def __call__(self, v: int) -> int:
        if self.last is not None and v < self.last - (1 << 31):
            self.base += (1 << 32)
        self.last = v
        return self.base + v


def run(args) -> int:
    backend = resolve_backend(args.backend)
    cap = open_camera(args.device, args.width, args.height, args.fps,
                      args.fourcc, backend)
    info = report_actual(cap)

    do_decode = args.mode == "sync"
    # rawfps 模式: 不让 OpenCV 把 MJPEG 解成 BGR, 测纯到达率上限
    if args.mode == "rawfps":
        try:
            cap.set(cv2.CAP_PROP_CONVERT_RGB, 0)
        except Exception:
            pass

    # 预热 + 方向探测
    flip = False
    warm = 0
    t_warm = time.perf_counter()
    while warm < 30 and time.perf_counter() - t_warm < 3.0:
        ok, frame = cap.read()
        if ok and frame is not None:
            warm += 1
            if do_decode and warm == 5:
                o = detect_orientation(frame)
                if o is None:
                    print("[警告] 预热帧未能解出编码区, 将继续尝试两种方向。")
                else:
                    flip = o
                    print(f"[编码区方向] {'需翻转(flipud)' if flip else '无需翻转'}")
    if warm == 0:
        print("[错误] 读不到任何帧, 检查相机连接/分辨率是否支持。")
        cap.release()
        return 2

    frames_csv = imu_csv = None
    fcsv_w = icsv_w = None
    if args.out and do_decode:
        os.makedirs(args.out, exist_ok=True)
        frames_csv = open(os.path.join(args.out, "frames.csv"), "w", newline="")
        imu_csv = open(os.path.join(args.out, "imu.csv"), "w", newline="")
        fcsv_w = csv.writer(frames_csv)
        icsv_w = csv.writer(imu_csv)
        fcsv_w.writerow(["frame_idx", "host_t_s", "cam_exp_start_us",
                         "cam_exp_end_us", "exposure_us", "n_imu"])
        icsv_w.writerow(["frame_idx", "cam_exp_end_us", "imu_t_us",
                         "acc_x_mg", "acc_y_mg", "acc_z_mg",
                         "gyro_x_dps", "gyro_y_dps", "gyro_z_dps"])

    # 采集主循环
    arrival = []           # 主机到达时间 (perf_counter)
    cam_ts = []            # 相机帧时间戳 (unwrap us)
    n_decoded = 0
    n_decode_fail = 0
    uw_frame = Unwrap32()
    uw_imu = Unwrap32()

    t0 = time.perf_counter()
    t_last_print = t0
    frame_idx = 0
    print(f"[开始采集] mode={args.mode} 时长={args.seconds}s ...")
    while time.perf_counter() - t0 < args.seconds:
        ok, frame = cap.read()
        t = time.perf_counter()
        if not ok or frame is None:
            continue
        arrival.append(t)

        if do_decode:
            f = np.flipud(frame) if flip else frame
            m = dec.decode_frame(f)
            if not m.ok:
                # 方向可能没探测对, 再试反方向
                m = dec.decode_frame(np.flipud(f))
            if m.ok:
                n_decoded += 1
                exp_end_uw = uw_frame(m.exp_time_end)
                cam_ts.append(exp_end_uw)
                if fcsv_w:
                    fcsv_w.writerow([frame_idx, f"{t - t0:.6f}",
                                     m.exp_time_start, m.exp_time_end,
                                     m.exposure_us, len(m.imu)])
                    for s in m.imu:
                        icsv_w.writerow([frame_idx, m.exp_time_end,
                                         uw_imu(s.u_time),
                                         f"{s.acc[0]:.3f}", f"{s.acc[1]:.3f}",
                                         f"{s.acc[2]:.3f}", f"{s.gyro[0]:.4f}",
                                         f"{s.gyro[1]:.4f}", f"{s.gyro[2]:.4f}"])
            else:
                n_decode_fail += 1

        frame_idx += 1
        if t - t_last_print >= 1.0:
            inst = len(arrival) / (t - t0)
            msg = f"\r  采集中... 帧数={len(arrival)} 平均fps={inst:.2f}"
            if do_decode:
                msg += f" 解码成功={n_decoded} 失败={n_decode_fail}"
            sys.stdout.write(msg)
            sys.stdout.flush()
            t_last_print = t

    cap.release()
    if frames_csv:
        frames_csv.close()
        imu_csv.close()
    print()

    # ---- 统计 ----
    elapsed = arrival[-1] - arrival[0] if len(arrival) > 1 else 0.0
    n = len(arrival)
    print("\n========== 结果 ==========")
    print(f"分辨率/格式 : {info['width']}x{info['height']} {info['fourcc']} "
          f"(设定 {args.fps}fps)")
    print(f"总帧数      : {n}")
    print(f"实际时长    : {elapsed:.3f} s")
    if elapsed > 0:
        print(f"** 实测平均 FPS (主机到达率) : {(n - 1) / elapsed:.2f} **")

    if len(arrival) > 2:
        diffs = [(arrival[i + 1] - arrival[i]) * 1000.0
                 for i in range(len(arrival) - 1)]
        print(f"帧间隔(ms)  : 均值={statistics.mean(diffs):.2f} "
              f"最小={min(diffs):.2f} 最大={max(diffs):.2f} "
              f"标准差={statistics.pstdev(diffs):.2f}")

    # 用相机自身帧时间戳算 fps (更能反映 sensor 真实帧率, 不受主机抖动影响)
    if len(cam_ts) > 2:
        span_us = cam_ts[-1] - cam_ts[0]
        if span_us > 0:
            print(f"** 相机时间戳 FPS (sensor 实际) : "
                  f"{(len(cam_ts) - 1) * 1e6 / span_us:.2f} **")
        cam_diffs = [(cam_ts[i + 1] - cam_ts[i]) / 1000.0
                     for i in range(len(cam_ts) - 1)]
        print(f"相机帧间隔(ms): 均值={statistics.mean(cam_diffs):.3f} "
              f"最小={min(cam_diffs):.3f} 最大={max(cam_diffs):.3f}")
        print(f"解码成功率   : {n_decoded}/{n} "
              f"({100.0 * n_decoded / max(n,1):.1f}%)")

    if args.out and do_decode:
        print(f"\nCSV 已保存到: {os.path.abspath(args.out)}/frames.csv, imu.csv")
        print("时间戳同步说明: frames.csv 的 cam_exp_end_us 为帧时间戳(曝光结束 EE),")
        print("imu.csv 的 imu_t_us 与之同一时钟域(us), 可直接做插值/对齐。")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="双目 UVC 相机 最高分辨率FPS + IMU时间戳同步测试")
    p.add_argument("--list", action="store_true", help="列出可用相机后退出")
    p.add_argument("-d", "--device", default="0",
                   help="相机设备: 索引(0) 或 /dev/video0 (默认 0)")
    p.add_argument("--width", type=int, default=4000, help="宽 (默认 4000)")
    p.add_argument("--height", type=int, default=1200, help="高 (默认 1200)")
    p.add_argument("--fps", type=int, default=60, help="设定帧率 (默认 60)")
    p.add_argument("--fourcc", default="MJPG",
                   help="像素格式 MJPG/YUYV (默认 MJPG; 4000x1200@60 必须 MJPG)")
    p.add_argument("--mode", choices=["rawfps", "sync"], default="sync",
                   help="rawfps=纯测帧率上限(不解码); sync=取流+解码IMU+同步 (默认)")
    p.add_argument("--seconds", type=float, default=10.0, help="采集时长秒 (默认 10)")
    p.add_argument("--out", default="", help="CSV 输出目录 (sync 模式有效)")
    p.add_argument("--backend", default="auto",
                   choices=["auto", "v4l2", "msmf", "dshow", "avfoundation", "any"],
                   help="OpenCV 后端 (默认 auto: Linux=v4l2, Win=msmf, Mac=avfoundation)")
    return p


def main() -> int:
    args = build_parser().parse_args()
    if args.list:
        list_devices(resolve_backend(args.backend))
        return 0
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
