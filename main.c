#include <raylib.h>
#include <raymath.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "utils.h"

int pipe_fds[2];
int pipe_video[2];
bool audio_stopped = false;
unsigned int audio_channels = 2;

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
  static int calls_count = 0;
  if (audio_stopped) {
    return;
  }
  // printf("\e[0G\e[0K(%05d) Requesting data from ffmpeg: %d frames.. ",
  //      calls_count++, frames);
  ssize_t bytes =
      read(pipe_fds[0], frame_data, sizeof(float) * audio_channels * frames);
  if (bytes <= 0) {
    // StopAudioStream(stream);
    audio_stopped = true;
    // printf("Finished");
    // fflush(stdout);
  } // else
  // printf("OK");
}

int main(int argc, char **argv) {
  float current_volume = 0.2;
  unsigned int video_width = 1920;
  unsigned int video_height = 1080;
  unsigned int audio_samples = 44100;
  // unsigned int audio_channels = 1;

  char cmd_buff[1024];
  snprintf(cmd_buff, 1024,
           "ffprobe -of compact -show_streams -show_entries "
           "stream=codec_type,width,height,sample_rate,channels "
           "%s 2>/dev/null",
           argv[1]);

  FILE *response = popen(cmd_buff, "r");
  String resp_str = MakeString();
  char buf[130];
  int bytes_read = 0;
  while (bytes_read = fread(buf, 1, 128, response)) {
    buf[bytes_read] = '\0';
    ConcatStringChars(&resp_str, buf);
  }

  StreamData *str_data = malloc(sizeof(struct StreamData));
  *str_data = (StreamData){.type = 'a', .pnext = NULL};
  ProbeLine *plines = malloc(sizeof(ProbeLine));
  *plines = (ProbeLine){NULL, NULL};
  ProbeLine *current_line = plines;

  char *line = strtok(resp_str.str, "\n");
  while (line) {
    current_line->line = line;
    current_line->pnext = malloc(sizeof(ProbeLine));
    current_line = current_line->pnext;
    *current_line = (ProbeLine){NULL, NULL};

    line = strtok(NULL, "\n");
  }

  current_line = plines;
  StreamData *curr_stream = str_data;
  while (current_line->line) {
    // TODO tokenize here
    ProbeLine *old_line = current_line;
    char *entr = strtok(current_line->line, "|");
    while (entr) {
      char *eqsign = strchr(entr, '=');
      char key[60], *value;
      if (eqsign) {
        strncpy(key, entr, eqsign - entr);
        key[eqsign - entr] = '\0';
      } else
        strcpy(key, entr);
      value = eqsign + 1;
      if (strcmp(key, "codec_type") == 0) {
        curr_stream->type = value[0];
      } else if (strcmp(key, "width") == 0) {
        curr_stream->width = atoi(value);
      } else if (strcmp(key, "height") == 0) {
        curr_stream->height = atoi(value);
      } else if (strcmp(key, "sample_rate") == 0) {
        curr_stream->sample_rate = atoi(value);
      } else if (strcmp(key, "channels") == 0) {
        curr_stream->channels = atoi(value);
      }
      entr = strtok(NULL, "|");
    }
    current_line = current_line->pnext;
    free(old_line);

    if (current_line->line) {
      curr_stream->pnext = malloc(sizeof(StreamData));
      *curr_stream->pnext = (StreamData){};
      curr_stream = curr_stream->pnext;
    }
  }
  free(current_line);
  current_line = NULL;

  curr_stream = str_data;
  while (curr_stream) {
    if (curr_stream->type == 'v') {
      video_width = curr_stream->width;
      video_height = curr_stream->height;
    }
    if (curr_stream->type == 'a') {
      audio_samples = curr_stream->sample_rate;
      audio_channels = curr_stream->channels;
    }
    StreamData *old_stream = curr_stream;
    curr_stream = curr_stream->pnext;
    free(old_stream);
  }

  pipe(pipe_fds);
  pipe(pipe_video);
  printf("Open pipe fds %d and %d\n", pipe_fds[0], pipe_fds[1]);
  if (sizeof(float) != 4) {
    return 0;
  }
  unsigned int scaled_width = Clamp(video_width, 100, 2000);
  unsigned int scaled_height = Clamp(video_height, 100, 1200);
  InitWindow(scaled_width, scaled_height, "My new window");
  InitAudioDevice();
  SetAudioStreamBufferSizeDefault(BUFFER_SIZE);

  pid_t child_pid = fork();
  if (child_pid == 0) {
    close(pipe_fds[0]);
    close(pipe_video[0]);
    char pipearg[100];
    char pipevid[100];
    snprintf(pipearg, 90, "pipe:%d", pipe_fds[1]);
    snprintf(pipevid, 100, "pipe:%d", pipe_video[1]);
    freopen("out.txt", "w", stdout);
    freopen("outerr.txt", "w", stderr);
    execlp("ffmpeg", "ffmpeg", "-i", argv[1], "-f:a", "f32le", pipearg, "-f:v",
           "rawvideo", "-pix_fmt", "rgb24", pipevid, NULL);
    return 0;
  }
  close(pipe_fds[1]);
  close(pipe_video[1]);

  AudioStream stream = LoadAudioStream(audio_samples, 32, audio_channels);
  SetAudioStreamCallback(stream, audio_cb);
  SetAudioStreamVolume(stream, current_volume);
  PlayAudioStream(stream);

  SetTargetFPS(60);
  // char tbuf[100];
  // double last_updated = GetTime();

  char *framebuf = malloc(3 * video_height * video_width);
  Image frame_img = GenImageColor(video_width, video_height, BLUE);
  ImageFormat(&frame_img, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
  Texture2D frame_tex = LoadTextureFromImage(frame_img);
  bool eof_encountered = false;

  while (!WindowShouldClose() && !eof_encountered) {
    if (audio_stopped) {
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

    ssize_t offset = 0;
    while (offset < 3 * video_width * video_height) {
      ssize_t bytes_read = read(pipe_video[0], framebuf + offset,
                                3 * video_width * video_height - offset);
      if (bytes_read <= 0) {
        eof_encountered = true;
        break;
      }
      offset += bytes_read;
    }
    if (!eof_encountered)
      UpdateTexture(frame_tex, framebuf);

    BeginDrawing();

    DrawTexture(frame_tex, 0, 0, WHITE);
    DrawText("Hello World!", 10, 10, 24, PURPLE);
    DrawFPS(900, 10);
    EndDrawing();
  }

  UnloadAudioStream(stream);
  CloseAudioDevice();
  CloseWindow();
  close(pipe_fds[0]);
  close(pipe_video[1]);

  return 0;
}
