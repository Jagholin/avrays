#include "avlib_fork.h"
#include "utils.h"
#include <unistd.h>

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

int initiate_decoding(DecoderContext *ctx, const char *file_name) {
  *ctx = (DecoderContext){0};

  char cmd_buff[1024];
  snprintf(cmd_buff, 1024,
           "ffprobe -of compact -show_streams -show_entries "
           "stream=codec_type,width,height,sample_rate,channels "
           "%s 2>/dev/null",
           file_name);

  FILE *response = popen(cmd_buff, "r");
  String resp_str = make_string();
  char buf[130];
  int bytes_read = 0;
  while ((bytes_read = fread(buf, 1, 128, response))) {
    buf[bytes_read] = '\0';
    concat_string_chars(&resp_str, buf);
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
      ctx->video_width = curr_stream->width;
      ctx->video_height = curr_stream->height;
    }
    if (curr_stream->type == 'a') {
      ctx->audio_sample_rate = curr_stream->sample_rate;
      ctx->audio_channels = curr_stream->channels;
    }
    StreamData *old_stream = curr_stream;
    curr_stream = curr_stream->pnext;
    free(old_stream);
  }

  pipe(ctx->pipe_audio);
  pipe(ctx->pipe_video);
  printf("Open pipe fds %d and %d\n", ctx->pipe_audio[0], ctx->pipe_audio[1]);
  if (sizeof(float) != 4) {
    return 1;
  }

  pid_t child_pid = fork();
  if (child_pid == 0) {
    close(ctx->pipe_audio[0]);
    close(ctx->pipe_video[0]);
    char pipearg[100];
    char pipevid[100];
    snprintf(pipearg, 90, "pipe:%d", ctx->pipe_audio[1]);
    snprintf(pipevid, 100, "pipe:%d", ctx->pipe_video[1]);
    freopen("out.txt", "w", stdout);
    freopen("outerr.txt", "w", stderr);
    execlp("ffmpeg", "ffmpeg", "-i", file_name, "-f:a", "f32le", pipearg,
           "-f:v", "rawvideo", "-pix_fmt", "rgb24", pipevid, NULL);
    return 1;
  }
  close(ctx->pipe_audio[1]);
  close(ctx->pipe_video[1]);

  ctx->image_buffer_size = 3 * ctx->video_height * ctx->video_width;
  ctx->image_buffer = malloc(ctx->image_buffer_size);
  return 0;
}

int pull_image(DecoderContext *ctx) {
  ssize_t offset = 0;
  while (offset < ctx->image_buffer_size) {
    ssize_t bytes_read = read(ctx->pipe_video[0], ctx->image_buffer + offset,
                              ctx->image_buffer_size - offset);
    if (bytes_read <= 0) {
      ctx->eof_encountered = true;
      return 1;
    }
    offset += bytes_read;
  }
  return 0;
}

int pull_audio(DecoderContext *ctx, void *audio_buffer, unsigned int frames) {
  ssize_t bytes = read(ctx->pipe_audio[0], audio_buffer,
                       sizeof(float) * ctx->audio_channels * frames);
  if (bytes <= 0) {
    // StopAudioStream(stream);
    ctx->audio_stopped = true;
    // printf("Finished");
    // fflush(stdout);
    return 1;
  } // else
  return 0;
}

bool is_decoder_finished(DecoderContext *ctx) {
  return ctx->eof_encountered || ctx->audio_stopped;
}
