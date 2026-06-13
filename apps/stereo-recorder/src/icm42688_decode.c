#include "icm42688_decode.h"

#include <string.h>

/* ---- 编码参数 (同厂商 demo) ---- */
#define GROUP_DT_SIZE 16     /* 每组字节数 */
#define CODE_VSIZE    8      /* 编码块高 (行步进) */
#define CODE_USIZE    8      /* 编码块宽 (列步进) */
#define VAL_0         50     /* bit0 阈值 */
#define VAL_1         220    /* bit1 / 空白阈值 */
#define START_ROW_FROM_BOTTOM 3
#define EDEV_ICM42688 1
#define FRAMEBUF_CAP  8192   /* 解码字节流缓冲上限 */

static uint32_t u32_be(const uint8_t *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static int16_t s16_be(const uint8_t *b)
{
    uint16_t v = (uint16_t)((b[0] << 8) | b[1]);
    return (int16_t)v;
}

/* IMU 量纲查表 (LSB -> 物理量) */
static double ares_of(int ascale)
{
    switch (ascale) {
    case 0x03: return 2000.0 / 32768.0;
    case 0x02: return 4000.0 / 32768.0;
    case 0x01: return 8000.0 / 32768.0;
    case 0x00: return 16000.0 / 32768.0;
    default:   return 4000.0 / 32768.0;
    }
}

static double gres_of(int gscale)
{
    switch (gscale) {
    case 0x07: return 15.125 / 32768.0;
    case 0x06: return 31.25 / 32768.0;
    case 0x05: return 62.5 / 32768.0;
    case 0x04: return 125.0 / 32768.0;
    case 0x03: return 250.0 / 32768.0;
    case 0x02: return 500.0 / 32768.0;
    case 0x01: return 1000.0 / 32768.0;
    case 0x00: return 2000.0 / 32768.0;
    default:   return 1000.0 / 32768.0;
    }
}

/* 取得第 row_index 行 (考虑 flip) 的平面行首指针; 越界返回 NULL。 */
static const uint8_t *row_ptr(const uint8_t *plane, int height, int linesize,
                              int row_index, int flip)
{
    if (row_index < 0 || row_index >= height)
        return NULL;
    int r = flip ? (height - 1 - row_index) : row_index;
    return plane + (size_t)r * linesize;
}

/*
 * 移植 F_LineDecode: 从某一行解出字节流, 追加到 out, 返回新增字节数。
 * 一行是若干 8px 黑白块, 每块代表 1 bit, 8 bit 拼成 1 字节 (LSB first)。
 */
static int line_decode(const uint8_t *plane, int width, int height,
                       int linesize, int row_index, int flip,
                       uint8_t *out, int out_cap)
{
    const uint8_t *row = row_ptr(plane, height, linesize, row_index, flip);
    if (!row)
        return 0;

    /* 起始空白校验: 前两像素必须为白 (>VAL_1) */
    if (!(row[0] > VAL_1 && row[1] > VAL_1))
        return 0;

    /* 找编码起始列: 全宽内第一个非白列 (左侧可能有白边距), 右移半块到块中心。
     * 注意: 必须扫整幅宽度 (与厂商 F_LineDecode 的 for(ind<h_size) 一致),
     * 否则白边距宽于一个块时会找不到起始列。 */
    int start = -1;
    for (int ind = 0; ind < width; ind++) {
        if (row[ind] < VAL_1) {
            start = ind + (CODE_USIZE >> 1);
            break;
        }
    }
    if (start < 0)
        return 0;

    /* 逐块取 bit: 块中心相邻两像素之和判 0/1, 遇空白结束 */
    int bits[8192];
    int nbits = 0;
    int max_bits = (int)(sizeof(bits) / sizeof(bits[0]));
    for (int ind = start; ind + 1 < width && nbits < max_bits;
         ind += CODE_USIZE) {
        int v = (int)row[ind] + (int)row[ind + 1];
        if (v < (VAL_0 << 1))
            bits[nbits++] = 0;
        else if (v < (VAL_1 << 1))
            bits[nbits++] = 1;
        else
            break;
    }

    int n_bytes = nbits / 8;
    int written = 0;
    for (int i = 0; i < n_bytes && written < out_cap; i++) {
        int val = 0;
        for (int b = 0; b < 8; b++)
            val |= bits[i * 8 + b] << b;
        out[written++] = (uint8_t)val;
    }
    return written;
}

/* 移植 F_Group_0_Analysis: 解析第 0 组 (协议头 + 帧曝光时间戳)。 */
static void group0_analysis(const uint8_t *d, FrameMeta *m)
{
    if (memcmp(d, d + 8, 8) == 0) {
        /* 旧协议: byte0~7 == byte8~15 */
        m->protocol_type = 0;
        m->device_type[0] = EDEV_ICM42688;
        m->device_dt_size[0] = 11;
        m->exp_time_start = u32_be(d);
        m->exp_time_end = u32_be(d + 4);
    } else {
        m->protocol_type = d[0] >> 4;
        m->device_type[0] = ((d[0] & 0xF) << 4) | ((d[1] & 0xF0) >> 4);
        m->device_dt_size[0] = d[1] & 0xF;
        m->device_type[1] = d[2];
        m->device_dt_size[1] = d[3] >> 4;
        m->device_type[2] = ((d[3] & 0xF) << 4) | ((d[4] & 0xF0) >> 4);
        m->device_dt_size[2] = d[4] & 0xF;
        m->device_type[3] = d[5];
        m->device_dt_size[3] = d[6] >> 4;
        m->device_type[4] = ((d[6] & 0xF) << 4) | ((d[7] & 0xF0) >> 4);
        m->device_dt_size[4] = d[7] & 0xF;
        m->exp_time_start = u32_be(d + 8);
        m->exp_time_end = u32_be(d + 12);
    }
    m->group_size = 1 + m->device_dt_size[0] + m->device_dt_size[1] +
                    m->device_dt_size[2] + m->device_dt_size[3] +
                    m->device_dt_size[4];
    m->ok = 1;
}

/* 移植 F_Analysis_Icm42688: 解析一组 16 字节 IMU 数据。 */
static void parse_icm42688(const uint8_t *g, int ascale, int gscale,
                           ImuSample *s)
{
    int16_t ax = s16_be(g + 4), ay = s16_be(g + 6), az = s16_be(g + 8);
    int16_t gx = s16_be(g + 10), gy = s16_be(g + 12), gz = s16_be(g + 14);

    /* 错误检测: acc 全 -1 或 gyroX == -32768 -> 数据无效, 清零(保留时间) */
    if ((ax == -1 && ay == -1 && az == -1) || gx == -32768)
        ax = ay = az = gx = gy = gz = 0;

    double ares = ares_of(ascale);
    double gres = gres_of(gscale);
    s->u_time = u32_be(g);
    s->raw_acc[0] = ax; s->raw_acc[1] = ay; s->raw_acc[2] = az;
    s->raw_gyro[0] = gx; s->raw_gyro[1] = gy; s->raw_gyro[2] = gz;
    s->acc[0] = ax * ares; s->acc[1] = ay * ares; s->acc[2] = az * ares;
    s->gyro[0] = gx * gres; s->gyro[1] = gy * gres; s->gyro[2] = gz * gres;
}

int icm_decode_plane(const uint8_t *plane, int width, int height, int linesize,
                     int ascale, int gscale, int flip, FrameMeta *out)
{
    memset(out, 0, sizeof(*out));

    uint8_t fb[FRAMEBUF_CAP];
    int fb_len = 0;
    int start_row = height - START_ROW_FROM_BOTTOM;

    /* ---- 第一轮: 先解出第 0 组 (>=16 字节) ---- */
    int line_index = 0;
    int total = GROUP_DT_SIZE;
    while (line_index < total) {
        int row_index = start_row - line_index * CODE_VSIZE;
        if (row_index < 0 || row_index > height)
            break;
        fb_len += line_decode(plane, width, height, linesize, row_index, flip,
                              fb + fb_len, FRAMEBUF_CAP - fb_len);
        if (total <= fb_len)
            break;
        line_index++;
    }
    if (fb_len < 16)
        return 0;

    group0_analysis(fb, out);
    if (!out->ok)
        return 0;

    /* ---- 第二轮: 解出剩余组 ---- */
    total = GROUP_DT_SIZE * out->group_size;
    line_index++;
    while (line_index < total) {
        int row_index = start_row - line_index * CODE_VSIZE;
        if (row_index < 0 || row_index > height)
            break;
        fb_len += line_decode(plane, width, height, linesize, row_index, flip,
                              fb + fb_len, FRAMEBUF_CAP - fb_len);
        if (total <= fb_len)
            break;
        line_index++;
    }

    /* ---- 解析各设备 (这里只处理 ICM42688) ---- */
    int group_index = 1;
    for (int dev = 0; dev < 5; dev++) {
        int dev_type = out->device_type[dev];
        int dev_n = out->device_dt_size[dev];
        for (int k = 0; k < dev_n; k++) {
            int off = (group_index + k) * GROUP_DT_SIZE;
            if (off + GROUP_DT_SIZE > fb_len)
                break;
            if (dev_type == EDEV_ICM42688 && out->n_imu < IMU_MAX_SAMPLES)
                parse_icm42688(fb + off, ascale, gscale,
                               &out->imu[out->n_imu++]);
        }
        group_index += dev_n;
    }
    return 1;
}
