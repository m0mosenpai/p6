BINS = wfs mkfs
CC = gcc
CFLAGS = -luuid -Wall -Werror -pedantic -std=gnu18 -g
FUSE_CFLAGS = `pkg-config fuse --cflags --libs`

.PHONY: all
all: $(BINS)

wfs:
	$(CC) $(CFLAGS) wfs.c $(FUSE_CFLAGS) -o wfs
mkfs:
	$(CC) $(CFLAGS) -o mkfs mkfs.c

.PHONY: clean
clean:
	rm -rf $(BINS)
