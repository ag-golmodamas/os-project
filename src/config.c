#include <stdio.h>
#include <string.h>

#include "config.h"

int validate_config(struct config cfg) {
  if (cfg.subcommand == NONE) {
    fprintf(stderr, "[ERR] Missing subcommand (run|exec)\n");
    return 1;
  }

  if (strcmp(cfg.command, "") == 0) {
    fprintf(stderr, "[ERR] Missing command (e.g. 'sleep 1000')\n");
    return 1;
  }

  /* cfg.name may be empty (name is optional) */
  return 0;
}
