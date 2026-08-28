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

#include "avlib.h"
#define RAYGUI_IMPLEMENTATION
#include "vendor/raygui/src/raygui.h"

const bool audio_dbg = false;
double start_clock_time;

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
  /*int size = */ avray_pull_audio(&dc, frame_data, frames);

  static bool info_given = false;
  if (!info_given) {
    TraceLog(LOG_INFO, "AVRAYS: sample rate: %d", dc.sample_rate);
    info_given = true;
  }
}

void calculate_letterbox(float *sf, int sw, int sh, Vector2 *displacement) {
  // Calculate scale factor
  float scale_factor_w = (float)sw / dc.video_width;
  float scale_factor_h = (float)sh / dc.video_height;
  *sf = scale_factor_w;
  if (scale_factor_h < *sf)
    *sf = scale_factor_h;

  float displayed_width = dc.video_width * *sf;
  float displayed_height = dc.video_height * *sf;

  // Calculate letterboxing displacement
  if (fabsf(displayed_width - sw) > fabsf(displayed_height - sh)) {
    displacement->x = 0.5 * (sw - displayed_width);
    displacement->y = 0;
  } else {
    displacement->x = 0;
    displacement->y = 0.5 * (sh - displayed_height);
  }
}

void rescale_window(float *sf, int *sw, int *sh, Vector2 *displacement,
                    bool fullscreen) {
  unsigned int scaled_width = Clamp(dc.video_width, 100, 2000);
  unsigned int scaled_height = Clamp(dc.video_height, 100, 1200);

  if (fullscreen) {
    // We just take monitor width/height and apply them instead
    scaled_width = GetMonitorWidth(GetCurrentMonitor());
    scaled_height = GetMonitorHeight(GetCurrentMonitor());
  }

  calculate_letterbox(sf, scaled_width, scaled_height, displacement);

  if (sw)
    *sw = scaled_width;
  if (sh)
    *sh = scaled_height;

  SetWindowSize(scaled_width, scaled_height);
}

void remove_file_from_argv(char **argv, int *len, int at) {
  for (; at < *len - 1; at++) {
    argv[at] = argv[at + 1];
  }
  *len -= 1;
}

void print_argv(char **argv, int argc) {
  for (int i = 0; i < argc; i++) {
    printf("%d: %s\n", i, argv[i]);
  }
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
  int result = avray_init_decoder(&dc);
  if (result != 0)
    exit(EXIT_FAILURE);

  unsigned int file_index = 1;
  int current_argc = argc;
  char *file_name = argv[file_index];
  while ((result = avray_open_file(&dc, file_name)) == RESULT_CANT_OPEN) {
    remove_file_from_argv(argv, &current_argc, file_index);
    // print_argv(argv, current_argc);
    if (file_index == current_argc)
      break;
    file_name = argv[file_index];
  }
  if (result != 0)
    exit(EXIT_FAILURE);

  float scale_factor = 1.0;
  int scaled_width = 1, scaled_height = 1;
  Vector2 letterbox = {};
  InitWindow(1024, 768, file_name);
  rescale_window(&scale_factor, &scaled_width, &scaled_height, &letterbox,
                 false);
  SetWindowState(FLAG_WINDOW_ALWAYS_RUN | FLAG_WINDOW_RESIZABLE);
  InitAudioDevice();

  SetTargetFPS(60);
  RaylibObjects video_tex;
  avray_init_graphics_objects(&dc, &video_tex);

  AudioStream stream = LoadAudioStream(dc.sample_rate, 32, 2);
  SetAudioStreamCallback(stream, audio_cb);
  SetAudioStreamVolume(stream, current_volume);
  PlayAudioStream(stream);

  bool stream_paused = false;
  float filename_display_timeout = 5.0;
  Vector2 overlay_dims = {};
  bool show_overlay = false;
  double old_mouse_press = -INFINITY;
  bool window_is_fullscreen = false;

  while (!WindowShouldClose() && !avray_is_decoder_stopped(&dc)) {
    // Get window's current dimensions
    int render_width = GetRenderWidth();
    int render_height = GetRenderHeight();

    mutex_lock(&dc.dc_mutex);
    DecoderState st = dc.state;
    mutex_unlock(&dc.dc_mutex);

    if (st == DS_READY || st == DS_FINISHED) {
      // Open the next file
      file_index++;
      if (file_index >= current_argc) {
        // No more files in queue
        break;
      }
      StopAudioStream(stream);
      UnloadAudioStream(stream);
      avray_free_graphics_objects(&video_tex);

      file_name = argv[file_index];
      while ((result = avray_open_file(&dc, file_name)) == RESULT_CANT_OPEN) {
        remove_file_from_argv(argv, &current_argc, file_index);
        // print_argv(argv, current_argc);
        if (file_index >= current_argc)
          break;
        file_name = argv[file_index];
      }
      if (result != RESULT_OK)
        break;
      rescale_window(&scale_factor, &scaled_width, &scaled_height, &letterbox,
                     window_is_fullscreen);
      avray_init_graphics_objects(&dc, &video_tex);

      stream = LoadAudioStream(dc.sample_rate, 32, 2);
      SetAudioStreamCallback(stream, audio_cb);
      SetAudioStreamVolume(stream, current_volume);
      PlayAudioStream(stream);

      SetWindowTitle(file_name);
      filename_display_timeout = 5.0;
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
        (st == DS_PLAYING || st == DS_STARTUP || st == DS_FILEEOF ||
         st == DS_FINISHED)) {
      // go to the previous file
      // -2 because the code in if (st==DS_READY) will increment this.
      file_index -= 2;
      avray_close_file(&dc);
    }
    if (IsKeyPressed(KEY_RIGHT_BRACKET) && file_index + 1 < current_argc &&
        (st == DS_PLAYING || st == DS_STARTUP || st == DS_FILEEOF)) {
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
      if (dc.video_timest != 0)
        avray_seek_to_frame(&dc, dc.video_timest + 10.0);
    }
    if (IsKeyPressed(KEY_A)) {
      if (dc.video_timest != 0)
        avray_seek_to_frame(&dc, dc.video_timest - 20.0);
    }
    if (IsKeyPressed(KEY_G)) {
      show_overlay = !show_overlay;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
      double new_press = GetTime();
      if (new_press - old_mouse_press < 0.3) {
        // we have a double click
        window_is_fullscreen = !window_is_fullscreen;
        if (window_is_fullscreen) {
          SetWindowState(FLAG_WINDOW_UNDECORATED |
                         FLAG_BORDERLESS_WINDOWED_MODE);
        } else {
          ClearWindowState(FLAG_WINDOW_UNDECORATED |
                           FLAG_BORDERLESS_WINDOWED_MODE);
        }
        rescale_window(&scale_factor, &scaled_width, &scaled_height, &letterbox,
                       window_is_fullscreen);
      }
      old_mouse_press = new_press;
    }
    ClearBackground(BLACK);

    avray_update_textures(&dc, &video_tex);

    BeginDrawing();

    calculate_letterbox(&scale_factor, render_width, render_height, &letterbox);
    avray_draw_video_textures(&video_tex, letterbox, 0.0, scale_factor, WHITE);

    if (!stream_paused)
      avray_update_timelines(&dc);

    /* Vector2 overlay_dims = */
    if (show_overlay)
      avray_draw_debug_overlay(&dc, &video_tex, 10, 50, &overlay_dims, true);

    if (stream_paused) {
      DrawRectangle(scaled_width / 2 - 30, scaled_height / 2 - 50, 20, 100,
                    ColorAlpha(WHITE, 0.8));
      DrawRectangle(scaled_width / 2 + 10, scaled_height / 2 - 50, 20, 100,
                    ColorAlpha(WHITE, 0.8));
    }

    DrawFPS(10, 10);
    float new_ts = dc.video_timest;

    int status =
        GuiSliderBar((Rectangle){20, render_height - 50, render_width - 40, 30},
                     NULL, NULL, &new_ts, 0, dc.duration);
    char msga[32], msgb[32], msg[128];
    time_to_str(dc.video_timest, msga, sizeof(msga));
    time_to_str(dc.duration, msgb, sizeof(msgb));
    snprintf(msg, sizeof(msg), "%s / %s", msga, msgb);
    DrawText(msg, 20, render_height - 80, 25, WHITE);

    if (status == RESULT_CHANGED) {
      avray_seek_to_frame(&dc, new_ts);
    }

    if (filename_display_timeout > 0) {
      DrawText(file_name, 100, 10, 20, WHITE);
    }
    filename_display_timeout -= 1. / 60.;
    EndDrawing();
  }

  avray_free_graphics_objects(&video_tex);
  avray_shutdown(&dc);
  UnloadAudioStream(stream);
  CloseAudioDevice();
  CloseWindow();

  return 0;
}
