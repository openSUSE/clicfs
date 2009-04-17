#include "clicfs.h"   
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <stdlib.h>

static off_t lastpart = (off_t)-1;
static unsigned char *inbuf = 0;
static unsigned char *outbuf = 0;

static size_t clic_read_block(unsigned char *buf, off_t block)
{
    off_t mapped_block = clic_map_block(block);
    assert(mapped_block < (off_t)num_pages);

    off_t part = (off_t)(mapped_block * 4096 / bsize);
    assert(part < parts);
    if ( part != lastpart) {
	size_t readin = clic_readpart(inbuf, part);
	if (readin == 0) {
	    return 0;
	}
	clic_decompress_part(outbuf, inbuf, readin);
	lastpart = part;
    }

    memcpy(buf, outbuf + 4096 * (mapped_block % (bsize / 4096)), 4096);

    return 4096;
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
      return 1;
    }
    const char *packfilename = argv[1];
    
    if (clicfs_read_pack(packfilename))
      return 1;

    inbuf = malloc(bsize + 300);
    outbuf = malloc(bsize + 300);

    FILE *outfile = fopen(thefile, "w");

    size_t delta = num_pages / 100;

    size_t i;
    unsigned char tbuf[4096];
    for (i = 0; i < num_pages; ++i)
    {
      size_t diff = clic_read_block(tbuf, i);
      if (i % delta == 0)
	{
	  fprintf(stderr, "read %d%%\n", (int)(i * 100 / num_pages));
	}
      assert(diff == 4096);
      if (fwrite(tbuf, 1, 4096, outfile) != 4096) {
	perror("write");
        break;
      }
    }

    fclose(outfile);

    return 0;
}
