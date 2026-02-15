#ifndef __SETUP_H__
#define __SETUP_H__

#define MAX_PATH 4096

/* Runtime storage path: ~/.zocker/containers */
extern char ZOCKER_PREFIX[MAX_PATH];

/* Initialize ~/.zocker/containers */
int init_zocker_prefix(void);

/* Validate storage directory */
int setup_zocker_dir(void);

/*
 * Create container directory and rootfs
 *
 * id         -> container name
 * base_image -> base rootfs (may be empty)
 * root_dir   -> output container root dir
 */
int setup_container_dir(const char *id,
                        const char *base_image,
                        char root_dir[MAX_PATH]);

#endif
