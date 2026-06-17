/*
 * capture_record.c —— 双目 (DECXIN / Nori 3D) UVC 相机 C 版录制主程序。
 *
 * 流水线 (生产者-消费者, 环形队列解耦):
 *
 *   [采集线程]  dshow/v4l2 取 4000x1200 MJPG@60 -> 解码成 YUV
 *               -> 整帧 Y 平面解出 帧曝光时间戳 + IMU (写 CSV)
 *               -> 把整帧按宽度切成 左半 / 右半, 各转成 NV12
 *               -> 分别 push 进 左/右两个环形队列
 *                         |                         |
 *   [左编码线程] <--------+        [右编码线程] <----+
 *     pop -> HEVC 编码 -> left.hevc        pop -> HEVC 编码 -> right.hevc
 *
 * "对称": 左右两路是完全对称的两个独立队列 + 编码线程, 互不影响。
 * 采集线程用 rb_push (队满丢最旧帧) 永不被慢编码器阻塞, 保证 USB 不丢帧。
 *
 * 录完得到 left.hevc / right.hevc (Annex-B 裸流), 再用 hevc2mp4 转 .mp4。
 */
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

#include "encoder.h"
#include "icm42688_decode.h"
#include "ring_buffer.h"

/* ---------------- 队列元素: 一个待编码的半幅帧 ---------------- */
typedef struct {
    AVFrame *frame;       /* NV12 半幅帧 */
    int64_t frame_idx;
} FrameItem;

static void frame_item_free(void *p)
{
    FrameItem *it = (FrameItem *)p;
    if (!it)
        return;
    av_frame_free(&it->frame);
    free(it);
}

/* ---------------- 队列元素: 一个待解码的 MJPEG 包 (带采集序号) ---------------- */
typedef struct {
    AVPacket *pkt;
    int64_t seq;          /* 采集顺序号, 用于解码后重排 */
} PktItem;

static void packet_item_free(void *p)
{
    PktItem *it = (PktItem *)p;
    if (!it)
        return;
    av_packet_free(&it->pkt);
    free(it);
}

/* 把整帧某半幅转成编码器需要的像素格式 (定义在下方) */
static AVFrame *make_half_frame(struct SwsContext *sws, const AVFrame *src,
                                int x_off, int half_w, int height,
                                enum AVPixelFormat dst_fmt);

/* ============================================================================
 *  每一路 (左/右) 拆成两级流水线: [转换线程 sws] -> [编码输入队列] -> [编码线程]
 *  sws 与编码串在一条线程里会超 60fps 预算(16.67ms)、长录丢帧;
 *  拆成两级后各级单独都在预算内, 可持续 60fps。
 * ========================================================================== */

/* ---------------- 转换线程: 整幅引用 -> 切本路半幅 -> NV12 -> 推 nv12 队列 ---- */
typedef struct {
    RingBuffer *in;       /* 上游: 整幅解码帧引用 (FrameItem) */
    RingBuffer *out;      /* 下游: 本路 NV12 半幅帧 (FrameItem) */
    int width, height, x_off;
    enum AVPixelFormat dst_fmt;
    const char *tag;
    int64_t prof_sws, prof_n;
} ConvertArg;

/* MJPEG 解出的是 yuvj420p (JPEG 全范围), 直接喂 sws 会走"范围转换"慢路径。
 * 我们只想原样打包成 NV12, 故把 yuvj420p 视作 yuv420p, 命中 sws 无缩放快速拷贝。 */
static enum AVPixelFormat dejpeg_fmt(int fmt)
{
    switch (fmt) {
        case AV_PIX_FMT_YUVJ420P: return AV_PIX_FMT_YUV420P;
        case AV_PIX_FMT_YUVJ422P: return AV_PIX_FMT_YUV422P;
        case AV_PIX_FMT_YUVJ444P: return AV_PIX_FMT_YUV444P;
        default: return (enum AVPixelFormat)fmt;
    }
}

static void *convert_thread(void *varg)
{
    ConvertArg *a = (ConvertArg *)varg;
    struct SwsContext *sws = NULL;
    void *item;
    while (rb_pop(a->in, &item) == 0) {
        FrameItem *it = (FrameItem *)item;
        if (!sws) {
            sws = sws_getContext(a->width, a->height, dejpeg_fmt(it->frame->format),
                                 a->width, a->height, a->dst_fmt,
                                 SWS_POINT, NULL, NULL, NULL);
            if (!sws) {
                fprintf(stderr, "[%s转换] sws_getContext 失败\n", a->tag);
                frame_item_free(it);
                continue;
            }
        }
        int64_t ts0 = av_gettime_relative();
        AVFrame *out = make_half_frame(sws, it->frame, a->x_off, a->width,
                                       a->height, a->dst_fmt);
        a->prof_sws += av_gettime_relative() - ts0;
        a->prof_n++;
        if (out) {
            out->pts = it->frame_idx;
            FrameItem *oit = malloc(sizeof(*oit));
            oit->frame = out; oit->frame_idx = it->frame_idx;
            if (rb_push(a->out, oit) < 0) frame_item_free(oit);
        }
        frame_item_free(it);
    }
    if (sws) sws_freeContext(sws);
    rb_close(a->out);     /* 让下游编码线程收尾 */
    return NULL;
}

/* ---------------- 编码线程: 取 NV12 -> HEVC 编码 -> 写裸流 ---- */
typedef struct {
    RingBuffer *in;       /* 上游: NV12 半幅帧 (FrameItem) */
    char enc_name[64];
    char out_path[512];
    int width, height, fps, bitrate_kbps;
    enum AVPixelFormat pix_fmt;
    const char *tag;
    int ok;
    long frames;
    int64_t prof_enc, prof_n;
} EncodeArg;

static void *encode_thread(void *varg)
{
    EncodeArg *a = (EncodeArg *)varg;
    char err[256] = {0};
    Encoder *enc = encoder_create(a->enc_name, a->width, a->height, a->fps,
                                  a->pix_fmt, a->bitrate_kbps, a->out_path,
                                  err, sizeof(err));
    if (!enc) {
        fprintf(stderr, "[%s编码] 创建编码器失败: %s\n", a->tag, err);
        void *item;
        while (rb_pop(a->in, &item) == 0)
            frame_item_free(item);
        a->ok = 0;
        return NULL;
    }

    void *item;
    while (rb_pop(a->in, &item) == 0) {
        FrameItem *it = (FrameItem *)item;
        int64_t ts0 = av_gettime_relative();
        if (encoder_send(enc, it->frame) < 0)
            fprintf(stderr, "[%s编码] 帧 %lld 编码出错\n", a->tag,
                    (long long)it->frame_idx);
        a->prof_enc += av_gettime_relative() - ts0;
        a->prof_n++;
        frame_item_free(it);
    }
    a->frames = encoder_finish(enc);
    encoder_destroy(enc);
    a->ok = 1;
    return NULL;
}

/* ---------------- 32bit us 时间戳去回绕 ---------------- */
typedef struct {
    int has_last;
    uint32_t last;
    uint64_t base;
} Unwrap32;

static uint64_t unwrap32(Unwrap32 *u, uint32_t v)
{
    if (u->has_last && v < u->last && (u->last - v) > (1u << 31))
        u->base += (1ull << 32);
    u->last = v;
    u->has_last = 1;
    return u->base + v;
}

/* ---------------- 把整帧某半幅转成编码器输入帧 ---------------- */
/* x_off: 该半幅在整帧中的起始列。返回新分配的 AVFrame。 */
static AVFrame *make_half_frame(struct SwsContext *sws, const AVFrame *src,
                                int x_off, int half_w, int height,
                                enum AVPixelFormat dst_fmt)
{
    AVFrame *dst = av_frame_alloc();
    if (!dst)
        return NULL;
    dst->format = dst_fmt;
    dst->width = half_w;
    dst->height = height;
    if (av_frame_get_buffer(dst, 32) < 0) {
        av_frame_free(&dst);
        return NULL;
    }

    /* 源指针偏移到该半幅起始列 (yuv420p: Y 偏 x_off, U/V 偏 x_off/2) */
    const uint8_t *src_data[4] = {
        src->data[0] + x_off,
        src->data[1] + x_off / 2,
        src->data[2] + x_off / 2,
        NULL,
    };
    int src_linesize[4] = {
        src->linesize[0], src->linesize[1], src->linesize[2], 0,
    };
    sws_scale(sws, src_data, src_linesize, 0, height, dst->data, dst->linesize);
    return dst;
}

/* ============================================================================
 *  并行解码: 单线程 MJPEG 解码 ~32ms/帧 (4000x1200), 远超 60fps 预算, 且该
 *  解码器不支持 slice 多线程。MJPEG 是帧内编码、各帧独立, 故用「多个独立解码器
 *  实例」并行解码不同帧, 再按采集序号重排, 恢复顺序后交给后续 IMU/编码。
 *
 *  采集(主) -> [pkt_q] -> N×解码worker -> [Reorder 重排] -> 收集线程 -> rb_l/rb_r
 * ========================================================================== */

/* ---------------- 重排缓冲: 多生产者(解码worker)插入, 单消费者(收集线程)按序取 ---- */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  can_insert;   /* 有空位 */
    pthread_cond_t  can_emit;     /* 有新帧 / 生产者退出 */
    int      cap;
    int64_t *seq;                 /* 各槽位的采集序号 */
    AVFrame **frm;                /* 各槽位的解码帧 */
    int      count;
    int64_t  next_emit;           /* 下一个要输出的序号 */
    int      producers;           /* 仍在工作的解码worker数 */
} Reorder;

static Reorder *reorder_create(int cap, int producers)
{
    Reorder *r = calloc(1, sizeof(*r));
    r->cap = cap;
    r->seq = malloc(sizeof(int64_t) * cap);
    r->frm = calloc(cap, sizeof(AVFrame *));
    for (int i = 0; i < cap; i++) r->seq[i] = -1;
    r->next_emit = 0;
    r->producers = producers;
    pthread_mutex_init(&r->mu, NULL);
    pthread_cond_init(&r->can_insert, NULL);
    pthread_cond_init(&r->can_emit, NULL);
    return r;
}

static void reorder_destroy(Reorder *r)
{
    if (!r) return;
    for (int i = 0; i < r->cap; i++)
        if (r->frm[i]) av_frame_free(&r->frm[i]);
    pthread_mutex_destroy(&r->mu);
    pthread_cond_destroy(&r->can_insert);
    pthread_cond_destroy(&r->can_emit);
    free(r->seq); free(r->frm); free(r);
}

/* 解码worker插入一帧 (阻塞直到有空位)。cap 已保证 > 最大乱序跨度, 不会死锁。 */
static void reorder_insert(Reorder *r, int64_t seq, AVFrame *f)
{
    pthread_mutex_lock(&r->mu);
    while (r->count == r->cap)
        pthread_cond_wait(&r->can_insert, &r->mu);
    int i;
    for (i = 0; i < r->cap; i++)
        if (r->seq[i] < 0) break;
    r->seq[i] = seq;
    r->frm[i] = f;
    r->count++;
    pthread_cond_signal(&r->can_emit);
    pthread_mutex_unlock(&r->mu);
}

/* 一个解码worker退出: 生产者计数减一, 归零时唤醒收集线程收尾。 */
static void reorder_producer_done(Reorder *r)
{
    pthread_mutex_lock(&r->mu);
    r->producers--;
    if (r->producers == 0)
        pthread_cond_broadcast(&r->can_emit);
    pthread_mutex_unlock(&r->mu);
}

/* 收集线程按 next_emit 取下一帧。返回 NULL 表示全部结束。
 * 序号无空洞 (pkt_q 用阻塞push, 分配序号后绝不丢), 故缺帧只可能是"还没解出来"。 */
static AVFrame *reorder_next(Reorder *r, int64_t *out_seq)
{
    pthread_mutex_lock(&r->mu);
    for (;;) {
        int i, found = -1;
        for (i = 0; i < r->cap; i++)
            if (r->seq[i] == r->next_emit) { found = i; break; }
        if (found >= 0) {
            AVFrame *f = r->frm[found];
            *out_seq = r->seq[found];
            r->seq[found] = -1;
            r->frm[found] = NULL;
            r->count--;
            r->next_emit++;
            pthread_cond_signal(&r->can_insert);
            pthread_mutex_unlock(&r->mu);
            return f;
        }
        if (r->producers == 0 && r->count == 0) {     /* 全部解码worker已退出且取空 */
            pthread_mutex_unlock(&r->mu);
            return NULL;
        }
        pthread_cond_wait(&r->can_emit, &r->mu);
    }
}

/* ---------------- 解码 worker 线程 ---------------- */
typedef struct {
    RingBuffer  *pkt_q;
    Reorder     *reorder;
    const AVCodec *dec;
    AVCodecParameters *par;
    const char  *tag;
    int64_t      prof_decode, prof_n;   /* 本worker累计解码耗时 */
} DecodeWorkerArg;

static void *decode_worker(void *varg)
{
    DecodeWorkerArg *a = (DecodeWorkerArg *)varg;
    AVCodecContext *dctx = avcodec_alloc_context3(a->dec);
    avcodec_parameters_to_context(dctx, a->par);
    if (avcodec_open2(dctx, a->dec, NULL) < 0) {
        fprintf(stderr, "[解码%s] 打开解码器失败\n", a->tag ? a->tag : "");
        /* 抽干自己这份不可能, 直接标记退出 */
        reorder_producer_done(a->reorder);
        avcodec_free_context(&dctx);
        return NULL;
    }

    void *pitem;
    while (rb_pop(a->pkt_q, &pitem) == 0) {
        PktItem *pi = (PktItem *)pitem;
        int64_t ts0 = av_gettime_relative();
        int ret = avcodec_send_packet(dctx, pi->pkt);
        if (ret >= 0) {
            AVFrame *frame = av_frame_alloc();
            if (avcodec_receive_frame(dctx, frame) == 0) {
                a->prof_decode += av_gettime_relative() - ts0;
                a->prof_n++;
                reorder_insert(a->reorder, pi->seq, frame);   /* 所有权移交重排缓冲 */
            } else {
                av_frame_free(&frame);
            }
        }
        packet_item_free(pi);
    }

    avcodec_free_context(&dctx);
    reorder_producer_done(a->reorder);
    return NULL;
}

/* ---------------- 收集线程: 按序取帧 -> IMU/CSV -> 克隆推入两路编码队列 ---- */
typedef struct {
    Reorder    *reorder;
    RingBuffer *rb_l, *rb_r;
    int         no_imu;
    FILE       *frames_csv, *imu_csv;
    int64_t     t_start;
    /* 输出统计 */
    int64_t frame_idx, n_decoded_imu, n_fail_imu, first_cam_ts, last_cam_ts;
    int64_t prof_imu, prof_clone, prof_n;
} CollectorArg;

static void *collector_thread(void *varg)
{
    CollectorArg *a = (CollectorArg *)varg;
    Unwrap32 uw_frame = {0}, uw_imu = {0};
    int flip = 0, flip_detected = a->no_imu;
    a->first_cam_ts = -1; a->last_cam_ts = -1;

    AVFrame *frame;
    int64_t seq;
    while ((frame = reorder_next(a->reorder, &seq)) != NULL) {
        double host_t = (av_gettime_relative() - a->t_start) / 1e6;
        int64_t ts0 = av_gettime_relative();

        /* ---- IMU / 时间戳: 用整帧 Y 平面解码 (按序, unwrap 状态正确) ---- */
        if (!a->no_imu) {
            FrameMeta meta;
            if (!flip_detected) {
                if (icm_decode_plane(frame->data[0], frame->width, frame->height,
                                     frame->linesize[0], AFS_4G, GFS_1000DPS, 0, &meta)) {
                    flip = 0; flip_detected = 1;
                } else if (icm_decode_plane(frame->data[0], frame->width, frame->height,
                                            frame->linesize[0], AFS_4G, GFS_1000DPS, 1, &meta)) {
                    flip = 1; flip_detected = 1;
                }
                if (flip_detected)
                    printf("[编码区方向] %s\n", flip ? "需翻转" : "无需翻转");
            } else {
                icm_decode_plane(frame->data[0], frame->width, frame->height,
                                 frame->linesize[0], AFS_4G, GFS_1000DPS, flip, &meta);
            }

            if (meta.ok) {
                a->n_decoded_imu++;
                uint64_t exp_end = unwrap32(&uw_frame, meta.exp_time_end);
                if (a->first_cam_ts < 0) a->first_cam_ts = exp_end;
                a->last_cam_ts = exp_end;
                if (a->frames_csv)
                    fprintf(a->frames_csv, "%lld,%.6f,%u,%u,%u,%d\n",
                            (long long)a->frame_idx, host_t,
                            meta.exp_time_start, meta.exp_time_end,
                            frame_exposure_us(&meta), meta.n_imu);
                if (a->imu_csv)
                    for (int k = 0; k < meta.n_imu; k++) {
                        ImuSample *s = &meta.imu[k];
                        fprintf(a->imu_csv,
                                "%lld,%u,%llu,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f\n",
                                (long long)a->frame_idx, meta.exp_time_end,
                                (unsigned long long)unwrap32(&uw_imu, s->u_time),
                                s->acc[0], s->acc[1], s->acc[2],
                                s->gyro[0], s->gyro[1], s->gyro[2]);
                    }
            } else {
                a->n_fail_imu++;
            }
        }
        int64_t ts1 = av_gettime_relative();
        a->prof_imu += ts1 - ts0;

        /* ---- 把整幅解码帧「按引用」推入两路队列 ---- */
        AVFrame *fl = av_frame_clone(frame);
        AVFrame *fr = av_frame_clone(frame);
        if (fl) {
            FrameItem *it = malloc(sizeof(*it));
            it->frame = fl; it->frame_idx = a->frame_idx;
            if (rb_push(a->rb_l, it) < 0) frame_item_free(it);
        }
        if (fr) {
            FrameItem *it = malloc(sizeof(*it));
            it->frame = fr; it->frame_idx = a->frame_idx;
            if (rb_push(a->rb_r, it) < 0) frame_item_free(it);
        }
        a->prof_clone += av_gettime_relative() - ts1;
        a->prof_n++;
        a->frame_idx++;
        av_frame_free(&frame);   /* 释放重排缓冲交来的所有权 */
    }

    /* 关闭下游, 让编码线程优雅收尾 */
    rb_close(a->rb_l);
    rb_close(a->rb_r);
    return NULL;
}

/* ---------------- 全局停止标志 (Ctrl+C) ---------------- */
static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s) { (void)s; g_stop = 1; }

/* ---------------- 配置 ---------------- */
typedef struct {
    char device[256];
    char enc_name[64];
    char out_dir[256];
    int width, height, fps;
    int code_width;       /* 左侧编码区宽度 (像素), 切左右图像时跳过 */
    int bitrate_kbps;
    double seconds;
    int queue_cap;
    int dec_threads;      /* 并行 MJPEG 解码器实例数 (0=自动) */
    int no_imu;
} Config;

static void usage(const char *prog)
{
    printf("用法: %s [选项]\n", prog);
#ifdef _WIN32
    printf("  --device NAME   dshow 设备名 (默认 \"DECXIN Camera\")\n");
#else
    printf("  --device PATH   v4l2 设备路径 (默认 /dev/video0)\n");
#endif
    printf("  --width N       整幅宽 (默认 4000)\n");
    printf("  --height N      整幅高 (默认 1200)\n");
    printf("  --code-width N  左侧编码区宽度, 切图时跳过 (默认 160; 左右各 (宽-它)/2)\n");
    printf("  --fps N         帧率 (默认 60)\n");
#ifdef _WIN32
    printf("  --encoder NAME  编码器 (默认 hevc_amf; 可选 libx265)\n");
#else
    printf("  --encoder NAME  编码器 (默认 hevc_mpp; 可选 libx265)\n");
#endif
    printf("  --bitrate KBPS  目标码率 kbps (默认 20000)\n");
    printf("  --seconds S     录制时长秒 (默认 10; 0=直到 Ctrl+C)\n");
    printf("  --out DIR       输出目录 (默认 out)\n");
    printf("  --queue N       每路环形队列容量 (默认 64)\n");
    printf("  --dec-threads N 并行 MJPEG 解码器实例数 (默认 0=自动, 取 4)\n");
    printf("  --no-imu        不解码 IMU / 不写 CSV\n");
    printf("  --list          列出视频设备后退出\n");
}

static int list_devices(void)
{
    avdevice_register_all();
#ifdef _WIN32
    const AVInputFormat *ifmt = av_find_input_format("dshow");
    if (!ifmt) {
        fprintf(stderr, "找不到 dshow 输入设备 (ffmpeg 未带 dshow?)\n");
        return 1;
    }
    AVFormatContext *ctx = avformat_alloc_context();
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "list_devices", "true", 0);
    printf("== dshow 设备列表 (看 DirectShow video devices) ==\n");
    avformat_open_input(&ctx, "video=dummy", ifmt, &opts);  /* 故意失败, 仅打印列表 */
    av_dict_free(&opts);
    if (ctx)
        avformat_close_input(&ctx);
#else
    printf("== v4l2 设备列表 ==\n");
    int rc = system("v4l2-ctl --list-devices 2>/dev/null || ls -l /dev/video* 2>/dev/null");
    (void)rc;
#endif
    return 0;
}

int main(int argc, char **argv)
{
    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
#ifdef _WIN32
    strcpy(cfg.device, "DECXIN Camera");
    strcpy(cfg.enc_name, "hevc_amf");
#else
    strcpy(cfg.device, "/dev/video0");
    strcpy(cfg.enc_name, "hevc_mpp");
#endif
    strcpy(cfg.out_dir, "out");
    cfg.width = 4000;
    cfg.height = 1200;
    cfg.code_width = 160;
    cfg.fps = 60;
    cfg.bitrate_kbps = 20000;
    cfg.seconds = 10.0;
    cfg.queue_cap = 64;
    cfg.dec_threads = 0;
    cfg.no_imu = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) strcpy(cfg.device, argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) cfg.width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) cfg.height = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--code-width") && i + 1 < argc) cfg.code_width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) cfg.fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--encoder") && i + 1 < argc) strcpy(cfg.enc_name, argv[++i]);
        else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc) cfg.bitrate_kbps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) cfg.seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) strcpy(cfg.out_dir, argv[++i]);
        else if (!strcmp(argv[i], "--queue") && i + 1 < argc) cfg.queue_cap = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dec-threads") && i + 1 < argc) cfg.dec_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-imu")) cfg.no_imu = 1;
        else if (!strcmp(argv[i], "--list")) return list_devices();
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "未知参数: %s\n", argv[i]); usage(argv[0]); return 1; }
    }

    int sensor_w = (cfg.width - cfg.code_width) / 2;   /* 单颗 sensor 图像宽 */
    if ((cfg.height % 2) || (cfg.code_width % 2) || (sensor_w % 2) || sensor_w <= 0) {
        fprintf(stderr, "高/编码区宽/单幅宽必须为正偶数 (NV12 要求): "
                "height=%d code_width=%d sensor_w=%d\n",
                cfg.height, cfg.code_width, sensor_w);
        return 1;
    }

    signal(SIGINT, on_sigint);
    avdevice_register_all();

    /* ---------- 打开相机 ---------- */
#ifdef _WIN32
    const AVInputFormat *ifmt = av_find_input_format("dshow");
    if (!ifmt) { fprintf(stderr, "找不到 dshow 输入\n"); return 1; }

    char url[320];
    snprintf(url, sizeof(url), "video=%s", cfg.device);
#else
    const AVInputFormat *ifmt = av_find_input_format("v4l2");
    if (!ifmt) { fprintf(stderr, "找不到 v4l2 输入\n"); return 1; }

    char url[320];
    snprintf(url, sizeof(url), "%s", cfg.device);
#endif
    char sval[64];
    AVDictionary *iopts = NULL;
    snprintf(sval, sizeof(sval), "%dx%d", cfg.width, cfg.height);
    av_dict_set(&iopts, "video_size", sval, 0);
    snprintf(sval, sizeof(sval), "%d", cfg.fps);
    av_dict_set(&iopts, "framerate", sval, 0);
#ifdef _WIN32
    av_dict_set(&iopts, "vcodec", "mjpeg", 0);    /* 4000x1200@60 必须 MJPEG */
    av_dict_set(&iopts, "rtbufsize", "512M", 0);
#else
    av_dict_set(&iopts, "input_format", "mjpeg", 0);    /* 4000x1200@60 必须 MJPEG */
#endif

    AVFormatContext *ifc = NULL;
    int ret = avformat_open_input(&ifc, url, ifmt, &iopts);
    av_dict_free(&iopts);
    if (ret < 0) {
        char eb[128]; av_strerror(ret, eb, sizeof(eb));
        fprintf(stderr, "打开相机失败: %s\n用 --list 查看设备, 或确认相机未被占用。\n", eb);
        return 2;
    }
    avformat_find_stream_info(ifc, NULL);

    int vstream = -1;
    for (unsigned i = 0; i < ifc->nb_streams; i++)
        if (ifc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vstream = i; break; }
    if (vstream < 0) { fprintf(stderr, "没有视频流\n"); avformat_close_input(&ifc); return 2; }

    AVCodecParameters *par = ifc->streams[vstream]->codecpar;
    const AVCodec *dec = avcodec_find_decoder(par->codec_id);
    if (!dec) { fprintf(stderr, "找不到解码器\n"); return 2; }
    /* 注意: 不再开"一个"共享解码器。MJPEG 单线程解码 ~32ms/帧 是硬瓶颈,
     * 改为下面 N 个独立解码器实例并行 (帧内编码各帧独立)。 */
    printf("[相机] %s  请求 %dx%d@%d MJPEG\n", cfg.device, cfg.width, cfg.height, cfg.fps);

    /* ---------- 准备输出目录 / CSV ---------- */
#ifdef _WIN32
    _mkdir(cfg.out_dir);
#else
    mkdir(cfg.out_dir, 0755);
#endif
    FILE *frames_csv = NULL, *imu_csv = NULL;
    if (!cfg.no_imu) {
        char p[600];
        snprintf(p, sizeof(p), "%s/frames.csv", cfg.out_dir);
        frames_csv = fopen(p, "w");
        snprintf(p, sizeof(p), "%s/imu.csv", cfg.out_dir);
        imu_csv = fopen(p, "w");
        if (frames_csv)
            fprintf(frames_csv, "frame_idx,host_t_s,cam_exp_start_us,cam_exp_end_us,exposure_us,n_imu\n");
        if (imu_csv)
            fprintf(imu_csv, "frame_idx,cam_exp_end_us,imu_t_us,acc_x_mg,acc_y_mg,acc_z_mg,gyro_x_dps,gyro_y_dps,gyro_z_dps\n");
    }

    /* ---------- 流水线: 采集(主) -> N×解码 -> 重排 -> 收集 -> 左右编码 ---------- */
    /* 整幅布局: [code_width 编码区][sensor_w 左][sensor_w 右]
     * 左图列区间 [code_width, code_width+sensor_w), 右图 [code_width+sensor_w, width) */
    int left_xoff = cfg.code_width;
    int right_xoff = cfg.code_width + sensor_w;
    enum AVPixelFormat enc_pix_fmt = AV_PIX_FMT_NV12;
    if (!strcmp(cfg.enc_name, "libx265"))
        enc_pix_fmt = AV_PIX_FMT_YUV420P;

    int num_dec = cfg.dec_threads > 0 ? cfg.dec_threads : 4;  /* 默认 4 路并行解码: 解码等效~8ms,
                                                              * 给热降频留足余量, 实测零丢帧 */
    /* 重排窗口必须 > 最大乱序跨度 (pkt_q 容量 + 解码线程数), 否则可能死锁 */
    int reorder_cap = cfg.queue_cap + num_dec + 8;
    printf("[并行解码] %d 个独立 MJPEG 解码器实例, 重排窗口=%d\n", num_dec, reorder_cap);

    RingBuffer *pkt_q = rb_create(cfg.queue_cap, packet_item_free);  /* 采集->解码 */
    /* 收集->转换: 整幅引用队列 (容量小即可, 稳态接近空); 转换->编码: NV12 队列 */
    RingBuffer *rb_l = rb_create(16, frame_item_free);              /* 收集->左转换(整幅) */
    RingBuffer *rb_r = rb_create(16, frame_item_free);              /* 收集->右转换(整幅) */
    RingBuffer *nv_l = rb_create(cfg.queue_cap, frame_item_free);   /* 左转换->左编码(NV12) */
    RingBuffer *nv_r = rb_create(cfg.queue_cap, frame_item_free);   /* 右转换->右编码(NV12) */
    Reorder *reorder = reorder_create(reorder_cap, num_dec);

    ConvertArg cvt_l = {0}, cvt_r = {0};
    cvt_l.in = rb_l; cvt_l.out = nv_l; cvt_l.x_off = left_xoff;
    cvt_r.in = rb_r; cvt_r.out = nv_r; cvt_r.x_off = right_xoff;
    cvt_l.width = cvt_r.width = sensor_w;
    cvt_l.height = cvt_r.height = cfg.height;
    cvt_l.dst_fmt = cvt_r.dst_fmt = enc_pix_fmt;
    cvt_l.tag = "左"; cvt_r.tag = "右";

    EncodeArg arg_l = {0}, arg_r = {0};
    arg_l.in = nv_l; arg_r.in = nv_r;
    strcpy(arg_l.enc_name, cfg.enc_name); strcpy(arg_r.enc_name, cfg.enc_name);
    snprintf(arg_l.out_path, sizeof(arg_l.out_path), "%s/left.hevc", cfg.out_dir);
    snprintf(arg_r.out_path, sizeof(arg_r.out_path), "%s/right.hevc", cfg.out_dir);
    arg_l.width = arg_r.width = sensor_w;
    arg_l.height = arg_r.height = cfg.height;
    arg_l.fps = arg_r.fps = cfg.fps;
    arg_l.pix_fmt = arg_r.pix_fmt = enc_pix_fmt;
    arg_l.bitrate_kbps = arg_r.bitrate_kbps = cfg.bitrate_kbps;
    arg_l.tag = "左"; arg_r.tag = "右";

    int64_t t_start = av_gettime_relative();

    CollectorArg carg = {0};
    carg.reorder = reorder; carg.rb_l = rb_l; carg.rb_r = rb_r;
    carg.no_imu = cfg.no_imu;
    carg.frames_csv = frames_csv; carg.imu_csv = imu_csv;
    carg.t_start = t_start;

    DecodeWorkerArg *dargs = calloc(num_dec, sizeof(*dargs));
    pthread_t *th_dec = calloc(num_dec, sizeof(*th_dec));
    pthread_t th_cvt_l, th_cvt_r, th_enc_l, th_enc_r, th_col;
    pthread_create(&th_enc_l, NULL, encode_thread, &arg_l);
    pthread_create(&th_enc_r, NULL, encode_thread, &arg_r);
    pthread_create(&th_cvt_l, NULL, convert_thread, &cvt_l);
    pthread_create(&th_cvt_r, NULL, convert_thread, &cvt_r);
    pthread_create(&th_col, NULL, collector_thread, &carg);
    for (int i = 0; i < num_dec; i++) {
        dargs[i].pkt_q = pkt_q; dargs[i].reorder = reorder;
        dargs[i].dec = dec; dargs[i].par = par;
        pthread_create(&th_dec[i], NULL, decode_worker, &dargs[i]);
    }

    /* ---------- 采集主循环: 只读 USB 包, 阻塞入解码队列 (分配序号后绝不丢) ---------- */
    AVPacket *pkt = av_packet_alloc();
    int64_t prof_read = 0, prof_read_n = 0, read_seq = 0;

    printf("[开始采集] 时长=%.1fs%s ...\n", cfg.seconds,
           cfg.seconds <= 0 ? " (Ctrl+C 停止)" : "");

    while (!g_stop) {
        if (cfg.seconds > 0 &&
            (av_gettime_relative() - t_start) / 1e6 >= cfg.seconds)
            break;

        int64_t ts0 = av_gettime_relative();
        ret = av_read_frame(ifc, pkt);
        if (ret < 0)
            break;
        int64_t ts1 = av_gettime_relative();
        prof_read += ts1 - ts0; prof_read_n++;
        if (pkt->stream_index != vstream) { av_packet_unref(pkt); continue; }

        /* 把这一包的所有权移交给带序号的 PktItem, 阻塞推入解码队列 */
        PktItem *pi = malloc(sizeof(*pi));
        pi->pkt = av_packet_alloc();
        av_packet_move_ref(pi->pkt, pkt);
        pi->seq = read_seq++;
        if (rb_push_block(pkt_q, pi) < 0)
            packet_item_free(pi);

        if (prof_read_n % cfg.fps == 0) {
            double el = (av_gettime_relative() - t_start) / 1e6;
            printf("\r  采集中... 读包=%lld 平均fps=%.2f 队列P=%zu/L=%zu/R=%zu 丢弃L=%zu/R=%zu",
                   (long long)prof_read_n, prof_read_n / (el > 0 ? el : 1),
                   rb_size(pkt_q), rb_size(rb_l), rb_size(rb_r),
                   rb_dropped(rb_l), rb_dropped(rb_r));
            fflush(stdout);
        }
    }
    printf("\n[采集结束] 关闭队列, 等待解码 + 编码线程冲刷...\n");

    /* ---------- 收尾: 关 pkt 队列 -> 解码worker -> 收集 -> 转换 -> 编码 逐级收尾 ---- */
    rb_close(pkt_q);
    for (int i = 0; i < num_dec; i++)
        pthread_join(th_dec[i], NULL);
    pthread_join(th_col, NULL);          /* 收集退出时关闭 rb_l/rb_r */
    pthread_join(th_cvt_l, NULL);        /* 转换退出时关闭 nv_l/nv_r */
    pthread_join(th_cvt_r, NULL);
    pthread_join(th_enc_l, NULL);
    pthread_join(th_enc_r, NULL);

    double elapsed = (av_gettime_relative() - t_start) / 1e6;
    int64_t frame_idx = carg.frame_idx;
    int64_t n_decoded_imu = carg.n_decoded_imu, n_fail_imu = carg.n_fail_imu;
    int64_t first_cam_ts = carg.first_cam_ts, last_cam_ts = carg.last_cam_ts;
    int64_t prof_decode_sum = 0, prof_decode_n = 0;
    for (int i = 0; i < num_dec; i++) {
        prof_decode_sum += dargs[i].prof_decode;
        prof_decode_n   += dargs[i].prof_n;
    }

    av_packet_free(&pkt);
    avformat_close_input(&ifc);
    if (frames_csv) fclose(frames_csv);
    if (imu_csv) fclose(imu_csv);
    rb_destroy(pkt_q);
    rb_destroy(rb_l);
    rb_destroy(rb_r);
    rb_destroy(nv_l);
    rb_destroy(nv_r);
    reorder_destroy(reorder);
    free(dargs);
    free(th_dec);

    /* ---------- 统计 ---------- */
    printf("\n========== 结果 ==========\n");
    printf("采集帧数      : %lld  (%.3fs, 平均到达 %.2f fps)\n",
           (long long)frame_idx, elapsed, frame_idx / (elapsed > 0 ? elapsed : 1));
    if (first_cam_ts >= 0 && last_cam_ts > first_cam_ts && n_decoded_imu > 1) {
        double span_s = (last_cam_ts - first_cam_ts) / 1e6;
        printf("相机时间戳FPS : %.2f (sensor 实际)\n", (n_decoded_imu - 1) / span_s);
    }
    if (!cfg.no_imu)
        printf("IMU 解码      : 成功 %lld / 失败 %lld\n",
               (long long)n_decoded_imu, (long long)n_fail_imu);
    printf("左路编码      : %s, 写出 %ld 帧 -> %s/left.hevc (丢弃 转换%zu/编码%zu)\n",
           arg_l.ok ? "OK" : "失败", arg_l.frames, cfg.out_dir,
           rb_dropped(rb_l), rb_dropped(nv_l));
    printf("右路编码      : %s, 写出 %ld 帧 -> %s/right.hevc (丢弃 转换%zu/编码%zu)\n",
           arg_r.ok ? "OK" : "失败", arg_r.frames, cfg.out_dir,
           rb_dropped(rb_r), rb_dropped(nv_r));
    printf("---- 各级平均耗时 (每帧, ms; 60fps 预算 16.67) ----\n");
    if (prof_read_n > 0)
        printf("  采集 USB读包 : %.2f\n", prof_read / 1000.0 / prof_read_n);
    if (prof_decode_n > 0)
        printf("  解码 %d路并行 : 单路 %.2f -> 等效 %.2f\n", num_dec,
               prof_decode_sum / 1000.0 / prof_decode_n,
               prof_decode_sum / 1000.0 / prof_decode_n / num_dec);
    if (carg.prof_n > 0)
        printf("  收集 IMU+克隆: %.2f\n",
               (carg.prof_imu + carg.prof_clone) / 1000.0 / carg.prof_n);
    if (cvt_l.prof_n > 0 && cvt_r.prof_n > 0)
        printf("  转换 sws L/R : %.2f / %.2f\n",
               cvt_l.prof_sws / 1000.0 / cvt_l.prof_n,
               cvt_r.prof_sws / 1000.0 / cvt_r.prof_n);
    if (arg_l.prof_n > 0 && arg_r.prof_n > 0)
        printf("  编码 L/R     : %.2f / %.2f\n",
               arg_l.prof_enc / 1000.0 / arg_l.prof_n,
               arg_r.prof_enc / 1000.0 / arg_r.prof_n);
    printf("\n下一步: 用 hevc2mp4 转 mp4:\n");
    printf("  ./hevc2mp4 %s/left.hevc  %s/left.mp4  %d\n", cfg.out_dir, cfg.out_dir, cfg.fps);
    printf("  ./hevc2mp4 %s/right.hevc %s/right.mp4 %d\n", cfg.out_dir, cfg.out_dir, cfg.fps);
    return 0;
}
