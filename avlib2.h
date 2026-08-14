#ifndef AVLIB_H
#define AVLIB_H

#include "utils.h"
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <raylib.h>
#include <stdbool.h>
#include <stdio.h>

struct DecoderPrivate;

typedef enum DecoderState {
  DS_UNINIT = 0, // default ZERO value
  DS_READY,      // after call to dec_init_decoder
  DS_STARTUP,    // filling buffers before playback
  DS_PLAYING,    // actively playing the video
  DS_FINISHED,   // video file at EOF
  DS_SHUTDOWN,   // after call to dec_shutdown
  DS_ERROR = 100,
} DecoderState;

typedef struct RaylibObjects {
  Shader video_shader;
  Texture2D tex_luma, tex_u, tex_v;
  int y_location, u_location, v_location;
  float video_timest;
  unsigned int frame_counter;
  unsigned int bytespp;
} RaylibObjects;

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
  unsigned long int abytes_pulled, vbytes_pulled;
  unsigned long int abytes_written, vbytes_written;

  DecoderState state;
} DecoderContext;

// extern FILE *tempfile;

#define RESULT_ERROR -1
#define RESULT_OK 0
#define RESULT_STALL 1
#define RESULT_EOF 2
int dec_init_decoder(DecoderContext *ctx);
int dec_open_file(DecoderContext *ctx, const char *file_name);
// int dec_continue_decoding(DecoderContext *ctx);
uint8_t *dec_pull_image(DecoderContext *ctx, float *timestamp);
void dec_release_image(DecoderContext *ctx);
int dec_pull_audio(DecoderContext *ctx, void *audio_buffer, unsigned int frames,
                   volatile AVRational *ts);
void free_decoder_context(DecoderContext *ctx);
bool dec_is_decoder_finished(DecoderContext *ctx);
void dec_seek_to_frame(DecoderContext *ctx, double ts);

void dec_update_timelines(DecoderContext *ctx);
void dec_initialize();
void dec_shutdown(DecoderContext *ctx);
// void dec_wait_ready(DecoderContext *ctx);

int dec_init_graphics_objects(DecoderContext *ctx, RaylibObjects *objs);
int dec_update_textures(DecoderContext *ctx, RaylibObjects *objs, float ts);
int dec_draw_video_textures(RaylibObjects *objs, Vector2 position,
                            float rotation, float scale_factor, Color tint);
void timeline_draw_ui(TimeLine tl, int x, int y, int width, int height,
                      unsigned int max);
#endif // !AVLIB_H
