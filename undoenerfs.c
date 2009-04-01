#include "doenerfs.h"   
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <stdlib.h>

static off_t lastpart = (off_t)-1;
static unsigned char *inbuf = 0;
static unsigned char *outbuf = 0;

static size_t doener_read_block(unsigned char *buf, off_t block)
{
    block = doener_map_block(block);

    off_t part = (int)(block * 4096 / bsize);
    if ( part != lastpart) {
      size_t readin = doener_readpart(inbuf, part);
      if (readin == 0) {
	return 0;
      }
      doener_decompress_part(outbuf, inbuf, readin);
      lastpart = part;
    }

    memcpy(buf, outbuf + 4096 * (block % (bsize / 4096)), 4096);

    return 4096;
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
      return 1;
    }
    const char *packfilename = argv[1];
    
    if (doenerfs_read_pack(packfilename))
      return 1;

    inbuf = malloc(bsize + 300);
    outbuf = malloc(bsize + 300);

    FILE *outfile = fopen(thefile, "w");

    size_t bcount = thefilesize / 4096;
    size_t i;
    unsigned char tbuf[4096];
    for (i = 0; i < bcount; ++i) 
    {
      size_t diff = doener_read_block(tbuf, i);
      assert(diff == 4096);
      fwrite(tbuf, 1, 4096, outfile);
    }

    fclose(outfile);

    return 0;
}
