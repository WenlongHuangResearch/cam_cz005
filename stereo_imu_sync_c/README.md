# 双目相机 双路 H.265 录制 (C 版) + IMU 时间戳同步

针对 **DECXIN-3261V1 / 3362V1 + Nori 3D Camera**（单路 USB3.0 UVC）。

整幅 `4000×1200` 的实际横向布局（已用真机帧确认）：

```
|<- 160px ->|<---- 1920px ---->|<---- 1920px ---->|
|  编码区    |     左 sensor     |     右 sensor     |
|(时间戳/IMU)|                  |                  |
```

最左侧 160px 竖直条带是**编码区**（黑白 8×8 块，含帧曝光时间戳 + IMU），右边
才是两颗 1920×1200 的 sensor。切左右图像时跳过这条编码区。

C 实现，基于 FFmpeg 的 `libav*` 库：

- 一个**采集线程**：dshow 取流 `4000×1200 MJPG@60` → 解码 → 整帧左侧编码区解出
  **帧曝光时间戳 + IMU**（写 CSV）→ 跳过 160px 编码区、切出**左/右两颗 sensor** →
  各转 NV12 → 分别推入**两个环形队列**。
- 两个**对称的编码线程**（左、右各一个）：从各自队列取帧，用 **AMD 硬件编码
  `hevc_amf`** 编成 H.265，写出 `left.hevc` / `right.hevc`（Annex-B 裸流）。
- 采集线程入队用「队满丢最旧帧」策略，**永不被慢编码器阻塞**，保证 USB 不丢帧。
- 独立程序 `hevc2mp4`：把 `.hevc` 裸流 **remux 成 `.mp4`**（stream copy，无损、秒级）。

> 单个 USB 设备不能被两个线程同时打开两次，所以「左右摄像头」是这一路流里的
> 两颗 sensor：采集线程读一次、切两路、喂给两路对称编码线程。
>
> 实测：4000×1200 满 **60fps**、IMU 解码 100% 成功、左右各输出 1920×1200 H.265。

## 目录

```
src/
  ring_buffer.[ch]       通用线程安全环形队列 (队满丢最旧帧)
  icm42688_decode.[ch]   底部编码区 -> 帧曝光时间戳 + IMU (移植自厂商 demo)
  encoder.[ch]           HEVC 编码器封装 (默认 hevc_amf)
  capture_record.c       采集主程序 (采集线程 + 双队列 + 双编码线程 + CSV)
  hevc_to_mp4.c          .hevc -> .mp4 remux 工具
Makefile
```

## 构建环境（MSYS2 / MinGW-w64）

本机已无 C 编译器，用 MSYS2 提供 gcc + FFmpeg 开发库：

```bash
# 1) 安装 MSYS2 (已装则跳过)
winget install MSYS2.MSYS2

# 2) 在 “MSYS2 MINGW64” 终端里装工具链 + ffmpeg 开发库
pacman -Sy
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-ffmpeg \
                   mingw-w64-x86_64-pkgconf make
```

## 编译

在 **MSYS2 MINGW64** 终端（不是普通 cmd / MSYS 终端）里：

```bash
cd /c/code/cam_cz005/stereo_imu_sync_c
make
```

生成 `stereo_record.exe` 和 `hevc2mp4.exe`。

> 直接在 PowerShell 里跑这两个 exe 也行，但要先把 `C:\msys64\mingw64\bin`
> 加进 `PATH`（否则找不到 `avcodec-*.dll` 等运行时库）。

## 运行

```bash
# 列出 dshow 视频设备 (确认设备名)
./stereo_record --list

# 录 10 秒: 双路 H265 + IMU 同步 CSV, 输出到 out/
./stereo_record --seconds 10 --out out

# 一直录到 Ctrl+C, 指定设备名与码率
./stereo_record --seconds 0 --bitrate 30000 --device "DECXIN Camera, UsbStr12"

# 没有 AMD 硬件编码时可退回软件编码 (4000x1200@60 实时可能跟不上)
./stereo_record --encoder libx265
```

主要参数：

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--device` | `DECXIN Camera` | dshow 设备名（用 `--list` 确认） |
| `--width/--height` | `4000/1200` | 整幅尺寸 |
| `--code-width` | `160` | 左侧编码区宽度，切图时跳过；左右各 `(宽-它)/2` |
| `--fps` | `60` | 帧率 |
| `--encoder` | `hevc_amf` | 编码器（可选 `libx265`） |
| `--bitrate` | `20000` | 每路目标码率 kbps |
| `--seconds` | `10` | 录制时长，`0` = 到 Ctrl+C |
| `--out` | `out` | 输出目录 |
| `--queue` | `8` | 每路环形队列容量（帧） |
| `--no-imu` | 关 | 不解 IMU / 不写 CSV |

## 转 MP4

```bash
./hevc2mp4 out/left.hevc  out/left.mp4  60
./hevc2mp4 out/right.hevc out/right.mp4 60
```

## 输出

```
out/
  left.hevc / right.hevc   左右两路 H.265 裸流
  left.mp4  / right.mp4     remux 后可直接播放
  frames.csv               frame_idx, host_t_s, cam_exp_start_us,
                           cam_exp_end_us, exposure_us, n_imu
  imu.csv                  frame_idx, cam_exp_end_us, imu_t_us,
                           acc_xyz(mg), gyro_xyz(dps)
```

`frames.csv` 的 `cam_exp_end_us`（帧时间戳=曝光结束 EE）与 `imu.csv` 的
`imu_t_us` 来自相机同一计数器（µs），可直接对齐 / 插值。程序已处理 32bit µs 回绕。

## 设计要点

- **对称**：左右是两套完全独立的「环形队列 + 编码线程」，互不干扰；任一路编码
  慢只会丢自己那一路的旧帧。
- **环形队列**：固定容量、互斥量 + 条件变量；生产者 `rb_push` 队满丢最旧帧不阻塞，
  消费者 `rb_pop` 阻塞等待；`rb_close` 优雅收尾。
- **时间戳同步**：IMU 与帧时间戳在采集线程里、拆分前从整帧编码区一次解出，天然
  同一时钟域，无需主机侧对齐。
- **无 B 帧编码**：`max_b_frames=0`，使 `dts==pts`，remux 时按帧号重建时间戳即可。
