#ifndef UTILS_H
#define UTILS_H
#include <stdlib.h>
#include <string.h>

typedef struct CleanupStack {
  struct CleanupStack *pnext;
  void *what;
  void (*cb)(void *);
} CleanupStack;
static CleanupStack *cs = NULL;

void CleanupMemFn(void *d) { free(d); }
void CleanupPushMem(void *memloc) {
  CleanupStack *newcs = malloc(sizeof(CleanupStack));
  *newcs = (CleanupStack){.what = memloc, .cb = &CleanupMemFn, .pnext = cs};
  cs = newcs;
}
void RunCleanups() {
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

String MakeString() {
  char *temp = malloc(256);
  temp[0] = '\0';

  return (String){.str = temp, .len = 0, .space = 256};
}

void FreeString(String s) { free(s.str); }

void ConcatStringChars(String *s, char *src) {
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
