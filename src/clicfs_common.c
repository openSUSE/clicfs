/* This file is part of Clic FS
   Copyright (C) 2009 Stephan Kulow (coolo@suse.de)

   Clicfs is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public
   License as published by the Free Software Foundation, version 2.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA
*/

#define _GNU_SOURCE

#include "clicfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <lzma.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

int preset = 0;
int packfilefd = -1;
int cowfilefd = -1;

char thefile[PATH_MAX];
uint64_t thefilesize = 0;
size_t pagesize = 4096;
uint64_t *sizes = 0;
uint64_t *offs = 0;
uint32_t parts = 0;
uint32_t largeparts = 0;
uint32_t pindex = 0;
size_t blocksize_large = 0;
size_t blocksize_small = 0;
unsigned char **blockmap;
uint32_t num_pages = 0;
uint32_t cow_pages = 0;
uint32_t cow_index_pages = 0;
uint32_t *cows = 0;
unsigned int cows_index = 0;
int cowfile_ro = 0;

static lzma_stream strm;

uint32_t clic_readindex_fd(int fd)
{
    uint32_t stringlen = 0;
    if (read(fd, &stringlen, sizeof(uint32_t)) != sizeof(uint32_t)) {
        return 0;
    }
    return stringlen;
}

uint64_t clic_readindex_fd64(int fd)
{
    uint64_t stringlen = 0;
    if (read(fd, &stringlen, sizeof(uint64_t)) != sizeof(uint64_t)) {
        return 0;
    }
    return stringlen;
}

int clicfs_read_cow(const char *cowfilename)
{
    cowfilefd = open(cowfilename, O_RDWR);

    if (cowfilefd == -1 ) {

        cowfilefd = open(cowfilename, O_RDONLY);
	if (cowfilefd == -1) {
	  fprintf(stderr, "cowfile %s can't be opened\n", cowfilename);
	  return 1;
	} else {
	  cowfile_ro = 1;
	}
    } else
      cowfile_ro = 0;

    char head[10];
    char expected[10];
    if (read(cowfilefd, head, 9) != 9) {
	fprintf(stderr, "can't read from %s\n", cowfilename);
	return 1;
    }

    head[9] = 0;
    sprintf(expected, "CLICCOW%02d", DOENER_MAGIC);
    if (strcmp(head,expected)) {
	fprintf(stderr, "wrong magic: %s vs %s\n", head, expected);
	return 1;
    }
    
    thefilesize = clic_readindex_fd64(cowfilefd);
    uint32_t newpages = thefilesize / pagesize;
    blockmap = realloc(blockmap, sizeof(unsigned char*)*newpages);
    uint32_t i;
    for (i = num_pages; i < newpages; ++i)
	blockmap[i] = 0;
    num_pages = newpages;
    cow_pages = clic_readindex_fd(cowfilefd);
    for (i = 0; i < cow_pages; ++i)
    {
	uint32_t pageindex = clic_readindex_fd(cowfilefd);
	uint32_t page = clic_readindex_fd(cowfilefd);
	assert(pageindex < num_pages);
	blockmap[pageindex] = (unsigned char*)(long)(page << 2) + 2;
    }
    cows = malloc(sizeof(uint32_t) * CLICFS_COW_COUNT);
    cows_index = 0;
    
    uint32_t index_len = clic_readindex_fd(cowfilefd);
    cow_index_pages = index_len / pagesize + 1;

    return 0;
}

int clicfs_read_pack(const char *packfilename)
{
    packfilefd = open(packfilename, O_LARGEFILE|O_RDONLY);
    if (packfilefd < 0) {
        fprintf(stderr, "packfile %s can't be opened\n", packfilename);
        return 1;
    }
    char head[7];
    char expected[7];
    if (read(packfilefd, head, 6) != 6) {
      perror("can't read from packfile\n");
      return 1;
    }
    head[6] = 0;
    sprintf(expected, "CLIC%02d", DOENER_MAGIC);
    if (strcmp(head,expected)) {
	fprintf(stderr, "wrong magic: %s vs %s\n", head, expected);
	return 1;
    }

    uint32_t stringlen = clic_readindex_fd(packfilefd);
    if (stringlen == 0 || stringlen >= PATH_MAX) {
	fprintf(stderr, "abnormal len %lx\n", (long)stringlen); 
        return 1;
    }
    if (read(packfilefd, thefile, stringlen) != stringlen) {
	fprintf(stderr, "short read %ld\n", (long)stringlen);
	return 1;
    }
    thefile[stringlen] = 0;

    uint64_t oparts = clic_readindex_fd(packfilefd);
    blocksize_small = clic_readindex_fd(packfilefd);
    blocksize_large = clic_readindex_fd(packfilefd);
    pagesize = clic_readindex_fd(packfilefd);
    thefilesize = oparts * blocksize_small * pagesize;
    preset = clic_readindex_fd(packfilefd);
    num_pages = clic_readindex_fd(packfilefd);
    blockmap = malloc(sizeof(unsigned char*)*num_pages);

    uint32_t i;
    for (i = 0; i < num_pages; ++i) {
	// make sure it's odd to diff between pointer and block
	blockmap[i] = (unsigned char*)(long)((clic_readindex_fd(packfilefd) << 2) + 1);
    }

    parts = clic_readindex_fd(packfilefd);
    largeparts = clic_readindex_fd(packfilefd);
    sizes = malloc(sizeof(uint64_t)*parts);
    offs = malloc(sizeof(uint64_t)*parts);

    for (i = 0; i < parts; ++i)
    {
        if (read(packfilefd, (char*)(sizes + i), sizeof(uint64_t)) != sizeof(uint64_t))
		parts = 0;
	if (!sizes[i]) {
		fprintf(stderr, "unreasonable size 0 for part %d\n", i);
		return 1;
        }
	if (read(packfilefd, (char*)(offs + i), sizeof(uint64_t)) != sizeof(uint64_t))
		parts = 0;
	if (i > 0 && offs[i] <= offs[i-1]) {
	  fprintf(stderr, "the offset for i is not larger than i-1: %ld\n", (long)i);
	  return 1;
	}
    }
    if (parts == 0) {
        fprintf(stderr, "unreasonable part number 0\n");
	return 1;
    }
    lseek(packfilefd, (oparts-parts)*sizeof(uint64_t)*2, SEEK_CUR);

    const uint32_t flags = LZMA_TELL_UNSUPPORTED_CHECK | LZMA_CONCATENATED;
    // C sucks
    lzma_stream tmp = LZMA_STREAM_INIT;
    strm = tmp;
    lzma_ret ret = lzma_auto_decoder(&strm, lzma_easy_decoder_memusage(preset), flags);
    if (ret != LZMA_OK) {
      return 1;
    }

    return 0;
}

off_t clic_map_block(off_t block)
{
    unsigned char *ptr = blockmap[block];
    size_t ret = (long)ptr;
    // calling map_block for detached blocks is bogus
    assert(PTR_CLASS(ret) == 1);
    return ret >> 2;
}

size_t clic_readpart(unsigned char *buffer, int part)
{
#if defined(DEBUG)
    fprintf(stderr, "uncompress part=%d/%d off=%ld size=%ld\n", part, parts, offs[part], sizes[part] );
#endif
    ssize_t readin = pread(packfilefd, buffer, sizes[part], offs[part]);
    if (readin != (ssize_t)sizes[part]) {
	fprintf(stderr, "short read: %d %ld %ld %ld\n", part, (long)offs[part], (long)sizes[part], (long)readin);
	// returning half read blocks won't help lzma
	return 0;
    }
    return readin;
}

int clic_decompress_part(unsigned char *out, const unsigned char *in, size_t readin)
{
    strm.next_in = in;
    strm.avail_in = readin;
    strm.next_out = out;
    // TODO: this doesn't need to be large all the time
    strm.avail_out = blocksize_large*pagesize;
    strm.total_in = 0;
    strm.total_out = 0;

    lzma_ret ret;
    while (1) {
	ret = lzma_code(&strm, LZMA_RUN);
#if defined(DEBUG)
	fprintf(stderr, "ret %d %ld %ld\n", ret, strm.avail_in, strm.avail_out );
#endif
	if (ret != LZMA_OK)
	    break;
	if (!strm.avail_in)
	  break;
    }

    if (ret == LZMA_DATA_ERROR) {
	fprintf(stderr, "lzma data corrupt!\n");
        return 0;
    }
#if defined(DEBUG)
    fprintf(stderr, "ret %d\n", ret);
#endif
    assert (ret == LZMA_OK);
    /* don't use lzma_end (will free buffers) or LZMA_FINISH (will forbid any new use) */
    return 1;
}

void clic_free_lzma()
{
    lzma_end(&strm);
}

void clic_find_block( off_t block, off_t *part, off_t *offset )
{
    // we have X blocks in large parts and Y blocks in appendix
    if (block > (off_t)(largeparts * blocksize_large) )
    {
	*part = (block - largeparts * blocksize_large) / blocksize_small + largeparts;
	*offset = (block - largeparts * blocksize_large) % blocksize_small;
    } else {
	*part = block / blocksize_large;
	*offset = block % blocksize_large;
    }
    //fprintf(stderr, "clic_find_block %ld => %ld %ld\n", block, *part, *offset);
}
