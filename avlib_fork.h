#ifndef AVLIB_H
#define AVLIB_H
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct DecoderContext {
  int pipe_audio[2];
  int pipe_video[2];
  unsigned int video_width;
  unsigned int video_height;
  uint8_t *image_buffer;
  size_t image_buffer_size;
  bool eof_encountered;
  bool audio_stopped;
  unsigned int audio_channels;
  unsigned int audio_sample_rate;
} DecoderContext;

int initiate_decoding(DecoderContext *ctx, const char *file_name);
int pull_image(DecoderContext *ctx);
int pull_audio(DecoderContext *ctx, void *audio_buffer, unsigned int frames);
void free_decoder_context(DecoderContext *ctx);
bool is_decoder_finished(DecoderContext *ctx);

#endif // !AVLIB_H
