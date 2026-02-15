#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include "service.h"

static void unit_name(const char *uuid, char *out, size_t out_sz) {
  snprintf(out, out_sz, "zocker-%s.service", uuid);
}

static int self_exe_path(char *out, size_t out_sz) {
  ssize_t n = readlink("/proc/self/exe", out, out_sz - 1);
  if (n < 0) return 1;
  out[n] = '\0';
  return 0;
}

/*
  OCI cpu.shares range is typically 2..262144.
  systemd CPUWeight is 1..10000.
  We map linearly, clamped.
*/
static int shares_to_cpu_weight(int shares) {
  if (shares <= 0) return 0;
  if (shares < 2) shares = 2;
  if (shares > 262144) shares = 262144;

  /* linear map: 2..262144 -> 1..10000 */
  long long num = (long long)(shares - 2) * 9999LL;
  long long den = 262144LL - 2LL;
  long long w = 1LL + (den ? (num / den) : 0LL);

  if (w < 1) w = 1;
  if (w > 10000) w = 10000;
  return (int)w;
}

int write_systemd_service(const char *uuid,
                          const char *name,
                          int64_t memory_limit_bytes,
                          int cpu_shares) {
  char unit[256];
  unit_name(uuid, unit, sizeof(unit));

  char path[PATH_MAX];
  snprintf(path, sizeof(path), "/etc/systemd/system/%s", unit);

  char exe[PATH_MAX];
  if (self_exe_path(exe, sizeof(exe)) != 0) {
    perror("readlink /proc/self/exe");
    return 1;
  }

  FILE *f = fopen(path, "w");
  if (!f) {
    perror("fopen service file (need sudo?)");
    return 1;
  }

  const char *disp = (name && name[0]) ? name : uuid;

  /* convert shares -> CPUWeight */
  int cpu_weight = shares_to_cpu_weight(cpu_shares);

  fprintf(f,
    "[Unit]\n"
    "Description=Zocker container %s (%s)\n"
    "After=network.target\n"
    "\n"
    "[Service]\n"
    "Type=simple\n"
    "ExecStart=%s start %s\n"
    "ExecStop=/bin/kill -TERM $MAINPID\n"
    "Restart=always\n"
    "RestartSec=1\n"
    "KillMode=control-group\n",
    disp, uuid, exe, uuid
  );

  /*
    These are the cgroup limits:
    - MemoryMax applies memory limit to the service cgroup
    - CPUWeight controls CPU scheduling weight in cgroup
  */
  if (memory_limit_bytes > 0) {
    fprintf(f, "MemoryMax=%lld\n", (long long)memory_limit_bytes);
  }
  if (cpu_weight > 0) {
    fprintf(f, "CPUWeight=%d\n", cpu_weight);
  }

  fprintf(f,
    "\n"
    "[Install]\n"
    "WantedBy=multi-user.target\n"
  );

  fclose(f);
  return 0;
}

static int run_cmd(const char *cmd) {
  int rc = system(cmd);
  return (rc == 0) ? 0 : 1;
}

int systemd_daemon_reload(void) { return run_cmd("systemctl daemon-reload"); }

int systemd_start(const char *uuid) {
  char unit[256], cmd[512];
  unit_name(uuid, unit, sizeof(unit));
  snprintf(cmd, sizeof(cmd), "systemctl start %s", unit);
  return run_cmd(cmd);
}

int systemd_stop(const char *uuid) {
  char unit[256], cmd[512];
  unit_name(uuid, unit, sizeof(unit));
  snprintf(cmd, sizeof(cmd), "systemctl stop %s", unit);
  return run_cmd(cmd);
}

int systemd_restart(const char *uuid) {
  char unit[256], cmd[512];
  unit_name(uuid, unit, sizeof(unit));
  snprintf(cmd, sizeof(cmd), "systemctl restart %s", unit);
  return run_cmd(cmd);
}

int systemd_is_active(const char *uuid) {
  char unit[256], cmd[512];
  unit_name(uuid, unit, sizeof(unit));
  snprintf(cmd, sizeof(cmd), "systemctl is-active --quiet %s", unit);
  return (system(cmd) == 0) ? 1 : 0;
}
