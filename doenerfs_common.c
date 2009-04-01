#include "doenerfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <lzma.h>

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

off_t doener_map_block(off_t block)
{
    int ret = binary_search(blocks, pindex, block);
    
    if (blocks[ret].orig == block) { // in index
	return blocks[ret].mapped;
    } else {
	// now the tricky part. If it's not in the index, it's
	// after the indexed blocks - the missing blocks
	return block - ret + pindex - 1;
    }
}

size_t doener_readpart(unsigned char *buffer, int part)
{
    if (fseek(packfile, offs[part], SEEK_SET)) {
	fprintf(stderr, "seek failed %d %ld\n", part, offs[part]);
	return 0;
    }
#if defined(DEBUG)
    fprintf(logger, "uncompress part=%d/%d com=%d off=%ld size=%ld ioff=%ld size=%ld\n", part, parts, com->index, offs[part], sizes[part], ioff, size );
#endif
    size_t readin = fread(buffer, 1, sizes[part], packfile);
    if (readin != sizes[part]) {
	fprintf(stderr, "short read: %d %ld %ld %ld\n", part, offs[part], sizes[part], readin);
    }
    return readin;
}

void doener_decompress_part(unsigned char *out, const unsigned char *in, size_t readin)
{
    const uint32_t flags = LZMA_TELL_UNSUPPORTED_CHECK | LZMA_CONCATENATED;
    lzma_stream strm = LZMA_STREAM_INIT;

    lzma_ret ret = lzma_auto_decoder(&strm, lzma_easy_decoder_memusage(preset), flags); 

    strm.next_in = in;
    strm.avail_in = readin;
    strm.next_out = out;
    strm.avail_out = bsize;

    while (1) {
	ret = lzma_code(&strm, LZMA_RUN);
//	fprintf(logger, "ret %d\n", ret);
	if (ret != LZMA_OK)
	    break;
    }

    //assert (ret == LZMA_OK);
    lzma_end(&strm);
}
