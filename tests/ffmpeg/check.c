#include <libavcodec/avcodec.h>
#include <stdio.h>
#include <string.h>

/* Synthetic black 16x16 frame, no game data. Generated with:
 * ffmpeg -f lavfi -i color=black:size=16x16:rate=1 -frames:v 1
 *   -pix_fmt yuv420p -c:v libx264 -x264-params keyint=1
 *   -bsf:v filter_units=remove_types=6 -f h264 black.h264
 */
static const unsigned char black[] = {
    0x00,0x00,0x00,0x01,0x67,0x64,0x10,0x0a,0xac,0xbb,0xd8,0x08,0x80,0x00,0x00,0x03,
    0x00,0x80,0x00,0x00,0x03,0x01,0x02,0x00,0x00,0x00,0x01,0x68,0xee,0x0f,0x2c,0x8b,
    0x00,0x00,0x01,0x65,0x88,0x84,0x04,0xbf,0xfe,0xf7,0xad,0xdf,0x81,0x4d,0xc3,0x2b,
    0x3d
};

#define CHECK(condition) do { \
    if (!(condition)) { fprintf(stderr, "Failed: %s\n", #condition); return 1; } \
} while (0)

int main(void)
{
    const enum AVCodecID codecs[] = {AV_CODEC_ID_ATRAC3, AV_CODEC_ID_ATRAC3P, AV_CODEC_ID_H264};
    for (unsigned i = 0; i < sizeof(codecs) / sizeof(codecs[0]); ++i)
        CHECK(avcodec_find_decoder(codecs[i]));
    AVCodecContext *context = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    CHECK(context && packet && frame);
    context->thread_count = 1;
    CHECK(avcodec_open2(context, NULL, NULL) == 0);
    CHECK(av_new_packet(packet, sizeof(black)) == 0);
    memcpy(packet->data, black, sizeof(black));
    CHECK(avcodec_send_packet(context, packet) == 0);
    int result = avcodec_receive_frame(context, frame);
    if (result == AVERROR(EAGAIN)) {
        CHECK(avcodec_send_packet(context, NULL) == 0);
        result = avcodec_receive_frame(context, frame);
    }
    CHECK(result == 0);
    CHECK(frame->width == 16 && frame->height == 16 && frame->format == AV_PIX_FMT_YUV420P);
    for (int plane = 0; plane < 3; ++plane) {
        int size = plane == 0 ? 16 : 8;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                CHECK(frame->data[plane][y * frame->linesize[plane] + x] == (plane == 0 ? 16 : 128));
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    puts("ATRAC3/ATRAC3plus decoders found; H.264 frame decoded correctly");
    puts(avcodec_license());
    return 0;
}
