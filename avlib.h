#ifndef AVLIB_H
#define AVLIB_H
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>

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
  bool eof_encountered;
  unsigned int audio_samples_per_frame;
  unsigned int sample_rate;
  unsigned int frame_rate;
  // unsigned int video_bitrate, audio_bitrate;
  unsigned int video_width;
  unsigned int video_height;
  enum AVPixelFormat pixel_format;

  AVRational video_tb, audio_tb;
  AVRational audio_time;
  AVRational video_time;
  AVRational video_framerate;
  float delta_time;
  float max_delta_time;
  float min_delta_time;

  pthread_mutex_t image_buffer_mtx;
  pthread_mutex_t audio_buffer_mtx;
  sem_t sem;
  sem_t startup_sem;
  bool startup_happened;
} DecoderContext;

#define RESULT_ERROR -1
#define RESULT_OK 0
#define RESULT_STALL 1
#define RESULT_EOF 2
int initiate_decoding(DecoderContext *ctx, const char *file_name);
int continue_decoding(DecoderContext *ctx);
uint8_t *pull_image(DecoderContext *ctx, float *timestamp);
void release_image(DecoderContext *ctx);
int pull_audio(DecoderContext *ctx, void *audio_buffer, unsigned int frames);
void free_decoder_context(DecoderContext *ctx);
bool is_decoder_finished(DecoderContext *ctx);
void seek_to_frame(DecoderContext *ctx, unsigned int frame);

#endif // !AVLIB_H
