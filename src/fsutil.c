#define _GNU_SOURCE
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fsutil.h"

static int unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
  (void)sb; (void)typeflag; (void)ftwbuf;
  if (remove(fpath) != 0) {
    fprintf(stderr, "[ERR] remove(%s): %s\n", fpath, strerror(errno));
    return 1;
  }
  return 0;
}

int rm_rf(const char *path) {
  /* FTW_DEPTH deletes children before parent */
  if (nftw(path, unlink_cb, 64, FTW_DEPTH | FTW_PHYS) != 0) {
    return 1;
  }
  return 0;
}
