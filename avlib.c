#include <asm-generic/errno-base.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>

#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/timestamp.h>
#include <libswresample/swresample.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "avlib.h"
#include "utils.h"

#define RING_FRAMES 10

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

int initiate_decoding(DecoderContext *ctx, const char *file_name) {
  *ctx = (DecoderContext){0};
  int result = avformat_open_input(&ctx->fmt_ctx, file_name, NULL, NULL);
  ERRCHECK("Can't open file");

  result = avformat_find_stream_info(ctx->fmt_ctx, NULL);
  ERRCHECK("Cant find stream info");

  ctx->swr = swr_alloc();
  ERRCHECK2(ctx->swr, "Cant allocate audio resampler");
  // You can use swr_config_frame and swr_convert_frame to convert frames
  // between sample fmts
  // av_opt_show2(ctx->swr, NULL, -1, 0);

  ctx->video_stream =
      av_find_best_stream(ctx->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  ERRCHECK2(ctx->video_stream >= 0, "Cant find video stream");
  ctx->audio_stream =
      av_find_best_stream(ctx->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  ERRCHECK2(ctx->audio_stream >= 0, "Cant find audio stream");
  AVCodecParameters *origin_par =
      ctx->fmt_ctx->streams[ctx->video_stream]->codecpar;
  AVCodecParameters *origin_par_audio =
      ctx->fmt_ctx->streams[ctx->audio_stream]->codecpar;

  printf("Audio sample rate: %d, bits per sample: %d, channels: %d, frame "
         "size: %d\n",
         origin_par_audio->sample_rate, origin_par_audio->bits_per_coded_sample,
         origin_par_audio->ch_layout.nb_channels, origin_par_audio->frame_size);
  printf("Video frame rate: %d/%d\n", origin_par->framerate.num,
         origin_par->framerate.den);

  const AVCodec *codec = avcodec_find_decoder(origin_par->codec_id);
  ERRCHECK2(codec, "Can't find decoder");
  const AVCodec *audio_codec = avcodec_find_decoder(origin_par_audio->codec_id);
  printf("Video decoded with %s, audio with %s\n", codec->name,
         audio_codec->name);

  ctx->ctx = avcodec_alloc_context3(codec);
  ERRCHECK2(ctx->ctx, "Cant allocate memory for decoder context");
  ctx->ctxa = avcodec_alloc_context3(audio_codec);
  ERRCHECK2(ctx->ctxa, "Cant allocate memory for audio decoder");

  result = avcodec_parameters_to_context(ctx->ctx, origin_par);
  ERRCHECK("Cant initiate video codec parameters");
  result = avcodec_parameters_to_context(ctx->ctxa, origin_par_audio);
  ERRCHECK("Cant initiate audio codec parameters");
  ctx->ctx->thread_count = 4;
  ctx->ctx->thread_type = FF_THREAD_SLICE;
  ctx->ctx->get_format = &get_format_cb;

  result = avcodec_open2(ctx->ctx, codec, NULL);
  ERRCHECK("Cant open decoder");
  result = avcodec_open2(ctx->ctxa, audio_codec, NULL);
  ERRCHECK("Cant open audio decoder");

  ctx->fr = av_frame_alloc();
  ERRCHECK2(ctx->fr, "Cant allocate frame");
  ctx->fr_audio_target = av_frame_alloc();
  ERRCHECK2(ctx->fr_audio_target, "Cant allocate frame");
  ctx->fr_audio_target->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
  ctx->fr_audio_target->format = AV_SAMPLE_FMT_FLT;

  ctx->pkt = av_packet_alloc();
  ERRCHECK2(ctx->pkt, "Cant allocate packet");

  const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(ctx->ctx->pix_fmt);
  printf("Frame is %dx%d (%s)\n", ctx->ctx->width, ctx->ctx->height,
         pix_desc->name);
  ctx->video_width = ctx->ctx->width;
  ctx->video_height = ctx->ctx->height;

  ctx->image_buffer_size = av_image_get_buffer_size(
      ctx->ctx->pix_fmt, ctx->ctx->width, ctx->ctx->height, 16);
  // TODO: initialize audio buffer

  ctx->image_buffer = make_ringbuffer(ctx->image_buffer_size * RING_FRAMES);

  printf("Audio codec data: sample format %s, sample rate %d, bytes per sample "
         "%d\n",
         av_get_sample_fmt_name(ctx->ctxa->sample_fmt), ctx->ctxa->sample_rate,
         av_get_bytes_per_sample(ctx->ctxa->sample_fmt));
  ctx->sample_rate = ctx->ctxa->sample_rate;

  printf("Time base is %d/%d\n",
         ctx->fmt_ctx->streams[ctx->video_stream]->time_base.num,
         ctx->fmt_ctx->streams[ctx->video_stream]->time_base.den);
  printf("audio Time base is %d/%d\n",
         ctx->fmt_ctx->streams[ctx->audio_stream]->time_base.num,
         ctx->fmt_ctx->streams[ctx->audio_stream]->time_base.den);
  ctx->video_tb = ctx->fmt_ctx->streams[ctx->video_stream]->time_base;
  ctx->audio_tb = ctx->fmt_ctx->streams[ctx->audio_stream]->time_base;

  return 0;
}

int continue_decoding(DecoderContext *ctx) {
  // First look if we can receive an additional audio frame from data
  // accumulated in SWR
  bool frame_is_audio = false;
  int result = 0;
  uint8_t *write_loc_audio = NULL;
  if (ctx->audio_configured) {
    write_loc_audio = write_ringbuffer_chunk_nocommit(&ctx->audio_buffer,
                                                      ctx->audio_buffer_size);
    if (write_loc_audio == NULL)
      return RESULT_STALL;
  }
  uint8_t *write_loc_image = write_ringbuffer_chunk_nocommit(
      &ctx->image_buffer, ctx->image_buffer_size);
  if (write_loc_image == NULL)
    return RESULT_STALL;
  bool frame_converted = false;
  if (ctx->audio_configured) {
    printf("SWR delay: %ld/%d\n", swr_get_delay(ctx->swr, ctx->sample_rate),
           ctx->audio_samples_per_frame);
  }

  if (ctx->audio_configured && swr_get_delay(ctx->swr, ctx->sample_rate) >
                                   ctx->audio_samples_per_frame) {
    swr_convert_frame(ctx->swr, ctx->fr_audio_target, NULL);
    frame_converted = true;
    frame_is_audio = true;
  } else {
    result = avcodec_receive_frame(ctx->ctx, ctx->fr);
    if (result != 0) {
      // try to get audio frame
      result = avcodec_receive_frame(ctx->ctxa, ctx->fr);
      if (result == 0)
        frame_is_audio = true;
    }
    while (result == AVERROR(EAGAIN)) {
      int res = av_read_frame(ctx->fmt_ctx, ctx->pkt);
      if (res >= 0 && ctx->pkt->stream_index != ctx->video_stream) {
        // Not a video packet
        if (ctx->pkt->stream_index == ctx->audio_stream) {
          res = avcodec_send_packet(ctx->ctxa, ctx->pkt);
          ERRCHECK2(res >= 0, "Cant send packet to audio decoder");
        }
        av_packet_unref(ctx->pkt);
        continue;
      }

      if (res < 0) {
        // Error or EOF, finish the stream.
        res = avcodec_send_packet(ctx->ctx, NULL);
      } else {
        if (ctx->pkt->pts == AV_NOPTS_VALUE) {
          printf("Needed to repopulate pts\n");
          ctx->pkt->pts = ctx->pkt->dts = ctx->i;
        }
        res = avcodec_send_packet(ctx->ctx, ctx->pkt);
      }
      av_packet_unref(ctx->pkt);

      ERRCHECK2(res >= 0, "Can't submit a packet to the decoder");

      // Retry decoding a frame
      result = avcodec_receive_frame(ctx->ctx, ctx->fr);
      if (result != 0) {
        result = avcodec_receive_frame(ctx->ctxa, ctx->fr);
        if (result == 0)
          frame_is_audio = true;
      }
    }
    if (result == AVERROR_EOF) {
      ctx->eof_encountered = true;
      return RESULT_EOF;
    } else if (result < 0) {
      printf("Err: decoding frame\n");
      return RESULT_ERROR;
    }
  }

  // Configure audio stuff JIT
  if (frame_is_audio && !ctx->audio_configured) {
    ctx->audio_samples_per_frame = ctx->fr->nb_samples;
    ctx->sample_rate = ctx->fr->sample_rate;
    ctx->fr_audio_target->sample_rate = ctx->fr->sample_rate;
    result = swr_config_frame(ctx->swr, ctx->fr_audio_target, ctx->fr);
    ERRCHECK("Cant configure SWR");
    ctx->audio_configured = true;
  }

  // int number_bytes_received = av_image_copy_to_buffer(
  //     write_loc_video, ctx->image_buffer_size,
  //     (const uint8_t *const *)ctx->fr->data, ctx->fr->linesize,
  //     ctx->ctx->pix_fmt, ctx->ctx->width, ctx->ctx->height, 1);
  // write_ringbuffer_commit(&ctx->image_buffer, ctx->image_buffer_size);
  // ERRCHECK2(number_bytes_received >= 0, "Can't copy image to buffer");
  int number_bytes_received = 0;

  if (frame_is_audio) {
    number_bytes_received = ctx->fr->nb_samples *
                            ctx->fr->ch_layout.nb_channels *
                            av_get_bytes_per_sample(ctx->fr->format);
    // if (!frame_converted)
    //   ctx->audio_time += (float)ctx->fr->nb_samples / ctx->sample_rate;
    if (!frame_converted)
      result = swr_convert_frame(ctx->swr, ctx->fr_audio_target, ctx->fr);

    if (ctx->audio_buffer_size == 0) {
      ctx->audio_buffer_size = ctx->fr_audio_target->linesize[0];
      size_t audio_ring_size = ctx->audio_buffer_size * RING_FRAMES;
      // If the ringbuffer size is too small compared to what raylib
      // expects(4096 bytes) make it bigger.
      if (audio_ring_size < 4096 * 8) {
        audio_ring_size = 4096 * 8;
        // It still has to divide by audio_buffer_size.
        size_t size_adj =
            ctx->audio_buffer_size - audio_ring_size % ctx->audio_buffer_size;
        audio_ring_size += size_adj;
      }
      ctx->audio_buffer = make_ringbuffer(audio_ring_size);
      printf("Audio buffer size is now %zu\n", ctx->audio_buffer.len);
      write_loc_audio = write_ringbuffer_chunk_nocommit(&ctx->audio_buffer,
                                                        ctx->audio_buffer_size);
    }
    memcpy(write_loc_audio, ctx->fr_audio_target->data[0],
           ctx->audio_buffer_size);
    ctx->audio_time +=
        (float)ctx->fr_audio_target->nb_samples / ctx->sample_rate;
    write_ringbuffer_commit(&ctx->audio_buffer, ctx->audio_buffer_size);
    printf("Audio linesize is %d, audio time is %f\n", ctx->fr->linesize[0],
           ctx->audio_time);
  } else {
    number_bytes_received = av_image_copy_to_buffer(
        write_loc_image, ctx->image_buffer_size,
        (const uint8_t *const *)ctx->fr->data, ctx->fr->linesize,
        ctx->ctx->pix_fmt, ctx->ctx->width, ctx->ctx->height, 1);
    ctx->video_time +=
        (float)ctx->fr->duration * ctx->video_tb.num / ctx->video_tb.den;
    write_ringbuffer_commit(&ctx->image_buffer, ctx->image_buffer_size);
    printf("video time is %f\n", ctx->video_time);
  }

  if (frame_converted)
    return RESULT_OK;
  printf("%10s, %10s, Frame duration: %8" PRId64
         " in %d/%d, Bytes received: %8d\n",
         av_ts2str(ctx->fr->pts), av_ts2str(ctx->fr->pkt_dts),
         ctx->fr->duration, ctx->fr->time_base.num, ctx->fr->time_base.den,
         number_bytes_received);
  // av_frame_unref(ctx->fr); Not really necessary, avcodec_receive_frame does
  // this for you
  ctx->i++;
  return RESULT_OK;
}

uint8_t *pull_image(DecoderContext *ctx) {
  printf("pull_image called\n");
  return read_ringbuffer_chunk(&ctx->image_buffer, ctx->image_buffer_size);
}

int pull_audio(DecoderContext *ctx, void *audio_buffer, unsigned int frames) {
  printf("pull_audio %s called\n", frames);
  if (!ctx->audio_configured) {
    return 0;
  }
  size_t bytes_to_read = frames * 2 * sizeof(float);
  return read_ringbuffer(&ctx->audio_buffer, audio_buffer, bytes_to_read);
}

void free_decoder_context(DecoderContext *ctx) {
  // TODO
}

bool is_decoder_finished(DecoderContext *ctx) { return ctx->eof_encountered; }

void ringbuffer_test() {
  RingBuffer trb = make_ringbuffer(16);
  uint8_t *wp = write_ringbuffer_chunk_nocommit(&trb, 4);
  char data[8];
  memcpy(wp, "abcd", 4);
  write_ringbuffer_commit(&trb, 4);
  wp = write_ringbuffer_chunk_nocommit(&trb, 4);
  memcpy(wp, "efgh", 4);
  write_ringbuffer_commit(&trb, 4);
  read_ringbuffer(&trb, (uint8_t *)data, 8);
  // write over buffer boundary
  write_to_ringbuffer(&trb, "ijklmnopqrs", 12);
  char data2[12];
  read_ringbuffer(&trb, (uint8_t *)data2, 12);
}

// int main(int argc, char **argv) {
//   // try out some simple stuff with ring buffer
//   if (argc < 2) {
//     printf("Not enough arguments");
//     return 1;
//   }
//   DecoderContext ctx;
//   if (initiate_decoding(&ctx, argv[1])) {
//     return 1;
//   }
//   while (pull_image(&ctx) == 0)
//     ;
//   return 0;
// }
/*
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
} */
