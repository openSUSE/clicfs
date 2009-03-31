#include "doenerfs.h"
#include <stdio.h>
#include <stdlib.h>

int preset = 0;
FILE *packfile = 0;

char thefile[PATH_MAX];
size_t thefilesize = 0;
uint64_t *sizes = 0;
uint64_t *offs = 0;
uint32_t parts = 0;
uint32_t pindex = 0;
size_t bsize = 0;
struct block *blocks;

static uint32_t readindex()
{
    uint32_t stringlen;
    if (fread((char*)&stringlen, sizeof(uint32_t), 1, packfile) != 1) {
        return 0;
    }
    return stringlen;
}

int doenerfs_read_pack(const char *packfilename)
{
    if (!packfilename) {
	fprintf(stderr, "usage: [-m <mb] <packfile> <mntpoint>\n");
        return 1;
    }
    packfile = fopen(packfilename, "r");
    if (!packfile) {
        fprintf(stderr, "packfile %s can't be opened\n", packfilename);
        return 1;
    }
    fseek(packfile, 2, SEEK_SET);

    uint32_t stringlen = readindex();
    if (stringlen == 0) {
	fprintf(stderr, "abnormal len 0\n"); 
        return 1;
    }
    if (fread(thefile, 1, stringlen, packfile) != stringlen) {
	fprintf(stderr, "short read\n");
	return 1;
    }
    thefile[stringlen] = 0;

    parts = readindex();
    bsize = readindex();
    thefilesize = readindex();
    preset = readindex();
    pindex = readindex();
    blocks = malloc(sizeof(struct block)*pindex);

    uint32_t i;
    for (i = 0; i < pindex; ++i) {
	blocks[i].mapped = i;
	blocks[i].orig = readindex();
    }

    qsort(blocks, pindex, sizeof(struct block), block_cmp);

    //fprintf(stderr, "file %ld %ld %ld %d\n", thefilesize, sparse_memory, bsize, parts);

    sizes = malloc(sizeof(uint64_t)*parts);
    offs = malloc(sizeof(uint64_t)*parts);

    for (i = 0; i < parts; ++i)
    {
	if (fread((char*)(sizes + i), sizeof(uint64_t), 1, packfile) != 1)
		parts = 0;
	if (!sizes[i]) {
		fprintf(stderr, "unreasonable size 0 for part %d\n", i);
		return 1;
        }
	if (fread((char*)(offs + i), sizeof(uint64_t), 1, packfile) != 1)
		parts = 0;
    }
    if (parts == 0) {
        fprintf(stderr, "unreasonable part number 0\n");
	return 1;
    }


    return 0;
}
