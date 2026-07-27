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
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "avlib.h"
#include "utils.h"

// 30 frames buffer is about a second for most videos
#define RING_FRAMES 30

// FILE *outfile;

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
  *ctx = (DecoderContext){.min_delta_time = INFINITY,
                          .max_delta_time = -INFINITY,
                          .audio_time = av_make_q(0, 1),
                          .video_time = av_make_q(0, 1)};
  pthread_mutex_init(&ctx->buffer_mtx, NULL);
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
  // ctx->ctx->get_format = &get_format_cb;

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

  ctx->image_buffer_size =
      av_image_get_buffer_size(ctx->ctx->pix_fmt, ctx->ctx->width,
                               ctx->ctx->height, 16) +
      sizeof(float);
  // TODO: initialize audio buffer

  ctx->image_buffer = make_ringbuffer(ctx->image_buffer_size * RING_FRAMES);

  printf("Audio codec data: sample format %s, sample rate %d, bytes per sample "
         "%d\n",
         av_get_sample_fmt_name(ctx->ctxa->sample_fmt), ctx->ctxa->sample_rate,
         av_get_bytes_per_sample(ctx->ctxa->sample_fmt));
  ctx->sample_rate = ctx->ctxa->sample_rate;

  printf("video Time base is %d/%d\n",
         ctx->fmt_ctx->streams[ctx->video_stream]->time_base.num,
         ctx->fmt_ctx->streams[ctx->video_stream]->time_base.den);
  printf("audio Time base is %d/%d\n",
         ctx->fmt_ctx->streams[ctx->audio_stream]->time_base.num,
         ctx->fmt_ctx->streams[ctx->audio_stream]->time_base.den);
  ctx->video_tb = ctx->fmt_ctx->streams[ctx->video_stream]->time_base;
  ctx->audio_tb = ctx->fmt_ctx->streams[ctx->audio_stream]->time_base;

  result = sem_init(&ctx->sem, 0, 1);
  ERRCHECK("Cant initialize semaphore?!");
  result = sem_init(&ctx->startup_sem, 0, 0);
  ERRCHECK("Cant initialize semaphore?!");

  printf("Codec framerate: %d/%d\n", ctx->ctx->framerate.num,
         ctx->ctx->framerate.den);
  ctx->video_framerate = ctx->ctx->framerate;

  return 0;
cleanup:
  return -1;
}

void signal_startup(DecoderContext *ctx) {
  if (ctx->startup_happened)
    return;
  sem_post(&ctx->startup_sem);
  ctx->startup_happened = true;
}

int continue_decoding(DecoderContext *ctx) {
  // So that we dont get stuck in the stall spinlock, we
  // utilize the semaphore to indicate that the decoding can proceed
  //
  // Decoding has to happen in a separate thread, otherwise there is a
  // danger of self deadlock
  sem_wait(&ctx->sem);
  sem_post(&ctx->sem);
  pthread_mutex_lock(&ctx->buffer_mtx);

  bool frame_is_audio = false;
  int result = 0;
  uint8_t *write_loc_audio = NULL;

  // First look if we can receive an additional audio frame from data
  // accumulated in SWR
  if (ctx->audio_configured) {
    write_loc_audio = write_ringbuffer_chunk_nocommit(&ctx->audio_buffer,
                                                      ctx->audio_buffer_size);
    if (write_loc_audio == NULL) {
      signal_startup(ctx);
      sem_wait(&ctx->sem); // indicate stall condition
      result = RESULT_STALL;
      goto cleanup;
    }
  }
  uint8_t *write_loc_image = write_ringbuffer_chunk_nocommit(
      &ctx->image_buffer, ctx->image_buffer_size);
  if (write_loc_image == NULL) {
    signal_startup(ctx);
    sem_wait(&ctx->sem); // indicate stall condition
    result = RESULT_STALL;
    goto cleanup;
  }
  bool frame_converted = false;
  if (ctx->audio_configured) {
    // printf("SWR delay: %ld/%d\n", swr_get_delay(ctx->swr, ctx->sample_rate),
    //      ctx->audio_samples_per_frame);
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
          ERRCHECK2(res >= 0, "Cant send packet to audio decoder: %d\n", res);
          result = avcodec_receive_frame(ctx->ctxa, ctx->fr);
          if (result == 0)
            frame_is_audio = true;
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

      ERRCHECK2(res >= 0, "Can't submit a packet to the decoder: %d\n", res);

      // Retry decoding a frame
      result = avcodec_receive_frame(ctx->ctx, ctx->fr);
      if (result != 0) {
        // result = avcodec_receive_frame(ctx->ctxa, ctx->fr);
        // if (result == 0)
        //   frame_is_audio = true;
      }
    }
    if (result == AVERROR_EOF) {
      ctx->eof_encountered = true;
      result = RESULT_EOF;
      goto cleanup;
    } else if (result < 0) {
      printf("Err: decoding frame\n");
      result = RESULT_ERROR;
      goto cleanup;
    }
  }

  // Configure audio stuff JIT
  if (frame_is_audio && !ctx->audio_configured) {
    ctx->audio_samples_per_frame = ctx->fr->nb_samples;
    ctx->sample_rate = ctx->fr->sample_rate;
    ctx->fr_audio_target->sample_rate = ctx->fr->sample_rate;
    result = swr_config_frame(ctx->swr, ctx->fr_audio_target, ctx->fr);
    // result = swr_alloc_set_opts2(
    //     &ctx->swr, &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO,
    //     AV_SAMPLE_FMT_FLT, ctx->fr->sample_rate, &ctx->fr->ch_layout,
    //     ctx->fr->format, ctx->fr->sample_rate, 0, NULL);
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
    if (!frame_converted) {
      result = swr_convert_frame(ctx->swr, ctx->fr_audio_target, ctx->fr);
    }

    if (ctx->audio_buffer_size == 0) {
      // ctx->audio_buffer_size = ctx->fr_audio_target->linesize[0];
      ctx->audio_buffer_size =
          ctx->fr_audio_target->nb_samples * sizeof(float) * 2;
      // How much is 1 second of sound? sample_rate samples
      size_t target_audio_ring_size = ctx->sample_rate * sizeof(float) * 2;
      // This is now also the bitrate for sound
      size_t size_adj = ctx->audio_buffer_size -
                        target_audio_ring_size % ctx->audio_buffer_size;
      target_audio_ring_size += size_adj;
      size_t audio_ring_size = target_audio_ring_size;
      // If the ringbuffer size is too small compared to what raylib
      // expects(4096 bytes) make it bigger.
      if (audio_ring_size < 4096 * 8) {
        audio_ring_size = 4096 * 8;
        // It still has to divide by audio_buffer_size.
        size_adj =
            ctx->audio_buffer_size - audio_ring_size % ctx->audio_buffer_size;
        audio_ring_size += size_adj;
      }
      ctx->audio_buffer = make_ringbuffer(audio_ring_size);
      printf("Audio buffer size is now %zu\n", ctx->audio_buffer.buf_size);
      write_loc_audio = write_ringbuffer_chunk_nocommit(&ctx->audio_buffer,
                                                        ctx->audio_buffer_size);
    }
    // assert(ctx->fr_audio_target->linesize[0] == ctx->audio_buffer_size);
    assert(ctx->fr_audio_target->nb_samples * 8 == ctx->audio_buffer_size);
    // fwrite(ctx->fr_audio_target->data[0], 1, ctx->audio_buffer_size,
    // outfile);
    memcpy(write_loc_audio, ctx->fr_audio_target->data[0],
           ctx->audio_buffer_size);
    ctx->audio_time =
        av_add_q(ctx->audio_time,
                 av_make_q(ctx->fr_audio_target->nb_samples, ctx->sample_rate));
    write_ringbuffer_commit(&ctx->audio_buffer, ctx->audio_buffer_size);
    // printf("Audio linesize is %d, audio time is %f\n", ctx->fr->linesize[0],
    //      ctx->audio_time);
  } else {
    number_bytes_received = av_image_copy_to_buffer(
        write_loc_image + sizeof(float), ctx->image_buffer_size,
        (const uint8_t *const *)ctx->fr->data, ctx->fr->linesize,
        ctx->ctx->pix_fmt, ctx->ctx->width, ctx->ctx->height, 1);
    ctx->video_time =
        av_mul_q(av_make_q(ctx->fr->best_effort_timestamp, 1), ctx->video_tb);
    (*(float *)write_loc_image) =
        (float)ctx->video_time.num / ctx->video_time.den;
    write_ringbuffer_commit(&ctx->image_buffer, ctx->image_buffer_size);
    // printf("video time is %f\n", ctx->video_time);
  }

  ctx->delta_time = av_q2d(av_sub_q(ctx->video_time, ctx->audio_time));
  if (ctx->delta_time > ctx->max_delta_time) {
    ctx->max_delta_time = ctx->delta_time;
  }
  if (ctx->delta_time < ctx->min_delta_time) {
    ctx->min_delta_time = ctx->delta_time;
  }

  if (!frame_converted) {
    // printf("%10s, %10s, Frame duration: %8" PRId64
    // " in %d/%d, Bytes received: %8d\n",
    // av_ts2str(ctx->fr->pts), av_ts2str(ctx->fr->pkt_dts),
    // ctx->fr->duration, ctx->fr->time_base.num, ctx->fr->time_base.den,
    // number_bytes_received);
    // av_frame_unref(ctx->fr); Not really necessary, avcodec_receive_frame does
    // this for you
    ctx->i++;
  }
  result = RESULT_OK;
cleanup:
  // ringbuffer_release_mtx(&ctx->audio_buffer);
  // ringbuffer_release_mtx(&ctx->image_buffer);
  pthread_mutex_unlock(&ctx->buffer_mtx);
  return result;
}

void try_unlock_decoder_sem(DecoderContext *ctx) {
  int sem_value;
  sem_getvalue(&ctx->sem, &sem_value);
  if (sem_value == 0) {
    // we can advance semaphore if both buffers are less than 80% empty
    if (ringbuffer_len(&ctx->image_buffer) < 0.8 * ctx->image_buffer.buf_size &&
        ringbuffer_len(&ctx->audio_buffer) < 0.8 * ctx->audio_buffer.buf_size) {
      sem_post(&ctx->sem);
    }
  }
}

uint8_t *pull_image(DecoderContext *ctx, float *timestamp) {
  pthread_mutex_lock(&ctx->buffer_mtx);
  // printf("pull_image called\n");
  uint8_t *data =
      read_ringbuffer_chunk(&ctx->image_buffer, ctx->image_buffer_size);
  if (data == NULL)
    return data;
  *timestamp = *(float *)data;
  return data + sizeof(float);
}

void release_image(DecoderContext *ctx) {
  try_unlock_decoder_sem(ctx);
  pthread_mutex_unlock(&ctx->buffer_mtx);
}

int pull_audio(DecoderContext *ctx, void *audio_buffer, unsigned int frames) {
  // printf("pull_audio %d called\n", frames);
  pthread_mutex_lock(&ctx->buffer_mtx);
  if (!ctx->audio_configured) {
    pthread_mutex_unlock(&ctx->buffer_mtx);
    return 0;
  }
  size_t bytes_to_read = frames * 2 * sizeof(float);
  int result = read_ringbuffer(&ctx->audio_buffer, audio_buffer, bytes_to_read);
  try_unlock_decoder_sem(ctx);
  pthread_mutex_unlock(&ctx->buffer_mtx);
  return result;
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
//   if (argc < 2) {
//     printf("Not enough arguments");
//     return 1;
//   }
//   outfile = fopen("audio_pure", "wb");
//   char buff[4096];
//   DecoderContext ctx;
//   if (initiate_decoding(&ctx, argv[1])) {
//     return 1;
//   }
//   while (true) {
//     int result = 0;
//     while (result == 0)
//       result = continue_decoding(&ctx);
//     if (result == RESULT_EOF || result == RESULT_ERROR)
//       break;
//
//     // pull audio
//     for (;;) {
//       int bytes = pull_audio(&ctx, buff, 4096 / 8);
//       if (bytes != 0) {
//         // fwrite(buff, 1, bytes, outfile);
//       } else
//         break;
//     }
//     for (;;) {
//       uint8_t *res = pull_image(&ctx);
//       release_image(&ctx);
//       if (res == NULL)
//         break;
//     }
//   }
//   fclose(outfile);
//   return 0;
// }
