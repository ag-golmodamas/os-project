#ifndef __SERVICE_H__
#define __SERVICE_H__

#include <stddef.h>
#include <stdint.h>

/*
  NEW:
  - memory_limit_bytes: bytes, 0 => don't set MemoryMax
  - cpu_shares: OCI shares, 0 => don't set CPUWeight
*/
int write_systemd_service(const char *uuid,
                          const char *name,
                          int64_t memory_limit_bytes,
                          int cpu_shares);

int systemd_daemon_reload(void);
int systemd_start(const char *uuid);
int systemd_stop(const char *uuid);
int systemd_restart(const char *uuid);

int systemd_is_active(const char *uuid);

#endif
