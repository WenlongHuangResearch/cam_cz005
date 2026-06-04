# 双目相机 最高分辨率 FPS 测试 + IMU 时间戳同步

针对 **DECXIN-3261V1 / 3362V1 模组 + Nori 3D Camera** 方案（USB3.0 / UVC 免驱）。
实现：以最高分辨率取流、实测最高帧率，同时从每帧底部编码区解出 IMU 数据，并与帧
曝光时间戳做硬件级时间同步。

## 关键结论（来自厂商规格书 / User Guide）

| 项目 | 值 |
| --- | --- |
| 传感器有效像素 | 1920×1200，全局快门，两颗水平拼接 |
| 整幅输出（含编码区） | **4000×1200** = 有效图像 + 底部编码区 |
| **最高分辨率最高帧率** | **4000×1200 @ 60fps，必须用 MJPEG** |
| YUYV(YUV2) | 4000×1200 @ 30fps（USB3 带宽上限） |
| 接口 / 驱动 | USB3.0，UVC 免驱（Win/Linux/Mac/Android） |
| IMU | TDK ICM42688，默认 ±4g / ±1000dps |
| 同步精度 | IMU↔帧 < 30µs；多 sensor 间 < 1µs |
| 时间戳 | 帧=曝光结束(EE)，IMU=测量完成时刻，同一时钟域，单位 µs，**32bit 会溢出** |

> 想跑满 60fps 一定要用 `--fourcc MJPG`。YUYV 在此分辨率被 USB3 带宽限制只能 30fps。

## 文件

- `tst_decode.py` —— 厂商 C++ 解码逻辑（`icm_decode.cpp` / `icm42688_decode.h`）的 Python 移植：
  从图像底部 8×8 黑白块编码区解出「帧曝光起止时间戳 + 各组 IMU 采样」。已用厂商样例
  BMP `4000x1200_0_0_10.bmp` 验证，结果与预期一致（曝光 7.49ms、11 条 IMU、acc Z≈-1g）。
- `capture_test.py` —— 取流主程序：开 MJPG 4000×1200@60fps、实测 FPS、逐帧解码 IMU、
  时间戳同步、导出 CSV。
- `requirements.txt` —— 依赖。

## 安装

```bash
pip install -r requirements.txt
```

## 在哪里跑：强烈建议 Windows（最省事）

相机是 UVC 免驱，本程序跨平台。**首选直接在 Windows 上跑**，无需 usbipd、无需碰 WSL 内核：

```powershell
# Windows: 装 Python 后
pip install numpy opencv-python
python capture_test.py --list
python capture_test.py -d 0 --mode rawfps --seconds 10
python capture_test.py -d 0 --mode sync   --seconds 10 --out out
# Windows 后端默认 msmf, 不稳可换 dshow:
python capture_test.py -d 0 --backend dshow --mode rawfps
```

### （可选）WSL2 透传 —— 较折腾，不推荐

WSL2 跑 UVC 摄像头有两道坎：

1. **`usbipd` 是 Windows 端命令**，必须在 **Windows PowerShell（管理员）** 里运行，不是在 WSL：
   ```powershell
   winget install usbipd
   usbipd list                 # 找到相机, 记下 BUSID (如 2-4)
   usbipd bind   --busid 2-4    # 首次共享(管理员)
   usbipd attach --wsl --busid 2-4
   ```
2. **WSL2 默认内核不含 UVC(V4L2) 驱动**，attach 后 `/dev/video*` 通常仍不出现，需要自行
   重编 WSL 内核打开 `CONFIG_USB_VIDEO_CLASS`。除非你已搞定内核, 否则请走上面的 Windows 方案。

## 运行

```bash
# 1) 列出相机
python3 capture_test.py --list

# 2) 纯测「最高分辨率最高 FPS」上限（不解码图像, 结果最准）
python3 capture_test.py -d /dev/video0 --mode rawfps --seconds 10

# 3) 取流 + 解码 IMU + 时间戳同步, 跑 10 秒并保存 CSV
python3 capture_test.py -d /dev/video0 --mode sync --seconds 10 --out out

# 4) YUYV 30fps 对照
python3 capture_test.py -d /dev/video0 --fourcc YUYV --fps 30 --mode rawfps
```

输出包含两种 FPS：
- **主机到达率 FPS**：主机实际收到帧的速率（含 USB/解码开销）。
- **相机时间戳 FPS**：用帧自带的曝光结束时间戳算的 sensor 真实帧率，不受主机抖动影响。

`--mode rawfps` 会关闭 OpenCV 的 MJPEG→BGR 解码（`CONVERT_RGB=0`），用于测相机+USB
的纯帧率上限；`--mode sync` 会解码每帧（用于取 IMU），若主机较弱可能略低于 60，此时
看「相机时间戳 FPS」判断 sensor 是否真的在 60fps。

## 时间戳同步

帧曝光时间戳（EE）与每条 IMU 时间戳来自相机同一计数器（µs）。`out/` 下：

- `frames.csv`：`frame_idx, host_t_s, cam_exp_start_us, cam_exp_end_us, exposure_us, n_imu`
- `imu.csv`：`frame_idx, cam_exp_end_us, imu_t_us, acc_xyz(mg), gyro_xyz(dps)`

`cam_exp_end_us`（帧时间戳）与 `imu_t_us` 同一时间轴，可直接对齐 / 插值。程序已处理
32bit µs 回绕（约每 71.6 分钟翻转一次）。

## 备注

- 编码区方向（BMP bottom-up vs 相机帧 top-down）由程序自动探测，无需手动配置。
- IMU 量纲默认 ±4g / ±1000dps，与厂商 demo 一致；如相机配置不同，改 `tst_decode.py`
  里的 `AFS_*/GFS_*` 即可。
