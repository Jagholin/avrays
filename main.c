#include <raylib.h>
#include <raymath.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "avlib_fork.h"
#include "utils.h"

const bool audio_dbg = false;
DecoderContext dc;

#define BUFFER_SIZE (4096 * 4)

typedef struct ProbeLine {
  char *line;
  struct ProbeLine *pnext;
} ProbeLine;

typedef struct StreamData {
  char type;
  unsigned int width;
  unsigned int height;
  unsigned int sample_rate;
  unsigned int channels;
  struct StreamData *pnext;
} StreamData;

void audio_cb(void *frame_data, unsigned int frames) {
  pull_audio(&dc, frame_data, frames);
  // printf("OK");
}

int main(int argc, char **argv) {
  float current_volume = 0.2;

  int result = initiate_decoding(&dc, argv[1]);
  if (result != 0) {
    return result;
  }
  unsigned int scaled_width = Clamp(dc.video_width, 100, 2000);
  unsigned int scaled_height = Clamp(dc.video_height, 100, 1200);
  InitWindow(scaled_width, scaled_height, "My new window");
  InitAudioDevice();
  SetAudioStreamBufferSizeDefault(BUFFER_SIZE);

  AudioStream stream =
      LoadAudioStream(dc.audio_sample_rate, 32, dc.audio_channels);
  SetAudioStreamCallback(stream, audio_cb);
  SetAudioStreamVolume(stream, current_volume);
  PlayAudioStream(stream);

  SetTargetFPS(60);
  // char tbuf[100];
  // double last_updated = GetTime();

  Image frame_img = GenImageColor(dc.video_width, dc.video_height, BLUE);
  ImageFormat(&frame_img, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
  Texture2D frame_tex = LoadTextureFromImage(frame_img);

  while (!WindowShouldClose() && !is_decoder_finished(&dc)) {
    if (dc.audio_stopped) {
      StopAudioStream(stream);
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
    ClearBackground(BLACK);
    // read everything from video pipe

    result = pull_image(&dc);
    if (result != 0) {
      break;
    }

    if (!dc.eof_encountered)
      UpdateTexture(frame_tex, dc.image_buffer);

    BeginDrawing();

    DrawTexture(frame_tex, 0, 0, WHITE);
    DrawText("Hello World!", 10, 10, 24, PURPLE);
    DrawFPS(900, 10);
    EndDrawing();
  }

  UnloadAudioStream(stream);
  CloseAudioDevice();
  CloseWindow();

  return 0;
}
