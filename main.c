#include <GL/gl.h>
#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

// #include "avlib_fork.h"
#include "avlib.h"
#include "utils.h"
#define RAYGUI_IMPLEMENTATION
#include "vendor/raygui/src/raygui.h"

const bool audio_dbg = false;

// FILE *tempfile;

#define RAILIB_AUDIO_BUFFER_INTERNAL 4096
#define BUFFER_SIZE (RAILIB_AUDIO_BUFFER_INTERNAL * 4)

volatile AVRational audio_timest = (AVRational){0, 1};
float cb_timing, min_cb_timing = INFINITY, max_cb_timing = -INFINITY;
// struct timespec StartTime;
// bool video_just_started = false;
DecoderContext dc;
FILE *log_file;

void open_log() { log_file = fopen("raylib.log", "w"); }
void close_log() {
  if (log_file)
    fclose(log_file);
}

void tracelog_impl(FILE *log_stream, TraceLogLevel logLevel, const char *text,
                   va_list args) {
  switch (logLevel) {
  case LOG_INFO:
    fputs("INFO: ", log_stream);
    break;
  case LOG_ERROR:
    fputs("ERROR: ", log_stream);
    break;
  case LOG_DEBUG:
    fputs("DEBUG: ", log_stream);
    break;
  case LOG_FATAL:
    fputs("FATAL: ", log_stream);
    break;
  case LOG_TRACE:
    fputs("TRACE: ", log_stream);
    break;
  case LOG_WARNING:
    fputs("WARNING: ", log_stream);
    break;
  case LOG_ALL:
  case LOG_NONE:
    return;
    break;
  }
  vfprintf(log_stream, text, args);
  fputc('\n', log_stream);
}

void tracelog_cb(int logLevel, const char *text, va_list args) {
  FILE *log_stream = stdout;
  if (logLevel >= LOG_ERROR) {
    log_stream = stderr;
  }
  va_list aq;
  va_copy(aq, args);
  va_end(aq);
  tracelog_impl(log_stream, logLevel, text, args);
  if (log_file) {
    tracelog_impl(log_file, logLevel, text, aq);
  }
}

void audio_cb(void *frame_data, unsigned int frames) {
  // the amount of data pulled by this function will always be no more than 4096
  // bytes (hardcoded in raylib, see ReadAudioBufferFramesInMixingFormat
  // internal function in raudio.c)
  // printf("Requested %lu bytes by audio callback\n", frames * sizeof(float) *
  // 2);
  struct timespec time_start, time_end;
  clock_gettime(CLOCK_MONOTONIC, &time_start);
  /*int size = */ dec_pull_audio(&dc, frame_data, frames, &audio_timest);

  // printf("pull audio returned %d\n", size);
  static bool info_given = false;
  if (!info_given) {
    TraceLog(LOG_INFO, "sample rate: %d", dc.sample_rate);
    info_given = true;
  }
  // audio_timest += (float)frames / dc.sample_rate;
  clock_gettime(CLOCK_MONOTONIC, &time_end);
  long int td = (time_end.tv_sec - time_start.tv_sec) * 1000000000 +
                time_end.tv_nsec - time_start.tv_nsec;
  cb_timing = (float)td / 1000000000;
  if (min_cb_timing > cb_timing)
    min_cb_timing = cb_timing;
  if (max_cb_timing < cb_timing)
    max_cb_timing = cb_timing;
}

int main(int argc, char **argv) {
  // tempfile = fopen("tempfile.out", "wb");
  open_log();
  atexit(&close_log);

  SetTraceLogLevel(LOG_DEBUG);
  SetTraceLogCallback(tracelog_cb);
  float current_volume = 0.2;
  if (argc != 2) {
    return 1;
  }

  char *file_name = argv[1];
  dec_initialize();
  int result = dec_init_decoder(&dc);
  if (result != 0)
    exit(EXIT_FAILURE);
  result = dec_open_file(&dc, file_name);
  if (result != 0)
    exit(EXIT_FAILURE);

  unsigned int scaled_width = Clamp(dc.video_width, 100, 2000);
  unsigned int scaled_height = Clamp(dc.video_height, 100, 1200);
  float scale_factor_w = (float)scaled_width / dc.video_width;
  float scale_factor_h = (float)scaled_height / dc.video_height;
  float scale_factor = scale_factor_w;
  if (scale_factor_h < scale_factor)
    scale_factor = scale_factor_h;

  InitWindow(scaled_width, scaled_height, file_name);
  SetWindowState(FLAG_WINDOW_ALWAYS_RUN);
  InitAudioDevice();
  SetAudioStreamBufferSizeDefault(BUFFER_SIZE);

  SetTargetFPS(60);
  RaylibObjects video_tex;
  dec_init_graphics_objects(&dc, &video_tex);
  // dec_wait_ready(&dc);

  AudioStream stream = LoadAudioStream(dc.sample_rate, 32, 2);
  SetAudioStreamCallback(stream, audio_cb);
  SetAudioStreamVolume(stream, current_volume);
  PlayAudioStream(stream);
  bool stream_paused = false;
  double start_ts = GetTime();
  float video_timest = 0;

  while (!WindowShouldClose() && !dec_is_decoder_finished(&dc)) {
    // if (dc.audio_stopped) {
    //   StopAudioStream(stream);
    // }
    if (IsKeyPressed(KEY_DOWN)) {
      current_volume -= 0.05;
      current_volume = Clamp(current_volume, 0.0, 1.0);
      SetAudioStreamVolume(stream, current_volume);
    }
    if (IsKeyPressed(KEY_UP)) {
      current_volume += 0.05;
      current_volume = Clamp(current_volume, 0.0, 1.0);
      SetAudioStreamVolume(stream, current_volume);
    }
    if (IsKeyPressed(KEY_SPACE)) {
      if (stream_paused) {
        PlayAudioStream(stream);
        stream_paused = false;
      } else {
        StopAudioStream(stream);
        stream_paused = true;
      }
    }
    if (IsKeyPressed(KEY_D)) {
      if (video_timest != 0)
        dec_seek_to_frame(&dc, video_timest + 10.0);
    }
    if (IsKeyPressed(KEY_A)) {
      if (video_timest != 0)
        dec_seek_to_frame(&dc, video_timest - 20.0);
    }
    ClearBackground(BLACK);

    double audio_ts_double = av_q2d(audio_timest);
    dec_update_textures(&dc, &video_tex, audio_ts_double);
    video_timest = dc.video_timest;

    BeginDrawing();
    // printf("delta: %f, max delta: %f, min delta: %f\n", dc.delta_time,
    //        dc.max_delta_time, dc.min_delta_time);

    dec_draw_video_textures(&video_tex, (Vector2){0, 0}, 0.0, scale_factor,
                            WHITE);

    if (!stream_paused)
      dec_update_timelines(&dc);

    Vector2 overlay_dims =
        dec_draw_debug_overlay(&dc, &video_tex, audio_ts_double, 10, 50);

    if (stream_paused) {
      DrawRectangle(scaled_width / 2 - 30, scaled_height / 2 - 50, 20, 100,
                    ColorAlpha(WHITE, 0.8));
      DrawRectangle(scaled_width / 2 + 10, scaled_height / 2 - 50, 20, 100,
                    ColorAlpha(WHITE, 0.8));
    }

    DrawFPS(10, 10);
    float new_ts = video_timest;
    // Get window's current dimensions
    int render_width = GetRenderWidth();
    int render_height = GetRenderHeight();

    int status =
        GuiSliderBar((Rectangle){20, render_height - 50, render_width - 40, 30},
                     NULL, NULL, &new_ts, 0, dc.duration);
    char msga[32], msgb[32], msg[128];
    time_to_str(video_timest, msga, sizeof(msga));
    time_to_str(dc.duration, msgb, sizeof(msgb));
    snprintf(msg, sizeof(msg), "%s / %s", msga, msgb);
    DrawText(msg, 20, render_height - 80, 25, WHITE);

    if (status == RESULT_CHANGED) {
      dec_seek_to_frame(&dc, new_ts);
    }
    EndDrawing();
  }

  // free_timeline(&abuffer_timeline);
  // free_timeline(&vbuffer_timeline);
  dec_shutdown(&dc);
  UnloadAudioStream(stream);
  CloseAudioDevice();
  CloseWindow();

  return 0;
}
