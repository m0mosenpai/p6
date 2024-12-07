#define _FILE_OFFSET_BITS 64
#define FUSE_USE_VERSION 30

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include "wfs.h"

void **disk_ptrs;
int total_disks;
DiskMode raid;

void freev(void **ptr, int len, int free_seg) {
    if (len < 0) while (*ptr) { free(*ptr); *ptr++ = NULL; }
    else { for (int i = 0; i < len; i++) free(ptr[i]); }
    if (free_seg) free(ptr);
}

int roundup(int n, int k) {
    int r = n % k;
    if (r == 0) return n;
    return n + k - r;
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

int validatepath(const char* path, void *disk_ptr) {
    struct wfs_sb sb;
    struct wfs_inode inode;
    struct wfs_dentry dentry;
    off_t i_blocks_ptr;
    off_t d_blocks_ptr;
    off_t blocks[N_BLOCKS];
    int inum;
    int found;
    int i;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    i_blocks_ptr = (off_t)disk_ptr + sb.i_blocks_ptr;
    d_blocks_ptr = (off_t)disk_ptr + sb.d_blocks_ptr;

    char *delim = "/";
    char *path_cpy = strdup(path);
    char *tok = strtok(path_cpy, delim);
    int dentries = BLOCK_SIZE / sizeof(struct wfs_dentry);
    inum = 0;

    while (tok != NULL) {
        memcpy(&inode, (void*)(i_blocks_ptr + (inum * BLOCK_SIZE)), sizeof(struct wfs_inode));
        memcpy(blocks, inode.blocks, sizeof(inode.blocks));
        inode.atim = time(NULL);
        found = 0;
        while (i < N_BLOCKS) {
            if (blocks[i] == inum) {
                for (off_t ptr = d_blocks_ptr + (inum * BLOCK_SIZE); ptr < dentries; ptr += sizeof(struct wfs_dentry)) {
                    memcpy(&dentry, (void*)ptr, sizeof(struct wfs_dentry));
                    if (strcmp(dentry.name, tok) == 0) {
                        inum = dentry.num;
                        found = 1;
                        break;
                    }
                }
                if (found) {
                    break;
                }
            }
            i++;
        }
        if (!found) {
            return -1;
        }
        tok = strtok(NULL, delim);
    }
    return inum;
}

struct wfs_inode *alloc_inode(void *disk_ptr, mode_t mode) {
    struct wfs_sb sb;
    off_t i_bitmap_ptr;
    off_t i_blocks_ptr;
    int inodesize;
    int free_i;
    time_t ctime;
    struct wfs_inode *new_inode;
    int i;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    inodesize = roundup(sb.num_inodes, 8) / 8;
    unsigned char inodebitmap[inodesize];
    i_bitmap_ptr = sb.i_bitmap_ptr;
    memcpy(&inodebitmap, (void*)i_bitmap_ptr, inodesize);

    i = 0;
    while (i < inodesize && inodebitmap[i] == 1) {
        i++;
    }
    if (i >= inodesize) {
        return 0;
    }
    free_i = i;

    i = 0;
    i_blocks_ptr = sb.i_blocks_ptr;
    while (i != free_i) {
        i_blocks_ptr += BLOCK_SIZE;
        i++;
    }
    new_inode = (struct wfs_inode*)i_blocks_ptr;

    ctime = time(NULL);
    new_inode->num = free_i;
    new_inode->mode = mode;
    new_inode->uid = getuid();
    new_inode->gid = getgid();
    new_inode->size = 0;
    new_inode->nlinks = 1;
    new_inode->atim = ctime;
    new_inode->mtim = ctime;
    new_inode->ctim = ctime;
    memset(new_inode->blocks, 0, N_BLOCKS*(sizeof(off_t)));
    inodebitmap[free_i] = 1;

    return new_inode;
}

static int wfs_getattr(const char *path, struct stat *stbuf) {
    int i;
    void *disk;
    struct wfs_sb sb;
    struct wfs_inode inode;
    off_t i_blocks_ptr;
    struct timespec tim;
    int inum;

    memset(stbuf, 0, sizeof(struct stat));

    // TO-DO: round-robin read (striped)
    if (raid == RAID_0) {
        for (i = 0; i < total_disks; i++) {
            disk = disk_ptrs[0];
            memcpy(&sb, disk, sizeof(struct wfs_sb));
        }
    }
    // read from any disk (mirrored)
    else if (raid == RAID_1) {
        void *disk_ptr = disk_ptrs[0];
        if ((inum = validatepath(path, disk_ptr)) == -1) {
            return -ENOENT;
        }
        memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
        i_blocks_ptr = (off_t)disk_ptr + sb.i_blocks_ptr;
        memcpy(&inode, (void*)(i_blocks_ptr + (inum * BLOCK_SIZE)), sizeof(struct wfs_inode));

        stbuf->st_uid = inode.uid;
        stbuf->st_gid = inode.gid;
        stbuf->st_mode = inode.mode;
        stbuf->st_size = inode.size;
        tim.tv_sec = inode.atim;
        stbuf->st_atim = tim;
        tim.tv_sec = inode.mtim;
        stbuf->st_mtim = tim;
    }
    // verified-mirroring ??
    else {

    }
    return 0;
}

static int wfs_mknod(const char *path, mode_t mode, dev_t rdev) {
    return 0;
}

// TO-DO: directory with same name exists or not
static int wfs_mkdir(const char *path, mode_t mode) {
    if (path == NULL || strlen(path) == 0) {
        return -ENOENT;
    }

    if (raid == RAID_0) {

    }
    else if (raid == RAID_1) {

    } else {

    }

    return 0;
}

// TO-DO: check if already linked or not
static int wfs_unlink(const char *path) {
    int res;

	res = unlink(path);
	if (res == -1)
		return -errno;

	return 0;
}

// TO-DO: check directory exists or not
static int wfs_rmdir(const char *path) {
    int res;

	res = rmdir(path);
	if (res == -1)
		return -errno;

	return 0;
}

static int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    int fd;
    int res;

	(void) fi;
    fd = open(path, O_RDONLY);
    if (fd == -1) {
        return -errno;
    }
    res = pread(fd, buf, size, offset);
    if (res == -1) {
        return -errno;
    }

    close(fd);
    return res;
}

static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    int fd;
    int res;

	(void) fi;
	fd = open(path, O_WRONLY);
	if (fd == -1)
		return -errno;

	res = pwrite(fd, buf, size, offset);
	if (res == -1)
		res = -errno;

	close(fd);
	return res;
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
    int ndisks = MIN_DISKS;
    char **disks = malloc(ndisks * sizeof(char*));

    // Works under the assumption "-s" or "-f" will always be present
    i = 1;
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

    for (i = 0; i < dcnt; i++) {
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
        if (i == 0) {
            total_disks = sb.num_disks;
            raid = sb.raid;
            disk_ptrs = malloc(total_disks * sizeof(void*));
            if (dcnt != total_disks) {
                close(fd);
                freev((void*)disks, ndisks, 1);
                freev((void*)fuse_argv, fuse_argc, 1);
                return -1;
            }
        }
        disk_ptrs[i] = disk_ptr;
        close(fd);
    }

    umask(0);
    return fuse_main(fuse_argc, fuse_argv, &ops, NULL);
}
