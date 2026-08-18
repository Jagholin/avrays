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
double start_clock_time;

// FILE *tempfile;

#define RAILIB_AUDIO_BUFFER_INTERNAL 4096
#define BUFFER_SIZE (RAILIB_AUDIO_BUFFER_INTERNAL * 4)

AVRational audio_timest = (AVRational){0, 1};
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

void tracelog_impl(FILE *log_stream, TraceLogLevel logLevel, double time,
                   const char *text, va_list args) {
  const char *log_type = "";
  switch (logLevel) {
  case LOG_INFO:
    log_type = "INFO";
    break;
  case LOG_ERROR:
    log_type = "ERROR";
    break;
  case LOG_DEBUG:
    log_type = "DEBUG";
    break;
  case LOG_FATAL:
    log_type = "FATAL";
    break;
  case LOG_TRACE:
    log_type = "TRACE";
    break;
  case LOG_WARNING:
    log_type = "WARNING";
    break;
  case LOG_ALL:
  case LOG_NONE:
    return;
    break;
  }
  fprintf(log_stream, "%s (%.3lf): ", log_type, time);
  vfprintf(log_stream, text, args);
  fputc('\n', log_stream);
}

void tracelog_cb(int logLevel, const char *text, va_list args) {
  FILE *log_stream = stdout;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  double time = (double)ts.tv_nsec / 1000000000;
  time += ts.tv_sec;
  time -= start_clock_time;

  if (logLevel >= LOG_ERROR) {
    log_stream = stderr;
  }
  va_list aq;
  va_copy(aq, args);
  va_end(aq);
  tracelog_impl(log_stream, logLevel, time, text, args);
  if (log_file) {
    tracelog_impl(log_file, logLevel, time, text, aq);
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
  /*int size = */ avray_pull_audio(&dc, frame_data, frames, &audio_timest);

  // printf("pull audio returned %d\n", size);
  static bool info_given = false;
  if (!info_given) {
    TraceLog(LOG_INFO, "AVRAYS: sample rate: %d", dc.sample_rate);
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

void rescale_window(float *sf, int *sw, int *sh) {
  unsigned int scaled_width = Clamp(dc.video_width, 100, 2000);
  unsigned int scaled_height = Clamp(dc.video_height, 100, 1200);
  float scale_factor_w = (float)scaled_width / dc.video_width;
  float scale_factor_h = (float)scaled_height / dc.video_height;
  float scale_factor = scale_factor_w;
  if (scale_factor_h < scale_factor)
    scale_factor = scale_factor_h;

  if (sf)
    *sf = scale_factor;
  if (sw)
    *sw = scaled_width;
  if (sh)
    *sh = scaled_height;

  SetWindowSize(scaled_width, scaled_height);
}

int main(int argc, char **argv) {
  // tempfile = fopen("tempfile.out", "wb");
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  start_clock_time = (double)ts.tv_nsec / 1000000000;
  start_clock_time += ts.tv_sec;
  open_log();
  atexit(&close_log);

  SetTraceLogLevel(LOG_DEBUG);
  SetTraceLogCallback(tracelog_cb);
  float current_volume = 0.2;
  if (argc < 2) {
    return 1;
  }
  avray_initialize();
  int result = avray_init_decoder(&dc);
  if (result != 0)
    exit(EXIT_FAILURE);

  unsigned int file_index = 1;
  char *file_name = argv[file_index];
  result = avray_open_file(&dc, file_name);
  if (result != 0)
    exit(EXIT_FAILURE);

  float scale_factor = 1.0;
  int scaled_width = 1, scaled_height = 1;
  InitWindow(1024, 768, file_name);
  rescale_window(&scale_factor, &scaled_width, &scaled_height);
  SetWindowState(FLAG_WINDOW_ALWAYS_RUN);
  InitAudioDevice();
  SetAudioStreamBufferSizeDefault(BUFFER_SIZE);

  SetTargetFPS(60);
  RaylibObjects video_tex;
  avray_init_graphics_objects(&dc, &video_tex);
  // avray_wait_ready(&dc);

  AudioStream stream = LoadAudioStream(dc.sample_rate, 32, 2);
  SetAudioStreamCallback(stream, audio_cb);
  SetAudioStreamVolume(stream, current_volume);
  PlayAudioStream(stream);
  bool stream_paused = false;
  float video_timest = 0;

  while (!WindowShouldClose() && !avray_is_decoder_stopped(&dc)) {
    mutex_lock(&dc.dc_mutex);
    DecoderState st = dc.state;
    mutex_unlock(&dc.dc_mutex);

    if (st == DS_READY) {
      // Open the next file
      file_index++;
      if (file_index >= argc) {
        // No more files in queue
        break;
      }
      StopAudioStream(stream);
      UnloadAudioStream(stream);
      avray_free_graphics_objects(&video_tex);

      file_name = argv[file_index];
      if (avray_open_file(&dc, file_name) != RESULT_OK)
        break;
      rescale_window(&scale_factor, &scaled_width, &scaled_height);
      avray_init_graphics_objects(&dc, &video_tex);

      stream = LoadAudioStream(dc.sample_rate, 32, 2);
      SetAudioStreamCallback(stream, audio_cb);
      SetAudioStreamVolume(stream, current_volume);
      PlayAudioStream(stream);

      video_timest = 0;
      audio_timest = AVRAT_ZERO;

      SetWindowTitle(file_name);
    }

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
    if (IsKeyPressed(KEY_LEFT_BRACKET) && file_index >= 2 &&
        (st == DS_PLAYING || st == DS_STARTUP || st == DS_FINISHED)) {
      // go to the previous file
      // -2 because the code in if (st==DS_READY) will increment this.
      file_index -= 2;
      avray_close_file(&dc);
    }
    if (IsKeyPressed(KEY_RIGHT_BRACKET) && file_index + 1 < argc &&
        (st == DS_PLAYING || st == DS_STARTUP || st == DS_FINISHED)) {
      // file_index will be incremented in the DS_READY handler above
      avray_close_file(&dc);
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
        avray_seek_to_frame(&dc, video_timest + 10.0);
    }
    if (IsKeyPressed(KEY_A)) {
      if (video_timest != 0)
        avray_seek_to_frame(&dc, video_timest - 20.0);
    }
    ClearBackground(BLACK);

    double audio_ts_double = av_q2d(audio_timest);
    avray_update_textures(&dc, &video_tex, audio_ts_double);
    video_timest = dc.video_timest;

    BeginDrawing();

    avray_draw_video_textures(&video_tex, (Vector2){0, 0}, 0.0, scale_factor,
                              WHITE);

    if (!stream_paused)
      avray_update_timelines(&dc);

    /* Vector2 overlay_dims = */
    avray_draw_debug_overlay(&dc, &video_tex, audio_ts_double, 10, 50);

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
      avray_seek_to_frame(&dc, new_ts);
    }
    EndDrawing();
  }

  avray_free_graphics_objects(&video_tex);
  avray_shutdown(&dc);
  UnloadAudioStream(stream);
  CloseAudioDevice();
  CloseWindow();

  return 0;
}
