# What is this

AVRay is a small library that plays video files inside of a raylib app. It's using libav and is doing a lot of hard work so you don't have to :)

# How to use it

The simplest option is to include a single header, 'stb_avray.h'. You will also have to `#define AVRAY_IMPLEMENTATION` once in your project, before including the header.

Since AVRay is just a wrapper for libav, you will also need to link your program with `libavcodec`, `libavutil`, `libavformat` and `libswresample`. Look for `libav` or `ffmpeg` to find and install them.

If you don't wish to use stb_avray.h, then you will need the following files: `avlib.c`, `avlib.h` and `utils.h`. Add them to your project, or compile the c file into a library (see CMakeLists.txt for an example).

## What files do what

- avlib.c, avlib.h, utils.h are source files of the library
- CMakeLists.txt, main.c implement a video player using the library
- main_minimal.c shows a minimal example using stb version of the library.
- make_stb is a bash script that generates stb_avray.h file.
- stb_avray.h is a single header version of the library. You can include it instead of the avlib.c/avlib.h/utils.h combo.

## Video player example

![Video player window](/readme/screen_player.png)

`main.c` implements a more complete video player with pausing,seeking and playlist functionality.

It uses raygui which is included in this repo as a submodule, so make sure to fetch it with `git submodule update --init --recursive` 

You can compile it using cmake by running `cmake -B build -DCMAKE_BUILD_TYPE=Release && cd build && cmake --build .`

The executable expects as argument(s) a list of files to be played. These will be added to "playlist".

The player has the following controls:

- Key `A` seeks ~10 seconds backwards
- Key `D` seeks ~10 seconds forwards
- `UP arrow` increases the volume
- `DOWN arrow` decreases the volume
- You can click on the timeline to seek to a particular location of the file
- Key `[` opens previous file in the playlist
- Key `]` opens next file in the playlist
- Key `G` shows or hides debug overlay
- `Space` pauses/unpauses the playback
- Double click switches borderless fullscreen mode on/off
- `Esc` closes the window, as is tradition in raylib.

## Minimal example

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

You can compile it with command `gcc main_minimal.c -lraylib -lavutil -lavformat -lswresample -lavcodec`.
Don't forget to add dependencies of the raylib itself, in linux it's `-lm -lX11`.

# Public interface

Functions in AVRay operate on a DecoderContext object, which you can find in the `avlib.h` file. The context is a state machine, whose current state can be read from its `state` member variable. The diagram looks like this:

![state machine, see DecoderState for descriptions](/readme/state_machine.png)

There is also DS_ERROR state, but if you reach it, something has gone wrong. This part of the library is still WIP.

```c
// First function you call, initializes the context and starts decoder thread.
int avray_init_decoder(DecoderContext *ctx);
// Try to open the file.
// Can only be called when in DS_FINISHED or DS_READY state.
// If the file can't be opened, returns RESULT_CANT_OPEN and remains in DS_READY state.
// If the file opened successfully, returns RESULT_OK and switches to DS_STARTUP state.
int avray_open_file(DecoderContext *ctx, const char *file_name);
// Tries to read next image frame from the buffer.
// If there is no next image, or if the state isn't DS_PLAYING or DS_FILEEOF, returns NULL.
// writes frame's display time to *timestamp.
// This is a low level func, you should probably use avray_update_textures instead.
// You have to always pair this func with call to avray_release_image, even when NULL is returned.
uint8_t *avray_pull_image(DecoderContext *ctx, float *timestamp);
// Complement to avray_pull_image.
// Make sure to always call this function after avray_pull_image to release mutex.
void avray_release_image(DecoderContext *ctx);
// Function similar to avray_pull_image, meant to be used in stream's audio callback
// (see example above)
// Returns # of bytes transferred or 0 if nothing was written to the *audio_buffer.
// Doesn't have or need a corresponding _release function.
int avray_pull_audio(DecoderContext *ctx, void *audio_buffer,
                                          unsigned int frames);
// Simple helper that checks state == DS_SHUTDOWN || state == DS_ERROR
bool avray_is_decoder_stopped(DecoderContext *ctx);
// Issues "seek" command to the decoder thread.
// ts is target timestamp in seconds.
// the seeking is rough, it will usually just jump to some "keyframe" in vicinity of ts
// which can be several seconds off.
void avray_seek_to_frame(DecoderContext *ctx, double ts);
// Issues "close file" command to the decoder thread.
// Will eventually switch state to DS_READY.
// THis happens asynchronously on another thread, so you have to wait for 
// state == DS_READY before you can continue and open another file, for example.
void avray_close_file(DecoderContext *ctx);
// Updates Timeline structs that are used to show lovely histograms in the debug overlay.
// If you don't use debug overlay, you don't need to call this function.
void avray_update_timelines(DecoderContext *ctx);
// Switches state to DS_SHUTDOWN and stops decoder thread.
void avray_shutdown(DecoderContext *ctx);

// Initializes textures and shaders in *objs
int avray_init_graphics_objects(DecoderContext *ctx, RaylibObjects *objs);
int avray_free_graphics_objects(RaylibObjects *objs);
// Checks timestamps and uploads next frame to the GPU, if necessary
int avray_update_textures(DecoderContext *ctx, RaylibObjects *objs);
// Renders video frame at given position and rotation/scale factor.
// Calls DrawTextureEx internally.
int avray_draw_video_textures(RaylibObjects *objs, Vector2 position,
                              float rotation, float scale_factor, Color tint);
// Draws timeline histogram at given position and with given size.
// max is maximum value, determines the scale for the y axis.
void timeline_draw_ui(TimeLine tl, int x, int y, int width, int height,
                      unsigned int max, bool draw_background);
// Draws debug overlay.
// if pdims is not NULL, stores dimensions(width/height) in *pdims.
// draw_background only works with pdims != NULL and will use stored dimensions from there.
Vector2 avray_draw_debug_overlay(DecoderContext *ctx, RaylibObjects *objs,
                                int x, int y, Vector2 *pdims,
                                bool draw_background);
```
Also see `avlib.h` for definitions of `DecoderContext` and return values.

# Limitations

Currently only yuv420p and yuv420p10 (in ffmpeg's terminology) pixel formats are supported. Support for others will be added if necessary, make a github issue with output from `ffprobe` to request adding support for a new format.

The library is made in linux, and uses `pthreads` for multithreading and synchronization. If you want to compile it on other operating system, you will probably need to link it with `pthreads` implementation for your OS, or rewrite pthreads-dependent code to use something else (the relevant code parts are conveniently placed in separate functions, so I hope this task should be easy enough).
