#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "name_map.h"

static int mkdir_p_one(const char *path, mode_t mode) {
  struct stat st;
  if (stat(path, &st) == 0) {
    if (S_ISDIR(st.st_mode)) return 0;
    errno = ENOTDIR;
    return 1;
  }
  if (mkdir(path, mode) != 0) return 1;
  return 0;
}

static void names_dir_path(const char *zocker_prefix, char *out, size_t out_sz) {
  snprintf(out, out_sz, "%s/names", zocker_prefix);
}

static void name_file_path(const char *zocker_prefix, const char *name, char *out, size_t out_sz) {
  snprintf(out, out_sz, "%s/names/%s", zocker_prefix, name);
}

int name_map_set(const char *zocker_prefix, const char *name, const char *uuid) {
  if (!name || name[0] == '\0') return 0; /* no name -> no mapping */

  char dir[4096];
  names_dir_path(zocker_prefix, dir, sizeof(dir));
  if (mkdir_p_one(dir, 0755) != 0 && errno != EEXIST) return 1;

  char path[4096];
  name_file_path(zocker_prefix, name, path, sizeof(path));

  FILE *f = fopen(path, "w");
  if (!f) return 1;

  fprintf(f, "%s\n", uuid);
  fclose(f);
  return 0;
}

int name_map_get(const char *zocker_prefix, const char *name, char *uuid_out, size_t uuid_out_sz) {
  if (!name || name[0] == '\0') return 1;

  char path[4096];
  name_file_path(zocker_prefix, name, path, sizeof(path));

  FILE *f = fopen(path, "r");
  if (!f) return 1;

  if (!fgets(uuid_out, (int)uuid_out_sz, f)) {
    fclose(f);
    return 1;
  }
  fclose(f);

  /* strip newline */
  uuid_out[strcspn(uuid_out, "\r\n")] = 0;
  return (uuid_out[0] != '\0') ? 0 : 1;
}

int name_map_del(const char *zocker_prefix, const char *name) {
  if (!name || name[0] == '\0') return 0;
  char path[4096];
  name_file_path(zocker_prefix, name, path, sizeof(path));
  unlink(path); /* ignore errors */
  return 0;
}
