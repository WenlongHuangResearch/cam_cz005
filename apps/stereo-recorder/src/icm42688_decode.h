/*
 * icm42688_decode.h —— 从图像底部"编码区"解出帧曝光时间戳 + IMU 采样。
 *
 * 移植自厂商 TimeStamp_Data_Decode_DemoCode (icm_decode.cpp / icm42688_decode.h)。
 *
 * 编码区规则 (Nori 3D Camera User Guide 第 6 节):
 *   - 每帧 12 组, 每组 16 字节; 8x8 黑白块编码。
 *   - 第 1 组: 协议头 + 帧曝光起止时间戳 (各 4B, 大端)。
 *   - 第 2~12 组: IMU 时间戳(4B) + acc/gyro 各 XYZ (int16 大端)。
 *   - 时间戳同一时钟域, 单位 us, 32bit (会回绕)。
 *
 * 解码只用图像的一个 8bit 灰度平面 (亮度 Y 即可, 黑白块在 Y 上区分最清晰)。
 */
#ifndef ICM42688_DECODE_H
#define ICM42688_DECODE_H

#include <stdint.h>

#define IMU_MAX_SAMPLES 64

/* IMU 量纲选择 (与厂商 demo 默认一致) */
#define AFS_4G        0x02
#define GFS_1000DPS   0x01

typedef struct {
    uint32_t u_time;       /* 采样时间戳, us (32bit 原始) */
    double acc[3];         /* x,y,z 单位 mg (1/1000 g) */
    double gyro[3];        /* x,y,z 单位 dps */
    int16_t raw_acc[3];
    int16_t raw_gyro[3];
} ImuSample;

typedef struct {
    int ok;                /* 是否解码成功 */
    int protocol_type;
    int device_type[5];
    int device_dt_size[5];
    uint32_t exp_time_start;  /* 帧曝光开始, us */
    uint32_t exp_time_end;    /* 帧曝光结束 (EE, 即帧时间戳), us */
    int group_size;
    int n_imu;
    ImuSample imu[IMU_MAX_SAMPLES];
} FrameMeta;

/* 该帧曝光时长 (us), 处理 32bit 回绕。 */
static inline uint32_t frame_exposure_us(const FrameMeta *m)
{
    return (uint32_t)(m->exp_time_end - m->exp_time_start);
}

/*
 * 从单个 8bit 平面解码编码区。
 *   plane    : 灰度/亮度平面首地址 (例如解码后 AVFrame 的 Y 平面 data[0])
 *   width    : 图像宽 (像素)
 *   height   : 图像高 (像素)
 *   linesize : 平面每行字节跨度 (stride, 可能 > width)
 *   ascale/gscale: IMU 量纲 (AFS_4G / GFS_1000DPS 等)
 *   flip     : 0=平面第 0 行为顶部 (厂商 raw 语义); 1=上下翻转读取
 *   out      : 输出解码结果
 * 返回 1 成功, 0 失败 (out->ok 同步置位)。
 */
int icm_decode_plane(const uint8_t *plane, int width, int height, int linesize,
                     int ascale, int gscale, int flip, FrameMeta *out);

#endif /* ICM42688_DECODE_H */
