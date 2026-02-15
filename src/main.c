#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "run.h"
#include "setup.h"
#include "uuid.h"

static void usage(void) {
  fprintf(stderr,
          "Usage:\n"
          "  zocker run [--name NAME] <command>\n"
          "  zocker start <uuid>\n"
          "  zocker stop <uuid>\n"
          "  zocker restart <uuid>\n"
          "  zocker exec <uuid> <command>\n"
          "  zocker ps [-a]\n"
          "  zocker rm [-f] <uuid>\n");
}


int main(int argc, char **argv) {
  if (init_zocker_prefix() != 0) {
    fprintf(stderr, "[ERR] Failed to initialize storage\n");
    return 1;
  }
  if (setup_zocker_dir() != 0) return 1;

  if (argc < 2) {
    usage();
    return 1;
  }

  /* -------- run -------- */
  if (strcmp(argv[1], "run") == 0) {
    struct config cfg = {
      .subcommand = RUN,
      .name = "",
      .command = "",
      .base_dir = "",
      .base_image = ""
    };

    int i = 2;
    while (i < argc) {
      if (strcmp(argv[i], "--name") == 0) {
        if (i + 1 >= argc) { fprintf(stderr, "[ERR] Missing --name value\n"); return 1; }
        strncpy(cfg.name, argv[i + 1], sizeof(cfg.name) - 1);
        i += 2;
      } else {
        /* rest is the command (single string) */
        strncpy(cfg.command, argv[i], sizeof(cfg.command) - 1);
        i++;
      }
    }

    /* Option B typically: uuid is generated even if name exists (handled inside your run.c) */
    if (validate_config(cfg) != 0) return 1;

    struct container cont = {0};
    container_from_config(cfg, &cont);
    return zocker_run(cont);
  }

  /* -------- start/stop/restart -------- */
  if (strcmp(argv[1], "start") == 0) {
    if (argc < 3) { usage(); return 1; }
    return zocker_start(argv[2]);
  }

  if (strcmp(argv[1], "stop") == 0) {
    if (argc < 3) { usage(); return 1; }
    return zocker_stop(argv[2]);
  }

  if (strcmp(argv[1], "restart") == 0) {
    if (argc < 3) { usage(); return 1; }
    return zocker_restart(argv[2]);
  }

  /* -------- exec -------- */
  if (strcmp(argv[1], "exec") == 0) {
    if (argc < 4) { usage(); return 1; }

    const char *ref = argv[2];

    /* join argv[3..] into one command string */
    char cmd[512] = {0};
    for (int i = 3; i < argc; i++) {
      if (i > 3) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
      strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
    }

    return zocker_exec(ref, cmd);
  }

  /* -------- ps -------- */
  if (strcmp(argv[1], "ps") == 0) {
    int all = 0;
    if (argc >= 3 && strcmp(argv[2], "-a") == 0) all = 1;
    return zocker_ps(all);
  }

  /* -------- rm -------- */
  if (strcmp(argv[1], "rm") == 0) {
    int force = 0;
    const char *id = NULL;

    if (argc >= 3 && strcmp(argv[2], "-f") == 0) {
      force = 1;
      if (argc < 4) { usage(); return 1; }
      id = argv[3];
    } else {
      if (argc < 3) { usage(); return 1; }
      id = argv[2];
    }

    return zocker_rm(id, force);
  }

  usage();
  return 1;
}

