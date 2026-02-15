#ifndef __RUN_H__
#define __RUN_H__

#include <stdint.h>
#include "config.h"

#define MAX_MOUNTS 32

struct mount_entry {
 char destination[256];   /* inside container, like /proc or /data */
 char type[32];           /* "proc" or "bind" */
 char source[512];        /* for bind: host path. for proc: "proc" */
};

struct container {
 char id[64];          /* human name (annotations.name) */
 char command[256];
 char base_dir[256];
 char base_image[256];

 /* OCI resources from config.json */
 int64_t memory_limit_bytes;  /* linux.resources.memory.limit */
 int     cpu_shares;          /* linux.resources.cpu.shares */

 /* OCI mounts from config.json */
 int mount_count;
 struct mount_entry mounts[MAX_MOUNTS];
};

void container_from_config(struct config cfg, struct container *c);

/* Project lifecycle */
int zocker_run(struct container cont);
int zocker_start(const char *uuid);
int zocker_stop(const char *uuid);
int zocker_restart(const char *uuid);
int zocker_ps(int all);
int zocker_rm(const char *uuid, int force);

/* exec into running container */
int zocker_exec(const char *uuid, const char *cmd);

#endif
