#include <raylib.h>
#include <raymath.h>
#include <stdint.h>
#include <stdio.h>
#include <threads.h>
#include <unistd.h>

// #include "avlib_fork.h"
#include "avlib.h"

const bool audio_dbg = false;
DecoderContext dc;

#define RAILIB_AUDIO_BUFFER_INTERNAL 4096
#define BUFFER_SIZE (RAILIB_AUDIO_BUFFER_INTERNAL * 4)

void audio_cb(void *frame_data, unsigned int frames) {
  // the amount of data pulled by this function will always be no more than 4096
  // bytes (hardcoded in raylib, see ReadAudioBufferFramesInMixingFormat
  // internal function in raudio.c)
  // printf("Requested %lu bytes by audio callback\n", frames * sizeof(float) *
  // 2);
  pull_audio(&dc, frame_data, frames);
}

void *decode_thread(void *d) {
  int result = 0;
  do {
    result = continue_decoding(&dc);
    if (result < 0)
      exit(EXIT_FAILURE);
  } while (result != RESULT_EOF);
  return (void *)result;
}

int main(int argc, char **argv) {
  float current_volume = 0.2;
  pthread_t decoder;

  char *file_name = argv[1];
  int result = initiate_decoding(&dc, file_name);
  if (result != 0)
    exit(EXIT_FAILURE);
  unsigned int scaled_width = Clamp(dc.video_width, 100, 2000);
  unsigned int scaled_height = Clamp(dc.video_height, 100, 1200);
  InitWindow(scaled_width, scaled_height, "My new window");
  InitAudioDevice();
  SetAudioStreamBufferSizeDefault(BUFFER_SIZE);

  SetTargetFPS(60);

  Image frame_img = GenImageColor(dc.video_width, dc.video_height, BLUE);
  ImageFormat(&frame_img, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE);
  Texture2D frame_tex = LoadTextureFromImage(frame_img);

  // FILE *testfile = fopen("output", "wb");
  result = pthread_create(&decoder, NULL, &decode_thread, argv);
  if (result) {
    printf("Couldn't create a decoder thread\n");
    return -1;
  }
  // wait until enough frames were decoded
  sem_wait(&dc.startup_sem);

  AudioStream stream = LoadAudioStream(dc.sample_rate, 32, 2);
  SetAudioStreamCallback(stream, audio_cb);
  SetAudioStreamVolume(stream, current_volume);
  PlayAudioStream(stream);

  // unsigned int i = 0;

  while (!WindowShouldClose() && !is_decoder_finished(&dc)) {
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
    ClearBackground(BLACK);

    uint8_t *image_buff = pull_image(&dc);
    // if (i == 500) {
    //   fwrite(image_buff, 1, dc.image_buffer_size, testfile);
    //   fclose(testfile);
    //   return 0;
    // }
    // i++;

    // result = pull_image(&dc);
    // if (result != 0) {
    //   break;
    // }
    //
    // if (!dc.eof_encountered)
    if (image_buff)
      UpdateTexture(frame_tex, image_buff);

    release_image(&dc);
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
