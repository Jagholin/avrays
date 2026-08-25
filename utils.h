/**********************************************************************************************
 *
 *   LICENSE: zlib/libpng
 *
 *   avray is licensed under an unmodified zlib/libpng license, which is an
 * OSI-certified, BSD-like license that allows static linking with closed source
 * software:
 *
 *   Copyright (c) 2026 Jagholin (github.com/Jagholin)
 *
 *   This software is provided "as-is", without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from the
 * use of this software.
 *
 *   Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 *     1. The origin of this software must not be misrepresented; you must not
 * claim that you wrote the original software. If you use this software in a
 * product, an acknowledgment in the product documentation would be appreciated
 * but is not required.
 *
 *     2. Altered source versions must be plainly marked as such, and must not
 * be misrepresented as being the original software.
 *
 *     3. This notice may not be removed or altered from any source
 * distribution.
 *
 **********************************************************************************************/
#ifndef UTILS_H
#define UTILS_H
#include <GL/gl.h>
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

UTILFUNC size_t ringbuffer_len(RingBuffer *rb) {
  // The size is the distance from read head to write head
  if (rb->write_head >= rb->read_head)
    return rb->write_head - rb->read_head;
  // rb->write_head < rb->read_head
  size_t result = rb->buf_size - rb->read_head;
  result += rb->write_head;
  return result;
}

UTILFUNC size_t write_to_ringbuffer(RingBuffer *rb, uint8_t *what, size_t len) {
  // calculate capacity
  size_t len_before = ringbuffer_len(rb);
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

  if (capacity <= len)
    return 0;
  // len = capacity;
  if (len + rb->write_head > rb->buf_size) {
    // we need to separate into 2 chunks
    // first chunk is to the end of the buffer, lets calculate its size
    ptrdiff_t first_ch_len = rb->buf_size - rb->write_head;
    ptrdiff_t second_ch_len = len - first_ch_len;
    assert(second_ch_len < rb->read_head);
    memcpy(rb->buffer + rb->write_head, what, first_ch_len);
    memcpy(rb->buffer, what + first_ch_len, second_ch_len);
    rb->write_head = second_ch_len;
    size_t len_after = ringbuffer_len(rb);
    assert(len_after - len_before == len);
    return len;
  } else {
    memcpy(rb->buffer + rb->write_head, what, len);
    rb->write_head += len;
    size_t len_after = ringbuffer_len(rb);
    assert(len_after - len_before == len);
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
  size_t len_before = ringbuffer_len(rb);
  rb->write_head += len;
  assert(rb->write_head <= rb->buf_size);
  if (rb->write_head == rb->buf_size)
    rb->write_head = 0;
  size_t len_after = ringbuffer_len(rb);
  assert(len_after - len_before == len);
}

UTILFUNC uint8_t *read_ringbuffer_chunk(RingBuffer *rb, size_t len) {
  size_t len_before = ringbuffer_len(rb);
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
  size_t len_after = ringbuffer_len(rb);
  assert(len_before - len_after == len);
  return result;
}

UTILFUNC size_t read_ringbuffer(RingBuffer *rb, uint8_t *dest, size_t len) {
  size_t len_before = ringbuffer_len(rb);
  size_t data_available = 0;
  if (rb->write_head >= rb->read_head) {
    data_available += rb->write_head - rb->read_head;
  } else {
    data_available += rb->buf_size - rb->read_head;
    data_available += rb->write_head;
  }

  if (data_available < len) {
    // return 0;
    len = data_available;
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
  size_t len_after = ringbuffer_len(rb);
  assert(len_before - len_after == len);
  return len;
}

UTILFUNC void free_ringbuffer(RingBuffer *rb) {
  if (rb->buffer) {
    av_free(rb->buffer);
    *rb = (RingBuffer){0};
  }
}

UTILFUNC void ringbuffer_flush(RingBuffer *rb) {
  rb->read_head = 0;
  rb->write_head = 0;
}

typedef struct TimeLine {
  void *buffer;
  size_t elem_size;
  unsigned int cursor;
  size_t len;
} TimeLine;

UTILFUNC TimeLine make_timeline(size_t elem_size, size_t length) {
  void *buf = malloc(elem_size * length);
  memset(buf, 0, elem_size * length);
  return (TimeLine){buf, elem_size, 0, length};
}

UTILFUNC void *timeline_push(TimeLine *self) {
  void *result = self->buffer + self->elem_size * self->cursor;
  self->cursor += 1;
  if (self->cursor >= self->len) {
    self->cursor = 0;
  }
  return result;
}

UTILFUNC void *timeline_get(TimeLine *self, unsigned int i) {
  size_t off = self->cursor + i;
  if (off >= self->len)
    off -= self->len;
  if (off >= self->len)
    return NULL;
  return self->buffer + self->elem_size * off;
}

UTILFUNC void free_timeline(TimeLine *self) {
  free(self->buffer);
  self->buffer = NULL;
}

typedef struct LinkedQueue {
  struct LinkedQueue *pnext;
  void *data;
} LinkedQueue;

UTILFUNC LinkedQueue *make_queue() {
  LinkedQueue *result = calloc(1, sizeof(LinkedQueue));
  return result;
}

UTILFUNC LinkedQueue *queue_add(LinkedQueue *q, void *data) {
  LinkedQueue *left = NULL, *right = q;
  while (right->pnext) {
    left = right;
    right = right->pnext;
  }
  LinkedQueue *new_link = make_queue();
  new_link->data = data;
  new_link->pnext = right;
  if (left) {
    left->pnext = new_link;
    return q;
  } else {
    return new_link;
  }
}

UTILFUNC void *queue_pop(LinkedQueue **q) {
  void *result = (*q)->data;
  LinkedQueue *head = *q;
  if (head->pnext == NULL) {
    return NULL;
  }
  (*q) = head->pnext;
  free(head);
  return result;
}

UTILFUNC void queue_free(LinkedQueue *q) {
  while (q) {
    LinkedQueue *t = q;
    q = q->pnext;
    if (t->data) {
      free(t->data);
    }
    free(t);
  }
}

#define ERRCHECK(...)                                                          \
  if (result < 0) {                                                            \
    TraceLog(LOG_ERROR, __VA_ARGS__);                                          \
    result = RESULT_ERROR;                                                     \
    goto cleanup;                                                              \
  }
#define ERRCHECK2(val, ...)                                                    \
  if (!(val)) {                                                                \
    TraceLog(LOG_ERROR, __VA_ARGS__);                                          \
    result = RESULT_ERROR;                                                     \
    goto cleanup;                                                              \
  }
#define ERRCHECKR(res, ...)                                                    \
  if (result < 0) {                                                            \
    TraceLog(LOG_ERROR, __VA_ARGS__);                                          \
    result = (res);                                                            \
    goto cleanup;                                                              \
  }
#define ERRCHECK2R(val, res, ...)                                              \
  if (!(val)) {                                                                \
    TraceLog(LOG_ERROR, __VA_ARGS__);                                          \
    result = (res);                                                            \
    goto cleanup;                                                              \
  }

#endif // !UTILS_H
