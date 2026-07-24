#include <stdlib.h>
#include <unistd.h>

int main() {
  char fname[] = "/tmp/FTMP_XXXXXX";
  int filedes = mkstemp(fname);
  close(filedes);
  return 0;
}
