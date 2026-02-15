#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "state_json.h"

static char *read_file_all(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) return NULL;

  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long n = ftell(f);
  if (n < 0) { fclose(f); return NULL; }
  if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

  char *buf = calloc(1, (size_t)n + 1);
  if (!buf) { fclose(f); return NULL; }

  if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    free(buf);
    return NULL;
  }

  fclose(f);
  return buf;
}

static int json_get_string(const char *buf, const char *key, char *out, size_t out_sz) {
  char pat[128];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(buf, pat);
  if (!p) return 1;

  p = strchr(p, ':');
  if (!p) return 1;
  p++;

  while (*p == ' ' || *p == '\t') p++;
  if (*p != '"') return 1;
  p++;

  size_t j = 0;
  while (*p && *p != '"') {
    if (*p == '\\' && p[1]) p++; /* minimal */
    if (j + 1 < out_sz) out[j++] = *p;
    p++;
  }
  out[j] = 0;
  return 0;
}

static int json_get_int(const char *buf, const char *key, int *out) {
  char pat[128];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(buf, pat);
  if (!p) return 1;

  p = strchr(p, ':');
  if (!p) return 1;
  p++;

  while (*p == ' ' || *p == '\t') p++;
  *out = atoi(p);
  return 0;
}

static int json_get_time(const char *buf, const char *key, time_t *out) {
  int v = 0;
  if (json_get_int(buf, key, &v) != 0) return 1;
  *out = (time_t)v;
  return 0;
}

int write_state_json_ex(const char *uuid,
                        const char *name,
                        const char *bundle,
                        int pid,
                        const char *status,
                        int exit_code,
                        time_t created_at,
                        time_t started_at,
                        time_t stopped_at) {
  char path[512];
  snprintf(path, sizeof(path), "%s/state.json", bundle);

  FILE *f = fopen(path, "w");
  if (!f) {
    perror("fopen state.json");
    return 1;
  }

  fprintf(f,
    "{\n"
    "  \"id\": \"%s\",\n"
    "  \"status\": \"%s\",\n"
    "  \"pid\": %d,\n"
    "  \"exit_code\": %d,\n"
    "  \"created_at\": %ld,\n"
    "  \"started_at\": %ld,\n"
    "  \"stopped_at\": %ld,\n"
    "  \"bundle\": \"%s/\",\n"
    "  \"annotations\": {\n"
    "    \"name\": \"%s\"\n"
    "  }\n"
    "}\n",
    uuid,
    status,
    pid,
    exit_code,
    (long)created_at,
    (long)started_at,
    (long)stopped_at,
    bundle,
    (name && name[0]) ? name : ""
  );

  fclose(f);
  return 0;
}

int read_state_json(const char *bundle, struct zocker_state *out) {
  if (!out) return 1;
  memset(out, 0, sizeof(*out));
  out->exit_code = -1;

  char path[512];
  snprintf(path, sizeof(path), "%s/state.json", bundle);

  char *buf = read_file_all(path);
  if (!buf) return 1;

  (void)json_get_string(buf, "id", out->id, sizeof(out->id));
  (void)json_get_string(buf, "status", out->status, sizeof(out->status));
  (void)json_get_int(buf, "pid", &out->pid);
  (void)json_get_int(buf, "exit_code", &out->exit_code);
  (void)json_get_time(buf, "created_at", &out->created_at);
  (void)json_get_time(buf, "started_at", &out->started_at);
  (void)json_get_time(buf, "stopped_at", &out->stopped_at);
  (void)json_get_string(buf, "bundle", out->bundle, sizeof(out->bundle));

  /* name is inside annotations; simplest: search for "name" key */
  (void)json_get_string(buf, "name", out->name, sizeof(out->name));

  free(buf);
  return 0;
}
