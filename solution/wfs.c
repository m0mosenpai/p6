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
size_t *disk_sizes;
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

int raid0_idx(int disk, int offset) {
    return (offset * total_disks + disk);
}

int raid0_disk(int dnum) {
    return dnum % total_disks;
}

int raid0_offset(int dnum) {
    return dnum / total_disks;
}

/*int mirror_data(void *maindisk, void *src, size_t size) {*/
/*    printf("===============> Copying to other disks\n");*/
/*    for (int i = 1; i < total_disks; i++) {*/
/*        printf("===============> DISK: %d\n", i+1);*/
/*        memcpy((void*)((off_t)disk_ptrs[i] + (off_t)src - (off_t)maindisk), src, size);*/
/*    }*/
/*    return 1;*/
/*}*/

void mirror_data(void *maindisk) {
    for (int i = 1; i < total_disks; i++) {
        memcpy(disk_ptrs[i], maindisk, disk_sizes[0]);
    }
}

void memcpy_v(void *dst, void *src, size_t size) {
    memcpy(dst, src, size);
    switch(raid) {
        case RAID_0:
            break;
        default:
            /*mirror_data(disk_ptrs[0], dst, size);*/
            mirror_data(disk_ptrs[0]);
            break;
    }
}

struct wfs_inode fetch_inode(int inum, void *disk_ptr) {
    struct wfs_sb sb;
    struct wfs_inode inode;
    off_t i_blocks_ptr;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    i_blocks_ptr = (off_t)disk_ptr + sb.i_blocks_ptr;
    memcpy(&inode, (void*)(i_blocks_ptr + (inum * BLOCK_SIZE)), sizeof(struct wfs_inode));
    inode.atim = time(NULL);
    memcpy_v((void*)(i_blocks_ptr + (inum * BLOCK_SIZE)), &inode, sizeof(struct wfs_inode));
    return inode;
}

int validatepath(const char* path, void *disk_ptr) {
    struct wfs_sb sb;
    struct wfs_inode inode;
    struct wfs_dentry dentry;
    off_t d_blocks_ptr;
    int inum;
    int blk;
    int dnum;
    int found;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));

    char *delim = "/";
    char *path_cpy = strdup(path);
    char *tok = strtok(path_cpy, delim);
    inum = 0;

    while (tok != NULL) {
        inode = fetch_inode(inum, disk_ptr);
        inode.atim = time(NULL);
        found = 0;
        for (int i = 0; i < N_BLOCKS; i++) {
            blk = inode.blocks[i];
            if (blk == -1)
                continue;

            dnum = raid0_offset(blk);
            d_blocks_ptr = (off_t)disk_ptrs[raid0_disk(blk)] + sb.d_blocks_ptr;
            off_t start = d_blocks_ptr + (dnum * BLOCK_SIZE);
            for (off_t ptr = start; ptr < start + BLOCK_SIZE; ptr += sizeof(struct wfs_dentry)) {
                memcpy(&dentry, (void*)ptr, sizeof(struct wfs_dentry));
                if (strcmp(dentry.name, tok) == 0 && dentry.num != -1) {
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

const char* getname(const char *path) {
    char *name;

    if ((name = strrchr(path, '/')) == NULL) {
        return 0;
    }
    name++;
    if (strlen(name) > MAX_NAME) {
        return 0;
    }
    return name;
}

int isdirempty(int inum, void *disk_ptr) {
    struct wfs_sb sb;
    struct wfs_inode inode;
    int i;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    inode = fetch_inode(inum, disk_ptr);

    i = 0;
    while (i < N_BLOCKS) {
        if (inode.blocks[i] != -1) {
            return 0;
        }
        i++;
    }
    return 1;
}


struct wfs_dentry fetch_block(int dnum, void *disk_ptr) {
    struct wfs_sb sb;
    struct wfs_dentry dentry;
    off_t d_blocks_ptr;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    d_blocks_ptr = (off_t)disk_ptrs[raid0_disk(dnum)] + sb.d_blocks_ptr;
    memcpy(&dentry, (void*)(d_blocks_ptr + (dnum * BLOCK_SIZE)), sizeof(struct wfs_dentry));
    return dentry;
}

int data_exists(const char* name, int inum, void *disk_ptr) {
    printf("[DEBUG] inside data_exists\n");
    struct wfs_sb sb;
    struct wfs_inode inode;
    struct wfs_dentry dentry;
    off_t d_blocks_ptr;
    int blk;
    int dnum;
    int i;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    inode = fetch_inode(inum, disk_ptr);

    i = 0;
    while (i < N_BLOCKS) {
        blk = inode.blocks[i];
        if (blk != -1) {
            dnum = raid0_offset(blk);
            d_blocks_ptr = (off_t)disk_ptrs[raid0_disk(blk)] + sb.d_blocks_ptr;
            off_t start = d_blocks_ptr + (dnum * BLOCK_SIZE);
            for (off_t ptr = start; ptr < start + BLOCK_SIZE; ptr += sizeof(struct wfs_dentry)) {
                memcpy(&dentry, (void*)ptr, sizeof(struct wfs_dentry));
                if (strcmp(dentry.name, name) == 0) {
                    printf("[DEBUG] found existing data with name %s\n", name);
                    return dentry.num;
                }
            }
        }
        i++;
    }
    printf("[DEBUG] no existing data found with name %s\n", name);
    return -1;
}

struct wfs_inode* alloc_inode(void *disk_ptr, mode_t mode) {
    printf("[DEBUG] inside alloc_inode\n");
    struct wfs_sb sb;
    off_t i_bitmap_ptr;
    off_t i_blocks_ptr;
    int inodesize;
    int free_i;
    time_t ctime;
    int i;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    inodesize = roundup(sb.num_inodes, 8) / 8;
    unsigned char inodebitmap[inodesize];
    i_bitmap_ptr = (off_t)disk_ptr + sb.i_bitmap_ptr;
    i_blocks_ptr = (off_t)disk_ptr + sb.i_blocks_ptr;
    memcpy(&inodebitmap, (void*)i_bitmap_ptr, inodesize);

    for (i = 0; i < sb.num_inodes; i++) {
        if ((inodebitmap[i / 8] & (1 << (i % 8))) == 0) {
            free_i = i;
            break;
        }
    }
    if (i >= sb.num_inodes) {
        printf("[DEBUG] all inodes full\n");
        return 0;
    }
    inodebitmap[free_i / 8] |= (1 << (free_i % 8));
    memcpy_v((void*)i_bitmap_ptr, &inodebitmap, inodesize);

    i = 0;
    while (i != free_i) {
        i_blocks_ptr += BLOCK_SIZE;
        i++;
    }

    ctime = time(NULL);
    struct wfs_inode new_inode = {
        .num = free_i,
        .mode = mode,
        .uid = getuid(),
        .gid = getgid(),
        .size = 0,
        .nlinks = 2,
        .atim = ctime,
        .mtim = ctime,
        .ctim = ctime,
    };
    memset(new_inode.blocks, -1, N_BLOCKS*(sizeof(off_t)));
    memcpy_v((void*)i_blocks_ptr, &new_inode, sizeof(struct wfs_inode));
    printf("[DEBUG] successfully allocated new inode\n");
    return (struct wfs_inode*)i_blocks_ptr;
}

struct wfs_dentry* alloc_datablock(void *disk_ptr, int *idx) {
    printf("[DEBUG] inside alloc_datablock\n");
    struct wfs_sb sb;
    off_t d_bitmap_ptr;
    off_t d_blocks_ptr;
    int dblocksize;
    int free_d;
    int i, j;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    dblocksize = roundup(sb.num_data_blocks, 8) / 8;
    unsigned char dbitmap[dblocksize];
    d_bitmap_ptr = (off_t)disk_ptr + sb.d_bitmap_ptr;
    memcpy(&dbitmap, (void*)d_bitmap_ptr, dblocksize);

    int found = 0;
    for (i = 0; i < sb.num_data_blocks; i++) {
        for (j = 0; j < total_disks; j++) {
            if ((dbitmap[i / 8] & (1 << (i % 8))) == 0) {
                *idx = raid0_idx(j, i);
                free_d = i;
                found = 1;
                printf("[DEBUG] block free on disk %d, offset %d\n", j, i);
                break;
            }
        }
        if (found) break;
    }
    if (!found) {
        return 0;
    }
    d_blocks_ptr = (off_t)disk_ptrs[j] + sb.d_blocks_ptr;
    dbitmap[free_d / 8] |= (1 << (free_d % 8));
    memcpy_v((void*)d_bitmap_ptr, &dbitmap, dblocksize);

    i = 0;
    while (i != free_d) {
        d_blocks_ptr += BLOCK_SIZE;
        i++;
    }
    memset((void*)d_blocks_ptr, -1, BLOCK_SIZE);
    printf("[DEBUG] successfully allocated empty block\n");
    return (struct wfs_dentry*)d_blocks_ptr;
}

int free_dentry(int inum, int t_inum, void *disk_ptr) {
    printf("[DEBUG] in free_dentry\n");
    struct wfs_inode inode;
    struct wfs_sb sb;
    struct wfs_dentry dentry;
    off_t i_blocks_ptr;
    off_t d_blocks_ptr;
    int blk;
    int dnum;
    int i;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    i_blocks_ptr = (off_t)disk_ptr + sb.i_blocks_ptr;
    inode = fetch_inode(inum, disk_ptr);

    i = 0;
    while (i < N_BLOCKS) {
        blk = inode.blocks[i];
        if (blk != -1) {
            dnum = raid0_offset(blk);
            d_blocks_ptr = (off_t)disk_ptrs[raid0_disk(blk)] + sb.d_blocks_ptr;
            off_t start = d_blocks_ptr + (dnum * BLOCK_SIZE);
            for (off_t ptr = start; ptr < start + BLOCK_SIZE; ptr += sizeof(struct wfs_dentry)) {
                memcpy(&dentry, (void*)ptr, sizeof(struct wfs_dentry));
                if (dentry.num == t_inum) {
                    dentry.num = -1;
                    inode.size -= sizeof(dentry);
                }
                memcpy_v((void*)ptr, &dentry, sizeof(struct wfs_dentry));
                printf("[DEBUG] successfully freed dentry with inum %d\n", t_inum);
                return 1;
            }
        }
        i++;
    }
    inode.mtim = time(NULL);
    memcpy_v((void*)(i_blocks_ptr + (inum * BLOCK_SIZE)), &inode, sizeof(struct wfs_inode));
    printf("[DEBUG] no dentry found with inum %d\n", t_inum);
    return 0;
}

void free_inode(int inum, void* disk_ptr) {
    printf("[DEBUG] in free_inode\n");
    struct wfs_inode inode;
    struct wfs_sb sb;
    off_t i_bitmap_ptr;
    off_t i_blocks_ptr;
    int inodesize;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    inodesize = roundup(sb.num_inodes, 8) / 8;
    unsigned char inodebitmap[inodesize];
    i_bitmap_ptr = (off_t)disk_ptr + sb.i_bitmap_ptr;
    i_blocks_ptr = (off_t)disk_ptr + sb.i_blocks_ptr;
    memcpy(&inodebitmap, (void*)i_bitmap_ptr, inodesize);
    inode = fetch_inode(inum, disk_ptr);

    inode.num = -1;
    inodebitmap[inum / 8] &= ~(1 << (inum % 8));
    memcpy_v((void*)(i_blocks_ptr + (inum * BLOCK_SIZE)), &inode, sizeof(struct wfs_inode));
    memcpy_v((void*)i_bitmap_ptr, &inodebitmap, inodesize);
    printf("[DEBUG] successfully freed inode with inum %d\n", inum);
}

void free_datablock(int dnum, void* disk_ptr) {
    printf("[DEBUG] inside free_datablock\n");
    struct wfs_sb sb;
    off_t d_bitmap_ptr;
    int dblocksize;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    dblocksize = roundup(sb.num_data_blocks, 8) / 8;
    unsigned char dbitmap[dblocksize];
    d_bitmap_ptr = (off_t)disk_ptr + sb.d_bitmap_ptr;
    memcpy(&dbitmap, (void*)d_bitmap_ptr, dblocksize);

    dbitmap[dnum / 8] &= ~(1 << (dnum % 8));
    memcpy_v((void*)d_bitmap_ptr, &dbitmap, dblocksize);
    printf("[DEBUG] successfully freed datablock with dnum %d\n", dnum);
}

int free_dir(int inum, int p_inum, const char *name, void *disk_ptr) {
    printf("[DEBUG] inside free_dir \n");
    // clear inode
    free_inode(inum, disk_ptr);

    // clear dentry in parent
    if (free_dentry(p_inum, inum, disk_ptr) != 1) {
        return 0;
    }
    return 1;
    printf("[DEBUG] successfully freed directory entry of %d from %d\n", inum, p_inum);
}


int free_file(int inum, int p_inum, const char *name, void *disk_ptr) {
    printf("[DEBUG] inside free_file \n");
    struct wfs_inode inode;
    struct wfs_sb sb;
    off_t i_blocks_ptr;
    int blk;
    int dnum;
    int i;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    i_blocks_ptr = (off_t)disk_ptr + sb.i_blocks_ptr;
    inode = fetch_inode(inum, disk_ptr);

    // clear inode
    free_inode(inum, disk_ptr);

    // clear file data
    i = 0;
    while (i < N_BLOCKS) {
        blk = inode.blocks[i];
        if (blk != -1) {
            dnum = raid0_offset(blk);
            free_datablock(dnum, disk_ptr);
            inode.blocks[i] = -1;
        }
        i++;
    }
    memcpy_v((void*)(i_blocks_ptr + (inum * BLOCK_SIZE)), &inode, sizeof(struct wfs_inode));

    // clear dentry in parent
    if (free_dentry(p_inum, inum, disk_ptr) != 1) {
        return 0;
    }
    printf("[DEBUG] successfully freed file with inum %d\n", inum);
    return 1;
}

struct wfs_dentry* fetch_available_block(int inum, void *disk_ptr) {
    printf("[DEBUG] inside fetch_available_block\n");
    struct wfs_inode inode;
    struct wfs_sb sb;
    struct wfs_dentry dentry;
    struct wfs_dentry *new_dentry;
    off_t i_blocks_ptr;
    off_t d_blocks_ptr;
    int blk;
    int dnum;
    int new_dnum;
    int i;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    i_blocks_ptr = (off_t)disk_ptr + sb.i_blocks_ptr;
    inode = fetch_inode(inum, disk_ptr);

    // get dentry from existing datablock
    i = 0;
    while (i < N_BLOCKS) {
        blk = inode.blocks[i];
        if (blk != -1) {
            dnum = raid0_offset(blk);
            d_blocks_ptr = (off_t)disk_ptrs[raid0_disk(blk)] + sb.d_blocks_ptr;
            off_t start = d_blocks_ptr + (dnum * BLOCK_SIZE);
            for (off_t ptr = start; ptr < start + BLOCK_SIZE; ptr += sizeof(struct wfs_dentry)) {
                memcpy(&dentry, (void*)ptr, sizeof(struct wfs_dentry));
                if (dentry.num == -1) {
                    printf("[DEBUG] found empty dentry in existing datablock\n");
                    return (struct wfs_dentry*)ptr;
                }
            }
        }
        i++;
    }
    // try to create new datablock
    i = 0;
    printf("[DEBUG] creating new datablock\n");
    while (i < N_BLOCKS) {
        blk = inode.blocks[i];
        if (blk == -1) {
            /*dnum = raid0_offset(blk);*/
            /*new_dentry = alloc_datablock(disk_ptrs[raid0_disk(blk)], &new_dnum);*/
            new_dentry = alloc_datablock(disk_ptr, &new_dnum);
            inode.blocks[i] = new_dnum;
            memcpy_v((void*)(i_blocks_ptr + (inum * BLOCK_SIZE)), &inode, sizeof(struct wfs_inode));
            printf("[DEBUG] successfully created new dentry\n");
            return new_dentry;
        }
        i++;
    }
    printf("[DEBUG] failed to create new empty dentry\n");
    return 0;
}

int read_blocks(int inum, const char *buffer, size_t size, off_t offset, void *disk_ptr) {
    printf("[DEBUG] inside read_blocks\n");
    struct wfs_inode inode;
    struct wfs_sb sb;
    off_t d_blocks_ptr, b_ptr;
    size_t bytes_read;
    int blk;
    int dnum;
    int i;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    inode = fetch_inode(inum, disk_ptr);
    if (!S_ISREG(inode.mode)) {
        return 0;
    }

    i = 0;
    bytes_read = 0;
    printf("size: %ld\n", size);
    printf("offset: %ld\n", offset);
    while (i < N_BLOCKS) {
        blk = inode.blocks[i];
        if (blk != -1) {
            dnum = raid0_offset(blk);
            printf("dnum: %d\n", dnum);
            d_blocks_ptr = (off_t)disk_ptrs[raid0_disk(blk)] + sb.d_blocks_ptr;
            b_ptr = d_blocks_ptr + (dnum * BLOCK_SIZE) + offset;
            if (size > BLOCK_SIZE) {
                memcpy((void*)(buffer + bytes_read), (void*)b_ptr, BLOCK_SIZE);
                bytes_read += BLOCK_SIZE;
                size -= BLOCK_SIZE;
                printf("read %ld bytes\n", bytes_read);
            }
            else if (size > 0 && size <= BLOCK_SIZE) {
                memcpy((void*)(buffer + bytes_read), (void*)b_ptr, size);
                bytes_read += size;
                size = 0;
            }
            if (size == 0) {
                printf("[DEBUG] successfully read %ld bytes\n", bytes_read);
                return bytes_read;
            }
        }
        i++;
    }
    return 0;
}

int write_blocks(int inum, const char *buffer, size_t size, off_t offset, void *disk_ptr) {
    printf("[DEBUG] inside write_blocks\n");
    struct wfs_inode inode;
    struct wfs_sb sb;
    off_t i_blocks_ptr;
    off_t d_blocks_ptr, b_ptr;
    size_t bytes_written;
    int blk;
    int new_dnum;
    int dnum;
    int i;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    i_blocks_ptr = (off_t)disk_ptr + sb.i_blocks_ptr;
    inode = fetch_inode(inum, disk_ptr);
    if (!S_ISREG(inode.mode)) {
        printf("incorrect mode!\n");
        return 0;
    }

    i = 0;
    bytes_written = 0;
    while (i < N_BLOCKS) {
        blk = inode.blocks[i];
        if (blk == -1) {
            dnum = raid0_offset(blk);
            d_blocks_ptr = (off_t)disk_ptrs[raid0_disk(blk)] + sb.d_blocks_ptr;
            alloc_datablock(disk_ptrs[raid0_disk(dnum)], &new_dnum);
            inode.blocks[i] = new_dnum;
            memcpy_v((void*)(i_blocks_ptr + (inum * BLOCK_SIZE)), &inode, sizeof(struct wfs_inode));

            b_ptr = d_blocks_ptr + (new_dnum * BLOCK_SIZE) + offset;
            if (size > BLOCK_SIZE) {
                memcpy_v((void*)b_ptr, (void*)(buffer + bytes_written), BLOCK_SIZE);
                bytes_written += BLOCK_SIZE;
                size -= BLOCK_SIZE;
                printf("wrote %ld bytes\n", bytes_written);
            }
            else if (size > 0 && size <= BLOCK_SIZE) {
                memcpy_v((void*)b_ptr, (void*)(buffer + bytes_written), size);
                bytes_written += size;
                size = 0;
            }
            if (size == 0) {
                inode.mtim = time(NULL);
                inode.size += bytes_written;
                memcpy_v((void*)(i_blocks_ptr + (inum * BLOCK_SIZE)), &inode, sizeof(struct wfs_inode));
                printf("[DEBUG] successfully wrote %ld bytes\n", bytes_written);
                return bytes_written;
            }
        }
        i++;
    }
    return 0;
}

int read_dentries(int inum, char *buffer, fuse_fill_dir_t filler, void *disk_ptr) {
    struct wfs_inode inode;
    struct wfs_sb sb;
    struct wfs_dentry dentry;
    off_t d_blocks_ptr;
    int blk;
    int dnum;
    int i;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    inode = fetch_inode(inum, disk_ptr);

    i = 0;
    while (i < N_BLOCKS) {
        blk = inode.blocks[i];
        if (blk != -1) {
            dnum = raid0_offset(blk);
            d_blocks_ptr = (off_t)disk_ptrs[raid0_disk(blk)] + sb.d_blocks_ptr;
            off_t start = d_blocks_ptr + (dnum * BLOCK_SIZE);
            for (off_t ptr = start; ptr < start + BLOCK_SIZE; ptr += sizeof(struct wfs_dentry)) {
                memcpy(&dentry, (void*)ptr, sizeof(struct wfs_dentry));
                if (dentry.num == -1)
                    continue;
                filler(buffer, dentry.name, NULL, 0);
            }
        }
        i++;
    }
    filler(buffer, ".", NULL, 0);
    filler(buffer, "..", NULL, 0);
    return 1;
}

static int wfs_getattr(const char *path, struct stat *stbuf) {
    void *curr_disk;
    struct wfs_inode inode;
    struct timespec tim;
    int inum;

    memset(stbuf, 0, sizeof(struct stat));

    curr_disk = disk_ptrs[0];
    if ((inum = validatepath(path, curr_disk)) == -1) {
        return -ENOENT;
    }
    inode = fetch_inode(inum, curr_disk);
    stbuf->st_uid = inode.uid;
    stbuf->st_gid = inode.gid;
    stbuf->st_mode = inode.mode;
    stbuf->st_size = inode.size;
    tim.tv_sec = inode.atim;
    stbuf->st_atim = tim;
    tim.tv_sec = inode.mtim;
    stbuf->st_mtim = tim;
    return 0;
}

static int wfs_mknod(const char *path, mode_t mode, dev_t rdev) {
    printf("\n******* inside mknod *******\n");
    int p_inum;
    int existing_inum;
    void *curr_disk;
    const char *name;
    const char *parentpath;
    struct wfs_sb sb;
    struct wfs_inode *new_inode;
    struct wfs_inode p_inode;
    struct wfs_inode existing_inode;
    struct wfs_dentry *block_ptr;
    off_t i_blocks_ptr;
    mode_t file_mode = mode | S_IFREG;

    if (path == NULL || strlen(path) == 0) {
        return -ENOENT;
    }
    name = getname(path);
    parentpath = getparentpath(path);

    curr_disk = disk_ptrs[0];
    memcpy(&sb, curr_disk, sizeof(struct wfs_sb));
    i_blocks_ptr = (off_t)curr_disk + sb.i_blocks_ptr;
    if ((p_inum = validatepath(parentpath, curr_disk)) == -1) {
        return -ENOENT;
    }
    if ((existing_inum = data_exists(name, p_inum, curr_disk)) != -1) {
        existing_inode = fetch_inode(existing_inum, curr_disk);
        if (S_ISREG(existing_inode.mode))
            return -EEXIST;
    }
    if ((new_inode = alloc_inode(curr_disk, file_mode)) == 0) {
        return -ENOSPC;
    };
    if ((block_ptr = fetch_available_block(p_inum, curr_disk)) == 0) {
        printf("no free block available\n");
        return -ENOSPC;
    }
    struct wfs_dentry new_dentry = {
        .num = new_inode->num
    };
    strcpy(new_dentry.name, name);
    p_inode = fetch_inode(p_inum, curr_disk);
    p_inode.size += sizeof(new_dentry);
    p_inode.mtim = time(NULL);
    p_inode.nlinks++;
    memcpy_v((void*)(i_blocks_ptr + (p_inum * BLOCK_SIZE)), &p_inode, sizeof(struct wfs_inode));
    memcpy_v(block_ptr, &new_dentry, sizeof(struct wfs_dentry));
    printf("successfully created new file\n");
    return 0;
}

static int wfs_mkdir(const char *path, mode_t mode) {
    printf("\n******* inside mkdir *******\n");
    int p_inum;
    int existing_inum;
    void *curr_disk;
    const char *name;
    const char *parentpath;
    struct wfs_sb sb;
    struct wfs_inode *new_inode;
    struct wfs_inode p_inode;
    struct wfs_inode existing_inode;
    struct wfs_dentry *block_ptr;
    off_t i_blocks_ptr;
    mode_t dir_mode = mode | S_IFDIR;

    if (path == NULL || strlen(path) == 0) {
        return -ENOENT;
    }
    name = getname(path);
    parentpath = getparentpath(path);

    curr_disk = disk_ptrs[0];
    memcpy(&sb, curr_disk, sizeof(struct wfs_sb));
    i_blocks_ptr = (off_t)curr_disk + sb.i_blocks_ptr;
    if ((p_inum = validatepath(parentpath, curr_disk)) == -1) {
        return -ENOENT;
    }
    if ((existing_inum = data_exists(name, p_inum, curr_disk)) != -1) {
        existing_inode = fetch_inode(existing_inum, curr_disk);
        if (S_ISDIR(existing_inode.mode))
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
    p_inode = fetch_inode(p_inum, curr_disk);
    p_inode.size += sizeof(new_dentry);
    p_inode.mtim = time(NULL);
    p_inode.nlinks++;
    memcpy_v((void*)(i_blocks_ptr + (p_inum * BLOCK_SIZE)), &p_inode, sizeof(struct wfs_inode));
    memcpy_v(block_ptr, &new_dentry, sizeof(struct wfs_dentry));
    return 0;
}

static int wfs_unlink(const char *path) {
    printf("\n******* inside unlink *******\n");
    int inum, p_inum;
    struct wfs_inode inode;
    void *curr_disk;
    const char *name;
    const char *parentpath;

    if (path == NULL || strlen(path) == 0) {
        return -ENOENT;
    }
    name = getname(path);
    parentpath = getparentpath(path);

    curr_disk = disk_ptrs[0];
    if ((inum = validatepath(path, curr_disk)) == -1) {
        return -ENOENT;
    }
    inode = fetch_inode(inum, curr_disk);
    if (!S_ISREG(inode.mode)) {
        return -ENOENT;
    }
    p_inum = validatepath(parentpath, curr_disk);
    if (free_file(inum, p_inum, name, curr_disk) != 1) {
        return -ENOENT;
    }
    return 0;
}

static int wfs_rmdir(const char *path) {
    printf("\n******* inside rmdir *******\n");
    int inum, p_inum;
    struct wfs_inode inode;
    void *curr_disk;
    const char *name;
    const char *parentpath;

    if (path == NULL || strlen(path) == 0) {
        return -ENOENT;
    }
    name = getname(path);
    parentpath = getparentpath(path);

    curr_disk = disk_ptrs[0];
    if ((inum = validatepath(path, curr_disk)) == -1) {
        return -ENOENT;
    }
    inode = fetch_inode(inum, curr_disk);
    if (!S_ISDIR(inode.mode)) {
        return -ENOENT;
    }
    if (!isdirempty(inum, curr_disk)) {
        return -ENOTEMPTY;
    }
    p_inum = validatepath(parentpath, curr_disk);
    if (free_dir(inum, p_inum, name, curr_disk) != 1) {
        return -ENOENT;
    }
    return 0;
}

static int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    printf("\n******* inside read *******\n");
    int inum;
    void *curr_disk;
    size_t bytes_read;

    if (path == NULL || strlen(path) == 0) {
        return -ENOENT;
    }

    curr_disk = disk_ptrs[0];
    if ((inum = validatepath(path, curr_disk)) == -1) {
        return -ENOENT;
    }
    if ((bytes_read = read_blocks(inum, buf, size, offset, curr_disk)) == 0) {
        return -ENOENT;
    }

    return bytes_read;
}

static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    printf("\n******* inside write *******\n");
    int inum;
    void *curr_disk;
    size_t bytes_written;

    if (path == NULL || strlen(path) == 0) {
        return -ENOENT;
    }

    curr_disk = disk_ptrs[0];
    if ((inum = validatepath(path, curr_disk)) == -1) {
        return -ENOENT;
    }
    if ((bytes_written = write_blocks(inum, buf, size, offset, curr_disk)) == 0) {
        return -ENOENT;
    }

    return bytes_written;
}

static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi) {
    int inum;
    void *curr_disk;

    if (path == NULL || strlen(path) == 0) {
        return -ENOENT;
    }

    curr_disk = disk_ptrs[0];
    if ((inum = validatepath(path, curr_disk)) == -1) {
        return -ENOENT;
    }
    if (read_dentries(inum, buf, filler, curr_disk) != 1) {
        return -ENOENT;
    }

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
            disk_sizes = malloc(total_disks * sizeof(size_t));
            if (dcnt != total_disks) {
                close(fd);
                freev((void*)disks, ndisks, 1);
                freev((void*)fuse_argv, fuse_argc, 1);
                return -1;
            }
        }
        disk_ptrs[i] = disk_ptr;
        struct stat st;
        fstat(fd, &st);
        disk_sizes[i] = st.st_size;

        close(fd);
    }

    umask(0);
    return fuse_main(fuse_argc, fuse_argv, &ops, NULL);
}
