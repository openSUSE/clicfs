#define _GNU_SOURCE 
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <lzma.h>
#include <limits.h>

static size_t compress(int preset, unsigned char *in, size_t insize, unsigned char *out, size_t outsize)
{
    lzma_stream strm = LZMA_STREAM_INIT;
    lzma_ret ret = lzma_easy_encoder(&strm, preset, LZMA_CHECK_CRC32);
    assert(ret == LZMA_OK);

    strm.next_in = in;
    strm.avail_in = insize;
    strm.next_out = out;
    strm.avail_out = outsize;

    while (1) {
	ret = lzma_code(&strm, LZMA_RUN);
	//fprintf(stderr, "ret %d\n", ret);
	if (ret != LZMA_OK)
	    break;
    }
    
    ret = lzma_code(&strm, LZMA_FINISH);
    //fprintf(stderr, "ret %d\n", ret);

    assert (ret == LZMA_STREAM_END);
    lzma_end(&strm);

    return strm.total_out;
}

int main(int argc, char **argv)
{
    if (argc < 4 ) {
	fprintf(stderr, "usage: %s <infile> <outfile> <blocksize> [preset] [profile]\n", argv[0]);
	return 1;
    }
    const char *infile = argv[1];
    const char *outfile = argv[2];
    int blocksize = atoi(argv[3]);

    if (blocksize % 4096) {
	fprintf(stderr, "blocksize needs to be 4096*x\n");
	return 1;
    }

    int preset = 2;
    const char *profile = 0;

    if (argc > 4)
    {
	preset = atoi(argv[4]);
    }

    if (argc > 5)
    {
	profile = argv[5];
    }

    struct stat st;
    stat(infile, &st);

    long parts = st.st_size / blocksize;
    long blocks = st.st_size / 4096;
    /* ext3 should be X blocks */
    if (blocks * 4096 != st.st_size)
	blocks++;
    
    uint32_t *found = malloc(sizeof(uint32_t)*blocks);
    memset(found, 0, sizeof(int)*blocks);
    uint32_t *ublocks = malloc(sizeof(uint32_t)*blocks);
    memset(ublocks, 0, sizeof(int)*blocks);

    long pindex = 0;
    // always take the first block to make the algorithm easier
    ublocks[pindex++] = 0;
    found[0] = pindex;
    int i;
    if (profile) {
	FILE *pr = fopen(profile, "r");
	assert(pr);
	char line[200];
	while (fgets(line, sizeof(line)-1, pr)) {
	    char fname[PATH_MAX];
	    long offset, size;
	    unsigned int addr;
	    if (strncmp(line, "read ", 5))
		continue;
	    if (sscanf(line, "read %s %ld %ld %x", fname, &offset, &size, &addr) == 4) {
		size = size / 4096;
		offset = offset / 4096;
		for (i = 0; i < size; i++) {
		    if (offset + i < blocks && found[offset+i] == 0) {
			ublocks[pindex++] = offset + i;
			found[offset + i] = pindex;
			//fprintf(stderr, "read %ld\n", offset + i);
			// we only care for the first 250MB - to save memory later
			if (pindex >= 64000)
			    break;
		    }
		}
	    }
	    if (pindex >= 64000)
		break;
	}
	fclose(pr);
    }

    fprintf(stderr, "pindex %ld %ld\n", pindex, blocks);

    //binary_search(ublocks, pindex, 267413);
    
    if (parts * blocksize != st.st_size)
	parts++;

    FILE *in = fopen(infile, "r");
    FILE *out = fopen(outfile, "w");

    unsigned char inbuf[blocksize];
    unsigned char outbuf[blocksize + 300];

    size_t total_in = 0;
    size_t total_out = 0;
    
    uint64_t *sizes = malloc(sizeof(uint64_t)*parts);
    uint64_t *offs = malloc(sizeof(uint64_t)*parts);

    off_t index_off = 2;
    int lastpercentage = 0;

    fwrite("SK", 1, 2, out);

    char fname[PATH_MAX];
    strcpy(fname, basename(infile));
    uint32_t stringlen = strlen(fname);
    fwrite((char*)&stringlen, 1, sizeof(uint32_t), out);
    fwrite(fname, 1, stringlen, out);
    index_off += sizeof(uint32_t) + stringlen;

    stringlen = parts;
    fwrite((char*)&stringlen, 1, sizeof(uint32_t), out);
    index_off += sizeof(uint32_t);

    stringlen = blocksize;
    fwrite((char*)&stringlen, 1, sizeof(uint32_t), out);
    index_off += sizeof(uint32_t);

    stringlen = st.st_size;
    fwrite((char*)&stringlen, 1, sizeof(uint32_t), out);
    index_off += sizeof(uint32_t);

    stringlen = preset;
    fwrite((char*)&stringlen, 1, sizeof(uint32_t), out);
    index_off += sizeof(uint32_t);

    stringlen = pindex;
    fwrite((char*)&stringlen, 1, sizeof(uint32_t), out);
    index_off += sizeof(uint32_t);

    for (i = 0; i < pindex; ++i)
    {
	stringlen = ublocks[i];
	fwrite((char*)&stringlen, 1, sizeof(uint32_t), out);
	index_off += sizeof(uint32_t);
    }

    index_off += 2 * parts * sizeof(uint64_t);
    fseek(out, index_off, SEEK_SET);

    uint32_t blocksperpart = blocksize/4096;

    uint32_t rindex = 0; // overall index
    uint32_t uindex = 0; // index for "unused" blocks

    for (i = 0; i < parts; ++i)
    {
	uint b;
	size_t readin = 0;
	for (b=0;b<blocksperpart;b++) {
	    if (rindex < pindex) {
		fseek(in, ublocks[rindex] * 4096, SEEK_SET);
	    } else {
		while (found[uindex])  uindex++;
		fseek(in, uindex * 4096, SEEK_SET);
		uindex++;
	    }
	    readin += fread(inbuf+readin, 1, 4096, in);
	    rindex++;

	}
	size_t outsize = compress(preset, inbuf, readin, outbuf, blocksize + 300);
	sizes[i] = outsize;
	offs[i] = total_out + index_off;
	total_in += readin;
	total_out += outsize;
	fwrite(outbuf, outsize, 1, out);
        if ((int)(i * 100. / parts) > lastpercentage) {
            fprintf(stderr, "part %d/%ld %d%% (total %d%%)\n", i, parts, (int)(outsize * 100 / readin), (int)(total_out * 100 / total_in));
            lastpercentage++;
        }
    }

    fseek(out, index_off - 2 * parts * sizeof(uint64_t), SEEK_SET);
    for (i = 0; i < parts; ++i)
    {
	fwrite((char*)(sizes + i), 1, sizeof(uint64_t), out);
	fwrite((char*)(offs + i), 1, sizeof(uint64_t), out);
    }
    fclose(out);

    return 0;
}
