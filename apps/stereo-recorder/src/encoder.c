#include "encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>

#ifndef _WIN32
#include <pthread.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#endif

#ifndef _WIN32
static pthread_mutex_t g_mpp_mu = PTHREAD_MUTEX_INITIALIZER;
#endif

struct Encoder {
    AVCodecContext *ctx;
    AVPacket *pkt;
    FILE *fp;
    long frames;
#ifndef _WIN32
    int use_mpp;
    MppCtx mpp_ctx;
    MppApi *mpp;
    MppEncCfg mpp_cfg;
    MppBufferGroup mpp_buf_grp;
    MppBuffer mpp_buf;
    MppBuffer mpp_pkt_buf;
    size_t mpp_frame_size;
    int width, height, fps, bitrate_kbps;
#endif
};

static void set_err(char *errbuf, int errcap, const char *msg)
{
    if (errbuf && errcap > 0) {
        strncpy(errbuf, msg, errcap - 1);
        errbuf[errcap - 1] = '\0';
    }
}

#ifndef _WIN32
static int mpp_write_packet(Encoder *e, MppPacket pkt)
{
    if (!pkt)
        return 0;
    void *pos = mpp_packet_get_pos(pkt);
    size_t len = mpp_packet_get_length(pkt);
    if (pos && len > 0) {
        fwrite(pos, 1, len, e->fp);
        e->frames++;
    }
    mpp_packet_deinit(&pkt);
    return 0;
}

static int mpp_write_headers(Encoder *e)
{
    MppPacket pkt = NULL;
    if (mpp_packet_init_with_buffer(&pkt, e->mpp_pkt_buf) != MPP_OK)
        return -1;
    mpp_packet_set_length(pkt, 0);
    if (e->mpp->control(e->mpp_ctx, MPP_ENC_GET_HDR_SYNC, pkt) != MPP_OK) {
        mpp_packet_deinit(&pkt);
        return -1;
    }
    return mpp_write_packet(e, pkt);
}

static Encoder *encoder_create_mpp(int width, int height, int fps,
                                   enum AVPixelFormat pix_fmt, int bitrate_kbps,
                                   const char *out_path, char *errbuf, int errcap)
{
    if (pix_fmt != AV_PIX_FMT_NV12) {
        set_err(errbuf, errcap, "hevc_mpp 需要 NV12 输入");
        return NULL;
    }

    Encoder *e = calloc(1, sizeof(*e));
    if (!e) {
        set_err(errbuf, errcap, "内存不足");
        return NULL;
    }
    e->use_mpp = 1;
    e->width = width;
    e->height = height;
    e->fps = fps;
    e->bitrate_kbps = bitrate_kbps;
    e->mpp_frame_size = (size_t)width * height * 3 / 2;

    pthread_mutex_lock(&g_mpp_mu);

    e->fp = fopen(out_path, "wb");
    if (!e->fp) {
        set_err(errbuf, errcap, "打开输出文件失败");
        pthread_mutex_unlock(&g_mpp_mu);
        encoder_destroy(e);
        return NULL;
    }

    if (mpp_create(&e->mpp_ctx, &e->mpp) != MPP_OK ||
        mpp_init(e->mpp_ctx, MPP_CTX_ENC, MPP_VIDEO_CodingHEVC) != MPP_OK) {
        set_err(errbuf, errcap, "初始化 Rockchip MPP HEVC 编码器失败");
        pthread_mutex_unlock(&g_mpp_mu);
        encoder_destroy(e);
        return NULL;
    }

    if (mpp_enc_cfg_init(&e->mpp_cfg) != MPP_OK) {
        set_err(errbuf, errcap, "mpp_enc_cfg_init 失败");
        pthread_mutex_unlock(&g_mpp_mu);
        encoder_destroy(e);
        return NULL;
    }

    int bps = bitrate_kbps > 0 ? bitrate_kbps * 1000 : 20000000;
    mpp_enc_cfg_set_s32(e->mpp_cfg, "prep:width", width);
    mpp_enc_cfg_set_s32(e->mpp_cfg, "prep:height", height);
    mpp_enc_cfg_set_s32(e->mpp_cfg, "prep:hor_stride", width);
    mpp_enc_cfg_set_s32(e->mpp_cfg, "prep:ver_stride", height);
    mpp_enc_cfg_set_s32(e->mpp_cfg, "prep:format", MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(e->mpp_cfg, "rc:mode", 1); /* CBR */
    mpp_enc_cfg_set_s32(e->mpp_cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(e->mpp_cfg, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(e->mpp_cfg, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(e->mpp_cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(e->mpp_cfg, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(e->mpp_cfg, "rc:fps_out_denorm", 1);
    mpp_enc_cfg_set_s32(e->mpp_cfg, "rc:gop", fps);
    mpp_enc_cfg_set_s32(e->mpp_cfg, "rc:bps_target", bps);
    mpp_enc_cfg_set_s32(e->mpp_cfg, "rc:bps_max", bps * 17 / 16);
    mpp_enc_cfg_set_s32(e->mpp_cfg, "rc:bps_min", bps * 15 / 16);
    mpp_enc_cfg_set_s32(e->mpp_cfg, "codec:type", MPP_VIDEO_CodingHEVC);

    if (e->mpp->control(e->mpp_ctx, MPP_ENC_SET_CFG, e->mpp_cfg) != MPP_OK) {
        set_err(errbuf, errcap, "MPP_ENC_SET_CFG 失败");
        pthread_mutex_unlock(&g_mpp_mu);
        encoder_destroy(e);
        return NULL;
    }

    RK_S64 nonblock = MPP_TIMEOUT_NON_BLOCK;
    e->mpp->control(e->mpp_ctx, MPP_SET_OUTPUT_TIMEOUT, &nonblock);

    MppBufferType buf_type = MPP_BUFFER_TYPE_ION;
    if (mpp_buffer_group_get_internal(&e->mpp_buf_grp, buf_type) != MPP_OK ||
        mpp_buffer_get(e->mpp_buf_grp, &e->mpp_buf, e->mpp_frame_size) != MPP_OK ||
        mpp_buffer_get(e->mpp_buf_grp, &e->mpp_pkt_buf, e->mpp_frame_size / 2) != MPP_OK) {
        set_err(errbuf, errcap, "分配 MPP 输入 buffer 失败");
        pthread_mutex_unlock(&g_mpp_mu);
        encoder_destroy(e);
        return NULL;
    }

    if (mpp_write_headers(e) < 0) {
        set_err(errbuf, errcap, "写入 MPP HEVC 头失败");
        pthread_mutex_unlock(&g_mpp_mu);
        encoder_destroy(e);
        return NULL;
    }

    pthread_mutex_unlock(&g_mpp_mu);
    return e;
}
#endif

Encoder *encoder_create(const char *encoder_name, int width, int height,
                        int fps, enum AVPixelFormat pix_fmt, int bitrate_kbps, const char *out_path,
                        char *errbuf, int errcap)
{
#ifndef _WIN32
    if (!strcmp(encoder_name, "hevc_mpp"))
        return encoder_create_mpp(width, height, fps, pix_fmt, bitrate_kbps,
                                  out_path, errbuf, errcap);
#endif

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
    c->pix_fmt = pix_fmt;
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
#ifndef _WIN32
    if (e->use_mpp) {
        uint8_t *dst = mpp_buffer_get_ptr(e->mpp_buf);
        if (!dst)
            return -1;
        for (int y = 0; y < e->height; y++)
            memcpy(dst + (size_t)y * e->width,
                   frame->data[0] + (size_t)y * frame->linesize[0],
                   e->width);
        uint8_t *dst_uv = dst + (size_t)e->width * e->height;
        for (int y = 0; y < e->height / 2; y++)
            memcpy(dst_uv + (size_t)y * e->width,
                   frame->data[1] + (size_t)y * frame->linesize[1],
                   e->width);
        MppFrame mfrm = NULL;
        if (mpp_frame_init(&mfrm) != MPP_OK)
            return -1;
        mpp_frame_set_width(mfrm, e->width);
        mpp_frame_set_height(mfrm, e->height);
        mpp_frame_set_hor_stride(mfrm, e->width);
        mpp_frame_set_ver_stride(mfrm, e->height);
        mpp_frame_set_fmt(mfrm, MPP_FMT_YUV420SP);
        mpp_frame_set_pts(mfrm, frame->pts);
        mpp_frame_set_buffer(mfrm, e->mpp_buf);

        MPP_RET ret = e->mpp->encode_put_frame(e->mpp_ctx, mfrm);
        mpp_frame_deinit(&mfrm);
        if (ret != MPP_OK)
            return -1;
        MppPacket pkt = NULL;
        ret = e->mpp->encode_get_packet(e->mpp_ctx, &pkt);
        if (ret == MPP_OK && pkt)
            mpp_write_packet(e, pkt);
        return 0;
    }
#endif

    int ret = avcodec_send_frame(e->ctx, frame);
    if (ret < 0)
        return ret;
    return drain_packets(e);
}

long encoder_finish(Encoder *e)
{
#ifndef _WIN32
    if (e->use_mpp) {
        MppFrame mfrm = NULL;
        if (mpp_frame_init(&mfrm) == MPP_OK) {
            mpp_frame_set_eos(mfrm, 1);
            e->mpp->encode_put_frame(e->mpp_ctx, mfrm);
            mpp_frame_deinit(&mfrm);
        }
        for (int i = 0; i < 256; i++) {
            MppPacket pkt = NULL;
            if (e->mpp->encode_get_packet(e->mpp_ctx, &pkt) != MPP_OK || !pkt)
                break;
            int eos = mpp_packet_get_eos(pkt);
            mpp_write_packet(e, pkt);
            if (eos)
                break;
        }
        if (e->fp) {
            fflush(e->fp);
            fclose(e->fp);
            e->fp = NULL;
        }
        return e->frames;
    }
#endif

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
#ifndef _WIN32
    if (e->mpp_buf)
        mpp_buffer_put(e->mpp_buf);
    if (e->mpp_pkt_buf)
        mpp_buffer_put(e->mpp_pkt_buf);
    if (e->mpp_buf_grp)
        mpp_buffer_group_put(e->mpp_buf_grp);
    if (e->mpp_cfg)
        mpp_enc_cfg_deinit(e->mpp_cfg);
    if (e->mpp_ctx)
        mpp_destroy(e->mpp_ctx);
#endif
    av_packet_free(&e->pkt);
    avcodec_free_context(&e->ctx);
    free(e);
}
