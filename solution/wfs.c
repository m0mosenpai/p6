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
void *maindisk;
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
    if (raid != RAID_0) {
        return 0;
    }
    return dnum % total_disks;
}

int raid0_offset(int dnum) {
    return dnum / total_disks;
}

/*int mirror_data(void *maindisk, void *src, size_t size) {*/
/*    for (int i = 1; i < total_disks; i++) {*/
/*        memcpy((void*)((off_t)disk_ptrs[i] + (off_t)src - (off_t)maindisk), src, size);*/
/*    }*/
/*    return 1;*/
/*}*/

/*void mirror_data(void *maindisk) {*/
void mirror_data_raid1() {
    for (int i = 1; i < total_disks; i++) {
        memcpy(disk_ptrs[i], maindisk, disk_sizes[0]);
    }
}

void mirror_data_raid0(off_t dst, size_t size) {
    for (int i = 1; i < total_disks; i++) {
        memcpy((void*)((off_t)disk_ptrs[i] + (dst - (off_t)maindisk)), (void*)dst, size);
    }
}

void memcpy_v(off_t dst, void *src, size_t size, int metadata) {
    memcpy((void*)dst, src, size);
    switch(raid) {
        case RAID_0:
            if (metadata == 1) {
                mirror_data_raid0(dst, size);
            }
            break;
        default:
            /*mirror_data(disk_ptrs[0], dst, size);*/
            /*mirror_data(disk_ptrs[0]);*/
            mirror_data_raid1();
            break;
    }
}

struct wfs_inode fetch_inode(int inum) {
    printf("[DEBUG] inside fetch_inode\n");
    void *disk_ptr = maindisk;
    struct wfs_sb sb;
    struct wfs_inode inode;
    off_t i_blocks_ptr;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    i_blocks_ptr = (off_t)disk_ptr + sb.i_blocks_ptr;
    memcpy(&inode, (void*)(i_blocks_ptr + (inum * BLOCK_SIZE)), sizeof(struct wfs_inode));
    inode.atim = time(NULL);
    memcpy_v((i_blocks_ptr + (inum * BLOCK_SIZE)), &inode, sizeof(struct wfs_inode), 1);
    printf("[DEBUG] successfully fetched inode %d\n", inode.num);
    return inode;
}

int validatepath(const char* path) {
    printf("[DEBUG] inside validatepath\n");
    void *disk_ptr = maindisk;
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
        inode = fetch_inode(inum);
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
            printf("[DEBUG] invalid path, inum: %d\n", inum);
            return -1;
        }
        tok = strtok(NULL, delim);
    }
    printf("[DEBUG] successfully validated path, inum: %d\n", inum);
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

int isdirempty(int inum) {
    printf("[DEBUG] inside isdirempty\n");
    void *disk_ptr = maindisk;
    struct wfs_sb sb;
    struct wfs_inode inode;
    struct wfs_dentry dentry;
    off_t d_blocks_ptr;
    int blk;
    int dnum;
    int i;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    inode = fetch_inode(inum);

    i = 0;
    while (i < N_BLOCKS) {
        blk = inode.blocks[i];
        if (blk != -1) {
            dnum = raid0_offset(blk);
            d_blocks_ptr = (off_t)disk_ptrs[raid0_disk(blk)] + sb.d_blocks_ptr;
            off_t start = d_blocks_ptr + (dnum * BLOCK_SIZE);
            for (off_t ptr = start; ptr < start + BLOCK_SIZE; ptr += sizeof(struct wfs_dentry)) {
                memcpy(&dentry, (void*)ptr, sizeof(struct wfs_dentry));
                if (dentry.num != -1) {
                    printf("[DEBUG] directory not empty\n");
                    return 0;
                }
            }
        }
        i++;
    }
    printf("[DEBUG] directory is empty\n");
    return 1;
}


off_t fetch_block(int dnum) {
    printf("[DEBUG] inside fetch_block\n");
    void *disk_ptr = maindisk;
    int parsed_dnum;
    int disk;
    struct wfs_sb sb;
    off_t d_blocks_ptr;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    parsed_dnum = raid0_offset(dnum);
    disk = raid0_disk(dnum);
    d_blocks_ptr = (off_t)disk_ptrs[disk] + sb.d_blocks_ptr;
    off_t block = d_blocks_ptr + (parsed_dnum * BLOCK_SIZE);
    return block;
}

struct wfs_dentry* fetch_empty_dentry(int dnum) {
    void *disk_ptr = maindisk;
    printf("[DEBUG] inside fetch_empty_dentry\n");
    int parsed_dnum;
    int disk;
    struct wfs_sb sb;
    struct wfs_dentry dentry;
    off_t d_blocks_ptr;

    parsed_dnum = raid0_offset(dnum);
    disk = raid0_disk(dnum);
    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    d_blocks_ptr = (off_t)disk_ptrs[disk] + sb.d_blocks_ptr;
    off_t start = d_blocks_ptr + (parsed_dnum * BLOCK_SIZE);
    for (off_t ptr = start; ptr < start + BLOCK_SIZE; ptr += sizeof(struct wfs_dentry)) {
        memcpy(&dentry, (void*)ptr, sizeof(struct wfs_dentry));
        if (dentry.num == -1) {
            printf("[DEBUG] found empty dentry in block %d\n", parsed_dnum);
            return (struct wfs_dentry*)ptr;
        }
    }
    printf("[DEBUG] no empty dentry in block %d\n", parsed_dnum);
    return 0;
}

int data_exists(const char* name, int inum) {
    printf("[DEBUG] inside data_exists\n");
    void *disk_ptr = maindisk;
    struct wfs_sb sb;
    struct wfs_inode inode;
    struct wfs_dentry dentry;
    off_t d_blocks_ptr;
    int blk;
    int dnum;
    int i;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    inode = fetch_inode(inum);

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

struct wfs_inode* alloc_inode(mode_t mode) {
    void *disk_ptr = maindisk;
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
    memcpy_v(i_bitmap_ptr, &inodebitmap, inodesize, 1);

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
        .nlinks = S_ISREG(mode) ? 2 : 1,
        .atim = ctime,
        .mtim = ctime,
        .ctim = ctime,
    };
    memset(new_inode.blocks, -1, N_BLOCKS*(sizeof(off_t)));
    memcpy_v(i_blocks_ptr, &new_inode, sizeof(struct wfs_inode), 1);
    printf("[DEBUG] successfully allocated new inode\n");
    return (struct wfs_inode*)i_blocks_ptr;
}

int alloc_datablock() {
    void *disk_ptr = maindisk;
    printf("[DEBUG] inside alloc_datablock\n");
    struct wfs_sb sb;
    off_t d_bitmap_ptr;
    off_t d_blocks_ptr;
    int dblocksize;
    int idx;
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
                idx = raid0_idx(j, i);
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
    memcpy_v(d_bitmap_ptr, &dbitmap, dblocksize, 0);

    i = 0;
    while (i != free_d) {
        d_blocks_ptr += BLOCK_SIZE;
        i++;
    }
    memset((void*)d_blocks_ptr, -1, BLOCK_SIZE);
    memcpy_v(d_blocks_ptr, (void*)d_blocks_ptr, BLOCK_SIZE, 0);
    printf("[DEBUG] successfully allocated empty block\n");
    return idx;
}

int free_dentry(int p_inum, int c_inum) {
    void *disk_ptr = maindisk;
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
    inode = fetch_inode(p_inum);

    i = 0;
    while (i < N_BLOCKS) {
        blk = inode.blocks[i];
        if (blk != -1) {
            dnum = raid0_offset(blk);
            d_blocks_ptr = (off_t)disk_ptrs[raid0_disk(blk)] + sb.d_blocks_ptr;
            off_t start = d_blocks_ptr + (dnum * BLOCK_SIZE);
            for (off_t ptr = start; ptr < start + BLOCK_SIZE; ptr += sizeof(struct wfs_dentry)) {
                memcpy(&dentry, (void*)ptr, sizeof(struct wfs_dentry));
                if (dentry.num == c_inum) {
                    dentry.num = -1;
                    memcpy_v(ptr, &dentry, sizeof(struct wfs_dentry), 0);
                    /*inode.size -= sizeof(dentry);*/
                    inode.mtim = time(NULL);
                    memcpy_v((i_blocks_ptr + (inode.num * BLOCK_SIZE)), &inode, sizeof(struct wfs_inode), 1);
                    printf("[DEBUG] successfully freed dentry with inum %d\n", c_inum);
                    return 1;
                }
            }
        }
        i++;
    }
    printf("[DEBUG] no dentry found with inum %d\n", c_inum);
    return 0;
}

void free_inode(int inum) {
    void *disk_ptr = maindisk;
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
    inode = fetch_inode(inum);

    inode.num = -1;
    inodebitmap[inum / 8] &= ~(1 << (inum % 8));
    memcpy_v((i_blocks_ptr + (inum * BLOCK_SIZE)), &inode, sizeof(struct wfs_inode), 1);
    memcpy_v(i_bitmap_ptr, &inodebitmap, inodesize, 1);
    printf("[DEBUG] successfully freed inode with inum %d\n", inum);
}

void free_datablock(int dnum) {
    printf("[DEBUG] inside free_datablock\n");
    int parsed_dnum = raid0_offset(dnum);
    void *disk_ptr = disk_ptrs[raid0_disk(dnum)];
    struct wfs_sb sb;
    off_t d_bitmap_ptr;
    off_t d_blocks_ptr;
    off_t b_ptr;
    int dblocksize;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    dblocksize = roundup(sb.num_data_blocks, 8) / 8;
    unsigned char dbitmap[dblocksize];
    d_bitmap_ptr = (off_t)disk_ptr + sb.d_bitmap_ptr;
    d_blocks_ptr = (off_t)disk_ptr + sb.d_blocks_ptr;
    memcpy(&dbitmap, (void*)d_bitmap_ptr, dblocksize);

    b_ptr = d_blocks_ptr + (parsed_dnum * BLOCK_SIZE);
    memset((void*)b_ptr, -1, BLOCK_SIZE);
    dbitmap[parsed_dnum / 8] &= ~(1 << (parsed_dnum % 8));
    memcpy_v(d_bitmap_ptr, &dbitmap, dblocksize, 0);
    printf("[DEBUG] successfully freed datablock with dnum %d\n", dnum);
}

int free_dir(int inum, int p_inum, const char *name) {
    printf("[DEBUG] inside free_dir \n");

    // clear dentry in parent
    if (free_dentry(p_inum, inum) != 1) {
        return 0;
    }
    // clear inode
    free_inode(inum);

    return 1;
    printf("[DEBUG] successfully freed directory entry of %d from %d\n", inum, p_inum);
}


int free_file(int inum, int p_inum, const char *name) {
    void *disk_ptr = maindisk;
    printf("[DEBUG] inside free_file \n");
    struct wfs_inode inode;
    struct wfs_sb sb;
    off_t i_blocks_ptr;
    int blk;
    int i;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    i_blocks_ptr = (off_t)disk_ptr + sb.i_blocks_ptr;
    inode = fetch_inode(inum);

    // clear dentry in parent
    if (free_dentry(p_inum, inum) != 1) {
        return 0;
    }

    // clear file data
    i = 0;
    while (i < N_BLOCKS) {
        blk = inode.blocks[i];
        if (blk != -1) {
            free_datablock(blk);
            inode.blocks[i] = -1;
            /*inode.size -= BLOCK_SIZE;*/
        }
        i++;
    }
    memcpy_v((i_blocks_ptr + (inode.num * BLOCK_SIZE)), &inode, sizeof(struct wfs_inode), 1);

    // clear inode
    free_inode(inum);

    printf("[DEBUG] successfully freed file with inum %d\n", inum);
    return 1;
}

struct wfs_dentry* fetch_available_block(int inum) {
    void *disk_ptr = maindisk;
    printf("[DEBUG] inside fetch_available_block\n");
    struct wfs_inode inode;
    struct wfs_sb sb;
    off_t i_blocks_ptr;
    int blk;
    int new_dnum;
    int i;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    i_blocks_ptr = (off_t)disk_ptr + sb.i_blocks_ptr;
    inode = fetch_inode(inum);
    printf("[DEBUG] reading from inode %d\n", inode.num);

    // get dentry from existing datablock
    i = 0;
    while (i < N_BLOCKS) {
        blk = inode.blocks[i];
        printf("Block: %d\n", blk);
        if (blk != -1) {
            printf("[DEBUG] found existing datablock\n");
            return fetch_empty_dentry(blk);
        }
        i++;
    }
    // try to create new datablock and fetch dentry
    i = 0;
    printf("[DEBUG] creating new datablock\n");
    while (i < N_BLOCKS) {
        blk = inode.blocks[i];
        if (blk == -1) {
            new_dnum = alloc_datablock();
            printf("[DEBUG] allocated new datablock at %d\n", new_dnum);
            inode.blocks[i] = new_dnum;
            printf("[DEBUG] writing to inode %d\n", inode.num);
            memcpy_v((i_blocks_ptr + (inum * BLOCK_SIZE)), &inode, sizeof(struct wfs_inode), 1);
            struct wfs_inode test = fetch_inode(inum);
            for (int j = 0; j < N_BLOCKS; j++) {
                printf("Verifying block: %ld\n", test.blocks[j]);
            }
            printf("[DEBUG] successfully created new dentry\n");
            return fetch_empty_dentry(new_dnum);
        }
        i++;
    }
    printf("[DEBUG] failed to create new empty dentry\n");
    return 0;
}

int read_blocks(int inum, const char *buffer, size_t size, off_t offset) {
    printf("[DEBUG] inside read_blocks\n");
    void *disk_ptr = maindisk;
    struct wfs_inode inode;
    struct wfs_sb sb;
    off_t d_blocks_ptr, b_ptr;
    size_t bytes_read, to_read;
    int blk, blk_offset;
    int dnum;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    inode = fetch_inode(inum);
    if (!S_ISREG(inode.mode)) {
        printf("[DEBUG] incorrect mode - can only read from file\n");
        return 0;
    }

    printf("[DEBUG] file size: %ld\n", inode.size);
    printf("[DEBUG] size: %ld\n", size);
    if (size > (inode.size - offset)) {
        size = inode.size - offset;
        printf("[DEBUG] adjusted size: %ld\n", size);
    }

    bytes_read = 0;
    printf("[DEBUG] offset: %ld\n", offset);
    while (bytes_read < size) {
        blk = (offset + bytes_read) / BLOCK_SIZE;
        blk_offset = (offset + bytes_read) % BLOCK_SIZE;
        if (blk < N_BLOCKS) {
            dnum = raid0_offset(blk);
            d_blocks_ptr = (off_t)disk_ptrs[raid0_disk(blk)] + sb.d_blocks_ptr;
            b_ptr = d_blocks_ptr + (dnum * BLOCK_SIZE);
            to_read = ((size - bytes_read) / BLOCK_SIZE) >= 1 ? BLOCK_SIZE : (size - bytes_read);
            printf("[DEBUG] bytes to read: %ld\n", to_read);
            memcpy((void*)(buffer + bytes_read), (void*)(b_ptr + blk_offset), to_read);
            bytes_read += to_read;
            printf("[DEBUG] bytes successfully read: %ld\n", to_read);
        }
        else {
            return 0;
        }
    }
    return bytes_read;
}

int write_blocks(int inum, const char *buffer, size_t size, off_t offset) {
    printf("[DEBUG] inside write_blocks\n");
    void *disk_ptr = maindisk;
    struct wfs_inode inode;
    struct wfs_sb sb;
    off_t i_blocks_ptr, d_blocks_ptr, b_ptr;
    size_t bytes_written, to_write;
    int blk, blk_offset;
    int dnum;
    int new_dnum;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    i_blocks_ptr = (off_t)disk_ptr + sb.i_blocks_ptr;
    inode = fetch_inode(inum);
    if (!S_ISREG(inode.mode)) {
        printf("[DEBUG] incorrect mode - can only write to file\n");
        return 0;
    }

    bytes_written = 0;
    printf("[DEBUG] size: %ld\n", size);
    printf("[DEBUG] offset: %ld\n", offset);
    while (bytes_written < size) {
        blk = (offset + bytes_written) / BLOCK_SIZE;
        blk_offset = (offset + bytes_written) % BLOCK_SIZE;
        if (blk < N_BLOCKS) {
            new_dnum = alloc_datablock();
            inode.blocks[blk] = new_dnum;
            memcpy_v((i_blocks_ptr + (inode.num * BLOCK_SIZE)), &inode, sizeof(struct wfs_inode), 1);
            dnum = raid0_offset(new_dnum);
            d_blocks_ptr = (off_t)disk_ptrs[raid0_disk(new_dnum)] + sb.d_blocks_ptr;
            b_ptr = d_blocks_ptr + (dnum * BLOCK_SIZE);
            to_write = ((size - bytes_written) / BLOCK_SIZE) >= 1 ? BLOCK_SIZE : (size - bytes_written);
            printf("[DEBUG] bytes to write: %ld\n", to_write);
            memcpy_v(b_ptr + blk_offset, (void*)(buffer + bytes_written), to_write, 0);
            bytes_written += to_write;
            printf("[DEBUG] bytes successfully written: %ld\n", to_write);
        }
        else {
            return 0;
        }
    }
    inode.mtim = time(NULL);
    inode.size += size;
    memcpy_v((i_blocks_ptr + (inode.num * BLOCK_SIZE)), &inode, sizeof(struct wfs_inode), 1);
    return bytes_written;
}

int read_dentries(int inum, char *buffer, fuse_fill_dir_t filler) {
    printf("[DEBUG] inside read_dentries\n");
    void *disk_ptr = maindisk;
    struct wfs_inode inode;
    struct wfs_sb sb;
    struct wfs_dentry dentry;
    off_t d_blocks_ptr;
    int blk;
    int dnum;
    int i;

    memcpy(&sb, disk_ptr, sizeof(struct wfs_sb));
    inode = fetch_inode(inum);

    i = 0;
    while (i < N_BLOCKS) {
        blk = inode.blocks[i];
        printf("block: %d\n", blk);
        if (blk != -1) {
            dnum = raid0_offset(blk);
            d_blocks_ptr = (off_t)disk_ptrs[raid0_disk(blk)] + sb.d_blocks_ptr;
            off_t start = d_blocks_ptr + (dnum * BLOCK_SIZE);
            for (off_t ptr = start; ptr < start + BLOCK_SIZE; ptr += sizeof(struct wfs_dentry)) {
                memcpy(&dentry, (void*)ptr, sizeof(struct wfs_dentry));
                if (dentry.num != -1) {
                    filler(buffer, dentry.name, NULL, 0);
                }
            }
        }
        i++;
    }
    filler(buffer, ".", NULL, 0);
    filler(buffer, "..", NULL, 0);
    printf("[DEBUG] successfully fetched dentries\n");
    return 1;
}

static int wfs_getattr(const char *path, struct stat *stbuf) {
    printf("\n******* inside getattr *******\n");
    struct wfs_inode inode;
    struct timespec tim;
    int inum;

    memset(stbuf, 0, sizeof(struct stat));

    printf("[DEBUG] path %s\n", path);
    if ((inum = validatepath(path)) == -1) {
        return -ENOENT;
    }
    inode = fetch_inode(inum);
    printf("[DEBUG] fetching inode %d\n", inode.num);
    stbuf->st_uid = inode.uid;
    stbuf->st_gid = inode.gid;
    stbuf->st_mode = inode.mode;
    stbuf->st_size = inode.size;
    tim.tv_sec = inode.atim;
    stbuf->st_atim = tim;
    tim.tv_sec = inode.mtim;
    stbuf->st_mtim = tim;
    printf("[DEBUG] populated stbuf %d\n", inode.num);
    printf("[DEBUG] size %ld\n", stbuf->st_size);
    printf("[DEBUG] mode %d\n", stbuf->st_mode);
    printf("[DEBUG] uid %d\n", stbuf->st_uid);
    printf("[DEBUG] gid %d\n", stbuf->st_gid);
    return 0;
}

static int wfs_mknod(const char *path, mode_t mode, dev_t rdev) {
    printf("\n******* inside mknod *******\n");
    int p_inum;
    int existing_inum;
    void *curr_disk = maindisk;
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

    memcpy(&sb, curr_disk, sizeof(struct wfs_sb));
    i_blocks_ptr = (off_t)curr_disk + sb.i_blocks_ptr;
    if ((p_inum = validatepath(parentpath)) == -1) {
        return -ENOENT;
    }
    if ((existing_inum = data_exists(name, p_inum)) != -1) {
        existing_inode = fetch_inode(existing_inum);
        if (S_ISREG(existing_inode.mode))
            return -EEXIST;
    }
    if ((new_inode = alloc_inode(file_mode)) == 0) {
        printf("[DEBUG] no more space for file inode\n");
        return -ENOSPC;
    };
    if ((block_ptr = fetch_available_block(p_inum)) == 0) {
        printf("[DEBUG] no more space for file datablock\n");
        return -ENOSPC;
    }
    struct wfs_dentry new_dentry = {
        .num = new_inode->num
    };
    strcpy(new_dentry.name, name);
    memcpy_v((off_t)block_ptr, &new_dentry, sizeof(struct wfs_dentry), 0);
    p_inode = fetch_inode(p_inum);
    /*p_inode.size += sizeof(new_dentry);*/
    p_inode.mtim = time(NULL);
    /*p_inode.nlinks++;*/
    memcpy_v((i_blocks_ptr + (p_inode.num * BLOCK_SIZE)), &p_inode, sizeof(struct wfs_inode), 1);
    printf("[DEBUG] successfully created new file\n");
    return 0;
}

static int wfs_mkdir(const char *path, mode_t mode) {
    printf("\n******* inside mkdir *******\n");
    int p_inum;
    int existing_inum;
    void *curr_disk = maindisk;
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

    memcpy(&sb, curr_disk, sizeof(struct wfs_sb));
    i_blocks_ptr = (off_t)curr_disk + sb.i_blocks_ptr;
    if ((p_inum = validatepath(parentpath)) == -1) {
        return -ENOENT;
    }
    if ((existing_inum = data_exists(name, p_inum)) != -1) {
        existing_inode = fetch_inode(existing_inum);
        if (S_ISDIR(existing_inode.mode))
            return -EEXIST;
    }
    if ((new_inode = alloc_inode(dir_mode)) == 0) {
        printf("[DEBUG] no more space for dir inode\n");
        return -ENOSPC;
    };
    if ((block_ptr = fetch_available_block(p_inum)) == 0) {
        printf("[DEBUG] no more space for dir datablock\n");
        return -ENOSPC;
    }
    struct wfs_dentry new_dentry = {
        .num = new_inode->num
    };
    strcpy(new_dentry.name, name);
    memcpy_v((off_t)block_ptr, &new_dentry, sizeof(struct wfs_dentry), 0);
    p_inode = fetch_inode(p_inum);
    /*p_inode.size += sizeof(new_dentry);*/
    p_inode.mtim = time(NULL);
    /*p_inode.nlinks++;*/
    memcpy_v((i_blocks_ptr + (p_inode.num * BLOCK_SIZE)), &p_inode, sizeof(struct wfs_inode), 1);
    printf("[DEBUG] successfully created new directory\n");
    return 0;
}

static int wfs_unlink(const char *path) {
    printf("\n******* inside unlink *******\n");
    int inum, p_inum;
    struct wfs_inode inode;
    const char *name;
    const char *parentpath;

    if (path == NULL || strlen(path) == 0) {
        return -ENOENT;
    }
    name = getname(path);
    parentpath = getparentpath(path);

    if ((inum = validatepath(path)) == -1) {
        return -ENOENT;
    }
    inode = fetch_inode(inum);
    if (!S_ISREG(inode.mode)) {
        return -ENOENT;
    }
    p_inum = validatepath(parentpath);
    if (free_file(inum, p_inum, name) != 1) {
        return -ENOENT;
    }
    printf("[DEBUG] successfully removed file\n");
    return 0;
}

static int wfs_rmdir(const char *path) {
    printf("\n******* inside rmdir *******\n");
    int inum, p_inum;
    struct wfs_inode inode;
    const char *name;
    const char *parentpath;

    if (path == NULL || strlen(path) == 0) {
        return -ENOENT;
    }
    name = getname(path);
    parentpath = getparentpath(path);

    if ((inum = validatepath(path)) == -1) {
        return -ENOENT;
    }
    inode = fetch_inode(inum);
    if (!S_ISDIR(inode.mode)) {
        return -ENOENT;
    }
    if (!isdirempty(inum)) {
        return -ENOTEMPTY;
    }
    p_inum = validatepath(parentpath);
    if (free_dir(inum, p_inum, name) != 1) {
        return -ENOENT;
    }
    printf("[DEBUG] successfully removed directory\n");
    return 0;
}

static int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    printf("\n******* inside read *******\n");
    int inum;
    size_t bytes_read;

    if (path == NULL || strlen(path) == 0) {
        return -ENOENT;
    }

    if ((inum = validatepath(path)) == -1) {
        return -ENOENT;
    }
    if ((bytes_read = read_blocks(inum, buf, size, offset)) == 0) {
        return -ENOENT;
    }

    printf("[DEBUG] successfully read file (%zu)\n", bytes_read);
    return bytes_read;
}

static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    printf("\n******* inside write *******\n");
    int inum;
    size_t bytes_written;

    if (path == NULL || strlen(path) == 0) {
        return -ENOENT;
    }

    if ((inum = validatepath(path)) == -1) {
        return -ENOENT;
    }
    if ((bytes_written = write_blocks(inum, buf, size, offset)) == 0) {
        return -ENOENT;
    }

    printf("[DEBUG] successfully wrote file (%zu)\n", bytes_written);
    return bytes_written;
}

static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi) {
    printf("\n******* inside readdir *******\n");
    int inum;
    const char *parentpath;

    if (path == NULL || strlen(path) == 0) {
        return -ENOENT;
    }
    parentpath = getparentpath(path);

    if ((inum = validatepath(parentpath)) == -1) {
        return -ENOENT;
    }
    if (read_dentries(inum, buf, filler) != 1) {
        printf("[DEBUG] failed to read dentries\n");
        return -ENOENT;
    }

    printf("[DEBUG] succesfully read directory entries\n");
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
    // set main disk
    maindisk = disk_ptrs[0];

    umask(0);
    return fuse_main(fuse_argc, fuse_argv, &ops, NULL);
}
