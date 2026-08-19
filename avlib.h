#ifndef AVLIB_H
#define AVLIB_H

#include "utils.h"
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <pthread.h>
#include <raylib.h>
#include <semaphore.h>
#include <stdbool.h>

#define AVRAT_ZERO (AVRational){0, 1};

struct DecoderPrivate;

typedef enum DecoderState {
  DS_UNINIT = 0, // default ZERO value
  DS_READY,      // after call to avray_init_decoder
  DS_STARTUP,    // filling buffers before playback
  DS_PLAYING,    // actively playing the video
  DS_FILEEOF,    // video file at EOF, but buffers aren't empty yet
  DS_FINISHED,   // File at EOF and buffers are empty. Decoder thread transfers
                 // this state to DS_READY.
  DS_SHUTDOWN,   // after call to avray_shutdown
  DS_ERROR = 100,
} DecoderState;

typedef struct RaylibObjects {
  Shader video_shader;
  Texture2D tex_luma, tex_u, tex_v;
  int y_location, u_location, v_location;
  unsigned int frame_counter;
  unsigned int bytespp;
} RaylibObjects;

typedef pthread_mutex_t MutexType;
typedef sem_t SemaphoreType;
typedef pthread_t ThreadType;

void mutex_lock(MutexType *m);
void mutex_unlock(MutexType *m);

typedef struct DecoderContext {
  struct DecoderPrivate *p;

  MutexType dc_mutex; // This mutex is locked when big changes are made to the
                      // other fields

  // Sample rate from audio codec, initialized in avray_open_file().
  unsigned int sample_rate;
  // frame rate from video codec, initialized in avray_open_file().
  unsigned int frame_rate;
  // unsigned int video_bitrate, audio_bitrate;
  // Video width from video codec, initialized in avray_open_file().
  unsigned int video_width;
  // Video height from video codec, initialized in avray_open_file().
  unsigned int video_height;
  // Pixel format used in the video stream, initialized in avray_open_file().
  enum AVPixelFormat pixel_format;

  // Video and audio time bases, initialized in avray_open_file().
  AVRational video_tb, audio_tb;
  // Timestamp of the last displayed image, updated in avray_update_textures().
  float video_timest;
  // Approximate duration in seconds, initialized in avray_open_file().
  double duration;

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
int time_to_str(double seconds, char *buf, size_t n);

int avray_init_decoder(DecoderContext *ctx);
int avray_open_file(DecoderContext *ctx, const char *file_name);
// int avray_continue_decoding(DecoderContext *ctx);
uint8_t *avray_pull_image(DecoderContext *ctx, float *timestamp);
void avray_release_image(DecoderContext *ctx);
int avray_pull_audio(DecoderContext *ctx, void *audio_buffer,
                     unsigned int frames, volatile AVRational *ts);
void free_decoder_context(DecoderContext *ctx);
bool avray_is_decoder_stopped(DecoderContext *ctx);
void avray_seek_to_frame(DecoderContext *ctx, double ts);
void avray_close_file(DecoderContext *ctx);

void avray_update_timelines(DecoderContext *ctx);
void avray_initialize();
void avray_shutdown(DecoderContext *ctx);
// void avray_wait_ready(DecoderContext *ctx);

int avray_init_graphics_objects(DecoderContext *ctx, RaylibObjects *objs);
int avray_free_graphics_objects(RaylibObjects *objs);
int avray_update_textures(DecoderContext *ctx, RaylibObjects *objs, float ts);
int avray_draw_video_textures(RaylibObjects *objs, Vector2 position,
                              float rotation, float scale_factor, Color tint);
void timeline_draw_ui(TimeLine tl, int x, int y, int width, int height,
                      unsigned int max);
Vector2 avray_draw_debug_overlay(DecoderContext *ctx, RaylibObjects *objs,
                                 double audio_ts, int x, int y);
#endif // !AVLIB_H
