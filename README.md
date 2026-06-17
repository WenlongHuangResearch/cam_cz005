# CAM CZ005

双目相机 4000x1200@60fps 录制、双路 H.265 编码、IMU/帧时间戳同步项目。

## 目录结构

```
apps/
  stereo-recorder/        C 版录制程序，可独立构建
docs/                     项目记录、排查结论、同步验证说明
vendor/                   相机厂商提供的原始资料与 demo，尽量保持原样
artifacts/                本地录制输出与临时产物，默认不入库
build/                    本地构建缓存/旧二进制，默认不入库
.github/workflows/        CI 配置
```

## 一键运行

从仓库根目录执行：

```bash
./run.sh
```

默认会自动构建缺失的程序，并开始持续采集；在 Orange Pi/Linux 上默认使用
Rockchip MPP 硬件 H.265 编码。按 `Ctrl+C` 停止后会自动把左右 `.hevc` 转成 `.mp4`。
也可以指定录制秒数：

```bash
./run.sh 10
```

录制输出默认写入：

```text
artifacts/stereo-recorder/recordings_run/
```

## 构建

推荐在 MSYS2 MINGW64 环境中构建：

```bash
make ci
```

程序产物会输出到：

```text
apps/stereo-recorder/build/bin/
```

也可以直接运行应用脚本：

```bash
apps/stereo-recorder/scripts/build.sh
apps/stereo-recorder/scripts/record_session.sh 10
```

## CI/CD 约定

- CI 从仓库根目录执行 `make ci`。
- 构建产物只放在 `build/` 或应用自己的 `build/` 目录，不提交。
- 真机录制结果、CSV、MP4/HEVC、预览图放在 `artifacts/`，不提交。
- `vendor/` 是厂商原始资料区，业务源码不要反向依赖 vendor 目录里的工程结构；需要移植的逻辑应进入 `apps/`。
