#include "encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>

struct Encoder {
    AVCodecContext *ctx;
    AVPacket *pkt;
    FILE *fp;
    long frames;
};

static void set_err(char *errbuf, int errcap, const char *msg)
{
    if (errbuf && errcap > 0) {
        strncpy(errbuf, msg, errcap - 1);
        errbuf[errcap - 1] = '\0';
    }
}

Encoder *encoder_create(const char *encoder_name, int width, int height,
                        int fps, int bitrate_kbps, const char *out_path,
                        char *errbuf, int errcap)
{
    const AVCodec *codec = avcodec_find_encoder_by_name(encoder_name);
    if (!codec) {
        set_err(errbuf, errcap, "找不到指定的 HEVC 编码器 (确认 ffmpeg 编译时带了该编码器)");
        return NULL;
    }

    Encoder *e = calloc(1, sizeof(*e));
    if (!e) {
        set_err(errbuf, errcap, "内存不足");
        return NULL;
    }

    e->ctx = avcodec_alloc_context3(codec);
    if (!e->ctx) {
        set_err(errbuf, errcap, "avcodec_alloc_context3 失败");
        free(e);
        return NULL;
    }

    AVCodecContext *c = e->ctx;
    c->width = width;
    c->height = height;
    c->pix_fmt = AV_PIX_FMT_NV12;          /* AMF 首选 NV12 */
    c->time_base = (AVRational){1, fps};
    c->framerate = (AVRational){fps, 1};
    c->gop_size = fps;                      /* 每秒一个 I 帧 */
    c->max_b_frames = 0;                    /* 无 B 帧: dts==pts, remux 简单, 低延迟 */
    if (bitrate_kbps > 0)
        c->bit_rate = (int64_t)bitrate_kbps * 1000;

    /* 针对实时录制的若干常用选项 (不同编码器会忽略不认识的项) */
    AVDictionary *opts = NULL;
    if (strcmp(encoder_name, "hevc_amf") == 0) {
        av_dict_set(&opts, "usage", "lowlatency", 0);
        av_dict_set(&opts, "quality", "speed", 0);
        if (bitrate_kbps > 0)
            av_dict_set(&opts, "rc", "cbr", 0);
    } else if (strcmp(encoder_name, "libx265") == 0) {
        av_dict_set(&opts, "preset", "ultrafast", 0);
        av_dict_set(&opts, "x265-params", "log-level=error", 0);
    }

    int ret = avcodec_open2(c, codec, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char eb[128];
        av_strerror(ret, eb, sizeof(eb));
        char msg[256];
        snprintf(msg, sizeof(msg), "avcodec_open2(%s) 失败: %s", encoder_name, eb);
        set_err(errbuf, errcap, msg);
        avcodec_free_context(&e->ctx);
        free(e);
        return NULL;
    }

    e->pkt = av_packet_alloc();
    e->fp = fopen(out_path, "wb");
    if (!e->pkt || !e->fp) {
        set_err(errbuf, errcap, "分配 packet 或打开输出文件失败");
        encoder_destroy(e);
        return NULL;
    }
    return e;
}

static int drain_packets(Encoder *e)
{
    int ret = 0;
    while (ret >= 0) {
        ret = avcodec_receive_packet(e->ctx, e->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return ret;
        fwrite(e->pkt->data, 1, e->pkt->size, e->fp);
        e->frames++;
        av_packet_unref(e->pkt);
    }
    return 0;
}

int encoder_send(Encoder *e, AVFrame *frame)
{
    int ret = avcodec_send_frame(e->ctx, frame);
    if (ret < 0)
        return ret;
    return drain_packets(e);
}

long encoder_finish(Encoder *e)
{
    avcodec_send_frame(e->ctx, NULL);   /* 进入冲刷 */
    drain_packets(e);
    if (e->fp) {
        fflush(e->fp);
        fclose(e->fp);
        e->fp = NULL;
    }
    return e->frames;
}

long encoder_frames(Encoder *e)
{
    return e ? e->frames : 0;
}

void encoder_destroy(Encoder *e)
{
    if (!e)
        return;
    if (e->fp)
        fclose(e->fp);
    av_packet_free(&e->pkt);
    avcodec_free_context(&e->ctx);
    free(e);
}
