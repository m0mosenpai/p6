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
int dentries = BLOCK_SIZE / sizeof(struct wfs_dentry);

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

int validatepath(const char* path, mode_t mode, void *disk_ptr) {
    struct wfs_sb sb;
    struct wfs_inode inode;
    struct wfs_dentry dentry;
    off_t i_blocks_ptr;
    off_t d_blocks_ptr;
    off_t blocks[N_BLOCKS];
    int inum;
    int found;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    i_blocks_ptr = (off_t)disk_ptr + sb.i_blocks_ptr;
    d_blocks_ptr = (off_t)disk_ptr + sb.d_blocks_ptr;

    char *delim = "/";
    char *path_cpy = strdup(path);
    char *tok = strtok(path_cpy, delim);
    inum = 0;

    while (tok != NULL) {
        memcpy(&inode, (void*)(i_blocks_ptr + (inum * BLOCK_SIZE)), sizeof(struct wfs_inode));
        if (mode != 0 && (inode.mode & S_IFMT) != mode) {
            return -1;
        }
        memcpy(blocks, inode.blocks, sizeof(inode.blocks));
        inode.atim = time(NULL);
        found = 0;
        for (int i = 0; i < N_BLOCKS; i++) {
            if (blocks[i] == -1)
                continue;

            for (off_t ptr = d_blocks_ptr + (blocks[i] * BLOCK_SIZE); ptr < dentries; ptr += sizeof(struct wfs_dentry)) {
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
        if (!found) {
            return -1;
        }
        tok = strtok(NULL, delim);
    }
    return inum;
}

const char* getparentpath(const char *path) {
    char *parentpath = strdup(path);
    char *ptr;

    if ((ptr = strrchr(parentpath, '/')) == NULL) {
        return NULL;
    }
    if (&parentpath[0] == ptr) {
        return "/";
    }
    *ptr = '\0';
    return parentpath;
}

struct wfs_inode fetch_inode(int inum, void *disk_ptr) {
    struct wfs_sb sb;
    struct wfs_inode inode;
    off_t i_blocks_ptr;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    i_blocks_ptr = (off_t)disk_ptr + sb.i_blocks_ptr;
    memcpy(&inode, (void*)(i_blocks_ptr + (inum * BLOCK_SIZE)), sizeof(struct wfs_inode));
    return inode;
}

int data_exists(const char* dataname, mode_t mode, int inum, void *disk_ptr) {
    struct wfs_sb sb;
    struct wfs_inode inode;
    struct wfs_dentry dentry;
    off_t d_blocks_ptr;
    int i = 0;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    d_blocks_ptr = (off_t)disk_ptr + sb.d_blocks_ptr;
    inode = fetch_inode(inum, disk_ptr);
    if (inode.mode != mode) {
        return 0;
    }

    while (i < N_BLOCKS) {
        if (inode.blocks[i] == -1)
            continue;

        for (off_t ptr = d_blocks_ptr + (inode.blocks[i] * BLOCK_SIZE); ptr < dentries; ptr += sizeof(struct wfs_dentry)) {
            memcpy(&dentry, (void*)ptr, sizeof(struct wfs_dentry));
            if (strcmp(dentry.name, dataname) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

struct wfs_inode* alloc_inode(void *disk_ptr, mode_t mode) {
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
    i_bitmap_ptr = (off_t)disk_ptr + sb.i_bitmap_ptr;
    i_blocks_ptr = (off_t)disk_ptr + sb.i_blocks_ptr;
    memcpy(&inodebitmap, (void*)i_bitmap_ptr, inodesize);

    i = 0;
    while (i < inodesize && inodebitmap[i] == 1) {
        i++;
    }
    if (i >= inodesize) {
        return 0;
    }
    free_i = i;
    inodebitmap[free_i] = 1;
    memcpy((void*)i_bitmap_ptr, &inodebitmap, inodesize);

    i = 0;
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

    return new_inode;
}

struct wfs_dentry* alloc_datablock(void *disk_ptr) {
    struct wfs_sb sb;
    off_t d_bitmap_ptr;
    off_t d_blocks_ptr;
    int dblocksize;
    int free_d;
    int i;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    dblocksize = roundup(sb.num_data_blocks, 8) / 8;
    unsigned char dbitmap[dblocksize];
    d_bitmap_ptr = (off_t)disk_ptr + sb.d_bitmap_ptr;
    d_blocks_ptr = (off_t)disk_ptr + sb.d_blocks_ptr;
    memcpy(&dbitmap, (void*)d_bitmap_ptr, dblocksize);

    i = 0;
    while (i < dblocksize && dbitmap[i] == 1) {
        i++;
    }
    if (i >= dblocksize) {
        return 0;
    }
    free_d = i;
    dbitmap[free_d] = 1;
    memcpy((void*)d_bitmap_ptr, &dbitmap, dblocksize);
    while (i != free_d) {
        d_blocks_ptr += BLOCK_SIZE;
        i++;
    }

    struct wfs_dentry dentry = {
        .name = "",
        .num = -1
    };
    for (i = 0; i < dentries; i++) {
        memcpy((struct wfs_dentry*)d_blocks_ptr + i*sizeof(dentry), &dentry, sizeof(dentry));
    }
    return (struct wfs_dentry*)d_blocks_ptr;
}

int free_datablock(const char *name, int inum, mode_t mode, void *disk_ptr) {
    struct wfs_inode inode;
    struct wfs_sb sb;
    struct wfs_dentry dentry;
    int dblocksize;
    off_t d_bitmap_ptr;
    off_t d_blocks_ptr;
    int dnum;
    int i;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    dblocksize = roundup(sb.num_data_blocks, 8) / 8;
    unsigned char dbitmap[dblocksize];
    d_bitmap_ptr = (off_t)disk_ptr + sb.d_bitmap_ptr;
    d_blocks_ptr = (off_t)disk_ptr + sb.d_blocks_ptr;
    memcpy(&dbitmap, (void*)d_bitmap_ptr, dblocksize);
    inode = fetch_inode(inum, disk_ptr);

    i = 0;
    while (i < N_BLOCKS) {
        if (inode.blocks[i] == -1 || (inode.mode & S_IFMT) != mode)
            continue;

        dnum = inode.blocks[i];
        for (off_t ptr = d_blocks_ptr + (inode.blocks[i] * BLOCK_SIZE); ptr < dentries; ptr += sizeof(struct wfs_dentry)) {
            memcpy(&dentry, (void*)ptr, sizeof(struct wfs_dentry));
            if (strcmp(dentry.name, name) == 0) {
                dentry.num = -1;
                memcpy((void*)ptr, &dentry, sizeof(struct wfs_dentry));
                dbitmap[dnum] = -1;
                memcpy((void*)d_bitmap_ptr, &dbitmap, dblocksize);
                return 1;
            }
        }
        i++;
    }

    return 0;
}

struct wfs_dentry* fetch_available_block(int inum, void *disk_ptr) {
    struct wfs_inode inode;
    struct wfs_sb sb;
    struct wfs_dentry dentry;
    off_t d_blocks_ptr;
    int i;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    d_blocks_ptr = (off_t)disk_ptr + sb.d_blocks_ptr;
    inode = fetch_inode(inum, disk_ptr);

    // get dentry from existing datablock
    i = 0;
    while (i < N_BLOCKS) {
        if (inode.blocks[i] == -1)
            continue;

        for (off_t ptr = d_blocks_ptr + (inode.blocks[i] * BLOCK_SIZE); ptr < dentries; ptr += sizeof(struct wfs_dentry)) {
            memcpy(&dentry, (void*)ptr, sizeof(struct wfs_dentry));
            if (dentry.num == -1) {
                return (struct wfs_dentry*)ptr;
            }
        }
        i++;
    }
    // try to create new datablock
    i = 0;
    while (i < N_BLOCKS) {
        if (inode.blocks[i] == -1) {
            return alloc_datablock(disk_ptr);
        }
        i++;
    }
    return 0;
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
        if ((inum = validatepath(path, 0, disk_ptr)) == -1) {
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
    int p_inum;
    void *curr_disk;
    char *name;
    struct wfs_inode *new_inode;
    struct wfs_dentry *block_ptr;
    mode_t file_mode = mode | S_IFREG;

    if (path == NULL || strlen(path) == 0) {
        return -ENOENT;
    }
    if ((name = strrchr(path, '/')) == NULL) {
        return 0;
    }
    name++;
    if (strlen(name) > MAX_NAME) {
        return -ENOENT;
    }

    curr_disk = disk_ptrs[0];
    const char *parentpath = getparentpath(path);
    if ((p_inum = validatepath(parentpath, S_IFDIR, curr_disk)) == -1) {
        return -ENOENT;
    }
    if (data_exists(name, file_mode, p_inum, curr_disk)) {
        return -EEXIST;
    }
    if ((new_inode = alloc_inode(curr_disk, file_mode)) == 0) {
        return -ENOSPC;
    };
    if ((block_ptr = fetch_available_block(p_inum, curr_disk)) == 0) {
        return -ENOSPC;
    }
    struct wfs_dentry new_dentry = {
        .num = new_inode->num
    };
    strcpy(new_dentry.name, name);
    memcpy(block_ptr, &new_dentry, sizeof(struct wfs_dentry));
    return 0;
}

static int wfs_mkdir(const char *path, mode_t mode) {
    int p_inum;
    void *curr_disk;
    char *name;
    struct wfs_inode *new_inode;
    struct wfs_dentry *block_ptr;
    mode_t dir_mode = mode | S_IFDIR;

    if (path == NULL || strlen(path) == 0) {
        return -ENOENT;
    }
    if ((name = strrchr(path, '/')) == NULL) {
        return 0;
    }
    name++;
    if (strlen(name) > MAX_NAME) {
        return -ENOENT;
    }

    curr_disk = disk_ptrs[0];
    const char *parentpath = getparentpath(path);
    if ((p_inum = validatepath(parentpath, S_IFDIR, curr_disk)) == -1) {
        return -ENOENT;
    }
    if (data_exists(name, dir_mode, p_inum, curr_disk)) {
        return -EEXIST;
    }
    if ((new_inode = alloc_inode(curr_disk, dir_mode)) == 0) {
        return -ENOSPC;
    };
    if ((block_ptr = fetch_available_block(p_inum, curr_disk)) == 0) {
        return -ENOSPC;
    }
    struct wfs_dentry new_dentry = {
        .num = new_inode->num
    };
    strcpy(new_dentry.name, name);
    memcpy(block_ptr, &new_dentry, sizeof(struct wfs_dentry));
    return 0;
}

static int wfs_unlink(const char *path) {
    int p_inum;
    void *curr_disk;
    char *name;
    mode_t dir_mode = S_IFDIR;
    mode_t file_mode = S_IFREG;

    if (path == NULL || strlen(path) == 0) {
        return -ENOENT;
    }
    if ((name = strrchr(path, '/')) == NULL) {
        return -ENOENT;
    }
    name++;
    if (strlen(name) > MAX_NAME) {
        return -ENOENT;
    }

    curr_disk = disk_ptrs[0];
    const char *parentpath = getparentpath(path);
    if ((p_inum = validatepath(parentpath, dir_mode, curr_disk)) == -1) {
        return -ENOENT;
    }
    if (free_datablock(name, p_inum, file_mode, curr_disk) != 1) {
        return -EEXIST;
    }
    return 0;
}

static int wfs_rmdir(const char *path) {
    int p_inum;
    void *curr_disk;
    char *name;
    mode_t dir_mode = S_IFDIR;

    if (path == NULL || strlen(path) == 0) {
        return -ENOENT;
    }
    if ((name = strrchr(path, '/')) == NULL) {
        return -ENOENT;
    }
    name++;
    if (strlen(name) > MAX_NAME) {
        return -ENOENT;
    }

    curr_disk = disk_ptrs[0];
    const char *parentpath = getparentpath(path);
    if ((p_inum = validatepath(parentpath, dir_mode, curr_disk)) == -1) {
        return -ENOENT;
    }
    if (free_datablock(name, p_inum, dir_mode, curr_disk) != 1) {
        return -EEXIST;
    }
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
