# What is this

AVRay is a small library that plays video files inside of a raylib app. It's using libav and is doing a lot of hard work so you don't have to :)

# How to use it

The simplest option is to include a single header, 'stb_avray.h'. You will also have to `#define AVRAY_IMPLEMENTATION` once in your project, before including the header.

Since AVRay is just a wrapper for libav, you will also need to link your program with `libavcodec`, `libavutil`, `libavformat` and `libswresample`. Look for `libav` or `ffmpeg` to find and install them.

A minimal program that uses AVRay could look something like this(see main_minimal.c):

```c
#define AVRAY_IMPLEMENTATION
// This will also include <raylib.h>
#include "stb_avray.h"

DecoderContext ctx = {}; // we have to place DecoderContext in a place where the audio callback can access it.

void audio_cb(void *buffer, unsigned int frames) {
  avray_pull_audio(&ctx, buffer, frames);
}

int main(int argc, char **argv) {
  RaylibObjects video_surface;
  InitWindow(1920, 1080, "Minimal videoplayer");
  avray_init_decoder(&ctx); // Initializes DecoderContext and starts decoding thread
  avray_open_file(&ctx, argv[1]);
  avray_init_graphics_objects(&ctx, &video_surface);
  SetWindowSize(ctx.video_width, ctx.video_height);
  SetTargetFPS(60);

  // Video is being synchronized to the audio, so having an active audio output is mandatory for progress.
  InitAudioDevice();
  AudioStream audio = LoadAudioStream(ctx.sample_rate, sizeof(float) * 8, 2);
  SetAudioStreamCallback(audio, &audio_cb);
  PlayAudioStream(audio);

  while (!WindowShouldClose() && !avray_is_decoder_stopped(&ctx)) {
    DecoderState state = ctx.state;
    // Exit when the playback is finished
    if (state == DS_FINISHED) {
      break;
    }
    // Update textures checks the current timestamp(driven by the audio stream)
    // and updates the textures if it's time to show the next video frame.
    avray_update_textures(&ctx, &video_surface);
    BeginDrawing();
    // This simply issues the draw commands.
    avray_draw_video_textures(&video_surface, (Vector2){}, 0.0, 1.0, WHITE);
    EndDrawing();
  }
  avray_free_graphics_objects(&video_surface);
  avray_shutdown(&ctx);
  UnloadAudioStream(audio);
  CloseAudioDevice();
  CloseWindow();
}
```


