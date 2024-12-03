#define _FILE_OFFSET_BITS 64
#define FUSE_USE_VERSION 30

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wfs.h"

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

int main(int argc, char *argv[]) {
    if (argc <= 1) {
        return -1;
    }

    /*int i;*/
    /*char **disks = calloc(MIN_DISKS, sizeof(char*));*/
    /*int ndisks = MIN_DISKS;*/
    /*int dcnt = 0;*/
    /*char *endptr, *str;*/
    /**/
    /*// ./wfs disk1 disk2 [FUSE options] mount_point*/
    /*for (i = 1; i < argc - 1; i++) {*/
    /*    i += 1;*/
    /*}*/

    // Initialize FUSE with specified operations
    // Filter argc and argv here and then pass it to fuse_main
    return fuse_main(argc, argv, &ops, NULL);
}
