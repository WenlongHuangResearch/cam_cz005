"""
tst_decode.py

将厂商 TimeStamp_Data_Decode_DemoCode_V010002 (icm_decode.cpp / icm42688_decode.h)
里的"图像编码区 -> 帧曝光时间戳 + IMU 数据"解码逻辑, 忠实移植为 Python。

编码规则 (来自 Nori 3D Camera User Guide V1.0 第 6 节):
  - 每帧编码区共 12 组, 每组 16 字节。
  - 第 1 组 (group 0): Header(8B) + 帧曝光时间戳(开始 4B + 结束 4B)。
  - 第 2~12 组: IMU 时间戳(4B) + 数据(12B = acc XYZ + gyro XYZ, 各 int16, 大端)。
  - 所有时间戳同一时钟域, 单位 us, 32bit (会溢出, 需处理回绕)。
  - 编码区: 8x8 黑白块; 空白(无效缓冲)像素灰度 > 210, demo 用 220 作阈值。
    像素中间通道值 0~50 表示 bit 0, 60~220 表示 bit 1, >220 表示结束/空白。

注意 BMP 像素是 bottom-up 存储, OpenCV 帧是 top-down。capture 程序里会处理方向。
本模块的 decode_frame() 接受一个 (H, W, C) 的 uint8 numpy 数组, 语义与厂商 raw
rgb_data 缓冲一致 (即第 0 行对应厂商代码里的 Vindex=0)。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional

import numpy as np

# ---- 设备类型 (对应 C++ enum) ----
EDEV_ICM42688 = 1

# ---- 编码参数 ----
GROUP_DT_SIZE = 16        # 每组字节数
CODE_HSIZE = 8            # 编码块宽 (USize)
CODE_VSIZE = 8            # 编码块高
VAL_0 = 50                # bit0 阈值
VAL_1 = 220               # bit1 / 空白阈值

# ---- IMU 量纲 (icm42688_decode.h, 默认 AFS_4G / GFS_1000DPS) ----
AFS_4G = 0x02
GFS_1000DPS = 0x01

_ARES = {0x03: 2000 / 32768.0, 0x02: 4000 / 32768.0,
         0x01: 8000 / 32768.0, 0x00: 16000 / 32768.0}
_GRES = {0x07: 15.125 / 32768.0, 0x06: 31.25 / 32768.0, 0x05: 62.5 / 32768.0,
         0x04: 125.0 / 32768.0, 0x03: 250.0 / 32768.0, 0x02: 500.0 / 32768.0,
         0x01: 1000.0 / 32768.0, 0x00: 2000.0 / 32768.0}


def _u32_be(b: bytes) -> int:
    return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]


def _s16_be(b: bytes) -> int:
    v = (b[0] << 8) | b[1]
    return v - 0x10000 if v & 0x8000 else v


@dataclass
class ImuSample:
    u_time: int               # 采样时间戳, us (32bit, 原始)
    acc: tuple                # (x, y, z) 单位 mg (1/1000 g)
    gyro: tuple               # (x, y, z) 单位 dps
    raw_acc: tuple = (0, 0, 0)
    raw_gyro: tuple = (0, 0, 0)


@dataclass
class FrameMeta:
    ok: bool = False
    protocol_type: int = 0
    device_type: List[int] = field(default_factory=lambda: [0] * 5)
    device_dt_size: List[int] = field(default_factory=lambda: [0] * 5)
    exp_time_start: int = 0   # 帧曝光开始, us (32bit)
    exp_time_end: int = 0     # 帧曝光结束 (EE, 即帧时间戳), us (32bit)
    group_size: int = 0
    imu: List[ImuSample] = field(default_factory=list)
    raw_groups: bytes = b""   # 解码出的原始字节流 (调试用)

    @property
    def exposure_us(self) -> int:
        """该帧曝光时长 (us), 处理 32bit 回绕。"""
        return (self.exp_time_end - self.exp_time_start) & 0xFFFFFFFF


def _line_decode(frame: np.ndarray, v_index: int, color_size: int,
                 h_size: int, u_size: int) -> bytes:
    """移植 F_LineDecode: 从第 v_index 行解码出字节流。返回 bytes (可能为空)。"""
    if v_index < 0 or v_index >= frame.shape[0]:
        return b""

    # 取中间通道 (C++: uCind = ColorSize >> 1; 24bit -> 取 G 通道)
    cind = color_size >> 1
    row = frame[v_index]                      # shape (W, C)
    chan = row[:, cind].astype(np.int32)      # 中间通道, 长度 W

    # 起始空白校验: 厂商代码检查 pData[0..3] (前几个 byte) 是否 > 220。
    # 等价为前两个像素的中间通道都为白。这里用前 2 个像素的中间通道判断。
    if not (chan[0] > VAL_1 and chan[1] > VAL_1):
        return b""

    # 找编码起始列: 第一个非白列, 然后右移半个块到块中心
    start = None
    for ind in range(h_size):
        if chan[ind] < VAL_1:
            start = ind + (u_size >> 1)
            break
    if start is None:
        return b""

    # 逐块取 bit: 每个块取块中心相邻两像素中间通道之和
    bits: List[int] = []
    ind = start
    while ind + 1 < h_size:
        v = chan[ind] + chan[ind + 1]
        if v < (VAL_0 << 1):
            bits.append(0)
        elif v < (VAL_1 << 1):
            bits.append(1)
        else:
            break                              # 遇到空白 -> 本行结束
        ind += u_size

    # bit -> byte (LSB first), 只取完整字节
    n_bytes = len(bits) // 8
    out = bytearray(n_bytes)
    for i in range(n_bytes):
        val = 0
        for b in range(8):
            val |= bits[i * 8 + b] << b
        out[i] = val
    return bytes(out)


def _group0_analysis(buf: bytes) -> FrameMeta:
    """移植 F_Group_0_Analysis: 解析第 0 组 (协议头 + 帧曝光时间戳)。"""
    meta = FrameMeta()
    if len(buf) < 16:
        return meta

    d = buf
    # 旧协议: byte0~7 == byte8~15
    if d[0:8] == d[8:16]:
        meta.protocol_type = 0
        meta.device_type[0] = EDEV_ICM42688
        meta.device_dt_size[0] = 11
        meta.exp_time_start = _u32_be(d[0:4])
        meta.exp_time_end = _u32_be(d[4:8])
    else:
        meta.protocol_type = d[0] >> 4
        meta.device_type[0] = ((d[0] & 0xF) << 4) | ((d[1] & 0xF0) >> 4)
        meta.device_dt_size[0] = d[1] & 0xF
        meta.device_type[1] = d[2]
        meta.device_dt_size[1] = d[3] >> 4
        meta.device_type[2] = ((d[3] & 0xF) << 4) | ((d[4] & 0xF0) >> 4)
        meta.device_dt_size[2] = d[4] & 0xF
        meta.device_type[3] = d[5]
        meta.device_dt_size[3] = d[6] >> 4
        meta.device_type[4] = ((d[6] & 0xF) << 4) | ((d[7] & 0xF0) >> 4)
        meta.device_dt_size[4] = d[7] & 0xF
        meta.exp_time_start = _u32_be(d[8:12])
        meta.exp_time_end = _u32_be(d[12:16])

    meta.group_size = (1 + meta.device_dt_size[0] + meta.device_dt_size[1]
                       + meta.device_dt_size[2] + meta.device_dt_size[3]
                       + meta.device_dt_size[4])
    meta.ok = True
    return meta


def _parse_icm42688(group: bytes, ascale: int, gscale: int) -> ImuSample:
    """移植 F_Analysis_Icm42688(_Int): 解析一组 16 字节 IMU 数据。"""
    u_time = _u32_be(group[0:4])
    ax = _s16_be(group[4:6])
    ay = _s16_be(group[6:8])
    az = _s16_be(group[8:10])
    gx = _s16_be(group[10:12])
    gy = _s16_be(group[12:14])
    gz = _s16_be(group[14:16])

    # 错误检测: acc 全 -1 或 gyroX == -32768 -> 数据无效, 清零(保留时间)
    if (ax == -1 and ay == -1 and az == -1) or gx == -32768:
        ax = ay = az = gx = gy = gz = 0

    ares = _ARES.get(ascale, _ARES[AFS_4G])
    gres = _GRES.get(gscale, _GRES[GFS_1000DPS])
    return ImuSample(
        u_time=u_time,
        acc=(ax * ares, ay * ares, az * ares),
        gyro=(gx * gres, gy * gres, gz * gres),
        raw_acc=(ax, ay, az),
        raw_gyro=(gx, gy, gz),
    )


def decode_frame(frame: np.ndarray,
                 ascale: int = AFS_4G,
                 gscale: int = GFS_1000DPS,
                 start_row_from_bottom: int = 3,
                 store_raw: bool = False) -> FrameMeta:
    """
    解码一帧图像编码区。

    frame: (H, W, C) uint8, 语义同厂商 raw rgb_data (第 0 行 = Vindex 0)。
           对 OpenCV (top-down) 帧, capture 程序会先 np.flipud 转成该语义。
    返回 FrameMeta (含帧曝光时间戳与 IMU 采样列表)。
    """
    if frame.ndim == 2:
        frame = frame[:, :, None]
    h, w, c = frame.shape

    start_row = h - start_row_from_bottom     # C++: startRowIndex = height - 3

    # ---- 第一轮: 先解出第 0 组 (>=16 字节) ----
    frame_buf = bytearray()
    line_index = 0
    total_size = GROUP_DT_SIZE
    while line_index < total_size:
        row_index = start_row - line_index * CODE_VSIZE
        if row_index < 0 or row_index > h:
            break
        seg = _line_decode(frame, row_index, c, w, CODE_HSIZE)
        frame_buf.extend(seg)
        if total_size <= len(frame_buf):
            break
        line_index += 1

    if len(frame_buf) < 16:
        return FrameMeta()                    # 没解到编码区

    meta = _group0_analysis(bytes(frame_buf[:16]))
    if not meta.ok:
        return meta

    # ---- 第二轮: 解出剩余组 ----
    total_size = GROUP_DT_SIZE * meta.group_size
    line_index += 1
    while line_index < total_size:
        row_index = start_row - line_index * CODE_VSIZE
        if row_index < 0 or row_index > h:
            break
        seg = _line_decode(frame, row_index, c, w, CODE_HSIZE)
        frame_buf.extend(seg)
        if total_size <= len(frame_buf):
            break
        line_index += 1

    if store_raw:
        meta.raw_groups = bytes(frame_buf)

    # ---- 解析各设备 (这里只处理 ICM42688) ----
    group_index = 1
    for dev in range(5):
        dev_type = meta.device_type[dev]
        dev_n = meta.device_dt_size[dev]
        for k in range(dev_n):
            off = (group_index + k) * GROUP_DT_SIZE
            grp = bytes(frame_buf[off:off + GROUP_DT_SIZE])
            if len(grp) < GROUP_DT_SIZE:
                break
            if dev_type == EDEV_ICM42688:
                meta.imu.append(_parse_icm42688(grp, ascale, gscale))
        group_index += dev_n

    return meta


# --------------------------------------------------------------------------
# 纯 Python BMP 读取 (仅用于离线验证, 不依赖 OpenCV)
# --------------------------------------------------------------------------
def read_bmp(path: str) -> np.ndarray:
    """读取 24/32bit BMP, 返回 (H, W, C) uint8, 顺序与厂商 raw rgb_data 一致
    (即数组第 0 行 = 文件像素数据第 0 行 = BMP bottom-up 的最底行)。"""
    import struct
    with open(path, "rb") as f:
        data = f.read()
    assert data[0:2] == b"BM", "不是 BMP 文件"
    offset = struct.unpack_from("<I", data, 10)[0]
    width = struct.unpack_from("<i", data, 18)[0]
    height = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    c = bpp // 8
    row_stride = ((width * c + 3) // 4) * 4    # BMP 行按 4 字节对齐
    h_abs = abs(height)
    pix = np.frombuffer(data, dtype=np.uint8, count=row_stride * h_abs,
                        offset=offset)
    pix = pix.reshape(h_abs, row_stride)[:, :width * c]
    arr = pix.reshape(h_abs, width, c)
    return arr


if __name__ == "__main__":
    import sys
    p = sys.argv[1] if len(sys.argv) > 1 else \
        "../TimeStamp_Data_Decode_DemoCode_V010002/TST_3D_Cam_Image_Decode/4000x1200_0_0_10.bmp"
    img = read_bmp(p)
    print(f"BMP shape = {img.shape}")
    m = decode_frame(img, store_raw=True)
    print(f"ok={m.ok} protocol={m.protocol_type} group_size={m.group_size}")
    print(f"exp_start={m.exp_time_start} us  exp_end={m.exp_time_end} us  "
          f"exposure={m.exposure_us} us")
    print(f"IMU samples: {len(m.imu)}")
    for i, s in enumerate(m.imu):
        print(f"  [{i:02d}] t={s.u_time} us  acc(mg)=({s.acc[0]:.2f},"
              f"{s.acc[1]:.2f},{s.acc[2]:.2f})  gyro(dps)=({s.gyro[0]:.3f},"
              f"{s.gyro[1]:.3f},{s.gyro[2]:.3f})")
