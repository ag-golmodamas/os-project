#include <stdio.h>
#include <string.h>

#include "uuid.h"

int generate_uuid(char out[37]) {

  FILE *f = fopen("/proc/sys/kernel/random/uuid", "r");
  if (!f) {
    perror("fopen uuid");
    return 1;
  }

  if (!fgets(out, 37, f)) {
    perror("fgets uuid");
    fclose(f);
    return 1;
  }

  /* Remove newline */
  out[strcspn(out, "\n")] = 0;

  fclose(f);
  return 0;
}
