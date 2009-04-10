#include "doenerfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <lzma.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

int preset = 0;
FILE *packfile = 0;
FILE *cowfile = 0;

char thefile[PATH_MAX];
size_t thefilesize = 0;
uint64_t *sizes = 0;
uint64_t *offs = 0;
uint32_t parts = 0;
uint32_t pindex = 0;
size_t bsize = 0;
unsigned char **blockmap;
size_t num_pages = 0;

static uint32_t readindex(FILE *f)
{
    uint32_t stringlen;
    if (fread((char*)&stringlen, sizeof(uint32_t), 1, f) != 1) {
        return 0;
    }
    return stringlen;
}

int doenerfs_read_cow(const char *cowfilename)
{
    cowfile = fopen(cowfilename, "r");
    if (!cowfile) {
	fprintf(stderr, "cowfile %s can't be opened\n", cowfilename);
        return 1;
    }
    struct stat st;
    stat(cowfilename, &st);
    fseek(cowfile, st.st_size - sizeof(uint32_t), SEEK_SET);
    uint32_t indexlen = readindex(cowfile) + sizeof(uint32_t);
    fprintf(stderr, "index %ld %ld\n", (long)indexlen, ftell(cowfile));
    if (fseek(cowfile, st.st_size - indexlen, SEEK_SET ))
	perror("seek");
    fprintf(stderr, "index %ld %ld\n", (long)indexlen, ftell(cowfile));
    thefilesize = readindex(cowfile);
    fprintf(stderr, "size %ld\n", (long)thefilesize);
    uint32_t newpages = thefilesize / 4096;
    blockmap = realloc(blockmap, sizeof(unsigned char*)*newpages);
    uint32_t i;
    for (i = num_pages; i < newpages; ++i)
	blockmap[i] = 0;
    uint32_t cowpages = readindex(cowfile);
    fprintf(stderr, "cows %ld\n", (long)cowpages);
    for (i = 0; i < cowpages; ++i)
    {
	uint32_t pageindex = readindex(cowfile);
	assert(pageindex < num_pages);
	blockmap[i] = (unsigned char*)(long)(pageindex << 2) + 2;
    }
    return 0;
}

int doenerfs_read_pack(const char *packfilename)
{
    packfile = fopen(packfilename, "r");
    if (!packfile) {
        fprintf(stderr, "packfile %s can't be opened\n", packfilename);
        return 1;
    }
    char head[5];
    char expected[5];
    fread(head, 1, 4, packfile);
    head[4] = 0;
    sprintf(expected, "SK%02d", DOENER_MAGIC);
    if (strcmp(head,expected)) {
	fprintf(stderr, "wrong magic: %s vs %s\n", head, expected);
	return 1;
    }

    uint32_t stringlen = readindex(packfile);
    if (stringlen == 0) {
	fprintf(stderr, "abnormal len 0\n"); 
        return 1;
    }
    if (fread(thefile, 1, stringlen, packfile) != stringlen) {
	fprintf(stderr, "short read\n");
	return 1;
    }
    thefile[stringlen] = 0;

    size_t oparts = readindex(packfile);
    bsize = readindex(packfile);
    thefilesize = readindex(packfile);
    preset = readindex(packfile);
    num_pages = readindex(packfile);
    blockmap = malloc(sizeof(unsigned char*)*num_pages);

    uint32_t i;
    for (i = 0; i < num_pages; ++i) {
	// make sure it's odd to diff between pointer and block
	blockmap[i] = (unsigned char*)(long)((readindex(packfile) << 2) + 1);
    }

    parts = readindex(packfile);
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
    fseek(packfile, (oparts-parts)*sizeof(uint64_t)*2, SEEK_CUR);

    return 0;
}

off_t doener_map_block(off_t block)
{
    unsigned char *ptr = blockmap[block];
    size_t ret = (long)ptr;
    // calling map_block for detached blocks is bogus
    assert((ret & 0x3) == 1);
    return ret >> 2;
}

size_t doener_readpart(unsigned char *buffer, int part)
{
    if (fseek(packfile, offs[part], SEEK_SET)) {
	fprintf(stderr, "seek failed %d %ld\n", part, (long)offs[part]);
	return 0;
    }
#if defined(DEBUG)
    fprintf(logger, "uncompress part=%d/%d com=%d off=%ld size=%ld ioff=%ld size=%ld\n", part, parts, com->index, offs[part], sizes[part], ioff, size );
#endif
    size_t readin = fread(buffer, 1, sizes[part], packfile);
    if (readin != sizes[part]) {
	fprintf(stderr, "short read: %d %ld %ld %ld\n", part, (long)offs[part], (long)sizes[part], (long)readin);
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
