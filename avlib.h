#ifndef AVLIB_H
#define AVLIB_H
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <stdbool.h>

#include "utils.h"

typedef struct DecoderContext {
  int video_stream;
  int audio_stream;
  AVFormatContext *fmt_ctx;
  AVCodecContext *ctx;
  AVCodecContext *ctxa;
  AVPacket *pkt;
  AVFrame *fr;
  AVFrame *fr_audio_target;
  // AVFrame *fra;
  SwrContext *swr;

  RingBuffer image_buffer;
  RingBuffer audio_buffer;
  size_t image_buffer_size;
  size_t audio_buffer_size;
  int i;
  bool audio_configured;
  unsigned int audio_samples_per_frame;
  unsigned int sample_rate;
  unsigned int frame_rate;

  AVRational video_tb, audio_tb;
  float audio_time;
  float video_time;
} DecoderContext;

int initiate_decoding(DecoderContext *ctx, const char *file_name);
int pull_image(DecoderContext *ctx);
void free_decoder_context(DecoderContext *ctx);

#endif // !AVLIB_H
