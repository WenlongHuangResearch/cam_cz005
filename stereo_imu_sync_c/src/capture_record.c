/*
 * capture_record.c —— 双目 (DECXIN / Nori 3D) UVC 相机 C 版录制主程序。
 *
 * 流水线 (生产者-消费者, 环形队列解耦):
 *
 *   [采集线程]  dshow 取 4000x1200 MJPG@60 -> 解码成 YUV
 *               -> 整帧 Y 平面解出 帧曝光时间戳 + IMU (写 CSV)
 *               -> 把整帧按宽度切成 左半 / 右半, 各转成 NV12
 *               -> 分别 push 进 左/右两个环形队列
 *                         |                         |
 *   [左编码线程] <--------+        [右编码线程] <----+
 *     pop -> hevc_amf 编码 -> left.hevc    pop -> hevc_amf 编码 -> right.hevc
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

/* ---------------- 编码线程参数 ---------------- */
typedef struct {
    RingBuffer *rb;
    char enc_name[64];
    char out_path[512];
    int width, height, fps, bitrate_kbps;
    const char *tag;      /* "左"/"右", 仅日志 */
    int ok;               /* 线程是否正常完成 */
    long frames;
} EncoderThreadArg;

static void *encoder_thread(void *varg)
{
    EncoderThreadArg *a = (EncoderThreadArg *)varg;
    char err[256] = {0};
    Encoder *enc = encoder_create(a->enc_name, a->width, a->height, a->fps,
                                  a->bitrate_kbps, a->out_path, err, sizeof(err));
    if (!enc) {
        fprintf(stderr, "[%s编码] 创建编码器失败: %s\n", a->tag, err);
        /* 把队列抽干, 避免采集线程因满队列无谓丢帧后仍卡住 (其实不会卡) */
        void *item;
        while (rb_pop(a->rb, &item) == 0)
            frame_item_free(item);
        a->ok = 0;
        return NULL;
    }

    void *item;
    while (rb_pop(a->rb, &item) == 0) {
        FrameItem *it = (FrameItem *)item;
        if (encoder_send(enc, it->frame) < 0)
            fprintf(stderr, "[%s编码] 帧 %lld 编码出错\n", a->tag,
                    (long long)it->frame_idx);
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

/* ---------------- 把整帧某半幅 (yuv420p) 转成 NV12 半幅帧 ---------------- */
/* x_off: 该半幅在整帧中的起始列 (0 或 width/2)。返回新分配的 NV12 AVFrame。 */
static AVFrame *make_half_nv12(struct SwsContext *sws, const AVFrame *src,
                               int x_off, int half_w, int height)
{
    AVFrame *dst = av_frame_alloc();
    if (!dst)
        return NULL;
    dst->format = AV_PIX_FMT_NV12;
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
    int no_imu;
} Config;

static void usage(const char *prog)
{
    printf("用法: %s [选项]\n", prog);
    printf("  --device NAME   dshow 设备名 (默认 \"DECXIN Camera\")\n");
    printf("  --width N       整幅宽 (默认 4000)\n");
    printf("  --height N      整幅高 (默认 1200)\n");
    printf("  --code-width N  左侧编码区宽度, 切图时跳过 (默认 160; 左右各 (宽-它)/2)\n");
    printf("  --fps N         帧率 (默认 60)\n");
    printf("  --encoder NAME  编码器 (默认 hevc_amf; 可选 libx265)\n");
    printf("  --bitrate KBPS  目标码率 kbps (默认 20000)\n");
    printf("  --seconds S     录制时长秒 (默认 10; 0=直到 Ctrl+C)\n");
    printf("  --out DIR       输出目录 (默认 out)\n");
    printf("  --queue N       每路环形队列容量 (默认 8)\n");
    printf("  --no-imu        不解码 IMU / 不写 CSV\n");
    printf("  --list          列出 dshow 视频设备后退出\n");
}

static int list_devices(void)
{
    avdevice_register_all();
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
    return 0;
}

int main(int argc, char **argv)
{
    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.device, "DECXIN Camera");
    strcpy(cfg.enc_name, "hevc_amf");
    strcpy(cfg.out_dir, "out");
    cfg.width = 4000;
    cfg.height = 1200;
    cfg.code_width = 160;
    cfg.fps = 60;
    cfg.bitrate_kbps = 20000;
    cfg.seconds = 10.0;
    cfg.queue_cap = 8;
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

    /* ---------- 打开 dshow 相机 ---------- */
    const AVInputFormat *ifmt = av_find_input_format("dshow");
    if (!ifmt) { fprintf(stderr, "找不到 dshow 输入\n"); return 1; }

    char url[320];
    snprintf(url, sizeof(url), "video=%s", cfg.device);
    char sval[64];
    AVDictionary *iopts = NULL;
    snprintf(sval, sizeof(sval), "%dx%d", cfg.width, cfg.height);
    av_dict_set(&iopts, "video_size", sval, 0);
    snprintf(sval, sizeof(sval), "%d", cfg.fps);
    av_dict_set(&iopts, "framerate", sval, 0);
    av_dict_set(&iopts, "vcodec", "mjpeg", 0);    /* 4000x1200@60 必须 MJPEG */
    av_dict_set(&iopts, "rtbufsize", "512M", 0);

    AVFormatContext *ifc = NULL;
    int ret = avformat_open_input(&ifc, url, ifmt, &iopts);
    av_dict_free(&iopts);
    if (ret < 0) {
        char eb[128]; av_strerror(ret, eb, sizeof(eb));
        fprintf(stderr, "打开相机失败: %s\n用 --list 查看设备名, 或确认相机未被占用。\n", eb);
        return 2;
    }
    avformat_find_stream_info(ifc, NULL);

    int vstream = -1;
    for (unsigned i = 0; i < ifc->nb_streams; i++)
        if (ifc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vstream = i; break; }
    if (vstream < 0) { fprintf(stderr, "没有视频流\n"); avformat_close_input(&ifc); return 2; }

    AVCodecParameters *par = ifc->streams[vstream]->codecpar;
    const AVCodec *dec = avcodec_find_decoder(par->codec_id);
    AVCodecContext *dctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dctx, par);
    if (avcodec_open2(dctx, dec, NULL) < 0) {
        fprintf(stderr, "打开解码器失败\n"); return 2;
    }
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

    /* ---------- 左右环形队列 + 对称编码线程 ---------- */
    /* 整幅布局: [code_width 编码区][sensor_w 左][sensor_w 右]
     * 左图列区间 [code_width, code_width+sensor_w), 右图 [code_width+sensor_w, width) */
    int left_xoff = cfg.code_width;
    int right_xoff = cfg.code_width + sensor_w;
    RingBuffer *rb_l = rb_create(cfg.queue_cap, frame_item_free);
    RingBuffer *rb_r = rb_create(cfg.queue_cap, frame_item_free);

    EncoderThreadArg arg_l = {0}, arg_r = {0};
    arg_l.rb = rb_l; arg_r.rb = rb_r;
    strcpy(arg_l.enc_name, cfg.enc_name); strcpy(arg_r.enc_name, cfg.enc_name);
    snprintf(arg_l.out_path, sizeof(arg_l.out_path), "%s/left.hevc", cfg.out_dir);
    snprintf(arg_r.out_path, sizeof(arg_r.out_path), "%s/right.hevc", cfg.out_dir);
    arg_l.width = arg_r.width = sensor_w;
    arg_l.height = arg_r.height = cfg.height;
    arg_l.fps = arg_r.fps = cfg.fps;
    arg_l.bitrate_kbps = arg_r.bitrate_kbps = cfg.bitrate_kbps;
    arg_l.tag = "左"; arg_r.tag = "右";

    pthread_t th_l, th_r;
    pthread_create(&th_l, NULL, encoder_thread, &arg_l);
    pthread_create(&th_r, NULL, encoder_thread, &arg_r);

    /* ---------- 采集主循环 ---------- */
    struct SwsContext *sws = NULL;   /* yuv420p(半幅) -> nv12, 延迟到拿到首帧格式再建 */
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    Unwrap32 uw_frame = {0}, uw_imu = {0};
    int flip = 0, flip_detected = cfg.no_imu;  /* no-imu 时不需要方向探测 */
    int64_t frame_idx = 0, n_decoded_imu = 0, n_fail_imu = 0;
    int64_t t_start = av_gettime_relative();
    int64_t first_cam_ts = -1, last_cam_ts = -1;

    printf("[开始采集] 时长=%.1fs%s ...\n", cfg.seconds,
           cfg.seconds <= 0 ? " (Ctrl+C 停止)" : "");

    while (!g_stop) {
        if (cfg.seconds > 0 &&
            (av_gettime_relative() - t_start) / 1e6 >= cfg.seconds)
            break;

        ret = av_read_frame(ifc, pkt);
        if (ret < 0)
            break;
        if (pkt->stream_index != vstream) { av_packet_unref(pkt); continue; }

        ret = avcodec_send_packet(dctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            continue;

        while (avcodec_receive_frame(dctx, frame) == 0) {
            double host_t = (av_gettime_relative() - t_start) / 1e6;

            /* ---- 首帧建 sws (源像素格式来自解码器) ---- */
            if (!sws) {
                sws = sws_getContext(sensor_w, cfg.height, dctx->pix_fmt,
                                     sensor_w, cfg.height, AV_PIX_FMT_NV12,
                                     SWS_BILINEAR, NULL, NULL, NULL);
                if (!sws) { fprintf(stderr, "sws_getContext 失败\n"); g_stop = 1; break; }
            }

            /* ---- IMU / 时间戳: 用整帧 Y 平面解码 ---- */
            if (!cfg.no_imu) {
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
                    n_decoded_imu++;
                    uint64_t exp_end = unwrap32(&uw_frame, meta.exp_time_end);
                    if (first_cam_ts < 0) first_cam_ts = exp_end;
                    last_cam_ts = exp_end;
                    if (frames_csv)
                        fprintf(frames_csv, "%lld,%.6f,%u,%u,%u,%d\n",
                                (long long)frame_idx, host_t,
                                meta.exp_time_start, meta.exp_time_end,
                                frame_exposure_us(&meta), meta.n_imu);
                    if (imu_csv)
                        for (int k = 0; k < meta.n_imu; k++) {
                            ImuSample *s = &meta.imu[k];
                            fprintf(imu_csv,
                                    "%lld,%u,%llu,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f\n",
                                    (long long)frame_idx, meta.exp_time_end,
                                    (unsigned long long)unwrap32(&uw_imu, s->u_time),
                                    s->acc[0], s->acc[1], s->acc[2],
                                    s->gyro[0], s->gyro[1], s->gyro[2]);
                        }
                } else {
                    n_fail_imu++;
                }
            }

            /* ---- 拆左右 -> NV12 -> 推入两路队列 ---- */
            AVFrame *fl = make_half_nv12(sws, frame, left_xoff, sensor_w, cfg.height);
            AVFrame *fr = make_half_nv12(sws, frame, right_xoff, sensor_w, cfg.height);
            if (fl) {
                fl->pts = frame_idx;
                FrameItem *it = malloc(sizeof(*it));
                it->frame = fl; it->frame_idx = frame_idx;
                if (rb_push(rb_l, it) < 0) frame_item_free(it);
            }
            if (fr) {
                fr->pts = frame_idx;
                FrameItem *it = malloc(sizeof(*it));
                it->frame = fr; it->frame_idx = frame_idx;
                if (rb_push(rb_r, it) < 0) frame_item_free(it);
            }

            frame_idx++;
            av_frame_unref(frame);

            if (frame_idx % cfg.fps == 0) {
                double el = (av_gettime_relative() - t_start) / 1e6;
                printf("\r  采集中... 帧=%lld 平均fps=%.2f 队列L=%zu/R=%zu 丢弃L=%zu/R=%zu",
                       (long long)frame_idx, frame_idx / (el > 0 ? el : 1),
                       rb_size(rb_l), rb_size(rb_r),
                       rb_dropped(rb_l), rb_dropped(rb_r));
                fflush(stdout);
            }
        }
    }
    printf("\n[采集结束] 关闭队列, 等待编码线程冲刷...\n");

    /* ---------- 收尾 ---------- */
    rb_close(rb_l);
    rb_close(rb_r);
    pthread_join(th_l, NULL);
    pthread_join(th_r, NULL);

    double elapsed = (av_gettime_relative() - t_start) / 1e6;
    av_frame_free(&frame);
    av_packet_free(&pkt);
    if (sws) sws_freeContext(sws);
    avcodec_free_context(&dctx);
    avformat_close_input(&ifc);
    if (frames_csv) fclose(frames_csv);
    if (imu_csv) fclose(imu_csv);
    rb_destroy(rb_l);
    rb_destroy(rb_r);

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
    printf("左路编码      : %s, 写出 %ld 帧 -> %s/left.hevc (丢弃 %zu)\n",
           arg_l.ok ? "OK" : "失败", arg_l.frames, cfg.out_dir, rb_dropped(rb_l));
    printf("右路编码      : %s, 写出 %ld 帧 -> %s/right.hevc (丢弃 %zu)\n",
           arg_r.ok ? "OK" : "失败", arg_r.frames, cfg.out_dir, rb_dropped(rb_r));
    printf("\n下一步: 用 hevc2mp4 转 mp4:\n");
    printf("  ./hevc2mp4 %s/left.hevc  %s/left.mp4  %d\n", cfg.out_dir, cfg.out_dir, cfg.fps);
    printf("  ./hevc2mp4 %s/right.hevc %s/right.mp4 %d\n", cfg.out_dir, cfg.out_dir, cfg.fps);
    return 0;
}
