
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

#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include <sys/types.h>

extern int preset;
extern int packfilefd;
extern int cowfilefd;

// magic 2 added large parts
#define DOENER_MAGIC 2

#define PTR_CLASS(x) ((long)x & 0x3)

enum { CLASS_MEMORY = 0,
       CLASS_RO = 1,
       CLASS_COW = 2 };

extern char thefile[PATH_MAX];
extern uint64_t thefilesize;
extern size_t pagesize;
extern uint64_t *sizes;
extern uint64_t *offs;
// the number of parts all in all
extern uint32_t parts;
// how many parts contain many blocks, the rest is small
extern uint32_t largeparts;
extern uint32_t pindex;
// the number of pages in a part (~32)
extern size_t blocksize_small;
// the number of pages in a large part (~3200)
extern size_t blocksize_large;
// the number of pages in total (full image)
extern uint32_t num_pages;
// the number of pages marked as CLASS_COW
extern uint32_t cow_pages;
// the offset in the cow file where the page index starts
extern off_t cow_index_start;
// the offset in the cow file where the pages start
extern off_t cow_pages_start;
// support temporary changes on ro medium
extern int cowfile_ro;

#define CLICFS_COW_COUNT 1000

// an array
extern uint32_t *cows;
extern unsigned int cows_index;

extern unsigned char **blockmap;

extern int clicfs_read_pack(const char *packfilename);
extern int clicfs_read_cow(const char *packfilename);
extern int clic_decompress_part(unsigned char *out, const unsigned char *in, size_t size);
extern size_t clic_readpart(unsigned char *buffer, int part);
extern off_t clic_map_block(off_t block);
extern uint32_t clic_readindex_fd( int fd );
extern void clic_free_lzma();

extern void clic_find_block( off_t block, off_t *part, off_t *offset );
