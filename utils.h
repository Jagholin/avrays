#ifndef UTILS_H
#define UTILS_H
#include <assert.h>
#include <libavutil/mem.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UTILFUNC [[maybe_unused]] static inline

typedef struct CleanupStack {
  struct CleanupStack *pnext;
  void *what;
  void (*cb)(void *);
} CleanupStack;
static CleanupStack *cs = NULL;

static inline void cleanup_mem_fn(void *d) { free(d); }
UTILFUNC void cleanup_push_mem(void *memloc) {
  CleanupStack *newcs = malloc(sizeof(CleanupStack));
  *newcs = (CleanupStack){.what = memloc, .cb = &cleanup_mem_fn, .pnext = cs};
  cs = newcs;
}
UTILFUNC void run_cleanups() {
  while (cs) {
    CleanupStack *t = cs;
    cs->cb(cs->what);
    cs = cs->pnext;
    free(t);
  }
}

typedef struct String {
  char *str;
  size_t len;
  size_t space;
} String;

UTILFUNC String make_string() {
  char *temp = malloc(256);
  temp[0] = '\0';

  return (String){.str = temp, .len = 0, .space = 256};
}

UTILFUNC void free_string(String s) { free(s.str); }

UTILFUNC void concat_string_chars(String *s, char *src) {
  size_t new_len = s->len + strlen(src);
  if (new_len >= s->space) {
    size_t new_space = s->space * 2;
    while (new_len >= new_space) {
      new_space *= 2;
    }
    s->str = realloc(s->str, new_space);
    s->space = new_space;
  }
  strcat(s->str, src);
  s->len = new_len;
}

typedef struct RingBuffer {
  uint8_t *buffer;
  size_t buf_size;
  ptrdiff_t read_head, write_head;
} RingBuffer;

UTILFUNC RingBuffer make_ringbuffer(size_t len) {
  uint8_t *temp = av_malloc(len);
  return (RingBuffer){.buffer = temp, .buf_size = len};
}

UTILFUNC size_t write_to_ringbuffer(RingBuffer *rb, uint8_t *what, size_t len) {
  // calculate capacity
  size_t capacity = 0;
  if (rb->write_head >= rb->read_head) {
    capacity = rb->buf_size - rb->write_head;
    capacity += rb->read_head;
  } else {
    capacity = rb->read_head - rb->write_head;
  }

  if (capacity == 0) {
    return 0;
  }

  if (capacity < len)
    len = capacity;
  if (len + rb->write_head > rb->buf_size) {
    // we need to separate into 2 chunks
    // first chunk is to the end of the buffer, lets calculate its size
    ptrdiff_t first_ch_len = rb->buf_size - rb->write_head;
    ptrdiff_t second_ch_len = len - first_ch_len;
    assert(second_ch_len < rb->read_head);
    memcpy(rb->buffer + rb->write_head, what, first_ch_len);
    memcpy(rb->buffer, what + first_ch_len, second_ch_len);
    rb->write_head = second_ch_len;
    return len;
  } else {
    memcpy(rb->buffer + rb->write_head, what, len);
    rb->write_head += len;
    return len;
  }
}

UTILFUNC uint8_t *write_ringbuffer_chunk_nocommit(RingBuffer *rb, size_t len) {
  assert(rb->buf_size % len == 0);
  if (rb->write_head + len > rb->buf_size) {
    return NULL;
  }

  // dont run past read_head
  if (rb->read_head > rb->write_head && rb->write_head + len >= rb->read_head) {
    return NULL;
  }

  if (rb->write_head + len == rb->buf_size && rb->read_head == 0) {
    return NULL;
  }

  return rb->buffer + rb->write_head;
}

UTILFUNC void write_ringbuffer_commit(RingBuffer *rb, size_t len) {
  rb->write_head += len;
  assert(rb->write_head <= rb->buf_size);
  if (rb->write_head == rb->buf_size)
    rb->write_head = 0;
}

UTILFUNC uint8_t *read_ringbuffer_chunk(RingBuffer *rb, size_t len) {
  // this function only works if reading doesn't cross rb's boundary
  assert(rb->buf_size % len == 0);
  if (rb->read_head + len > rb->buf_size) {
    return NULL;
  }

  // If we run past write_head, we don't have enough data
  if (rb->write_head >= rb->read_head && rb->read_head + len > rb->write_head) {
    return NULL;
  }

  uint8_t *result = rb->buffer + rb->read_head;
  rb->read_head += len;
  assert(rb->read_head <= rb->buf_size);
  if (rb->read_head == rb->buf_size)
    rb->read_head = 0;
  return result;
}

UTILFUNC size_t read_ringbuffer(RingBuffer *rb, uint8_t *dest, size_t len) {
  size_t data_available = 0;
  if (rb->write_head >= rb->read_head) {
    data_available += rb->write_head - rb->read_head;
  } else {
    data_available += rb->buf_size - rb->read_head;
    data_available += rb->write_head;
  }

  if (data_available < len) {
    return 0;
  }
  if (rb->read_head + len > rb->buf_size) {
    size_t first_ch_len = rb->buf_size - rb->read_head;
    size_t second_ch_len = len - first_ch_len;
    memcpy(dest, rb->buffer + rb->read_head, first_ch_len);
    memcpy(dest + first_ch_len, rb->buffer, second_ch_len);
    rb->read_head = second_ch_len;
  } else {
    memcpy(dest, rb->buffer + rb->read_head, len);
    rb->read_head += len;
  }
  return len;
}

UTILFUNC size_t ringbuffer_len(RingBuffer *rb) {
  // The size is the distance from read head to write head
  if (rb->write_head >= rb->read_head)
    return rb->write_head - rb->read_head;
  // rb->write_head < rb->read_head
  size_t result = rb->buf_size - rb->read_head;
  result += rb->write_head;
  return result;
}

UTILFUNC void free_ringbuffer(RingBuffer *rb) {
  av_free(rb->buffer);
  *rb = (RingBuffer){0};
}

#define ERRCHECK(...)                                                          \
  if (result < 0) {                                                            \
    printf("Err: ");                                                           \
    printf(__VA_ARGS__);                                                       \
    goto cleanup;                                                              \
  }
#define ERRCHECK2(val, ...)                                                    \
  if (!(val)) {                                                                \
    printf("Err: ");                                                           \
    printf(__VA_ARGS__);                                                       \
    goto cleanup;                                                              \
  }

#endif // !UTILS_H
