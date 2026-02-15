#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>

#include "config_json.h"

/* ---------- helpers ---------- */

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

/* Read next JSON string starting at first quote (") and unescape minimally into out */
static const char *read_json_string(const char *p, char *out, size_t out_sz) {
 if (!p || *p != '"') return NULL;
 p++; /* inside string */

 size_t j = 0;
 while (*p) {
   if (*p == '\\' && p[1]) {
     if (j + 1 < out_sz) out[j++] = p[1];
     p += 2;
     continue;
   }
   if (*p == '"') break;
   if (j + 1 < out_sz) out[j++] = *p;
   p++;
 }
 if (*p != '"') return NULL;
 out[j] = '\0';
 return p + 1; /* after closing quote */
}

/* Apply "KEY=VALUE" using setenv */
static void apply_env_kv(const char *kv) {
 const char *eq = strchr(kv, '=');
 if (!eq) return;

 size_t klen = (size_t)(eq - kv);
 if (klen == 0) return;

 char key[256];
 if (klen >= sizeof(key)) klen = sizeof(key) - 1;
 memcpy(key, kv, klen);
 key[klen] = '\0';

 const char *val = eq + 1;
 setenv(key, val, 1);
}

/*
Parse an integer that appears after a key inside some region.
Example usage:
  region = strstr(buf, "\"memory\"");
  find "\"limit\"" after region, then parse number after ':'
*/
static int parse_int64_after_key(const char *region, const char *key, int64_t *out) {
 if (!region || !key || !out) return 1;

 const char *p = strstr(region, key);
 if (!p) return 1;

 p = strchr(p, ':');
 if (!p) return 1;
 p++;

 while (*p && isspace((unsigned char)*p)) p++;

 errno = 0;
 char *end = NULL;
 long long v = strtoll(p, &end, 10);
 if (end == p) return 1;
 if (errno != 0) return 1;

 *out = (int64_t)v;
 return 0;
}

static int parse_int_after_key(const char *region, const char *key, int *out) {
 int64_t tmp = 0;
 if (parse_int64_after_key(region, key, &tmp) != 0) return 1;
 if (tmp < 0) tmp = 0;
 if (tmp > 2147483647LL) tmp = 2147483647LL;
 *out = (int)tmp;
 return 0;
}

/* Find "key": "value" inside [start,end) */
static int json_get_string_in_range(const char *start, const char *end,
                                  const char *key,
                                  char *out, size_t out_sz) {
 if (!start || !end || start >= end || !key || !out) return 1;

 /* make a temporary null-terminated slice */
 size_t len = (size_t)(end - start);
 char *tmp = (char *)calloc(1, len + 1);
 if (!tmp) return 1;
 memcpy(tmp, start, len);
 tmp[len] = '\0';

 const char *p = strstr(tmp, key);
 if (!p) { free(tmp); return 1; }

 p = strchr(p, ':');
 if (!p) { free(tmp); return 1; }
 p++;

 while (*p && isspace((unsigned char)*p)) p++;
 if (*p != '"') { free(tmp); return 1; }

 char val[1024];
 const char *next = read_json_string(p, val, sizeof(val));
 if (!next) { free(tmp); return 1; }

 strncpy(out, val, out_sz - 1);
 out[out_sz - 1] = '\0';

 free(tmp);
 return 0;
}

/* Parse mounts[] into c->mounts */
static void parse_mounts(struct container *c, const char *buf) {
 if (!c || !buf) return;

 c->mount_count = 0;

 const char *m = strstr(buf, "\"mounts\"");
 if (!m) return;

 m = strchr(m, '[');
 if (!m) return;
 m++; /* after '[' */

 while (*m && c->mount_count < MAX_MOUNTS) {
   /* skip whitespace/commas */
   while (*m && (*m == ' ' || *m == '\t' || *m == '\n' || *m == '\r' || *m == ',')) m++;
   if (!*m || *m == ']') break;

   if (*m != '{') {
     m++;
     continue;
   }

   const char *obj_start = m;
   const char *obj_end = strchr(obj_start, '}');
   if (!obj_end) break;
   obj_end++; /* include '}' */

   struct mount_entry me;
   memset(&me, 0, sizeof(me));

   (void)json_get_string_in_range(obj_start, obj_end, "\"destination\"", me.destination, sizeof(me.destination));
   (void)json_get_string_in_range(obj_start, obj_end, "\"type\"", me.type, sizeof(me.type));
   (void)json_get_string_in_range(obj_start, obj_end, "\"source\"", me.source, sizeof(me.source));

   /* Only add if destination+type exist */
   if (me.destination[0] && me.type[0]) {
     c->mounts[c->mount_count++] = me;
   }

   m = obj_end;
 }
}

/* ---------- public API ---------- */

int write_config_json(struct container *c, const char *root_dir) {
 char path[512];
 snprintf(path, sizeof(path), "%s/config.json", root_dir);

 FILE *f = fopen(path, "w");
 if (!f) {
   perror("fopen config.json");
   return 1;
 }

 /* Defaults from the assignment example */
 long long mem = (c && c->memory_limit_bytes > 0) ? (long long)c->memory_limit_bytes : 536870912LL;
 int shares = (c && c->cpu_shares > 0) ? c->cpu_shares : 1024;

 fprintf(f,
   "{\n"
   "  \"process\": {\n"
   "    \"terminal\": false,\n"
   "    \"args\": [\"/bin/sh\", \"-c\", \"%s\"],\n"
   "    \"env\": [\n"
   "      \"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\"\n"
   "    ],\n"
   "    \"cwd\": \"/\"\n"
   "  },\n"
   "  \"mounts\": [\n"
   "    {\n"
   "      \"destination\": \"/proc\",\n"
   "      \"type\": \"proc\",\n"
   "      \"source\": \"proc\"\n"
   "    }\n"
   "  ],\n"
   "  \"linux\": {\n"
   "    \"namespaces\": [\n"
   "      { \"type\": \"pid\" },\n"
   "      { \"type\": \"mount\" },\n"
   "      { \"type\": \"uts\" }\n"
   "    ],\n"
   "    \"resources\": {\n"
   "      \"memory\": { \"limit\": %lld },\n"
   "      \"cpu\": { \"shares\": %d }\n"
   "    }\n"
   "  }\n"
   "}\n",
   c ? c->command : "",
   mem,
   shares
 );

 fclose(f);
 return 0;
}

/*
- Extract args[2] into c->command
- Extract env[] and apply them via setenv()
- Extract linux.resources.memory.limit into c->memory_limit_bytes
- Extract linux.resources.cpu.shares into c->cpu_shares
- NEW: Extract mounts[] into c->mounts
*/
int read_config_json(struct container *c, const char *root_dir) {
 if (!c) return 1;

 /* defaults if not found */
 c->memory_limit_bytes = 0;
 c->cpu_shares = 0;
 c->mount_count = 0;

 char path[512];
 snprintf(path, sizeof(path), "%s/config.json", root_dir);

 char *buf = read_file_all(path);
 if (!buf) {
   perror("fopen config.json");
   return 1;
 }

 /* ---- parse args ---- */
 char *p = strstr(buf, "\"args\"");
 if (!p) { free(buf); return 1; }

 p = strchr(p, '[');
 if (!p) { free(buf); return 1; }
 p++; /* after '[' */

 int string_index = 0;
 c->command[0] = '\0';

 while (*p) {
   while (*p && *p != '"' && *p != ']') p++;
   if (*p == ']') break;
   if (*p != '"') { p++; continue; }

   char tmp[512];
   const char *next = read_json_string(p, tmp, sizeof(tmp));
   if (!next) break;
   p = (char *)next;

   if (string_index == 2) {
     strncpy(c->command, tmp, sizeof(c->command) - 1);
     c->command[sizeof(c->command) - 1] = '\0';
     break;
   }
   string_index++;
 }

 if (c->command[0] == '\0') {
   free(buf);
   return 1;
 }

 /* ---- parse env ---- */
 char *e = strstr(buf, "\"env\"");
 if (e) {
   e = strchr(e, '[');
   if (e) {
     e++; /* after '[' */
     while (*e) {
       while (*e && *e != '"' && *e != ']') e++;
       if (*e == ']') break;
       if (*e != '"') { e++; continue; }

       char kv[1024];
       const char *next = read_json_string(e, kv, sizeof(kv));
       if (!next) break;
       e = (char *)next;

       apply_env_kv(kv);
     }
   }
 }

 /* ---- parse memory.limit and cpu.shares ---- */
 const char *linux = strstr(buf, "\"linux\"");
 if (linux) {
   const char *resources = strstr(linux, "\"resources\"");
   if (resources) {
     const char *mem = strstr(resources, "\"memory\"");
     if (mem) {
       (void)parse_int64_after_key(mem, "\"limit\"", &c->memory_limit_bytes);
     }
     const char *cpu = strstr(resources, "\"cpu\"");
     if (cpu) {
       (void)parse_int_after_key(cpu, "\"shares\"", &c->cpu_shares);
     }
   }
 }

 /* ---- NEW: parse mounts[] ---- */
 parse_mounts(c, buf);

 free(buf);
 return 0;
}
