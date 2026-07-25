#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libavutil/timestamp.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>

#include "utils.h"

enum AVPixelFormat get_format_cb(struct AVCodecContext *s,
                                 const enum AVPixelFormat *fmt) {
  for (size_t i = 0;; i++) {
    const enum AVPixelFormat pf = fmt[i];
    if (pf == AV_PIX_FMT_NONE) {
      break;
    }
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pf);
    printf("Pixel format to choose: %s\n", desc->name);
  }
  return fmt[0];
}

int main(int argc, char **argv) {
  int result;
  int video_stream;
  AVFormatContext *fmt_ctx = NULL;

  result = avformat_open_input(&fmt_ctx, argv[1], NULL, NULL);
  ERRCHECK("Can't open file");

  result = avformat_find_stream_info(fmt_ctx, NULL);
  ERRCHECK("Cant find stream info");

  video_stream =
      av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  ERRCHECK2(video_stream >= 0, "Cant find video stream");
  AVCodecParameters *origin_par = fmt_ctx->streams[video_stream]->codecpar;

  const AVCodec *codec = avcodec_find_decoder(origin_par->codec_id);
  ERRCHECK2(codec, "Can't find decoder");
  printf("Video decoded with %s\n", codec->name);

  AVCodecContext *ctx = avcodec_alloc_context3(codec);
  ERRCHECK2(ctx, "Cant allocate memory for decoder context");

  result = avcodec_parameters_to_context(ctx, origin_par);
  ERRCHECK("Cant initiate codec parameters");
  ctx->thread_count = 4;
  ctx->thread_type = FF_THREAD_SLICE;
  ctx->get_format = &get_format_cb;

  result = avcodec_open2(ctx, codec, NULL);
  ERRCHECK("Cant open decoder");

  AVFrame *fr = av_frame_alloc();
  ERRCHECK2(fr, "Cant allocate frame");

  AVPacket *pkt = av_packet_alloc();
  ERRCHECK2(pkt, "Cant allocate packet");

  const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(ctx->pix_fmt);
  printf("Frame is %dx%d (%s)\n", ctx->width, ctx->height, pix_desc->name);

  size_t byte_buffer_size =
      av_image_get_buffer_size(ctx->pix_fmt, ctx->width, ctx->height, 16);

  uint8_t *byte_buffer = av_malloc(byte_buffer_size);
  ERRCHECK2(byte_buffer, "Cant allocate frame byte buffer");

  printf("Time base is %d/%d\n", fmt_ctx->streams[video_stream]->time_base.num,
         fmt_ctx->streams[video_stream]->time_base.den);

  sleep(5);
  int i = 0;
  while (result >= 0) {
    result = av_read_frame(fmt_ctx, pkt);
    if (result >= 0 && pkt->stream_index != video_stream) {
      av_packet_unref(pkt);
      continue;
    }

    if (result < 0) {
      result = avcodec_send_packet(ctx, NULL);
    } else {
      if (pkt->pts == AV_NOPTS_VALUE) {
        pkt->pts = pkt->dts = i;
      }
      result = avcodec_send_packet(ctx, pkt);
    }
    av_packet_unref(pkt);

    ERRCHECK2(result >= 0, "Can't submit a packet to the decoder");

    while (result >= 0) {
      result = avcodec_receive_frame(ctx, fr);
      if (result == AVERROR_EOF)
        return 0;
      else if (result == AVERROR(EAGAIN)) {
        result = 0;
        break;
      } else if (result < 0) {
        printf("Err: decoding frame\n");
        return 0;
      }

      int number_bytes_received = av_image_copy_to_buffer(
          byte_buffer, byte_buffer_size, (const uint8_t *const *)fr->data,
          fr->linesize, ctx->pix_fmt, ctx->width, ctx->height, 1);
      ERRCHECK2(number_bytes_received >= 0, "Can't copy image to buffer");

      // TODO lol
      printf("%10s, %10s, Frame duration: %8" PRId64 ", Bytes received: %8d\n",
             av_ts2str(fr->pts), av_ts2str(fr->pkt_dts), fr->duration,
             number_bytes_received);
      av_frame_unref(fr);
    }
    i++;
  }
  return 0;
}
