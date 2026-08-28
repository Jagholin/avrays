/**********************************************************************************************
 *
 *   LICENSE: zlib/libpng
 *
 *   avray is licensed under an unmodified zlib/libpng license, which is an
 * OSI-certified, BSD-like license that allows static linking with closed source
 * software:
 *
 *   Copyright (c) 2026 Jagholin (github.com/Jagholin)
 *
 *   This software is provided "as-is", without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from the
 * use of this software.
 *
 *   Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 *     1. The origin of this software must not be misrepresented; you must not
 * claim that you wrote the original software. If you use this software in a
 * product, an acknowledgment in the product documentation would be appreciated
 * but is not required.
 *
 *     2. Altered source versions must be plainly marked as such, and must not
 * be misrepresented as being the original software.
 *
 *     3. This notice may not be removed or altered from any source
 * distribution.
 *
 **********************************************************************************************/
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
  DS_UNINIT = 0,  // default ZERO value
  DS_READY,       // after call to avray_init_decoder
  DS_STARTUP,     // filling buffers before playback
  DS_PLAYING,     // actively playing the video
  DS_FILEEOF,     // video file at EOF, but buffers aren't empty yet
  DS_FINISHED,    // File at EOF and buffers are empty. Otherwise the same as
                  // DS_READY, can call avray_open_file
  DS_SHUTDOWN,    // after call to avray_shutdown
  DS_ERROR = 100, // error condition
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

#define RESULT_ERROR -1
#define RESULT_OK 0
#define RESULT_STALL 1
#define RESULT_EOF 2
#define RESULT_CANT_OPEN 3
int time_to_str(double seconds, char *buf, size_t n);

int avray_init_decoder(DecoderContext *ctx);
int avray_open_file(DecoderContext *ctx, const char *file_name);
// int avray_continue_decoding(DecoderContext *ctx);
uint8_t *avray_pull_image(DecoderContext *ctx, float *timestamp);
void avray_release_image(DecoderContext *ctx);
int avray_pull_audio(DecoderContext *ctx, void *audio_buffer,
                     unsigned int frames);
// call avray_shutdown instead
// void free_decoder_context(DecoderContext *ctx);
bool avray_is_decoder_stopped(DecoderContext *ctx);
void avray_seek_to_frame(DecoderContext *ctx, double ts);
void avray_close_file(DecoderContext *ctx);

void avray_update_timelines(DecoderContext *ctx);
void avray_shutdown(DecoderContext *ctx);
// void avray_wait_ready(DecoderContext *ctx);

int avray_init_graphics_objects(DecoderContext *ctx, RaylibObjects *objs);
int avray_free_graphics_objects(RaylibObjects *objs);
int avray_update_textures(DecoderContext *ctx, RaylibObjects *objs);
int avray_draw_video_textures(RaylibObjects *objs, Vector2 position,
                              float rotation, float scale_factor, Color tint);
void timeline_draw_ui(TimeLine tl, int x, int y, int width, int height,
                      unsigned int max, bool draw_background);
Vector2 avray_draw_debug_overlay(DecoderContext *ctx, RaylibObjects *objs,
                                 int x, int y, Vector2 *pdims,
                                 bool draw_background);
#endif // !AVLIB_H
