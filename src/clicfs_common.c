/* This file is part of Clic FS
    Copyright (C) 2009 Stephan Kulow (coolo@suse.de)

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/

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
FILE *packfile = 0;
int cowfilefd = -1;

char thefile[PATH_MAX];
size_t thefilesize = 0;
size_t pagesize = 4096;
uint64_t *sizes = 0;
uint64_t *offs = 0;
uint32_t parts = 0;
uint32_t pindex = 0;
size_t bsize = 0;
unsigned char **blockmap;
uint32_t num_pages = 0;
uint32_t cow_pages = 0;
uint32_t *cows = 0;
unsigned int cows_index = 0;

static lzma_stream strm;

uint32_t clic_readindex_fd(int fd)
{
    uint32_t stringlen = 0;
    if (read(fd, &stringlen, sizeof(uint32_t)) != sizeof(uint32_t)) {
        return 0;
    }
    return stringlen;
}

uint32_t clic_readindex_file(FILE * f)
{
    uint32_t stringlen = 0;
    if (fread(&stringlen, 1, sizeof(uint32_t), f) != sizeof(uint32_t)) {
        return 0;
    }
    return stringlen;
}

int clicfs_read_cow(const char *cowfilename)
{
    cowfilefd = open(cowfilename, O_RDWR);
    if (cowfilefd == -1 ) {
	fprintf(stderr, "cowfile %s can't be opened\n", cowfilename);
        return 1;
    }
   
    struct stat st;
    fstat(cowfilefd, &st);
    lseek(cowfilefd, st.st_size - sizeof(uint32_t), SEEK_SET);
    uint32_t indexlen = clic_readindex_fd(cowfilefd) + sizeof(uint32_t);
    if (lseek(cowfilefd, st.st_size - indexlen, SEEK_SET ) == -1)
	perror("seek");
    thefilesize = clic_readindex_fd(cowfilefd);
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
    cows = malloc(sizeof(uint32_t) * DOENER_COW_COUNT);
    cows_index = 0;
    return 0;
}

int clicfs_read_pack(const char *packfilename)
{
    packfile = fopen(packfilename, "r");
    if (!packfile) {
        fprintf(stderr, "packfile %s can't be opened\n", packfilename);
        return 1;
    }
    char head[7];
    char expected[7];
    fread(head, 1, 6, packfile);
    head[6] = 0;
    sprintf(expected, "CLIC%02d", DOENER_MAGIC);
    if (strcmp(head,expected)) {
	fprintf(stderr, "wrong magic: %s vs %s\n", head, expected);
	return 1;
    }

    uint32_t stringlen = clic_readindex_file(packfile);
    if (stringlen == 0 || stringlen >= PATH_MAX) {
	fprintf(stderr, "abnormal len %lx\n", (long)stringlen); 
        return 1;
    }
    if (fread(thefile, 1, stringlen, packfile) != stringlen) {
	fprintf(stderr, "short read %ld\n", (long)stringlen);
	return 1;
    }
    thefile[stringlen] = 0;

    size_t oparts = clic_readindex_file(packfile);
    bsize = clic_readindex_file(packfile);
    pagesize = clic_readindex_file(packfile);
    thefilesize = oparts * bsize * pagesize;
    preset = clic_readindex_file(packfile);
    num_pages = clic_readindex_file(packfile);
    blockmap = malloc(sizeof(unsigned char*)*num_pages);

    uint32_t i;
    for (i = 0; i < num_pages; ++i) {
	// make sure it's odd to diff between pointer and block
	blockmap[i] = (unsigned char*)(long)((clic_readindex_file(packfile) << 2) + 1);
    }

    parts = clic_readindex_file(packfile);
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
	if (i > 0 && offs[i] <= offs[i-1]) {
	  fprintf(stderr, "the offset for i is not larger than i-1: %ld\n", (long)i);
	  return 1;
	}
    }
    if (parts == 0) {
        fprintf(stderr, "unreasonable part number 0\n");
	return 1;
    }
    fseek(packfile, (oparts-parts)*sizeof(uint64_t)*2, SEEK_CUR);

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
    assert((ret & 0x3) == 1);
    return ret >> 2;
}

size_t clic_readpart(unsigned char *buffer, int part)
{
    if (fseek(packfile, offs[part], SEEK_SET)) {
	fprintf(stderr, "seek failed %d %ld\n", part, (long)offs[part]);
	return 0;
    }
#if defined(DEBUG)
    fprintf(stderr, "uncompress part=%d/%d off=%ld size=%ld\n", part, parts, offs[part], sizes[part] );
#endif
    size_t readin = fread(buffer, 1, sizes[part], packfile);
    if (readin != sizes[part]) {
	fprintf(stderr, "short read: %d %ld %ld %ld\n", part, (long)offs[part], (long)sizes[part], (long)readin);
    }
    return readin;
}

void clic_decompress_part(unsigned char *out, const unsigned char *in, size_t readin)
{
    strm.next_in = in;
    strm.avail_in = readin;
    strm.next_out = out;
    strm.avail_out = bsize*pagesize;
    strm.total_in = 0;
    strm.total_out = 0;

    lzma_ret ret;
    while (1) {
	ret = lzma_code(&strm, LZMA_RUN);
	//fprintf(stderr, "ret %d %ld %ld\n", ret, strm.avail_in, strm.avail_out );
	if (ret != LZMA_OK)
	    break;
	if (!strm.avail_in)
	  break;
    }

    assert (ret == LZMA_OK);
    /* don't use lzma_end (will free buffers) or LZMA_FINISH (will forbid any new use) */
}
