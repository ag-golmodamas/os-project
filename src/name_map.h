#ifndef __NAME_MAP_H__
#define __NAME_MAP_H__

#include <stddef.h>

int name_map_set(const char *zocker_prefix, const char *name, const char *uuid);
int name_map_get(const char *zocker_prefix, const char *name, char *uuid_out, size_t uuid_out_sz);
int name_map_del(const char *zocker_prefix, const char *name);

#endif
