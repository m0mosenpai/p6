#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "wfs.h"

#define MIN_DISKS 2

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
    long raid = -1;
    long inodes = -1;
    long blocks = -1;
    char **disks = malloc(MIN_DISKS * sizeof(char*));
    int ndisks = MIN_DISKS;
    int dcnt = 0;
    char *endptr, *str;

    // ./mkfs -r 1 -d disk1 -d disk2 -i 32 -b 200
    for (i = 1; i < argc - 1; i++) {
        errno = 0;
        if (strcmp(argv[i], "-r") == 0) {
            str = argv[i + 1];
            raid = strtol(str, &endptr, 10);
            if (errno != 0 || *endptr != '\0' || (raid < 0 || raid > 1)) {
                return -1;
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
                return -1;
            }
        }
        else if (strcmp(argv[i], "-b") == 0) {
            str = argv[i + 1];
            blocks = strtol(str, &endptr, 10);
            if (errno != 0 || endptr == str || *endptr != '\0' || blocks <= 0) {
                return -1;
            }
        }
        else {
            return -1;
        }
        i += 1;
    }

    if (raid < 0 || inodes <= 0 || blocks <= 0 || dcnt < 2) {
        return -1;
    }

    // round up blocks to multiple of 32
    blocks = roundup(blocks, 32);
    /*if (blocks % 32 != 0) {*/
    /*    blocks = blocks + 32 - (blocks % 32);*/
    /*}*/

    int inodesize, dblocksize;
    /*int niblocks, ndblocks;*/
    /*int ibmapsize, dbmapsize;*/

    for (i = 0; i < dcnt; i++) {
        FILE *disk = fopen(disks[i], "wb");
        if (disk == NULL) {
            return -1;
        }

        inodesize = inodes / 8;
        /*niblocks = inodesize / BLOCK_SIZE + 1;*/
        /*ibmapsize = niblocks * BLOCK_SIZE;*/
        dblocksize = blocks / 8;
        /*ndblocks = dblocksize / BLOCK_SIZE + 1;*/
        /*dbmapsize = ndblocks * BLOCK_SIZE;*/

        // init superblock
        struct wfs_sb superblock = {
            .num_inodes = inodes,
            .num_data_blocks = blocks,
            .i_bitmap_ptr = BLOCK_SIZE,
            .d_bitmap_ptr = superblock.i_bitmap_ptr + inodesize,
            .i_blocks_ptr = roundup(superblock.d_bitmap_ptr + dblocksize, BLOCK_SIZE),
            .d_blocks_ptr = superblock.i_blocks_ptr + inodes*BLOCK_SIZE,
        };
        if (fwrite(&superblock, BLOCK_SIZE, 1, disk) != 1) {
            return -1;
        }

        // init inode bitmap & set root inode
        unsigned char inodebitmap[inodesize];
        memset(inodebitmap, 0, inodesize);
        inodebitmap[0] = 1;
        /*if (fwrite(&inodebitmap, ibmapsize, 1, disk) != 1) {*/
        if (fwrite(&inodebitmap, sizeof(inodebitmap), 1, disk) != 1) {
            return -1;
        }

        // init data bitmap
        unsigned char dbitmap[dblocksize];
        memset(dbitmap, 0, dblocksize);
        /*if (fwrite(&dbitmap, dbmapsize, 1, disk) != 1) {*/
        if (fwrite(&dbitmap, sizeof(dbitmap), 1, disk) != 1) {
            return -1;
        }

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
        memset(root.blocks, 0, N_BLOCKS*(sizeof(off_t)));
        off_t fpos = roundup(ftell(disk), BLOCK_SIZE);
        fseek(disk, fpos, SEEK_CUR);
        if (fwrite(&root, BLOCK_SIZE, 1, disk) != 1) {
            return -1;
        }
        fclose(disk);
    }
    /*printf("successfully written!\n");*/

    return 0;
}
