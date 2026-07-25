#ifndef AVLIB_H
#define AVLIB_H
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

typedef struct DecoderContext {
  int video_stream;
  AVFormatContext *fmt_ctx;
  AVCodecContext *ctx;
  AVPacket *pkt;
  AVFrame *fr;

  uint8_t *image_buffer;
  size_t image_buffer_size;
  int i;
} DecoderContext;

int initiate_decoding(DecoderContext *ctx, const char *file_name);
int pull_image(DecoderContext *ctx);
void free_decoder_context(DecoderContext *ctx);

#endif // !AVLIB_H
