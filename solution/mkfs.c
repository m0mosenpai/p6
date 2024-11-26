#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#define MIN_DISKS 2

int main(int argc, char *argv[]) {
    if (argc <= 1) return -1;

    int i;
    long raid = -1;
    long inodes = -1;
    long blocks = -1;
    char **disks = malloc(MIN_DISKS * sizeof(char*));
    int ndisks = MIN_DISKS;
    char *endptr, *str;
    int dcnt = 0;

    // ./mkfs -r 1 -d disk1 -d disk2 -i 32 -b 200
    for (i = 1; i < argc; i++) {
        errno = 0;
        if (strcmp(argv[i], "-r") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            str = argv[i + 1];
            raid = strtol(str, &endptr, 10);
            if (errno != 0 || *endptr != '\0' || (raid < 0 || raid > 1)) {
                return -1;
            }
        }
        else if (strcmp(argv[i], "-d") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
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
            if (i + 1 >= argc) {
                return -1;
            }
            str = argv[i + 1];
            inodes = strtol(str, &endptr, 10);
            if (errno != 0 || endptr == str || *endptr != '\0' || inodes <= 0) {
                return -1;
            }
        }
        else if (strcmp(argv[i], "-b") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
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

    if (dcnt < 2)
        return -1;

    // round up blocks to multiple of 32
    if (blocks % 32 != 0) {
        blocks = blocks + 32 - (blocks % 32);
    }

    return 0;
}
