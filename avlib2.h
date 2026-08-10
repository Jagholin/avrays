#ifndef AVLIB_H
#define AVLIB_H

#include "utils.h"
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <stdbool.h>

struct DecoderPrivate;

typedef enum DecoderState {
  DS_UNINIT = 0,
  DS_INITIALIZED,
  DS_READY,
  DS_ERROR = 100,
} DecoderState;

typedef struct DecoderContext {
  struct DecoderPrivate *p;

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

  TimeLine vbuffer_timeline;
  TimeLine abuffer_timeline;

  DecoderState state;
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

void dec_update_timelines(DecoderContext *ctx);
void dec_initialize();
void dec_shutdown();
void dec_wait_ready(DecoderContext *ctx);

#endif // !AVLIB_H
