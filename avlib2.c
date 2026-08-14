#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <pthread.h>
#include <semaphore.h>

#include "avlib2.h"
#include "utils.h"

// Size of the video buffer if the framerate is not available
#define RING_FRAMES 30

typedef pthread_mutex_t MutexType;
typedef sem_t SemaphoreType;
typedef pthread_t ThreadType;

void create_mutex(MutexType *m) { pthread_mutex_init(m, NULL); }
void create_semaphore(SemaphoreType *s, int init_value) {
  sem_init(s, 0, init_value);
}
void free_mutex(MutexType *m) { pthread_mutex_destroy(m); }
void free_semaphore(SemaphoreType *s) {
  sem_close(s);
  sem_destroy(s);
}

void semaphore_wait(SemaphoreType *s) { sem_wait(s); }
void semaphore_incr(SemaphoreType *s) { sem_post(s); }
void mutex_lock(MutexType *m) { pthread_mutex_lock(m); }
void mutex_unlock(MutexType *m) { pthread_mutex_unlock(m); }
void thread_create(ThreadType *t, void *(*thread_proc)(void *)) {
  pthread_create(t, NULL, thread_proc, NULL);
}

struct DecoderPrivate {
  int video_stream;
  int audio_stream;
  AVFormatContext *fmt_ctx; // Format (container) decoder context
  AVCodecContext *ctxv;     // Video decoder context
  AVCodecContext *ctxa;     // Audio decoder context
  AVPacket *pkt;
  bool pkt_populated;
  AVFrame *fr;
  bool fr_populated;
  bool frame_is_audio;
  AVFrame *fr_audio_target;
  bool fr_audio_populated;
  // AVFrame *fra;
  SwrContext *swr;

  RingBuffer image_buffer;
  RingBuffer audio_buffer;
  size_t image_buffer_size;
  size_t audio_buffer_size;
  // int i;
  bool audio_configured;
  // bool eof_encountered;

  MutexType image_buffer_mtx;
  MutexType audio_buffer_mtx;
  MutexType command_q_mtx;
  SemaphoreType sem;
  // SemaphoreType startup_sem;
};

const char *state_str(DecoderState st) {
  switch (st) {
  case DS_READY:
    return "READY";
  case DS_PLAYING:
    return "PLAYING";
  case DS_ERROR:
    return "ERROR";
  case DS_UNINIT:
    return "UNINIT";
  case DS_STARTUP:
    return "STARTUP";
  case DS_SHUTDOWN:
    return "SHUTDOWN";
  case DS_FILEEOF:
    return "FILEEOF";
  case DS_FINISHED:
    return "FINISHED";
  default:
    return "UNKNOWN?";
  }
}

void switch_dec_state(DecoderContext *ctx, DecoderState newState) {
  TraceLog(LOG_INFO, "VADECODER: Switching to new state: %s",
           state_str(newState));
  ctx->state = newState;
}

struct SeekCommand {
  float target;
  bool active;
};

struct SeekCommand seek_command;
static DecoderContext *dc;
ThreadType decoder_thread;

int dec_init_decoder(DecoderContext *ctx) {
  if (dc != NULL) {
    return RESULT_ERROR;
  }
  *ctx = (DecoderContext){.min_delta_time = INFINITY,
                          .max_delta_time = -INFINITY,
                          .audio_time = av_make_q(0, 1),
                          .video_time = av_make_q(0, 1)};
  ctx->p = malloc(sizeof(struct DecoderPrivate));
  memset(ctx->p, 0, sizeof(struct DecoderPrivate));
  struct DecoderPrivate *p = ctx->p;

  create_mutex(&p->image_buffer_mtx);
  create_mutex(&p->audio_buffer_mtx);
  create_mutex(&p->command_q_mtx);
  create_semaphore(&p->sem, 1);
  // create_semaphore(&p->startup_sem, 0);

  int result = RESULT_OK;
  p->swr = swr_alloc();
  ERRCHECK2(p->swr, "Cant allocate audio resampler");

  p->fr = av_frame_alloc();
  ERRCHECK2(p->fr, "Cant allocate frame");
  p->fr_audio_target = av_frame_alloc();
  ERRCHECK2(p->fr_audio_target, "Cant allocate frame");
  p->fr_audio_target->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
  p->fr_audio_target->format = AV_SAMPLE_FMT_FLT;

  p->pkt = av_packet_alloc();
  ERRCHECK2(p->pkt, "Cant allocate packet");

  ctx->vbuffer_timeline = make_timeline(sizeof(unsigned int), 120);
  ctx->abuffer_timeline = make_timeline(sizeof(unsigned int), 120);

  dc = ctx;
  // dc->state = DS_READY;
  switch_dec_state(dc, DS_READY);
  return RESULT_OK;
cleanup:
  free_decoder_context(ctx);
  return RESULT_ERROR;
}

int dec_open_file(DecoderContext *ctx, const char *file_name) {
  assert(ctx->state == DS_READY);
  struct DecoderPrivate *p = ctx->p;
  int result = avformat_open_input(&p->fmt_ctx, file_name, NULL, NULL);
  ERRCHECK("Can't open file");

  result = avformat_find_stream_info(p->fmt_ctx, NULL);
  ERRCHECK("Cant find stream info");

  p->video_stream =
      av_find_best_stream(p->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  ERRCHECK2(p->video_stream >= 0, "Cant find video stream");
  p->audio_stream =
      av_find_best_stream(p->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  ERRCHECK2(p->audio_stream >= 0, "Cant find audio stream");
  AVCodecParameters *origin_par =
      p->fmt_ctx->streams[p->video_stream]->codecpar;
  AVCodecParameters *origin_par_audio =
      p->fmt_ctx->streams[p->audio_stream]->codecpar;

  printf("Audio sample rate: %d, bits per sample: %d, channels: %d, frame "
         "size: %d\n",
         origin_par_audio->sample_rate, origin_par_audio->bits_per_coded_sample,
         origin_par_audio->ch_layout.nb_channels, origin_par_audio->frame_size);
  printf("Video frame rate: %d/%d\n", origin_par->framerate.num,
         origin_par->framerate.den);

  const AVCodec *codec = avcodec_find_decoder(origin_par->codec_id);
  ERRCHECK2(codec, "Can't find decoder");
  const AVCodec *audio_codec = avcodec_find_decoder(origin_par_audio->codec_id);
  printf("Video decoded with %s, audio with %s\n", codec->name,
         audio_codec->name);

  p->ctxv = avcodec_alloc_context3(codec);
  ERRCHECK2(p->ctxv, "Cant allocate memory for decoder context");
  p->ctxa = avcodec_alloc_context3(audio_codec);
  ERRCHECK2(p->ctxa, "Cant allocate memory for audio decoder");

  result = avcodec_parameters_to_context(p->ctxv, origin_par);
  ERRCHECK("Cant initiate video codec parameters");
  result = avcodec_parameters_to_context(p->ctxa, origin_par_audio);
  ERRCHECK("Cant initiate audio codec parameters");
  p->ctxv->thread_count = 4;
  p->ctxv->thread_type = FF_THREAD_SLICE;

  result = avcodec_open2(p->ctxv, codec, NULL);
  ERRCHECK("Cant open decoder");
  result = avcodec_open2(p->ctxa, audio_codec, NULL);
  ERRCHECK("Cant open audio decoder");

  const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(p->ctxv->pix_fmt);
  printf("Frame is %dx%d (%s)\n", p->ctxv->width, p->ctxv->height,
         pix_desc->name);
  ctx->video_width = p->ctxv->width;
  ctx->video_height = p->ctxv->height;

  p->image_buffer_size =
      av_image_get_buffer_size(p->ctxv->pix_fmt, p->ctxv->width,
                               p->ctxv->height, 16) +
      sizeof(float);

  AVRational framerate = p->ctxv->framerate;
  size_t buffer_frames = (size_t)av_q2d(framerate);
  if (framerate.den == 1 && framerate.num == 0) {
    buffer_frames = RING_FRAMES;
  }

  p->image_buffer = make_ringbuffer(p->image_buffer_size * buffer_frames);

  printf("Audio codec data: sample format %s, sample rate %d, bytes per sample "
         "%d\n",
         av_get_sample_fmt_name(p->ctxa->sample_fmt), p->ctxa->sample_rate,
         av_get_bytes_per_sample(p->ctxa->sample_fmt));
  ctx->sample_rate = p->ctxa->sample_rate;

  printf("video Time base is %d/%d\n",
         p->fmt_ctx->streams[p->video_stream]->time_base.num,
         p->fmt_ctx->streams[p->video_stream]->time_base.den);
  printf("audio Time base is %d/%d\n",
         p->fmt_ctx->streams[p->audio_stream]->time_base.num,
         p->fmt_ctx->streams[p->audio_stream]->time_base.den);
  ctx->video_tb = p->fmt_ctx->streams[p->video_stream]->time_base;
  ctx->audio_tb = p->fmt_ctx->streams[p->audio_stream]->time_base;

  printf("Codec framerate: %d/%d\n", p->ctxv->framerate.num,
         p->ctxv->framerate.den);
  ctx->video_framerate = p->ctxv->framerate;
  ctx->pixel_format = origin_par->format;

  // ctx->state = DS_STARTUP;
  switch_dec_state(ctx, DS_STARTUP);

  return RESULT_OK;
cleanup:
  // TODO: cleanup
  return result;
}

// Feeds packet pkt to video or audio decoder
static int feed_codec(struct DecoderPrivate *p, int *stream) {
  if (!p->pkt_populated) {
    return RESULT_OK;
  }

  int packet_stream = p->pkt->stream_index;
  if (stream) {
    *stream = packet_stream;
  }
  int result = 0;
  if (packet_stream == p->video_stream) {
    result = avcodec_send_packet(p->ctxv, p->pkt);
  } else if (packet_stream == p->audio_stream) {
    result = avcodec_send_packet(p->ctxa, p->pkt);
  }
  av_packet_unref(p->pkt);
  p->pkt_populated = false;
  return result;
}

static int send_eof2codecs(struct DecoderPrivate *p) {
  int res1 = avcodec_send_packet(p->ctxv, NULL);
  int res2 = avcodec_send_packet(p->ctxa, NULL);
  if (res1 <= res2)
    return res1;
  return res2;
}

static int pull_frame(struct DecoderPrivate *p) {
  if (p->fr_populated) {
    printf("pull_frame shortcut\n");
    return RESULT_OK;
  }

  // Try to receive audio frame, then a video frame
  int result = avcodec_receive_frame(p->ctxa, p->fr);
  if (result == 0) {
    p->fr_populated = true;
    p->frame_is_audio = true;
    return RESULT_OK;
  }
  result = avcodec_receive_frame(p->ctxv, p->fr);
  if (result == 0) {
    p->fr_populated = true;
    p->frame_is_audio = false;
  }
  return result;
}

static void signal_startup(DecoderContext *ctx) {
  if (ctx->state != DS_STARTUP)
    return;
  // semaphore_incr(&ctx->p->startup_sem);
  // ctx->state = DS_PLAYING;
  switch_dec_state(ctx, DS_PLAYING);
}

static int process_video_frame(DecoderContext *ctx) {
  struct DecoderPrivate *p = ctx->p;
  if (!p->fr_populated) {
    return RESULT_OK;
  }
  int result = RESULT_OK;
  uint8_t *write_loc =
      write_ringbuffer_chunk_nocommit(&p->image_buffer, p->image_buffer_size);
  if (write_loc == NULL) {
    return RESULT_STALL;
  }
  mutex_lock(&p->image_buffer_mtx);
  av_image_copy_to_buffer(write_loc + sizeof(float),
                          p->image_buffer_size - sizeof(float),
                          (uint8_t const *const *)p->fr->data, p->fr->linesize,
                          p->ctxv->pix_fmt, p->ctxv->width, p->ctxv->height, 1);

  ctx->video_time =
      av_mul_q(av_make_q(p->fr->best_effort_timestamp, 1), ctx->video_tb);
  (*(float *)write_loc) = (float)ctx->video_time.num / ctx->video_time.den;

  write_ringbuffer_commit(&p->image_buffer, p->image_buffer_size);
  ctx->vbytes_written += p->image_buffer_size;
  mutex_unlock(&p->image_buffer_mtx);
  p->fr_populated = false;
  return result;
}

static void init_audio_buffer(struct DecoderPrivate *p) {
  assert(p->audio_buffer_size == 0);
  // TODO: check that p->fr and p->fr_audio_target are both valid audio frames
  assert(p->fr_audio_target->sample_rate != 0 && p->fr->sample_rate != 0);

  p->audio_buffer_size = p->fr_audio_target->nb_samples * sizeof(float) * 2;
  // How much is 1 second of sound? sample_rate samples
  size_t target_audio_ring_size = p->fr->sample_rate * sizeof(float) * 2;
  // This is now also the bitrate for sound
  size_t size_adj =
      p->audio_buffer_size - target_audio_ring_size % p->audio_buffer_size;
  target_audio_ring_size += size_adj;
  size_t audio_ring_size = target_audio_ring_size;
  // If the ringbuffer size is too small compared to what raylib
  // expects(4096 bytes) make it bigger.
  if (audio_ring_size < 4096 * 8) {
    audio_ring_size = 4096 * 8;
    // It still has to divide by audio_buffer_size.
    size_adj = p->audio_buffer_size - audio_ring_size % p->audio_buffer_size;
    audio_ring_size += size_adj;
  }
  p->audio_buffer = make_ringbuffer(audio_ring_size);
  printf("Audio buffer size is now %zu\n", p->audio_buffer.buf_size);
}

static void init_swr_ifneeded(DecoderContext *ctx) {
  struct DecoderPrivate *p = ctx->p;
  // p->fr has to be an audio frame here
  if (p->audio_configured &&
      p->fr->nb_samples == p->fr_audio_target->nb_samples)
    return;

  if (p->audio_configured) {
    // If it was configured already, we have to reconfigure a few things
    av_frame_unref(p->fr_audio_target);

    p->fr_audio_target->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    p->fr_audio_target->format = AV_SAMPLE_FMT_FLT;
  }

  ctx->sample_rate = p->fr->sample_rate;
  p->fr_audio_target->sample_rate = ctx->sample_rate;

  int result = swr_config_frame(p->swr, p->fr_audio_target, p->fr);
  assert(result == 0);

  p->audio_configured = true;
}

static int process_audio_frame(DecoderContext *ctx) {
  struct DecoderPrivate *p = ctx->p;
  int result = RESULT_OK;
  if (!p->fr_audio_populated) {
    init_swr_ifneeded(ctx);
    assert(p->fr_populated);
    result = swr_convert_frame(p->swr, p->fr_audio_target, p->fr);
    ERRCHECK("Cant convert audio frame");
    p->fr_populated = false;
    p->fr_audio_populated = true;
  }

  if (p->audio_buffer_size == 0) {
    init_audio_buffer(p);
  }

  mutex_lock(&p->audio_buffer_mtx);
  size_t frame_size = p->fr_audio_target->nb_samples * sizeof(float) * 2;
  // fwrite(p->fr->data[0], 1, p->fr->nb_samples * 8, tempfile);
  // fflush(tempfile);
  size_t bytes_written = write_to_ringbuffer(
      &p->audio_buffer, p->fr_audio_target->data[0], frame_size);
  ctx->abytes_written += bytes_written;

  if (bytes_written == 0) {
    // No space left in the buffer, indicate STALL condition
    mutex_unlock(&p->audio_buffer_mtx);
    return RESULT_STALL;
  }

  p->fr_audio_populated = false;
  mutex_unlock(&p->audio_buffer_mtx);
cleanup:
  return result;
}

int dec_continue_decoding(DecoderContext *ctx) {
  assert(ctx == dc);
  if (ctx->state != DS_PLAYING && ctx->state != DS_STARTUP) {
    return RESULT_OK;
  }
  struct DecoderPrivate *p = ctx->p;

  // 1. Wait for decoding semaphore to release
  semaphore_wait(&p->sem);
  semaphore_incr(&p->sem);

  int result = RESULT_OK;

  // Assert that SWR doent have any data left in its buffers
  if (p->audio_configured)
    assert(swr_get_delay(p->swr, ctx->sample_rate) == 0);

  // 2. If we already have a frame waiting from previous iteration, just use it
  if (!(p->fr_populated || p->fr_audio_populated)) {
    // 2a. try pulling a frame
    result = pull_frame(p);

    while (result == AVERROR(EAGAIN)) {
      // 2b. frame doesn't exist, so we feed the decoder with the next packet
      assert(p->pkt_populated == false);
      result = av_read_frame(p->fmt_ctx, p->pkt);
      if (result < 0) {
        send_eof2codecs(p);
      } else {
        p->pkt_populated = true;
        result = feed_codec(p, NULL);
        ERRCHECK("Feed codec error");
      }
      // 2c. try pulling a frame again if we dont have it yet
      result = pull_frame(p);
    }
  }
  if (result != RESULT_OK) {
    goto cleanup;
  }

  // 3. Process the frame that we received
  if (p->frame_is_audio) {
    result = process_audio_frame(ctx);
  } else {
    result = process_video_frame(ctx);
  }
  // 4. If the buffer is full, signal stall condition
  if (result == RESULT_STALL) {
    // 4a. If we are in a startup state, signal that we decoded enough.
    signal_startup(ctx);
    semaphore_wait(&p->sem);
    goto cleanup;
  }

cleanup:
  return result;
}

void try_unlock_decoder_sem(DecoderContext *ctx) {
  int sem_value;
  struct DecoderPrivate *p = ctx->p;
  sem_getvalue(&p->sem, &sem_value);
  if (sem_value == 0) {
    // we can advance semaphore if both buffers are less than 80% empty
    if (ringbuffer_len(&p->image_buffer) < 0.8 * p->image_buffer.buf_size &&
        ringbuffer_len(&p->audio_buffer) < 0.8 * p->audio_buffer.buf_size) {
      semaphore_incr(&p->sem);
    }
  }
}

void try_move_to_finished_state(DecoderContext *ctx) {
  if (ctx->state != DS_FILEEOF)
    return;
  if (ringbuffer_len(&ctx->p->audio_buffer) == 0 &&
      ringbuffer_len(&ctx->p->image_buffer) == 0) {
    switch_dec_state(ctx, DS_FINISHED);
  }
}

uint8_t *dec_pull_image(DecoderContext *ctx, float *timestamp) {
  struct DecoderPrivate *p = ctx->p;

  mutex_lock(&p->image_buffer_mtx);
  if (ctx->state != DS_PLAYING && ctx->state != DS_FILEEOF) {
    return NULL;
  }
  // printf("pull_image called\n");
  uint8_t *data = read_ringbuffer_chunk(&p->image_buffer, p->image_buffer_size);
  if (data == NULL) {
    try_move_to_finished_state(ctx);
    return data;
  }
  ctx->vbytes_pulled += p->image_buffer_size;
  *timestamp = *(float *)data;
  return data + sizeof(float);
}

void dec_release_image(DecoderContext *ctx) {
  try_unlock_decoder_sem(ctx);
  mutex_unlock(&ctx->p->image_buffer_mtx);
}

int dec_pull_audio(DecoderContext *ctx, void *audio_buffer, unsigned int frames,
                   volatile AVRational *ts) {
  // printf("pull_audio %d called\n", frames);
  struct DecoderPrivate *p = ctx->p;
  if (ctx->state != DS_PLAYING && ctx->state != DS_FILEEOF) {
    return 0;
  }
  mutex_lock(&p->audio_buffer_mtx);
  if (!p->audio_configured) {
    mutex_unlock(&p->audio_buffer_mtx);
    return 0;
  }
  size_t bytes_to_read = frames * 2 * sizeof(float);
  int result = read_ringbuffer(&p->audio_buffer, audio_buffer, bytes_to_read);
  ctx->abytes_pulled += result;
  try_unlock_decoder_sem(ctx);
  const int bytes_per_frame = sizeof(float) * 2;
  *ts = av_add_q(*ts, av_make_q(result / bytes_per_frame, ctx->sample_rate));
  mutex_unlock(&p->audio_buffer_mtx);
  return result;
}

void dec_seek_to_frame(DecoderContext *ctx, double ts) {
  int64_t time = floor(ts * ctx->video_tb.den / ctx->video_tb.num);
  struct DecoderPrivate *p = ctx->p;
  mutex_lock(&p->audio_buffer_mtx);
  mutex_lock(&p->image_buffer_mtx);
  printf("seeking to %ld\n", time);
  int result = avformat_seek_file(p->fmt_ctx, p->video_stream, 0, time,
                                  INT64_MAX, AVSEEK_FLAG_ANY);
  printf("Seek file returns %d\n", result);
  avcodec_flush_buffers(p->ctxv);
  avcodec_flush_buffers(p->ctxa);
  if (ctx->state != DS_STARTUP) {
    // repurpose "startup" tech here
    assert(ctx->state == DS_PLAYING);
    // ctx->state = DS_STARTUP;
    switch_dec_state(ctx, DS_STARTUP);
    // decrease semaphore count, it should now become 0
    // semaphore_wait(&p->startup_sem);
  }
  ringbuffer_flush(&p->audio_buffer);
  ringbuffer_flush(&p->image_buffer);
  p->fr_audio_populated = false;
  p->fr_populated = false;
  if (p->pkt_populated) {
    p->pkt_populated = false;
    av_packet_unref(p->pkt);
  }
  try_unlock_decoder_sem(ctx);
  mutex_unlock(&p->image_buffer_mtx);
  mutex_unlock(&p->audio_buffer_mtx);
}

void *decode_thread(void *_) {
  int result = 0;
  static int call_count = 0;
  // wait for dc to be populated
  while (!dc) {
    usleep(1000);
  }
  do {
    switch (dc->state) {
    case DS_PLAYING:
      if (seek_command.active) {
        printf("seeking command received, seek to %ld\n", seek_command.target);
        // seek_to_frame(dc, seek_command.target);
        seek_command.active = false;
      }
      // FALLTHROUGH
    case DS_STARTUP:
      result = dec_continue_decoding(dc);
      call_count++;
      if (result < 0) {
        if (result == AVERROR_EOF) {
          // dc->state = DS_SHUTDOWN;
          switch_dec_state(dc, DS_FILEEOF);
        } else {
          char err_desc[256];
          av_strerror(result, err_desc, 256);
          fprintf(stderr, "Call count at err: %d, err: %d(%s)\n", call_count,
                  result, err_desc);
          // dc->state = DS_ERROR;
          switch_dec_state(dc, DS_ERROR);
        }
      }
      // exit(EXIT_FAILURE);
      break;
    default:
      usleep(1000);
      break;
    }
  } while (dc->state != DS_SHUTDOWN);
  return (void *)result;
}

bool dec_is_decoder_finished(DecoderContext *ctx) {
  return ctx->state == DS_SHUTDOWN || ctx->state == DS_FINISHED ||
         ctx->state == DS_ERROR;
}

void free_decoder_context(DecoderContext *ctx) {
  assert(dc == ctx || dc == NULL);
  struct DecoderPrivate *p = ctx->p;

  free_semaphore(&p->sem);
  // free_semaphore(&p->startup_sem);
  free_mutex(&p->command_q_mtx);
  free_mutex(&p->audio_buffer_mtx);
  free_mutex(&p->image_buffer_mtx);
  free_ringbuffer(&p->image_buffer);
  free_ringbuffer(&p->audio_buffer);
  if (p->pkt) {
    av_packet_free(&p->pkt);
  }
  if (p->fr_audio_target) {
    av_frame_free(&p->fr_audio_target);
  }
  if (p->fr) {
    av_frame_free(&p->fr);
  }
  if (p->ctxa) {
    avcodec_free_context(&p->ctxa);
  }
  if (p->ctxv) {
    avcodec_free_context(&p->ctxv);
  }
  if (p->swr) {
    swr_free(&p->swr);
  }
  if (p->fmt_ctx) {
    avformat_close_input(&p->fmt_ctx);
  }
  free(p);
  ctx->p = NULL;
  dc = NULL;
}

void dec_update_timelines(DecoderContext *ctx) {
  struct DecoderPrivate *p = ctx->p;
  unsigned int *tl_loc = (unsigned int *)timeline_push(&ctx->abuffer_timeline);
  if (p->audio_buffer_size > 0) {
    *tl_loc = ringbuffer_len(&p->audio_buffer) * 100 / p->audio_buffer.buf_size;
  } else {
    *tl_loc = 0;
  }
  tl_loc = (unsigned int *)timeline_push(&ctx->vbuffer_timeline);
  *tl_loc = ringbuffer_len(&p->image_buffer) * 100 / p->image_buffer.buf_size;
}

void dec_initialize() { thread_create(&decoder_thread, &decode_thread); }
// void dec_wait_ready(DecoderContext *ctx) {
//   semaphore_wait(&ctx->p->startup_sem);
// }

void dec_shutdown(DecoderContext *ctx) {
  printf("Thread shutdown isn't implemented yet\n");
  switch_dec_state(ctx, DS_SHUTDOWN);
  // TODO: implement
}

// Raylib specific things
#include <raylib.h>

const char *fs_yuv420p10 = "#version 130\n"

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
                           "   float smp = samplet(tex_luma);"
                           "   float luma_f = smp;"
                           "   smp = samplet(tex_u);"
                           "   float u_f = smp - 0.5;"
                           "   smp = samplet(tex_v);"
                           "   float v_f = smp - 0.5;"

                           "   luma_f = 1.1643*(luma_f - 0.0625);"
                           "   float r=luma_f+1.5958*v_f;"
                           "   float g=luma_f-0.39173*u_f-0.81290*v_f;"
                           "   float b=luma_f+2.017*u_f;"
                           "   finalColor=vec4(r, g, b, 1.0);"
                           "}";

// shader code adopted from
// https://stackoverflow.com/questions/30191911/is-it-possible-to-draw-yuv422-and-yuv420-texture-using-opengl
const char *fs_yuv420p = "#version 130 \n"
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

int dec_init_graphics_objects(DecoderContext *ctx, RaylibObjects *objs) {
  *objs = (RaylibObjects){};
  PixelFormat pf;
  const char *shader_code;

  switch (ctx->pixel_format) {
  case AV_PIX_FMT_YUV420P:
    pf = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE;
    objs->bytespp = 1;
    shader_code = fs_yuv420p;
    break;
  case AV_PIX_FMT_YUV420P10LE:
    pf = PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA;
    objs->bytespp = 2;
    shader_code = fs_yuv420p10;
    break;
  default:
    printf("Dont recognize pix fmt %d\n", ctx->pixel_format);
    return RESULT_ERROR;
  }

  Image frame_img = GenImageColor(ctx->video_width, ctx->video_height, BLUE);
  ImageFormat(&frame_img, pf);
  objs->tex_luma = LoadTextureFromImage(frame_img);

  // Make another texture with half height and width for UV channels
  UnloadImage(frame_img);
  frame_img = GenImageColor(ctx->video_width / 2, ctx->video_height / 2, GREEN);
  ImageFormat(&frame_img, pf);
  objs->tex_u = LoadTextureFromImage(frame_img);
  UnloadImage(frame_img);
  frame_img = GenImageColor(ctx->video_width / 2, ctx->video_height / 2, GREEN);
  ImageFormat(&frame_img, pf);
  objs->tex_v = LoadTextureFromImage(frame_img);
  UnloadImage(frame_img);

  objs->video_shader = LoadShaderFromMemory(NULL, shader_code);
  objs->y_location = GetShaderLocation(objs->video_shader, "tex_luma");
  objs->u_location = GetShaderLocation(objs->video_shader, "tex_u");
  objs->v_location = GetShaderLocation(objs->video_shader, "tex_v");

  objs->video_timest = 0;

  return RESULT_OK;
}

int dec_update_textures(DecoderContext *ctx, RaylibObjects *objs, float ts) {
  if (objs->video_timest < ts) {
    uint8_t *image_buff = NULL;
    while (objs->video_timest < ts) {
      uint8_t *prev_image = image_buff;
      image_buff = dec_pull_image(ctx, &objs->video_timest);
      if (image_buff == NULL) {
        // If we ran out of frames, just show the last one no matter the
        // timestamp.
        image_buff = prev_image;
        break;
      }
      objs->frame_counter++;
      // If we are going for the next iteration, have to make sure to
      // release_image.
      if (objs->video_timest < ts) {
        dec_release_image(ctx);
      }
    }
    if (image_buff) {
      UpdateTexture(objs->tex_luma, image_buff);
      image_buff += ctx->video_height * ctx->video_width * objs->bytespp;
      UpdateTexture(objs->tex_u, image_buff);
      image_buff += ctx->video_height * ctx->video_width * objs->bytespp / 4;
      UpdateTexture(objs->tex_v, image_buff);
    }
    dec_release_image(ctx);
  }

  return RESULT_OK;
}

int dec_draw_video_textures(RaylibObjects *objs, Vector2 position,
                            float rotation, float scale_factor, Color tint) {
  BeginShaderMode(objs->video_shader);
  {
    SetShaderValueTexture(objs->video_shader, objs->y_location, objs->tex_luma);
    SetShaderValueTexture(objs->video_shader, objs->u_location, objs->tex_u);
    SetShaderValueTexture(objs->video_shader, objs->v_location, objs->tex_v);

    // DrawTexture(frame_tex, 0, 0, WHITE);
    DrawTextureEx(objs->tex_luma, position, rotation, scale_factor, tint);
  }
  EndShaderMode();
  return RESULT_OK;
}

void timeline_draw_ui(TimeLine tl, int x, int y, int width, int height,
                      unsigned int max) {
  const unsigned short int font_size = 20;
  char text_buffer[80];
  snprintf(text_buffer, 16, "%d", max);
  int text_width = MeasureText(text_buffer, font_size);
  int zero_width = MeasureText("0", font_size);
  int max_text_width = text_width;
  if (zero_width > max_text_width)
    max_text_width = zero_width;
  DrawRectangle(x - 2, y - 2, width + 4, height + 4,
                (Color){.r = 40, .g = 40, .b = 40, .a = 190});
  DrawText(text_buffer, x + max_text_width - text_width + 1, y + 1, font_size,
           WHITE);
  DrawText("0", x + max_text_width - zero_width + 1, y + height - font_size + 1,
           font_size, WHITE);
  int gstart_x = x + max_text_width + 8;
  float segm_size_x = (width - max_text_width - 8) / (float)(tl.len - 1);
  DrawLine(gstart_x, y, gstart_x, y + height, GREEN);
  DrawLine(gstart_x, y + height, x + width, y + height, GREEN);
  DrawLine(gstart_x, y, x + width, y, GREEN);

  Vector2 *line_points = calloc(tl.len, sizeof(Vector2));
  unsigned int min_sample = max;
  unsigned int max_sample = 0;
  unsigned int avg = 0;
  for (unsigned int i = 0; i < tl.len; ++i) {
    unsigned int sample = *(unsigned int *)timeline_get(&tl, i);
    avg += sample;
    if (sample < min_sample)
      min_sample = sample;
    if (sample > max_sample)
      max_sample = sample;
    line_points[i].x = gstart_x + i * segm_size_x;
    line_points[i].y = y + height - (float)sample * height / max;
  }
  avg /= tl.len;

  snprintf(text_buffer, 80, "min:%d max:%d avg:%d", min_sample, max_sample,
           avg);
  DrawText(text_buffer, gstart_x + 8, y + height - font_size - 2, font_size,
           WHITE);
  DrawLineStrip(line_points, tl.len, BLUE);
  free(line_points);
}

void dec_draw_debug_overlay(DecoderContext *ctx, RaylibObjects *objs,
                            double audio_ts_double, int x, int y) {
  struct DecoderPrivate *p = ctx->p;
  timeline_draw_ui(ctx->abuffer_timeline, x, y, 300, 80, 100);
  timeline_draw_ui(ctx->vbuffer_timeline, x, y + 100, 300, 80, 100);
  char msg[128];
  snprintf(msg, sizeof(msg), "atime: %.2f vtime: %.2f d: %.2f f: %d",
           audio_ts_double, objs->video_timest,
           fabs(audio_ts_double - objs->video_timest), objs->frame_counter);

  DrawText(msg, x, y + 200, 20, WHITE);
  snprintf(msg, sizeof(msg), "abytes: %ld written: %ld", ctx->abytes_pulled,
           ctx->abytes_written);
  DrawText(msg, x, y + 225, 20, WHITE);
  snprintf(msg, sizeof(msg), "vbytes: %ld, written: %ld", ctx->vbytes_pulled,
           ctx->vbytes_written);
  DrawText(msg, x, y + 250, 20, WHITE);

  if (p) {
    snprintf(msg, sizeof(msg), "abuffer: %zu vbuffer: %zu",
             ringbuffer_len(&p->audio_buffer),
             ringbuffer_len(&p->image_buffer));
    DrawText(msg, x, y + 275, 20, WHITE);
  }
}
