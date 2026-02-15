#ifndef __CONFIG_H__
#define __CONFIG_H__

enum COMMAND {
  NONE = 0,
  RUN = 10,
  EXEC = 11,
};

struct config {
  enum COMMAND subcommand;

  /* Human-friendly name (optional). Example: demo */
  char name[64];

  /* Real unique container ID (UUID). Always set for run. */
  char id[64];

  char command[256];
  char base_dir[512];
  char base_image[256];
};

int validate_config(struct config cfg);

#endif
