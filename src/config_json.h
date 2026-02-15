#ifndef __CONFIG_JSON_H__
#define __CONFIG_JSON_H__

#include "run.h"

/* Write config.json for container */
int write_config_json(struct container *c, const char *root_dir);

/* Read config.json:
  - fills c->command
  - applies env via setenv()
  - fills cpu/memory
  - fills mounts[]
*/
int read_config_json(struct container *c, const char *root_dir);

#endif
