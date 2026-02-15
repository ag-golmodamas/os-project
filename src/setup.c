#define _GNU_SOURCE
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "setup.h"

/* Runtime prefix: ~/.zocker/containers */
char ZOCKER_PREFIX[MAX_PATH];

static int mkdir_p(const char *path, mode_t mode) {
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

/*
  IMPORTANT FIX:
  - Previously used: cp -a (preserves symlinks)
  - Many libs on Ubuntu are symlinks (e.g., libpcre2-8.so.0 -> libpcre2-8.so.0.11.x)
  - That caused container to contain the symlink but not the real target file -> runtime loader failure.
  - Now we use: cp -aL (dereference symlinks), so the destination is a real file.
*/
static int copy_file(const char *src, const char *dst) {
  char dst_copy[MAX_PATH];
  strncpy(dst_copy, dst, sizeof(dst_copy) - 1);
  dst_copy[sizeof(dst_copy) - 1] = '\0';

  char *dir = dirname(dst_copy);
  if (mkdir_p(dir, 0755) != 0) {
    fprintf(stderr, "[ERR] mkdir_p(%s): %s\n", dir, strerror(errno));
    return 1;
  }

  /* -a preserves perms/timestamps; -L dereferences symlinks (copy the real file) */
  char cmd[MAX_PATH * 2];
  snprintf(cmd, sizeof(cmd), "cp -aL %s %s", src, dst);
  int rc = system(cmd);
  return (rc == 0) ? 0 : 1;
}

static int copy_bin_into_root(const char *root_dir, const char *src_bin,
                              const char *name) {
  char dst[MAX_PATH];
  snprintf(dst, sizeof(dst), "%s/bin/%s", root_dir, name);
  return copy_file(src_bin, dst);
}

/* Copy shared libs needed by a binary into the rootfs using ldd output */
static int copy_ldd_deps_into_root(const char *root_dir,
                                   const char *host_bin_path) {
  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "ldd %s", host_bin_path);

  FILE *fp = popen(cmd, "r");
  if (!fp) {
    perror("popen ldd");
    return 1;
  }

  char line[2048];
  while (fgets(line, sizeof(line), fp)) {
    char *path = NULL;

    char *arrow = strstr(line, "=>");
    if (arrow) {
      path = arrow + 2;
      while (*path == ' ' || *path == '\t') path++;
    } else {
      path = line;
      while (*path == ' ' || *path == '\t') path++;
    }

    /* skip lines that don't contain an absolute path */
    if (*path != '/') continue;

    char abs[PATH_MAX];
    size_t i = 0;
    while (path[i] && path[i] != ' ' && path[i] != '\t' && path[i] != '\n') {
      if (i + 1 < sizeof(abs)) abs[i] = path[i];
      i++;
    }
    if (i == 0) continue;
    if (i >= sizeof(abs)) i = sizeof(abs) - 1;
    abs[i] = '\0';

    char dst[MAX_PATH];
    snprintf(dst, sizeof(dst), "%s%s", root_dir, abs);

    if (copy_file(abs, dst) != 0) {
      /* don't hard fail: some lines like linux-vdso are not real files */
      fprintf(stderr, "[WARN] failed to copy dep %s\n", abs);
    }
  }

  pclose(fp);
  return 0;
}

/* Choice 1: prefix based ONLY on current uid */
int init_zocker_prefix(void) {
  struct passwd *pw = getpwuid(getuid());
  if (!pw) {
    perror("getpwuid");
    return 1;
  }

  const char *home = pw->pw_dir;
  char base_dir[MAX_PATH];

  snprintf(base_dir, sizeof(base_dir), "%s/.zocker", home);
  snprintf(ZOCKER_PREFIX, sizeof(ZOCKER_PREFIX), "%s/containers", base_dir);

  if (mkdir_p(base_dir, 0755) != 0 && errno != EEXIST) {
    perror("mkdir_p base_dir");
    return 1;
  }
  if (mkdir_p(ZOCKER_PREFIX, 0755) != 0 && errno != EEXIST) {
    perror("mkdir_p containers");
    return 1;
  }

  return 0;
}

int setup_zocker_dir(void) {
  struct stat st;
  if (stat(ZOCKER_PREFIX, &st) == -1) {
    perror("stat ZOCKER_PREFIX");
    return 1;
  }
  if (!S_ISDIR(st.st_mode)) {
    fprintf(stderr, "[ERR] ZOCKER_PREFIX %s is not a directory\n", ZOCKER_PREFIX);
    return 1;
  }
  return 0;
}

int setup_container_dir(const char *id, const char *base_image,
                        char root_dir[MAX_PATH]) {
  (void)base_image; /* not used yet in Part 1 minimal rootfs mode */

  struct stat st;
  snprintf(root_dir, MAX_PATH, "%s/%s", ZOCKER_PREFIX, id);

  if (stat(root_dir, &st) == -1) {
    if (errno != ENOENT) {
      perror("stat container dir");
      return 1;
    }
    if (mkdir(root_dir, 0755) != 0) {
      perror("mkdir container dir");
      return 1;
    }
  } else if (!S_ISDIR(st.st_mode)) {
    fprintf(stderr, "[ERR] %s is not a directory\n", root_dir);
    return 1;
  }

  /* Make base dirs */
  char bin_dir[MAX_PATH], lib_dir[MAX_PATH], lib64_dir[MAX_PATH];
  snprintf(bin_dir, sizeof(bin_dir), "%s/bin", root_dir);
  snprintf(lib_dir, sizeof(lib_dir), "%s/lib", root_dir);
  snprintf(lib64_dir, sizeof(lib64_dir), "%s/lib64", root_dir);

  if (mkdir_p(bin_dir, 0755) != 0) return 1;
  if (mkdir_p(lib_dir, 0755) != 0) return 1;
  if (mkdir_p(lib64_dir, 0755) != 0) return 1;

  /*
    IMPORTANT: /usr/bin/sh is often a symlink to dash.
    If we cp -a it, it stays a symlink and breaks unless dash exists too.
    So we copy dash as the real shell, then create /bin/sh -> dash.
  */
  const char *dash_src = NULL;
  if (access("/bin/dash", X_OK) == 0) dash_src = "/bin/dash";
  else if (access("/usr/bin/dash", X_OK) == 0) dash_src = "/usr/bin/dash";

  if (!dash_src) {
    fprintf(stderr, "[ERR] dash not found on host (expected /bin/dash)\n");
    return 1;
  }

  if (copy_bin_into_root(root_dir, dash_src, "dash") != 0) {
    fprintf(stderr, "[ERR] Failed to copy dash\n");
    return 1;
  }

  /* create /bin/sh -> dash */
  {
    char sh_path[MAX_PATH];
    snprintf(sh_path, sizeof(sh_path), "%s/bin/sh", root_dir);
    unlink(sh_path);
    if (symlink("dash", sh_path) != 0) {
      perror("symlink sh->dash");
      return 1;
    }
  }

  /* Other binaries */
  if (copy_bin_into_root(root_dir, "/usr/bin/ls", "ls") != 0) return 1;
  if (copy_bin_into_root(root_dir, "/usr/bin/env", "env") != 0) return 1;
  if (copy_bin_into_root(root_dir, "/usr/bin/sleep", "sleep") != 0) return 1;
  if (copy_bin_into_root(root_dir, "/usr/bin/head", "head") != 0) return 1;
copy_ldd_deps_into_root(root_dir, "/usr/bin/head");
  if (copy_bin_into_root(root_dir, "/usr/bin/cat", "cat") != 0) return 1;


  
  /* Copy deps */
  copy_ldd_deps_into_root(root_dir, dash_src);
  copy_ldd_deps_into_root(root_dir, "/usr/bin/ls");
  copy_ldd_deps_into_root(root_dir, "/usr/bin/env");
  copy_ldd_deps_into_root(root_dir, "/usr/bin/sleep");
  copy_ldd_deps_into_root(root_dir, "/usr/bin/cat");

  /* Ensure dynamic loader exists at the same path */
  {
    const char *ld_real = "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2";
    char dst_ld[MAX_PATH];
    snprintf(dst_ld, sizeof(dst_ld), "%s%s", root_dir, ld_real);
    if (access(ld_real, R_OK) == 0) {
      copy_file(ld_real, dst_ld);
    }
  }

  /* Ensure libc exists */
  {
    const char *libc_real = "/lib/x86_64-linux-gnu/libc.so.6";
    char dst_libc[MAX_PATH];
    snprintf(dst_libc, sizeof(dst_libc), "%s%s", root_dir, libc_real);
    if (access(libc_real, R_OK) == 0) {
      copy_file(libc_real, dst_libc);
    }
  }

  return 0;
}
