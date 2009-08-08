
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

#include "clicfs.h"   
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <stdlib.h>

static unsigned char *inbuf = 0;
static unsigned char *outbuf = 0;

int main(int argc, char *argv[])
{
    if (argc != 2) {
      return 1;
    }
    const char *packfilename = argv[1];
    
    if (clicfs_read_pack(packfilename))
      return 1;

    inbuf = malloc(blocksize_large*pagesize + 300);
    outbuf = malloc(blocksize_large*pagesize);

    FILE *outfile = fopen(thefile, "w");

    size_t delta = num_pages / 100;

    size_t page;
    off_t cpart;
    for (cpart = 0; cpart < parts; ++cpart)
    {
	size_t readin = clic_readpart(inbuf, cpart);
	if (readin == 0) {
	    return 1;
	}
	clic_decompress_part(outbuf, inbuf, readin);

	for (page = 0; page < num_pages; ++page)
	{
	    off_t mapped_block = clic_map_block(page);
	    assert(mapped_block < (off_t)num_pages);

	    off_t part, off;
	    clic_find_block( mapped_block, &part, &off);
	    assert(part < parts);

	    if (part != cpart) continue;

	    if (page % delta == 0)
	    {
		fprintf(stderr, "read %d%%\n", (int)(page * 100 / num_pages));
	    }
	    if (fseeko(outfile, page * pagesize, SEEK_SET)) {
		perror("seek");
		return 1;
	    }

	    if (fwrite(outbuf + pagesize * off, 1, pagesize, outfile) != pagesize) {
		perror("write");
		break;
	    }
	}
    }

    fclose(outfile);

    return 0;
}
