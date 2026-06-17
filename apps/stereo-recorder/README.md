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

C 实现，基于 FFmpeg 的 `libav*` 库。为吃满 **60fps**，整条链路是多级并行流水线
（各级用线程安全环形队列解耦）：

```
采集(主线程) ──[pkt_q 阻塞]──> N×解码worker ──[重排]──> 收集线程
                                                          │ IMU/时间戳→CSV, 克隆整幅
                                              ┌───────────┴───────────┐
                                        [rb_l 整幅]              [rb_r 整幅]
                                        左转换线程(sws)          右转换线程(sws)
                                        [nv_l NV12]              [nv_r NV12]
                                        左编码线程(amf)          右编码线程(amf)
                                        left.hevc                right.hevc
```

- **采集主线程**：Windows 用 dshow、Linux/Orange Pi 用 v4l2 取流
  `4000×1200 MJPG@60`，只做 USB 读包并按序号阻塞入队，天然按 60fps 节流；
  分配序号后绝不丢包（重排无空洞），过载时由上游缓冲兜底。
- **N 个独立 MJPEG 解码器实例**（默认 4，`--dec-threads` 可调）：MJPEG 帧内编码各帧
  独立，可完美并行。单线程解码 4000×1200 约 32ms（远超 16.67ms 预算），多实例并行后
  解码等效 ~6–8ms。
- **重排缓冲**：解码乱序完成后按采集序号还原顺序。
- **收集线程**：按序解出 **帧曝光时间戳 + IMU**（写 CSV），把整幅解码帧「按引用」
  （`av_frame_clone`，不拷像素）分发到左右两路。
- **左右各两级**：转换线程做「切半幅 + yuvj420p→NV12」（sws），编码线程用
  硬件 H.265（Windows 默认 `hevc_amf`，Orange Pi/Linux 默认 `hevc_mpp`）。sws 与编码
  拆成两级，各自都在预算内，长录不丢帧。
- 各下游队列用「队满丢最旧帧」策略，互不阻塞。
- 独立程序 `hevc2mp4`：把 `.hevc` 裸流 **remux 成 `.mp4`**（stream copy，无损、秒级）。

> 单个 USB 设备不能被两个线程同时打开两次，所以「左右摄像头」是这一路流里的两颗
> sensor：采集线程读一次、解码后切两路、喂给两路对称的转换+编码线程。
>
> 实测（16 核机）：4000×1200 满 **60fps**、IMU 解码 100% 成功、左右各输出
> 1920×1200 H.265、30s 录制左右各 1805 帧**零丢帧**。
>
> **掉帧排查心得**：瓶颈依次是 ① 单线程 MJPEG 解码(32ms)、② 两次 sws 串在采集线程、
> ③ sws 与编码串在一条线程(17ms>预算)、④ `yuvj420p` 触发 sws 范围转换慢路径。
> 逐一拆分/并行/规避后才稳定 60fps。运行结束会打印各级每帧平均耗时, 便于在别的机器
> 上重新调 `--dec-threads`。

## 目录

```
src/
  ring_buffer.[ch]       通用线程安全环形队列 (队满丢最旧帧)
  icm42688_decode.[ch]   底部编码区 -> 帧曝光时间戳 + IMU (移植自厂商 demo)
  encoder.[ch]           HEVC 编码器封装 (Windows hevc_amf / Orange Pi hevc_mpp)
  capture_record.c       采集主程序 (采集线程 + 双队列 + 双编码线程 + CSV)
  hevc_to_mp4.c          .hevc -> .mp4 remux 工具
scripts/
  build.sh               构建脚本
  install_build.sh       安装 MSYS2 依赖并构建
  record_session.sh      录制一组数据并转 MP4
  verify.sh              检查录制输出
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
cd /c/code/cam_cz005/apps/stereo-recorder
make
```

生成 `build/bin/stereo_record.exe` 和 `build/bin/hevc2mp4.exe`。

> 直接在 PowerShell 里跑这两个 exe 也行，但要先把 `C:\msys64\mingw64\bin`
> 加进 `PATH`（否则找不到 `avcodec-*.dll` 等运行时库）。

## 运行

```bash
# 列出 dshow 视频设备 (确认设备名)
./build/bin/stereo_record --list

# 录 10 秒: 双路 H265 + IMU 同步 CSV, 输出到 out/
./build/bin/stereo_record --seconds 10 --out out

# 一直录到 Ctrl+C, 指定设备名与码率
./build/bin/stereo_record --seconds 0 --bitrate 30000 --device "DECXIN Camera, UsbStr12"

# 没有 AMD 硬件编码时可退回软件编码 (4000x1200@60 实时可能跟不上)
./build/bin/stereo_record --encoder libx265
```

主要参数：

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--device` | `DECXIN Camera` | dshow 设备名（用 `--list` 确认） |
| `--width/--height` | `4000/1200` | 整幅尺寸 |
| `--code-width` | `160` | 左侧编码区宽度，切图时跳过；左右各 `(宽-它)/2` |
| `--fps` | `60` | 帧率 |
| `--encoder` | Windows: `hevc_amf`; Linux: `hevc_mpp` | 编码器（可选 `libx265`，实时可能跟不上） |
| `--bitrate` | `20000` | 每路目标码率 kbps |
| `--seconds` | `10` | 录制时长，`0` = 到 Ctrl+C |
| `--out` | `out` | 输出目录 |
| `--queue` | `64` | NV12/解码队列容量（帧） |
| `--dec-threads` | `0` | 并行 MJPEG 解码器实例数，`0`=自动取 4；CPU 弱可调大、强可调小 |
| `--no-imu` | 关 | 不解 IMU / 不写 CSV |

## 转 MP4

```bash
./build/bin/hevc2mp4 out/left.hevc  out/left.mp4  60
./build/bin/hevc2mp4 out/right.hevc out/right.mp4 60
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

- **并行解码 + 重排**：单线程 MJPEG 解码是硬瓶颈，用多个独立解码器实例并行（帧内
  编码各帧独立），再按采集序号重排恢复顺序。`pkt_q` 用**阻塞式入队**确保序号无空洞，
  重排逻辑因此简单可靠。
- **流水线分级**：sws 转换与 hevc 编码拆成两级独立线程，各自单帧耗时都在 16.67ms
  预算内（合在一条线程会超），长录不丢帧。
- **零拷贝分发**：收集线程用 `av_frame_clone`（仅加引用计数）把整幅解码帧分发到左右
  两路，不复制像素。
- **环形队列**：固定容量、互斥量 + 条件变量；`rb_push` 队满丢最旧帧不阻塞（下游各级），
  `rb_push_block` 队满阻塞不丢（pkt_q），`rb_pop` 阻塞等待；`rb_close` 逐级优雅收尾。
- **时间戳同步**：IMU 与帧时间戳在收集线程里、拆分前从整帧编码区一次解出（按序，
  unwrap 状态正确），天然同一时钟域，无需主机侧对齐。
- **无 B 帧编码**：`max_b_frames=0`，使 `dts==pts`，remux 时按帧号重建时间戳即可。
