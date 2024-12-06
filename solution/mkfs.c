#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "wfs.h"

void freev(void **ptr, int len, int free_seg) {
    if (len < 0) while (*ptr) { free(*ptr); *ptr++ = NULL; }
    else { for (int i = 0; i < len; i++) free(ptr[i]); }
    if (free_seg) free(ptr);
}

void generate_id(int disk_index, char *id, size_t size) {
    time_t ctime = time(NULL);
    srand((unsigned) clock());
    int random = rand();
    snprintf(id, size, "%d-%ld-%d", disk_index, (long)ctime, random);
}

int roundup(int n, int k) {
    int r = n % k;
    if (r == 0) return n;
    return n + k - r;
}

int main(int argc, char *argv[]) {
    if (argc <= 1) {
        return -1;
    }

    int i;
    long inodes = -1;
    long blocks = -1;
    char **disks = calloc(MIN_DISKS, sizeof(char*));
    int ndisks = MIN_DISKS;
    int dcnt = 0;
    char *endptr, *str;
    DiskMode raid;

    // ./mkfs -r 1 -d disk1 -d disk2 -i 32 -b 200
    for (i = 1; i < argc - 1; i++) {
        errno = 0;
        if (strcmp(argv[i], "-r") == 0) {
            str = argv[i + 1];
            if (strcmp(str, "0") == 0) raid = RAID_0;
            else if (strcmp(str, "1") == 0) raid = RAID_1;
            else if (strcmp(str, "1v") == 0) raid = RAID_1v;
            else {
                freev((void*)disks, ndisks, 1);
                return 1;
            }
        }
        else if (strcmp(argv[i], "-d") == 0) {
            str = argv[i + 1];
            if (dcnt >= ndisks) {
                ndisks *= 2;
                disks = reallocarray(disks, ndisks, sizeof(char*));
            }
            disks[dcnt] = malloc(strlen(str) + 1);
            strcpy(disks[dcnt], str);
            dcnt++;
        }
        else if (strcmp(argv[i], "-i") == 0) {
            str = argv[i + 1];
            inodes = strtol(str, &endptr, 10);
            if (errno != 0 || endptr == str || *endptr != '\0' || inodes <= 0) {
                freev((void*)disks, ndisks, 1);
                return 1;
            }
        }
        else if (strcmp(argv[i], "-b") == 0) {
            str = argv[i + 1];
            blocks = strtol(str, &endptr, 10);
            if (errno != 0 || endptr == str || *endptr != '\0' || blocks <= 0) {
                freev((void*)disks, ndisks, 1);
                return 1;
            }
        }
        else {
            freev((void*)disks, ndisks, 1);
            return 1;
        }
        i += 1;
    }

    if (inodes <= 0 || blocks <= 0 || dcnt < 2) {
        freev((void*)disks, ndisks, 1);
        return 1;
    }

    // round up inodes and blocks to multiple of 32
    inodes = roundup(inodes, 32);
    blocks = roundup(blocks, 32);

    int inodesize, dblocksize;
    ssize_t req_totalsize;
    char id[DISK_ID_SIZE];
    char disk_ids[MAX_DISKS][DISK_ID_SIZE];

    for (i = 0; i < dcnt; i++) {
        generate_id(i+1, id, sizeof(id));
        strcpy(disk_ids[i], id);
    }

    for (i = 0; i < dcnt; i++) {
        FILE *disk = fopen(disks[i], "r+b");
        if (disk == NULL) {
            freev((void*)disks, ndisks, 1);
            return -1;
        }

        // init inode bitmap & set root inode
        inodesize = roundup(inodes, 8) / 8;
        unsigned char inodebitmap[inodesize];
        memset(inodebitmap, 0, inodesize);
        inodebitmap[0] = 1;

        // init data bitmap
        dblocksize = roundup(blocks, 8) / 8;
        unsigned char dbitmap[dblocksize];
        memset(dbitmap, 0, dblocksize);

        // init root inode
        time_t ctime;
        ctime = time(NULL);
        struct wfs_inode root = {
            .num = 0,
            .mode = S_IFDIR | 755,
            .uid = getuid(),
            .gid = getgid(),
            .size = 0,
            .nlinks = 1,
            .atim = ctime,
            .mtim = ctime,
            .ctim = ctime,
        };
        memset(root.blocks, -1, N_BLOCKS*(sizeof(off_t)));

        // init superblock
        struct wfs_sb superblock = {
            .num_inodes = inodes,
            .num_data_blocks = blocks,
            .i_bitmap_ptr = sizeof(struct wfs_sb),
            .d_bitmap_ptr = superblock.i_bitmap_ptr + sizeof(inodebitmap),
            .i_blocks_ptr = roundup(superblock.d_bitmap_ptr + sizeof(dblocksize), BLOCK_SIZE),
            .d_blocks_ptr = superblock.i_blocks_ptr + inodes*BLOCK_SIZE,
            .raid = raid,
            .num_disks = dcnt
        };
        strcpy(superblock.id, disk_ids[i]);
        for (int j = 0; j < dcnt; j++) {
            strcpy(superblock.disks[j], disk_ids[j]);
        }

        req_totalsize = sizeof(struct wfs_sb) + sizeof(inodebitmap) + sizeof(dbitmap) + inodes*BLOCK_SIZE + blocks*BLOCK_SIZE;
        fseek(disk, 0, SEEK_END);
        if (ftell(disk) < req_totalsize) {
            fclose(disk);
            freev((void*)disks, ndisks, 1);
            return -1;
        }
        fseek(disk, 0, SEEK_SET);

        // superblock
        if (fwrite(&superblock, sizeof(struct wfs_sb), 1, disk) != 1) {
            fclose(disk);
            freev((void*)disks, ndisks, 1);
            return -1;
        }

        // inode bitmap
        fseek(disk, superblock.i_bitmap_ptr, SEEK_SET);
        if (fwrite(&inodebitmap, sizeof(inodebitmap), 1, disk) != 1) {
            fclose(disk);
            freev((void*)disks, ndisks, 1);
            return -1;
        }

        // data bitmap
        fseek(disk, superblock.d_bitmap_ptr, SEEK_SET);
        if (fwrite(&dbitmap, sizeof(dbitmap), 1, disk) != 1) {
            fclose(disk);
            freev((void*)disks, ndisks, 1);
            return -1;
        }

        // root inode
        fseek(disk, superblock.i_blocks_ptr, SEEK_SET);
        if (fwrite(&root, BLOCK_SIZE, 1, disk) != 1) {
            fclose(disk);
            freev((void*)disks, ndisks, 1);
            return -1;
        }

        fclose(disk);
    }

    freev((void*)disks, ndisks, 1);
    return 0;
}
