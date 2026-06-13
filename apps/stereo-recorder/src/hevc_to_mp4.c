/*
 * hevc_to_mp4.c —— 把 H.265 (HEVC) Annex-B 裸流 remux 成 MP4。
 *
 * 仅做封装转换 (stream copy), 不重新编码, 速度极快、画质无损。
 * 裸流没有时间戳, 这里按指定帧率重建每帧 pts/dts (编码时无 B 帧, dts==pts)。
 *
 * 用法: hevc2mp4 <输入.hevc> <输出.mp4> [fps=60]
 */
#include <stdio.h>
#include <stdlib.h>

#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "用法: %s <输入.hevc> <输出.mp4> [fps=60]\n", argv[0]);
        return 1;
    }
    const char *in_path = argv[1];
    const char *out_path = argv[2];
    int fps = (argc > 3) ? atoi(argv[3]) : 60;
    if (fps <= 0) fps = 60;

    AVFormatContext *ifc = NULL;
    int ret = avformat_open_input(&ifc, in_path, NULL, NULL);
    if (ret < 0) {
        char eb[128]; av_strerror(ret, eb, sizeof(eb));
        fprintf(stderr, "打开输入失败 %s: %s\n", in_path, eb);
        return 2;
    }
    if (avformat_find_stream_info(ifc, NULL) < 0) {
        fprintf(stderr, "find_stream_info 失败\n");
        avformat_close_input(&ifc);
        return 2;
    }

    int vstream = -1;
    for (unsigned i = 0; i < ifc->nb_streams; i++)
        if (ifc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vstream = i; break; }
    if (vstream < 0) {
        fprintf(stderr, "输入里没有视频流\n");
        avformat_close_input(&ifc);
        return 2;
    }

    AVFormatContext *ofc = NULL;
    avformat_alloc_output_context2(&ofc, NULL, NULL, out_path);
    if (!ofc) {
        fprintf(stderr, "无法为 %s 创建输出上下文\n", out_path);
        avformat_close_input(&ifc);
        return 2;
    }

    AVStream *ost = avformat_new_stream(ofc, NULL);
    avcodec_parameters_copy(ost->codecpar, ifc->streams[vstream]->codecpar);
    ost->codecpar->codec_tag = 0;
    /* 输出时间基 1/fps; mov 复用器会按需转换 NAL 格式并写 hvcC */
    AVRational tb = (AVRational){1, fps};
    ost->time_base = tb;
    ost->avg_frame_rate = (AVRational){fps, 1};
    ost->r_frame_rate = (AVRational){fps, 1};

    if (!(ofc->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofc->pb, out_path, AVIO_FLAG_WRITE);
        if (ret < 0) {
            char eb[128]; av_strerror(ret, eb, sizeof(eb));
            fprintf(stderr, "打开输出文件失败 %s: %s\n", out_path, eb);
            avformat_close_input(&ifc);
            avformat_free_context(ofc);
            return 2;
        }
    }

    ret = avformat_write_header(ofc, NULL);
    if (ret < 0) {
        char eb[128]; av_strerror(ret, eb, sizeof(eb));
        fprintf(stderr, "write_header 失败: %s\n", eb);
        goto end;
    }

    AVPacket *pkt = av_packet_alloc();
    AVRational src_tb = (AVRational){1, fps};   /* 我们按帧号在该时基上排时间戳 */
    int64_t n = 0;
    while (av_read_frame(ifc, pkt) >= 0) {
        if (pkt->stream_index != vstream) { av_packet_unref(pkt); continue; }
        /* 裸流时间戳不可靠, 按帧号重建 (无 B 帧: dts==pts), 再换算到复用器时基 */
        pkt->stream_index = 0;
        pkt->pts = n;
        pkt->dts = n;
        pkt->duration = 1;
        pkt->pos = -1;
        av_packet_rescale_ts(pkt, src_tb, ost->time_base);
        ret = av_interleaved_write_frame(ofc, pkt);
        av_packet_unref(pkt);
        if (ret < 0) {
            char eb[128]; av_strerror(ret, eb, sizeof(eb));
            fprintf(stderr, "写帧 %lld 失败: %s\n", (long long)n, eb);
            break;
        }
        n++;
    }
    av_packet_free(&pkt);
    av_write_trailer(ofc);
    printf("OK: %s -> %s  (%lld 帧 @ %dfps)\n", in_path, out_path, (long long)n, fps);

end:
    if (ofc && !(ofc->oformat->flags & AVFMT_NOFILE))
        avio_closep(&ofc->pb);
    if (ofc)
        avformat_free_context(ofc);
    avformat_close_input(&ifc);
    return 0;
}
