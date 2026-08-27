#define AVRAY_IMPLEMENTATION
// This will also include <raylib.h>
#include "stb_avray.h"

DecoderContext ctx = {};

void audio_cb(void *buffer, unsigned int frames) {
  avray_pull_audio(&ctx, buffer, frames);
}

int main(int argc, char **argv) {
  RaylibObjects video_surface;
  InitWindow(1920, 1080, "Minimal videoplayer");
  avray_init_decoder(&ctx);
  avray_open_file(&ctx, argv[1]);
  avray_init_graphics_objects(&ctx, &video_surface);
  SetWindowSize(ctx.video_width, ctx.video_height);
  SetTargetFPS(60);

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
    avray_update_textures(&ctx, &video_surface);
    BeginDrawing();
    avray_draw_video_textures(&video_surface, (Vector2){}, 0.0, 1.0, WHITE);
    EndDrawing();
  }
  avray_free_graphics_objects(&video_surface);
  avray_shutdown(&ctx);
  UnloadAudioStream(audio);
  CloseAudioDevice();
  CloseWindow();
}
