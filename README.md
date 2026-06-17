# CAM CZ005

双目相机 `4000x1200@60fps` 录制、双路 H.265 编码、帧时间戳和 IMU 同步项目。

当前主程序在 `apps/stereo-recorder/`。仓库根目录提供了 `run.sh`，用于在 Orange Pi/Linux 上一键构建、录制、停止后自动转 MP4。

## 一键运行

在仓库根目录执行：

```bash
./run.sh
```

默认行为：

- 自动编译 `apps/stereo-recorder/build/bin/stereo_record`
- 使用 `/dev/video0` 打开相机
- 请求整幅 `4000x1200 MJPEG@60fps`
- 左右两路分别用 H.265 编码
- 一直录制，直到按 `Ctrl+C`
- 停止后自动把 `left.hevc/right.hevc` 转成 `left.mp4/right.mp4`

录 10 秒：

```bash
./run.sh 10
```

指定输出根目录：

```bash
./run.sh 10 artifacts/test
```

给 `stereo_record` 继续传参数：

```bash
./run.sh 0 "" --bitrate 30000
./run.sh 10 "" --device /dev/video2
./run.sh 10 "" --dec-threads 6
```

`run.sh` 参数格式是：

```text
./run.sh [seconds] [output_root] [extra stereo_record args...]
```

其中 `seconds=0` 表示一直录到 `Ctrl+C`。如果想跳过 `output_root` 但继续传 recorder 参数，第二个参数传空字符串 `""`。

## 输出目录

默认输出到：

```text
artifacts/stereo-recorder/recordings_run/
```

每次运行会新建一个带时间戳的目录，例如：

```text
artifacts/stereo-recorder/recordings_run/rec_20260616_201544_4000x1200_60fps/
```

目录内容：

```text
left.hevc       左路 H.265 裸流
right.hevc      右路 H.265 裸流
left.mp4        左路 MP4，停止后由 hevc2mp4 自动生成
right.mp4       右路 MP4，停止后由 hevc2mp4 自动生成
frames.csv      每帧主机时间、相机曝光时间戳、该帧 IMU 数量
imu.csv         每条 IMU 数据，和相机帧时间戳属于同一时钟域
```

`artifacts/` 是本地录制数据目录，默认不提交到仓库。

## 图像布局

相机输出的整幅图像是 `4000x1200`，横向布局如下：

```text
|<- 160px ->|<---- 1920px ---->|<---- 1920px ---->|
|  编码区    |      左目图像      |      右目图像      |
| 时间戳/IMU |                  |                  |
```

代码里的默认值：

```c
cfg.width = 4000;
cfg.height = 1200;
cfg.code_width = 160;
cfg.fps = 60;
```

`code_width=160` 表示最左侧 160 像素是编码区，不作为普通图像写入左右视频。程序会计算：

```text
single_sensor_width = (4000 - 160) / 2 = 1920
```

所以最终输出的 `left.mp4/right.mp4` 都是 `1920x1200`。

## Linux / Orange Pi 默认配置

Linux 版本使用 V4L2 打开相机：

```text
device       = /dev/video0
video_size   = 4000x1200
framerate    = 60
input_format = mjpeg
encoder      = hevc_mpp
```

这不是简单地打开 `/dev/video0` 让系统自己选择默认模式。程序会在打开 V4L2 时显式传入 `video_size/framerate/input_format`，强制请求 `4000x1200 MJPEG@60fps`。

相关代码在：

```text
apps/stereo-recorder/src/capture_record.c
```

关键点：

- 默认设备是 `/dev/video0`
- 默认编码器是 `hevc_mpp`
- 打开 V4L2 时设置 `video_size=4000x1200`
- 打开 V4L2 时设置 `framerate=60`
- 打开 V4L2 时设置 `input_format=mjpeg`

`4000x1200@60fps` 必须使用 MJPEG。不要让相机落到默认 YUYV 或低帧率模式，否则 USB 带宽和实时性都会不对。

## Windows 默认配置

Windows 版本使用 DirectShow 打开相机：

```text
device    = DECXIN Camera
input     = dshow
vcodec    = mjpeg
encoder   = hevc_amf
```

DirectShow 按设备名打开，例如：

```bash
./build/bin/stereo_record.exe --device "DECXIN Camera"
```

Linux 和 Windows 的主要区别：

| 项目 | Linux / Orange Pi | Windows |
| --- | --- | --- |
| 相机输入 | V4L2 | DirectShow |
| 设备表示 | `/dev/video0` | `DECXIN Camera` 这类设备名 |
| 输入格式参数 | `input_format=mjpeg` | `vcodec=mjpeg` |
| 默认编码器 | `hevc_mpp` | `hevc_amf` |
| 硬件编码 | Rockchip MPP | AMD AMF |

录制主流水线是共用的：采集 MJPEG、并行解码、重排、解 IMU、拆左右图、转 NV12、左右分别编码。

## 构建

仓库根目录：

```bash
make
```

或者直接构建 recorder：

```bash
make -C apps/stereo-recorder
```

生成产物：

```text
apps/stereo-recorder/build/bin/stereo_record
apps/stereo-recorder/build/bin/hevc2mp4
```

清理：

```bash
make clean
```

CI 使用：

```bash
make ci
```

### Linux / Orange Pi 依赖

需要 C 编译器、pkg-config、FFmpeg 开发库，以及 Rockchip MPP 开发库。

程序会通过 `pkg-config` 查找：

```text
libavformat
libavcodec
libavdevice
libavutil
libswscale
rockchip_mpp
```

如果对象文件曾经在不同平台或不同架构上编译过，建议先清理：

```bash
make -C apps/stereo-recorder clean all
```

### Windows / MSYS2

推荐在 `MSYS2 MINGW64` 终端中构建：

```bash
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-ffmpeg \
                   mingw-w64-x86_64-pkgconf make

make -C apps/stereo-recorder
```

生成：

```text
apps/stereo-recorder/build/bin/stereo_record.exe
apps/stereo-recorder/build/bin/hevc2mp4.exe
```

## 直接运行 recorder

列出相机设备：

```bash
apps/stereo-recorder/build/bin/stereo_record --list
```

录 10 秒：

```bash
apps/stereo-recorder/build/bin/stereo_record --seconds 10 --out out
```

一直录到 `Ctrl+C`：

```bash
apps/stereo-recorder/build/bin/stereo_record --seconds 0 --out out
```

常用参数：

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--device` | Linux: `/dev/video0`; Windows: `DECXIN Camera` | 相机设备 |
| `--width` | `4000` | 整幅输入宽度 |
| `--height` | `1200` | 整幅输入高度 |
| `--code-width` | `160` | 左侧编码区宽度 |
| `--fps` | `60` | 输入帧率 |
| `--encoder` | Linux: `hevc_mpp`; Windows: `hevc_amf` | H.265 编码器 |
| `--bitrate` | `20000` | 每路目标码率，单位 kbps |
| `--seconds` | `10` | 录制秒数，`0` 表示直到 Ctrl+C |
| `--out` | `out` | 输出目录 |
| `--queue` | `64` | 每路环形队列容量 |
| `--dec-threads` | `0` | MJPEG 解码线程数，`0` 表示自动取 4 |
| `--no-imu` | 关闭 | 不解码 IMU，不写 CSV |
| `--list` | - | 列出视频设备 |

## 手动转 MP4

如果只生成了 `.hevc`，可以手动转 MP4：

```bash
apps/stereo-recorder/build/bin/hevc2mp4 out/left.hevc  out/left.mp4  60
apps/stereo-recorder/build/bin/hevc2mp4 out/right.hevc out/right.mp4 60
```

这个步骤是 remux，不重新编码，速度很快，也不会损失画质。

## 主要代码位置

```text
run.sh
  一键构建、录制、Ctrl+C 停止、自动转 MP4

apps/stereo-recorder/src/capture_record.c
  主程序；参数默认值；dshow/v4l2 打开相机；采集、解码、重排、拆左右、写 CSV

apps/stereo-recorder/src/encoder.c
  编码器封装；Windows hevc_amf；Linux/Orange Pi hevc_mpp

apps/stereo-recorder/src/hevc_to_mp4.c
  left.hevc/right.hevc 转 left.mp4/right.mp4

apps/stereo-recorder/src/icm42688_decode.c
  从左侧 160px 编码区解出帧时间戳和 IMU

apps/stereo-recorder/src/ring_buffer.c
  多线程队列
```

更详细的 recorder 内部流水线说明见：

```text
apps/stereo-recorder/README.md
```

## 排查建议

如果录制不是 60fps，优先检查：

1. 是否真的请求到了 `4000x1200 MJPEG@60fps`
2. Linux 下设备是不是正确的 `/dev/video0`，必要时用 `--device /dev/videoX`
3. 是否使用了 `input_format=mjpeg`，不要落到 YUYV
4. Orange Pi 上是否使用 `hevc_mpp`
5. 编译产物是否来自当前机器，跨平台切换后建议 `make clean all`
6. 录制结束日志里的平均到达 fps、sensor 实际 fps、各级平均耗时、丢弃帧数

如果 MP4 时长异常，先看 `.hevc` 里实际帧数和 `frames.csv` 的时间跨度，再确认转 MP4 时传入的 fps 是否是 `60`。
