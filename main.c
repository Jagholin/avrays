#include <GL/gl.h>
#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>
#include <stdint.h>
#include <stdio.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

// #include "avlib_fork.h"
#include "avlib.h"

const bool audio_dbg = false;
DecoderContext dc;

#define RAILIB_AUDIO_BUFFER_INTERNAL 4096
#define BUFFER_SIZE (RAILIB_AUDIO_BUFFER_INTERNAL * 4)

const char *fs_yuv420p10 = "#version 420\n"

                           "in vec2 fragTexCoord;"
                           "out vec4 finalColor;"
                           "uniform sampler2D tex_luma;"
                           "uniform sampler2D tex_u;"
                           "uniform sampler2D tex_v;"
                           "float samplet(sampler2D t) {"
                           "   vec4 smp = texture(t, fragTexCoord);"
                           "   float res = smp.r * 0.25 + smp.a * 64.0;"
                           "   return res; }"
                           "void main() {"
                           "    float smp = samplet(tex_luma);"
                           "    float luma_f = smp;"
                           "    smp = samplet(tex_u);"
                           "    float u_f = smp - 0.5;"
                           "    smp = samplet(tex_v);"
                           "    float v_f = smp - 0.5;"

                           "    luma_f = 1.1643*(luma_f - 0.0625);"
                           "    float r=luma_f+1.5958*v_f;"
                           "    float g=luma_f-0.39173*u_f-0.81290*v_f;"
                           "    float b=luma_f+2.017*u_f;"
                           "    finalColor=vec4(r, g, b, 1.0);"
                           "}";

// shader code adopted from
// https://stackoverflow.com/questions/30191911/is-it-possible-to-draw-yuv422-and-yuv420-texture-using-opengl
const char *fs_yuv420p = "#version 420 \n"
                         "in vec2 fragTexCoord;"
                         "in vec4 fragColor;"
                         "out vec4 finalColor;"
                         "uniform sampler2D tex_luma;"
                         "uniform sampler2D tex_u;"
                         "uniform sampler2D tex_v;"
                         "void main() {"
                         "   float luma = texture(tex_luma, fragTexCoord).r;"
                         "   float u = texture(tex_u, fragTexCoord).r;"
                         "   float v = texture(tex_v, fragTexCoord).r;"
                         "   luma=1.1643*(luma-0.0625);"
                         "   u=u-0.5;"
                         "   v=v-0.5;"
                         "   float r=luma+1.5958*v;"
                         "   float g=luma-0.39173*u-0.81290*v;"
                         "   float b=luma+2.017*u;"
                         "   finalColor=vec4(r, g, b, 1.0);"
                         "}";

/*volatile*/ float audio_timest = 0.0;
float cb_timing, min_cb_timing = INFINITY, max_cb_timing = -INFINITY;

void audio_cb(void *frame_data, unsigned int frames) {
  // the amount of data pulled by this function will always be no more than 4096
  // bytes (hardcoded in raylib, see ReadAudioBufferFramesInMixingFormat
  // internal function in raudio.c)
  // printf("Requested %lu bytes by audio callback\n", frames * sizeof(float) *
  // 2);
  struct timespec time_start, time_end;
  clock_gettime(CLOCK_MONOTONIC, &time_start);
  int size = pull_audio(&dc, frame_data, frames);

  printf("pull audio returned %d\n", size);
  audio_timest += (float)frames / dc.sample_rate;
  clock_gettime(CLOCK_MONOTONIC, &time_end);
  long int td = (time_end.tv_sec - time_start.tv_sec) * 1000000000 +
                time_end.tv_nsec - time_start.tv_nsec;
  cb_timing = (float)td / 1000000000;
  if (min_cb_timing > cb_timing)
    min_cb_timing = cb_timing;
  if (max_cb_timing < cb_timing)
    max_cb_timing = cb_timing;
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

Texture2D make_integer_tex(unsigned int width, unsigned int height) {
  Texture2D new_tx = {.height = height, .width = width, .mipmaps = 1};
  glGenTextures(1, &new_tx.id);
  rlCheckErrors();
  glBindTexture(GL_TEXTURE_2D, new_tx.id);
  rlCheckErrors();
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, width, height, 0, GL_RED_INTEGER,
               GL_UNSIGNED_SHORT, NULL);
  rlCheckErrors();

  return new_tx;
}

int main(int argc, char **argv) {
  struct timespec times;
  clock_getres(CLOCK_MONOTONIC, &times);
  printf("Resolution of the clock is %ld s, %ld ns\n", times.tv_sec,
         times.tv_nsec);

  float current_volume = 0.2;
  pthread_t decoder_thread;

  char *file_name = argv[1];
  int result = initiate_decoding(&dc, file_name);
  if (result != 0)
    exit(EXIT_FAILURE);
  unsigned int scaled_width = Clamp(dc.video_width, 100, 2000);
  unsigned int scaled_height = Clamp(dc.video_height, 100, 1200);
  float scale_factor_w = (float)scaled_width / dc.video_width;
  float scale_factor_h = (float)scaled_height / dc.video_height;
  float scale_factor = scale_factor_w;
  if (scale_factor_h < scale_factor)
    scale_factor = scale_factor_h;

  InitWindow(scaled_width, scaled_height, "My new window");
  InitAudioDevice();
  SetAudioStreamBufferSizeDefault(BUFFER_SIZE);

  SetTargetFPS(60);

  PixelFormat pf = PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA;
  const unsigned int bytespp = 2;
  const char *shader_code = fs_yuv420p10;

  Image frame_img = GenImageColor(dc.video_width, dc.video_height, BLUE);
  ImageFormat(&frame_img, pf);
  Texture2D frame_tex = LoadTextureFromImage(frame_img);

  // Make another texture with half height and width for UV channels
  frame_img = GenImageColor(dc.video_width / 2, dc.video_height / 2, GREEN);
  ImageFormat(&frame_img, pf);
  Texture2D u_tex = LoadTextureFromImage(frame_img);
  frame_img = GenImageColor(dc.video_width / 2, dc.video_height / 2, GREEN);
  ImageFormat(&frame_img, pf);
  Texture2D v_tex = LoadTextureFromImage(frame_img);

  Texture2D i_tex = make_integer_tex(dc.video_width, dc.video_height);

  Shader video_shader = LoadShaderFromMemory(NULL, shader_code);
  int y_location = GetShaderLocation(video_shader, "tex_luma");
  int u_location = GetShaderLocation(video_shader, "tex_u");
  int v_location = GetShaderLocation(video_shader, "tex_v");

  // FILE *testfile = fopen("output", "wb");
  result = pthread_create(&decoder_thread, NULL, &decode_thread, argv);
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
  double start_ts = GetTime();

  // unsigned int i = 0;
  float video_timest = 0;
  printf("u tex id: %d, v tex id: %d\n", u_tex.id, v_tex.id);

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

    if (video_timest < audio_timest) {
      uint8_t *image_buff = NULL;
      while (video_timest < audio_timest) {
        image_buff = pull_image(&dc, &video_timest);
        if (image_buff == NULL)
          break;
        // If we are going for the next iteration, have to make sure to
        // release_image.
        // printf("incoming ts: %f\n", video_timest);
        if (video_timest < audio_timest) {
          release_image(&dc);
        }
      }
      if (image_buff) {
        printf("audio ts: %f, video ts: %f\n", audio_timest, video_timest);
        printf("max cb timing: %f, min cb timing: %f\n", max_cb_timing,
               min_cb_timing);
        printf("Time since start: %g\n", GetTime() - start_ts);
        // printf("audio queue length: %f, video queue: %zu\n",
        //        ringbuffer_len(&dc.audio_buffer) / (float)(48000 * 8),
        //        ringbuffer_len(&dc.image_buffer) / dc.image_buffer_size);
        UpdateTexture(frame_tex, image_buff);
        image_buff += dc.video_height * dc.video_width * bytespp;
        UpdateTexture(u_tex, image_buff);
        image_buff += dc.video_height * dc.video_width * bytespp / 4;
        UpdateTexture(v_tex, image_buff);
      }
      release_image(&dc);
    }

    BeginDrawing();
    // printf("delta: %f, max delta: %f, min delta: %f\n", dc.delta_time,
    //        dc.max_delta_time, dc.min_delta_time);

    BeginShaderMode(video_shader);
    {
      // rlActiveTextureSlot(1);
      // rlEnableTexture(u_tex.id);
      // rlActiveTextureSlot(2);
      // rlEnableTexture(v_tex.id);
      // rlActiveTextureSlot(0);
      SetShaderValueTexture(video_shader, y_location, frame_tex);
      SetShaderValueTexture(video_shader, u_location, u_tex);
      SetShaderValueTexture(video_shader, v_location, v_tex);

      // DrawTexture(frame_tex, 0, 0, WHITE);
      DrawTextureEx(frame_tex, (Vector2){0, 0}, 0.0, scale_factor, WHITE);
    }
    EndShaderMode();

    DrawText("Hello World!", 10, 10, 24, PURPLE);
    DrawFPS(10, 38);
    EndDrawing();
  }

  UnloadAudioStream(stream);
  CloseAudioDevice();
  CloseWindow();

  return 0;
}
