#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>

#include "config_json.h"
#include "fsutil.h"
#include "run.h"
#include "service.h"
#include "setup.h"
#include "state_json.h"
#include "uuid.h"

/* ----------- stop handling globals ----------- */
static volatile sig_atomic_t g_stop_requested = 0;
static pid_t g_child_pid = -1;
static char g_root_dir[MAX_PATH];
static char g_cont_name[64];   /* annotations.name (human name) */
static char g_cont_uuid[64];   /* state.json id + unit uuid */

static void on_sigterm(int signo) {
 (void)signo;
 g_stop_requested = 1;
 if (g_child_pid > 0) {
   kill(g_child_pid, SIGTERM);
 }
}

static void strip_trailing_slash(char *p) {
 if (!p) return;
 size_t n = strlen(p);
 while (n > 0 && p[n - 1] == '/') {
   p[n - 1] = '\0';
   n--;
 }
}

static void format_duration(time_t secs, char out[32]) {
 if (secs < 0) secs = 0;
 long s = (long)secs;
 long h = s / 3600;
 long m = (s % 3600) / 60;
 long r = s % 60;
 snprintf(out, 32, "%02ld:%02ld:%02ld", h, m, r);
}

/* ---------- UUID-only helpers ---------- */

static int is_hex(char c) {
 return (c >= '0' && c <= '9') ||
        (c >= 'a' && c <= 'f') ||
        (c >= 'A' && c <= 'F');
}

/* Strict UUID format: 8-4-4-4-12 (36 chars) */
static int looks_like_uuid(const char *s) {
 if (!s) return 0;
 if (strlen(s) != 36) return 0;

 for (int i = 0; i < 36; i++) {
   if (i == 8 || i == 13 || i == 18 || i == 23) {
     if (s[i] != '-') return 0;
   } else {
     if (!is_hex(s[i])) return 0;
   }
 }
 return 1;
}

/*
Resolve UUID -> bundle path + name (from state.json if exists)
This does NOT scan by name. It is 1-to-1 and safe.
*/
static int resolve_uuid_bundle(const char *uuid,
                             char out_bundle[MAX_PATH],
                             char out_name[64]) {
 if (!looks_like_uuid(uuid)) return 1;

 snprintf(out_bundle, MAX_PATH, "%s/%s", ZOCKER_PREFIX, uuid);

 struct stat st;
 if (stat(out_bundle, &st) != 0) return 1;
 if (!S_ISDIR(st.st_mode)) return 1;

 out_name[0] = '\0';

 struct zocker_state s = {0};
 if (read_state_json(out_bundle, &s) == 0) {
   if (s.id[0] && strcmp(s.id, uuid) != 0) return 1;
   if (s.name[0]) {
     strncpy(out_name, s.name, 63);
     out_name[63] = '\0';
   }
 }
 return 0;
}

/* ---------- mkdir -p helper (for mount destinations) ---------- */
static int mkdir_p_path(const char *path, mode_t mode) {
 if (!path || path[0] == '\0') return 1;

 char tmp[MAX_PATH];
 strncpy(tmp, path, sizeof(tmp) - 1);
 tmp[sizeof(tmp) - 1] = '\0';

 size_t len = strlen(tmp);
 if (len == 0) return 0;
 if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

 for (char *p = tmp + 1; *p; p++) {
   if (*p == '/') {
     *p = '\0';
     if (mkdir(tmp, mode) != 0 && errno != EEXIST) return 1;
     *p = '/';
   }
 }
 if (mkdir(tmp, mode) != 0 && errno != EEXIST) return 1;
 return 0;
}

/* Apply mounts from config.json BEFORE chroot:
  - dest is rootfs + destination
  - proc: mount("proc", dest, "proc", 0, NULL)
  - bind: mount(source, dest, NULL, MS_BIND|MS_REC, NULL)
*/
static int apply_mounts_pre_chroot(const char *rootfs, const struct container *c) {
 if (!rootfs || !c) return 1;

 for (int i = 0; i < c->mount_count; i++) {
   const struct mount_entry *m = &c->mounts[i];

   if (m->destination[0] != '/') {
     fprintf(stderr, "[ERR] mount destination must be absolute: %s\n", m->destination);
     return 1;
   }

   char dest[MAX_PATH];
   snprintf(dest, sizeof(dest), "%s%s", rootfs, m->destination);

   if (mkdir_p_path(dest, 0755) != 0) {
     fprintf(stderr, "[ERR] mkdir_p for mount destination failed: %s (%s)\n", dest, strerror(errno));
     return 1;
   }

   if (strcmp(m->type, "proc") == 0) {
     /* source is usually "proc" */
     if (mount("proc", dest, "proc", 0, NULL) != 0) {
       fprintf(stderr, "[ERR] mount proc -> %s failed: %s\n", dest, strerror(errno));
       return 1;
     }
     continue;
   }

   if (strcmp(m->type, "bind") == 0) {
     if (m->source[0] != '/') {
       fprintf(stderr, "[ERR] bind mount source must be absolute host path: %s\n", m->source);
       return 1;
     }

     /* MS_REC helps bind mount directories with nested mounts */
     if (mount(m->source, dest, NULL, MS_BIND | MS_REC, NULL) != 0) {
       fprintf(stderr, "[ERR] bind mount %s -> %s failed: %s\n", m->source, dest, strerror(errno));
       return 1;
     }
     continue;
   }

   /* Unknown mount types can be ignored or error; assignment-wise better to warn+ignore */
   fprintf(stderr, "[WARN] unsupported mount type '%s' (ignored)\n", m->type);
 }

 return 0;
}

void container_from_config(struct config cfg, struct container *c) {
 strncpy(c->id, cfg.name, sizeof(c->id) - 1);
 c->id[sizeof(c->id) - 1] = '\0';

 strncpy(c->command, cfg.command, sizeof(c->command) - 1);
 c->command[sizeof(c->command) - 1] = '\0';

 strncpy(c->base_dir, cfg.base_dir, sizeof(c->base_dir) - 1);
 c->base_dir[sizeof(c->base_dir) - 1] = '\0';

 strncpy(c->base_image, cfg.base_image, sizeof(c->base_image) - 1);
 c->base_image[sizeof(c->base_image) - 1] = '\0';

 c->memory_limit_bytes = 0;
 c->cpu_shares = 0;
 c->mount_count = 0;
}

/* OPTION B: UUID bundle + UUID unit */
int zocker_run(struct container cont) {
 if (cont.id[0] == '\0') {
   fprintf(stderr, "[ERR] Missing container name\n");
   return 1;
 }

 char uuid[37] = {0};
 if (generate_uuid(uuid) != 0) {
   fprintf(stderr, "[ERR] Failed to generate UUID\n");
   return 1;
 }

 printf("[INFO] Container name: %s\n", cont.id);
 printf("[INFO] Container id:   %s\n", uuid);

 char root_dir[MAX_PATH] = {0};

 if (setup_container_dir(uuid, cont.base_image, root_dir) != 0) {
   fprintf(stderr, "[ERR] setup_container_dir failed\n");
   return 1;
 }

 if (write_config_json(&cont, root_dir) != 0) {
   fprintf(stderr, "[ERR] Failed to write config.json\n");
   return 1;
 }

 time_t now = time(NULL);
 if (write_state_json_ex(uuid, cont.id, root_dir, 0, "created",
                         -1, now, 0, 0) != 0) {
   fprintf(stderr, "[ERR] Failed to write state.json\n");
   return 1;
 }

 struct container cfg2 = {0};
 if (read_config_json(&cfg2, root_dir) != 0) {
   fprintf(stderr, "[ERR] Failed to read back config.json for resources\n");
   return 1;
 }

 if (write_systemd_service(uuid, cont.id, cfg2.memory_limit_bytes, cfg2.cpu_shares) != 0) {
   fprintf(stderr, "[ERR] Failed to write systemd unit (need sudo?)\n");
   return 1;
 }

 if (systemd_daemon_reload() != 0) {
   fprintf(stderr, "[ERR] systemctl daemon-reload failed\n");
   return 1;
 }

 if (systemd_start(uuid) != 0) {
   fprintf(stderr, "[ERR] systemctl start failed\n");
   return 1;
 }

 printf("[OK] Started container %s (%s) via systemd\n", cont.id, uuid);
 return 0;
}

/* zocker_start UUID-only */
int zocker_start(const char *ref) {
 if (!ref || ref[0] == '\0') {
   fprintf(stderr, "[ERR] start: missing UUID\n");
   return 1;
 }
 if (!looks_like_uuid(ref)) {
   fprintf(stderr, "[ERR] start expects a UUID. Use: zocker ps -a to find IDs.\n");
   return 1;
 }

 struct sigaction sa;
 memset(&sa, 0, sizeof(sa));
 sa.sa_handler = on_sigterm;
 sigemptyset(&sa.sa_mask);
 sa.sa_flags = 0;
 if (sigaction(SIGTERM, &sa, NULL) != 0) {
   perror("sigaction(SIGTERM)");
   return 1;
 }

 char uuid[64] = {0};
 strncpy(uuid, ref, sizeof(uuid) - 1);
 uuid[sizeof(uuid) - 1] = '\0';

 char bundle[MAX_PATH] = {0};
 char name[64] = {0};

 if (resolve_uuid_bundle(uuid, bundle, name) != 0) {
   fprintf(stderr, "[ERR] start: bundle for UUID '%s' not found\n", uuid);
   return 1;
 }

 strncpy(g_cont_uuid, uuid, sizeof(g_cont_uuid) - 1);
 g_cont_uuid[sizeof(g_cont_uuid) - 1] = '\0';

 strncpy(g_cont_name, name[0] ? name : "-", sizeof(g_cont_name) - 1);
 g_cont_name[sizeof(g_cont_name) - 1] = '\0';

 strncpy(g_root_dir, bundle, sizeof(g_root_dir) - 1);
 g_root_dir[sizeof(g_root_dir) - 1] = '\0';

 /* Read config.json:
    - sets cont.command
    - applies env
    - fills mounts[]
  */
 struct container cont = {0};
 strncpy(cont.id, name[0] ? name : uuid, sizeof(cont.id) - 1);
 cont.id[sizeof(cont.id) - 1] = '\0';

 if (read_config_json(&cont, g_root_dir) != 0) {
   fprintf(stderr, "[ERR] Failed to read config.json for %s\n", uuid);
   return 1;
 }

 if (unshare(CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWTIME) != 0) {
   perror("unshare");
   return 1;
 }

 pid_t pid = fork();
 if (pid < 0) {
   perror("fork");
   return 1;
 }

 if (pid == 0) {
   /* Child */
   if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
     perror("mount private");
     _exit(1);
   }

   /* NEW: apply mounts BEFORE chroot so bind sources can be host paths */
   if (apply_mounts_pre_chroot(g_root_dir, &cont) != 0) {
     fprintf(stderr, "[ERR] Failed to apply mounts\n");
     _exit(1);
   }

   if (chroot(g_root_dir) != 0) {
     perror("chroot");
     _exit(1);
   }

   if (chdir("/") != 0) {
     perror("chdir");
     _exit(1);
   }

   const char *hn = name[0] ? name : uuid;
   if (sethostname(hn, 64) != 0) {
     perror("sethostname");
     _exit(1);
   }

   execl("/bin/sh", "sh", "-c", cont.command, NULL);
   perror("execl");
   _exit(1);
 }

 /* Parent */
 g_child_pid = pid;

 struct zocker_state prev = {0};
 time_t created_at = 0;
 if (read_state_json(g_root_dir, &prev) == 0) {
   created_at = prev.created_at;
 }
 if (created_at == 0) created_at = time(NULL);

 time_t started_at = time(NULL);

 if (write_state_json_ex(uuid, name, g_root_dir, pid, "running",
                         -1, created_at, started_at, 0) != 0) {
   fprintf(stderr, "[ERR] Failed to write running state\n");
   return 1;
 }

 int status = 0;
 while (1) {
   pid_t w = waitpid(pid, &status, 0);
   if (w == pid) break;
   if (w < 0 && errno == EINTR) continue;
   perror("waitpid");
   break;
 }

 int exit_code = -1;
 if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
 else if (WIFSIGNALED(status)) exit_code = 128 + WTERMSIG(status);

 time_t stopped_at = time(NULL);

 if (write_state_json_ex(uuid, name, g_root_dir, 0, "stopped",
                         exit_code, created_at, started_at, stopped_at) != 0) {
   fprintf(stderr, "[ERR] Failed to write stopped state\n");
   return 1;
 }

 if (g_stop_requested) return 0;

 if (WIFEXITED(status) && WEXITSTATUS(status) != 0) return 1;
 if (WIFSIGNALED(status)) return 1;

 return 0;
}

/* UUID-only stop */
int zocker_stop(const char *ref) {
 if (!ref || ref[0] == '\0') {
   fprintf(stderr, "[ERR] stop: missing UUID\n");
   return 1;
 }
 if (!looks_like_uuid(ref)) {
   fprintf(stderr, "[ERR] stop expects a UUID (not a name). Use: zocker ps -a\n");
   return 1;
 }

 char bundle[MAX_PATH] = {0};
 char name[64] = {0};
 if (resolve_uuid_bundle(ref, bundle, name) != 0) {
   fprintf(stderr, "[ERR] stop: container UUID '%s' not found\n", ref);
   return 1;
 }

 if (systemd_stop(ref) != 0) {
   fprintf(stderr, "[ERR] systemctl stop failed\n");
   return 1;
 }
 return 0;
}

/* UUID-only restart */
int zocker_restart(const char *ref) {
 if (!ref || ref[0] == '\0') {
   fprintf(stderr, "[ERR] restart: missing UUID\n");
   return 1;
 }
 if (!looks_like_uuid(ref)) {
   fprintf(stderr, "[ERR] restart expects a UUID (not a name). Use: zocker ps -a\n");
   return 1;
 }

 char bundle[MAX_PATH] = {0};
 char name[64] = {0};
 if (resolve_uuid_bundle(ref, bundle, name) != 0) {
   fprintf(stderr, "[ERR] restart: container UUID '%s' not found\n", ref);
   return 1;
 }

 if (systemd_restart(ref) != 0) {
   fprintf(stderr, "[ERR] systemctl restart failed\n");
   return 1;
 }
 return 0;
}

/* ----- exec implementation (UUID-only) ----- */
int zocker_exec(const char *ref, const char *cmd) {
 if (!ref || ref[0] == '\0') {
   fprintf(stderr, "[ERR] exec: missing UUID\n");
   return 1;
 }
 if (!looks_like_uuid(ref)) {
   fprintf(stderr, "[ERR] exec expects a UUID (not a name). Use: zocker ps -a\n");
   return 1;
 }
 if (!cmd || cmd[0] == '\0') {
   fprintf(stderr, "[ERR] exec: missing command\n");
   return 1;
 }

 char bundle[MAX_PATH] = {0};
 char name[64] = {0};
 if (resolve_uuid_bundle(ref, bundle, name) != 0) {
   fprintf(stderr, "[ERR] exec: container UUID '%s' not found\n", ref);
   return 1;
 }

 /* Must be running: get PID from state.json */
 struct zocker_state s = {0};
 if (read_state_json(bundle, &s) != 0) {
   fprintf(stderr, "[ERR] exec: failed to read state.json for %s\n", ref);
   return 1;
 }

 if (strcmp(s.status, "running") != 0 || s.pid <= 0) {
   fprintf(stderr, "[ERR] exec: container is not running (uuid=%s)\n", ref);
   return 1;
 }

 /* Apply env from config.json */
 {
   struct container tmp = {0};
   if (read_config_json(&tmp, bundle) != 0) {
     fprintf(stderr, "[ERR] exec: failed to read config.json for %s\n", ref);
     return 1;
   }
 }

 char ns_mnt[128], ns_uts[128], ns_pid[128];
 snprintf(ns_mnt, sizeof(ns_mnt), "/proc/%d/ns/mnt", s.pid);
 snprintf(ns_uts, sizeof(ns_uts), "/proc/%d/ns/uts", s.pid);
 snprintf(ns_pid, sizeof(ns_pid), "/proc/%d/ns/pid", s.pid);

 int fd_mnt = open(ns_mnt, O_RDONLY);
 int fd_uts = open(ns_uts, O_RDONLY);
 int fd_pid = open(ns_pid, O_RDONLY);

 if (fd_mnt < 0 || fd_uts < 0 || fd_pid < 0) {
   perror("exec: open ns");
   if (fd_mnt >= 0) close(fd_mnt);
   if (fd_uts >= 0) close(fd_uts);
   if (fd_pid >= 0) close(fd_pid);
   return 1;
 }

 if (setns(fd_mnt, 0) != 0) { perror("exec: setns mnt"); goto fail; }
 if (setns(fd_uts, 0) != 0) { perror("exec: setns uts"); goto fail; }
 if (setns(fd_pid, 0) != 0) { perror("exec: setns pid"); goto fail; }

 close(fd_mnt);
 close(fd_uts);
 close(fd_pid);

 pid_t child = fork();
 if (child < 0) {
   perror("exec: fork");
   return 1;
 }

 if (child == 0) {
   char root[MAX_PATH];
   strncpy(root, bundle, sizeof(root) - 1);
   root[sizeof(root) - 1] = '\0';
   strip_trailing_slash(root);

   if (chroot(root) != 0) {
     perror("exec: chroot");
     _exit(1);
   }
   if (chdir("/") != 0) {
     perror("exec: chdir");
     _exit(1);
   }

   execl("/bin/sh", "sh", "-c", cmd, NULL);
   perror("exec: execl /bin/sh");
   _exit(1);
 }

 int st = 0;
 if (waitpid(child, &st, 0) < 0) {
   perror("exec: waitpid");
   return 1;
 }

 if (WIFEXITED(st)) return WEXITSTATUS(st);
 if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
 return 1;

fail:
 close(fd_mnt);
 close(fd_uts);
 close(fd_pid);
 return 1;
}

/*
ps:
- scans UUID bundle dirs only
- skips "names" directory and any non-UUID legacy directories
- active determined by systemd unit UUID
*/
int zocker_ps(int all) {
 DIR *d = opendir(ZOCKER_PREFIX);
 if (!d) {
   perror("opendir containers");
   return 1;
 }

 printf("%-16s %-36s %-10s %-10s %-5s\n",
        "NAME", "ID", "STATUS", "AGE/SINCE", "EXIT");

 struct dirent *de;
 while ((de = readdir(d)) != NULL) {
   if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
   if (strcmp(de->d_name, "names") == 0) continue;
   if (!looks_like_uuid(de->d_name)) continue;

   char bundle[MAX_PATH];
   snprintf(bundle, sizeof(bundle), "%s/%s", ZOCKER_PREFIX, de->d_name);

   struct stat st;
   if (stat(bundle, &st) != 0) continue;
   if (!S_ISDIR(st.st_mode)) continue;

   struct zocker_state s = {0};
   int have_state = (read_state_json(bundle, &s) == 0);

   char name[64] = "-";
   char id[64] = {0};

   strncpy(id, de->d_name, sizeof(id) - 1);
   id[sizeof(id) - 1] = '\0';

   if (have_state && s.name[0]) {
     strncpy(name, s.name, sizeof(name) - 1);
     name[sizeof(name) - 1] = '\0';
   }

   int active = systemd_is_active(id);
   if (!all && !active) continue;

   const char *status_str = active ? "running" : "stopped";

   time_t now = time(NULL);
   char dur[32] = "-";
   char exit_s[16] = "-";

   if (active) {
     time_t base = 0;
     if (have_state && s.started_at > 0) base = s.started_at;
     else if (have_state && s.created_at > 0) base = s.created_at;
     if (base > 0) format_duration(now - base, dur);
   } else {
     time_t base = 0;
     if (have_state && s.stopped_at > 0) base = s.stopped_at;
     if (base > 0) format_duration(now - base, dur);

     if (have_state && s.exit_code >= 0) {
       snprintf(exit_s, sizeof(exit_s), "%d", s.exit_code);
     }
   }

   printf("%-16s %-36s %-10s %-10s %-5s\n",
          name, id, status_str, dur, exit_s);
 }

 closedir(d);
 return 0;
}

/*
rm:
- UUID-only
- stops unit by UUID (if -f and active)
- removes unit file /etc/systemd/system/zocker-<uuid>.service
- removes bundle dir ZOCKER_PREFIX/<uuid>
*/
int zocker_rm(const char *ref, int force) {
 if (!ref || ref[0] == '\0') {
   fprintf(stderr, "[ERR] rm: missing UUID\n");
   return 1;
 }
 if (!looks_like_uuid(ref)) {
   fprintf(stderr, "[ERR] rm expects a UUID (not a name). Use: zocker ps -a\n");
   return 1;
 }

 char uuid[64] = {0};
 strncpy(uuid, ref, sizeof(uuid) - 1);
 uuid[sizeof(uuid) - 1] = '\0';

 char bundle[MAX_PATH] = {0};
 char name[64] = {0};

 if (resolve_uuid_bundle(uuid, bundle, name) != 0) {
   fprintf(stderr, "[ERR] rm: container UUID '%s' not found\n", uuid);
   return 1;
 }

 int active = systemd_is_active(uuid);

 if (active && !force) {
   fprintf(stderr, "[ERR] Container is running. Use -f to force.\n");
   return 1;
 }

 if (active && force) {
   if (systemd_stop(uuid) != 0) {
     fprintf(stderr, "[ERR] Failed to stop container\n");
     return 1;
   }
 }

 {
   char unit_path[512];
   snprintf(unit_path, sizeof(unit_path), "/etc/systemd/system/zocker-%s.service", uuid);
   unlink(unit_path);
   systemd_daemon_reload();
 }

 if (rm_rf(bundle) != 0) {
   fprintf(stderr, "[ERR] Failed to remove bundle %s\n", bundle);
   return 1;
 }

 printf("[OK] Removed container %s (%s)\n", name[0] ? name : "-", uuid);
 return 0;
}
