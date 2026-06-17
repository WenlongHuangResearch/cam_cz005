/*
 * encoder.h —— HEVC(H.265) 编码器封装。
 *
 * 默认用 AMD 硬件编码器 hevc_amf (适配本机 Radeon 780M), 也可传入其它
 * 编码器名 (如 libx265 软件编码)。编码结果以 Annex-B 裸流写入指定文件,
 * 之后由 hevc2mp4 程序 remux 成 .mp4。
 */
#ifndef ENCODER_H
#define ENCODER_H

#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

typedef struct Encoder Encoder;

/*
 * 创建编码器并打开输出文件。
 *   encoder_name : 编码器名, 如 "hevc_amf" / "libx265"
 *   width,height : 编码分辨率 (左右半幅各自的尺寸)
 *   fps          : 帧率
 *   pix_fmt      : 输入帧像素格式
 *   bitrate_kbps : 目标码率 (kbps); <=0 时使用默认
 *   out_path     : 输出 .hevc 文件路径
 *   errbuf/errcap: 失败时写入错误描述
 * 成功返回 Encoder*; 失败返回 NULL。
 */
Encoder *encoder_create(const char *encoder_name, int width, int height,
                        int fps, enum AVPixelFormat pix_fmt, int bitrate_kbps, const char *out_path,
                        char *errbuf, int errcap);

/* 编码一帧 (NV12, pts 已设置), 内部把产生的包写入文件。返回 0 成功, <0 失败。 */
int encoder_send(Encoder *e, AVFrame *frame);

/* 冲刷编码器残留帧并关闭输出文件。返回写出的总帧数。 */
long encoder_finish(Encoder *e);

void encoder_destroy(Encoder *e);

/* 已成功编码写出的包数 (帧数)。 */
long encoder_frames(Encoder *e);

#endif /* ENCODER_H */
