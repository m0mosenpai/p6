#define _FILE_OFFSET_BITS 64
#define FUSE_USE_VERSION 30

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include "wfs.h"

void **disk_ptrs;

void freev(void **ptr, int len, int free_seg) {
    if (len < 0) while (*ptr) { free(*ptr); *ptr++ = NULL; }
    else { for (int i = 0; i < len; i++) free(ptr[i]); }
    if (free_seg) free(ptr);
}

void* mapdisk(int dd) {
    struct stat st;
    if (fstat(dd, &st) < 0) {
        return 0;
    }
    void *map_ptr = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, dd, 0);
    if (map_ptr == MAP_FAILED) {
        return 0;
    }
    return map_ptr;
}

int validatedisk(struct wfs_sb sb) {
    for (int j = 0; j < sb.num_disks; j++) {
        if (strcmp(sb.id, sb.disks[j]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int wfs_getattr(const char *path, struct stat *stbuf) {
    return 0;
}

static int wfs_mknod(const char *path, mode_t mode, dev_t rdev) {
    return 0;
}

static int wfs_mkdir(const char *path, mode_t mode) {
    return 0;
}

static int wfs_unlink(const char *path) {
    return 0;
}

static int wfs_rmdir(const char *path) {
    return 0;
}

static int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    return 0;
}

static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    return 0;
}

static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi) {
    return 0;
}

static struct fuse_operations ops = {
  .getattr = wfs_getattr,
  .mknod   = wfs_mknod,
  .mkdir   = wfs_mkdir,
  .unlink  = wfs_unlink,
  .rmdir   = wfs_rmdir,
  .read    = wfs_read,
  .write   = wfs_write,
  .readdir = wfs_readdir,
};

// ./wfs disk1 disk2 [FUSE options] mount_point
int main(int argc, char *argv[]) {
    if (argc <= 2) {
        return -1;
    }

    int i;
    int dcnt = 0;
    struct wfs_sb sb;
    void *disk_ptr = NULL;

    // get first disk
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        return -1;
    }

    if ((disk_ptr = mapdisk(fd)) == NULL) {
        close(fd);
        return -1;
    }
    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    if (!validatedisk(sb)) {
        close(fd);
        return -1;
    }
    disk_ptrs = malloc(sb.num_disks * sizeof(void*));
    disk_ptrs[0] = disk_ptr;
    dcnt++;

    int ndisks = MIN_DISKS;
    char **disks = malloc(ndisks * sizeof(char*));
    disks[0] = malloc(strlen(argv[1]) + 1);
    strcpy(disks[0], argv[1]);

    // TO-DO: read disks in only one place
    // Works under the assumption "-s" or "-f" will always be present
    i = 2;
    char *delim = "-";
    while (i < argc && strncmp(argv[i], delim, strlen(delim)) != 0) {
        if (dcnt >= ndisks) {
            ndisks *= 2;
            disks = reallocarray(disks, ndisks, sizeof(char*));
        }
        disks[i-1] = malloc(strlen(argv[i]) + 1);
        strcpy(disks[i-1], argv[i]);
        dcnt++;
        i++;
    }
    if (dcnt != sb.num_disks) {
        freev((void*)disks, ndisks, 1);
        return -1;
    }

    int fuse_argc = argc - dcnt - 1;
    if (fuse_argc == 0) {
        freev((void*)disks, ndisks, 1);
        return -1;
    }
    char **fuse_argv = malloc(fuse_argc * sizeof(char*));
    while (i < argc) {
        fuse_argv[i-dcnt-1] = malloc(strlen(argv[i]) + 1);
        strcpy(fuse_argv[i-dcnt-1], argv[i]);
        i++;
    }

    for (i = 0; i < sb.num_disks; i++) {
        int fd = open(disks[i], O_RDWR);
        if (fd < 0) {
            freev((void*)disks, ndisks, 1);
            freev((void*)fuse_argv, fuse_argc, 1);
            return -1;
        }
        if ((disk_ptr = mapdisk(fd)) == NULL) {
            close(fd);
            freev((void*)disks, ndisks, 1);
            freev((void*)fuse_argv, fuse_argc, 1);
            return -1;
        }
        memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
        if (!validatedisk(sb)) {
            close(fd);
            freev((void*)disks, ndisks, 1);
            freev((void*)fuse_argv, fuse_argc, 1);
            return -1;
        }
        disk_ptrs[i-1] = disk_ptr;
        close(fd);
    }

    umask(0);
    return fuse_main(fuse_argc, fuse_argv, &ops, NULL);
}
