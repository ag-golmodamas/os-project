#ifndef __STATE_JSON_H__
#define __STATE_JSON_H__

#include <time.h>

struct zocker_state {
  char id[64];        /* UUID */
  char name[64];      /* annotations.name */
  char status[16];    /* created|running|stopped */
  int pid;
  int exit_code;      /* -1 unknown, else actual */
  time_t created_at;
  time_t started_at;
  time_t stopped_at;
  char bundle[512];
};

int write_state_json_ex(const char *uuid,
                        const char *name,
                        const char *bundle,
                        int pid,
                        const char *status,
                        int exit_code,
                        time_t created_at,
                        time_t started_at,
                        time_t stopped_at);

int read_state_json(const char *bundle, struct zocker_state *out);

#endif

