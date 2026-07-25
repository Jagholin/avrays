#ifndef UTILS_H
#define UTILS_H
#include <stdlib.h>
#include <string.h>

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

#define ERRCHECK(msg)                                                          \
  if (result < 0) {                                                            \
    printf("ERR: %s\n", msg);                                                  \
    return 1;                                                                  \
  }
#define ERRCHECK2(val, msg)                                                    \
  if (!(val)) {                                                                \
    printf("ERR: %s\n", msg);                                                  \
    return 1;                                                                  \
  }

#endif // !UTILS_H
