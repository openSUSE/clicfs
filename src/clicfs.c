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

#define FUSE_USE_VERSION  26

#include <unistd.h>
#include "clicfs.h"   
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>

FILE *logger = 0;

static uint32_t write_pages = 0;

static size_t detached_allocated = 0;
static size_t sparse_memory = 0;
static char *cowfilename = 0;

static struct timeval start;

static uint32_t clic_find_next_cow()
{
    //fprintf(stderr, "clic_find_next %ld %ld\n", (long)cows_index, (long)cow_pages);
    if (cows_index > 0)
	return cows[--cows_index];
    return cow_pages++;
}

static int clic_write_cow()
{
    if (!cowfilename)
	return 0;

    uint32_t indexlen = sizeof(uint32_t) * 2;
    uint32_t i;
    for (i = 0; i < num_pages; ++i)
    {
	long ptr = (long)blockmap[i];
	//fprintf(stderr, "ptr %ld %d\n", (long)i, (int)(ptr & 0x3));
	if (ptr && (ptr & 0x3) == 0) { // detached now
	    uint32_t cowindex = clic_find_next_cow();
	    lseek(cowfilefd, cowindex * pagesize, SEEK_SET);
	    write(cowfilefd, blockmap[i], pagesize);
	    free(blockmap[i]);
	    detached_allocated -= (pagesize / 1024);
	    blockmap[i] = (unsigned char*)(long)(cowindex << 2) + 2;
	}
    }

    lseek(cowfilefd, cow_pages * pagesize, SEEK_SET);
    uint32_t stringlen = thefilesize;
    write(cowfilefd, (char*)&stringlen, sizeof(uint32_t));
    stringlen = cow_pages;
    write(cowfilefd, (char*)&stringlen, sizeof(uint32_t));
    lseek(cowfilefd, cow_pages * pagesize + sizeof(uint32_t) * 2, SEEK_SET);
    stringlen = 0;
    for (i = 0; i < num_pages; ++i)
    {
	long ptr = (long)blockmap[i];
	if ((ptr & 0x3) == 2) { // block
	    uint32_t key = i, value = ptr >> 2;
	    write(cowfilefd, (char*)&key, sizeof(uint32_t));
	    write(cowfilefd, (char*)&value, sizeof(uint32_t));
	    indexlen += 2 * sizeof(uint32_t);
	    stringlen++;
	}
    }
    assert(stringlen == cow_pages);
    write(cowfilefd, (char*)&indexlen, sizeof(uint32_t));
    
    return 0;
}

static int clic_getattr(const char *path, struct stat *stbuf)
{
    //fprintf(logger, "getattr %s\n", path);

    int res = 0;
    memset(stbuf, 0, sizeof(struct stat));
    if(strcmp(path, "/") == 0) {
	stbuf->st_mode = S_IFDIR | 0755;
	stbuf->st_nlink = 2;
    }
    else if(path[0] == '/' && strcmp(path + 1, thefile) == 0) {
	stbuf->st_mode = S_IFREG | 0644;
	stbuf->st_nlink = 1;
	stbuf->st_size = thefilesize;
    }
    else
	res = -ENOENT;
  
    return res;
}
  
static int clic_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
    //fprintf(logger, "readdir %s\n", path);
    (void) offset;
    (void) fi;
  
    if(strcmp(path, "/") != 0)
	return -ENOENT;
  
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    filler(buf, thefile, NULL, 0);
  
    return 0;
}
  
static int clic_open(const char *path, struct fuse_file_info *fi)
{
    (void)fi;
    if(path[0] == '/' && strcmp(path + 1, thefile) != 0)
	return -ENOENT;
  
//    if((fi->flags & 3) != O_RDONLY)
//	return -EACCES;
  
   fi->keep_cache = 1;
    return 0;
}

struct buffer_combo {
    unsigned char *out_buffer;
    
    uint32_t part;
    pthread_mutex_t lock;
    int free;
    int index;
    int used;
};

static int used_counter = 0;

struct buffer_combo *coms;    
static unsigned int com_count = 1;

pthread_mutex_t picker = PTHREAD_MUTEX_INITIALIZER, seeker = PTHREAD_MUTEX_INITIALIZER;;

FILE *pack;

static const unsigned char *clic_uncompress(uint32_t part)
{
    struct buffer_combo *com;

    //fprintf(logger, "clic_uncompress %d %d %d\n", part, parts, wparts);

    pthread_mutex_lock(&picker);
    int index = -1;
    unsigned int i;
    for (i = 0; i < com_count; ++i)
    {
	if (coms[i].part == part)
	{
	    index = i;
	    break;
	}
    }
    if (index == -1)
    {
	index = 0;
	for (i = 0; i < com_count -1; ++i)
	{
	    if (coms[i].free) {
		index = i;
		break;
	    }
	}
	for (i = index + 1; i < com_count; ++i)
	{
	    if (coms[i].free && coms[index].used > coms[i].used)
		index = i;
	}
    }
    com = coms + index;
    pthread_mutex_lock(&com->lock);
    com->free = 0;
    com->used = used_counter++;
    pthread_mutex_unlock(&picker);

    if (com->part == part)
    {
	const unsigned char *buf = com->out_buffer;
	com->free = 1;
	pthread_mutex_unlock(&com->lock);
	return buf;
    }

    com->part = part;

    pthread_mutex_lock(&seeker);
    unsigned char *inbuffer = malloc(sizes[part]);
    struct timeval begin, end;
    gettimeofday(&begin, 0);
    size_t readin = clic_readpart(inbuffer, part);
    gettimeofday(&end, 0);

#if defined(DEBUG)
    if (logger) fprintf(logger, "uncompress %d %d %ld %ld (read took %ld - started %ld)\n", part, com->index, (long)offs[part], (long)sizes[part], (end.tv_sec - begin.tv_sec) * 1000 + (end.tv_usec - begin.tv_usec) / 1000, (begin.tv_sec - start.tv_sec) * 1000 + (begin.tv_usec - start.tv_usec) / 1000 );
#endif
    if (!readin)
      return 0;
    pthread_mutex_unlock(&seeker);

    clic_decompress_part(com->out_buffer, inbuffer, readin);
    free(inbuffer);

    com->part = part;
    com->free = 1;

    pthread_mutex_unlock(&com->lock);

    return com->out_buffer;
}

static void clic_log_access(size_t block)
{
   if (!logger) return;

   static size_t firstblock = 0;
   static ssize_t lastblock = -1;

   if (lastblock >= 0 && block != (size_t)(lastblock + 1))
   {
       fprintf(logger, "access %ld+%ld\n", (long)firstblock, (long)lastblock-firstblock);
       firstblock = block;
   }
   lastblock = block;
   if (block > firstblock + 30) 
   {
      fprintf(logger, "access %ld+%ld\n", (long)firstblock, (long)lastblock-firstblock);
      firstblock = block;
   }
}

static size_t clic_read_block(char *buf, size_t block);

static int clic_detach(size_t block)
{
    if (detached_allocated > 1500 && cowfilefd != -1)
	clic_write_cow();
    
    unsigned char *ptr = blockmap[block];
    if (((long)ptr & 0x3) == 1 || ((long)ptr & 0x3) == 2)
    {
	if (((long)ptr & 0x3) == 2) {
	    if (cows_index == DOENER_COW_COUNT - 1)
		clic_write_cow();
	}

	char *newptr = malloc(pagesize);
	detached_allocated += (pagesize / 1024);
	if (logger && detached_allocated % 1024 == 0 ) fprintf(logger, "detached %dMB\n", (int)(detached_allocated / 1024));

	clic_read_block(newptr, block);
	if (((long)ptr & 0x3) == 2) // we need to mark the place in the cow obsolete
	    cows[cows_index++] = (long)ptr >> 2;
	blockmap[block] = (unsigned char*)newptr;

	return 1;
    }

    if (!blockmap[block])
    {
	blockmap[block] = malloc(pagesize);
	assert(((long)ptr & 0x3) == 0);
	detached_allocated += (pagesize / 1024);
	if (logger && detached_allocated % 1024 == 0 ) fprintf(logger, "detached %dMB\n", (int)(detached_allocated / 1024));
	memset(blockmap[block],0,pagesize);
	return 1;
    }

    return 0;
}

static size_t clic_write_block(const char *buf, off_t block, size_t size)
{
    clic_detach(block);
    memcpy(blockmap[block], buf, size);
    return size;
}

static int clic_write(const char *path, const char *buf, size_t size, off_t offset,
		       struct fuse_file_info *fi)
{
    //if (logger) fprintf(logger, "write %s %ld %ld\n", path, offset, size);
    (void) fi;
    if(path[0] == '/' && strcmp(path + 1, thefile) != 0)
	return -ENOENT;

    if (offset >= (off_t)thefilesize) {
        return 0;
    }

    off_t block = offset / pagesize;
    off_t ioff = offset - block * pagesize;

    assert(ioff == 0 || ioff + size <= pagesize);

    if (size <= pagesize) {
	return clic_write_block(buf+ioff, block, size);
    } else {
	size_t wrote = 0;
	do
	{
	    size_t diff = clic_write_block(buf, block, size > pagesize ? pagesize : size);
	    size -= diff;
	    buf += diff;
	    block++;
	    wrote += diff;
	} while (size > 0);

	return wrote;
    }
}

static size_t clic_read_block(char *buf, size_t block)
{
    if (block >= write_pages)
	return 0;

    assert(block < write_pages);
    clic_log_access(block);

    if (!blockmap[block]) { // sparse block 
        memset(buf, 0, pagesize);
        return pagesize;
    }

    long ptr = (long)blockmap[block];
    if ((ptr & 0x3) == 0) {
	// detached
	memcpy(buf, blockmap[block], pagesize);
	return pagesize;
    }

    if ((ptr & 0x3) == 2) {
	lseek(cowfilefd, (ptr >> 2) * pagesize, SEEK_SET);
	return read(cowfilefd, buf, pagesize);
    }

    assert((ptr & 0x3) == 1); // in read only part
    assert(block < num_pages);

    off_t mapped_block = clic_map_block(block);

    size_t part = (size_t)(mapped_block / bsize);
    assert(part < parts);

    const unsigned char *partbuf = clic_uncompress(part);
    assert(partbuf);
    memcpy(buf, partbuf + pagesize * (mapped_block % bsize), pagesize);

    return pagesize;
}

static int clic_read(const char *path, char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
    // fprintf(stdout, "read %ld %ld %ld\n", offset, size, thefilesize);
    (void) fi;
    if(path[0] == '/' && strcmp(path + 1, thefile) != 0)
	return -ENOENT;

    size_t readtotal = 0;

    assert(size % pagesize == 0);
    assert(offset % pagesize == 0);

    do
    {
	if (offset >= (off_t)thefilesize)
		break;
	size_t diff = clic_read_block(buf, offset / pagesize);
	if (!diff)
	  break;
	size -= diff;
	buf += diff;
	offset += diff;
	readtotal += diff;
    } while (size > 0);

    return readtotal;
}
  
static int clic_flush(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    // TODO write out cow
    if (logger)	fflush(logger);
    clic_write_cow();
    return 0;
}

static int clic_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    (void)datasync;
    // TODO write out cow
    if (logger) fflush(logger);
    clic_write_cow();
    fsync(cowfilefd);
    return 0;
}

static struct fuse_operations clic_oper = {
    .getattr   = clic_getattr,
    .readdir = clic_readdir,
    .open   = clic_open,
    .read   = clic_read,
    .write  = clic_write,
    .flush  = clic_flush,
    .fsync = clic_fsync
};
  
static void clic_init_buffer(int i)
{
    coms[i].part = -1;
    coms[i].used = 0;
    coms[i].index = i + 1;
    coms[i].free = 1;
    coms[i].out_buffer = malloc(bsize*pagesize);
    pthread_mutex_init(&coms[i].lock, 0);
}

char *packfilename = 0;
char *logfile = 0;

enum  { FUSE_OPT_MEMORY, FUSE_OPT_LOGGER, FUSE_OPT_COWFILE };

struct fuse_opt clic_opt[] = {
    FUSE_OPT_KEY("-m %s", FUSE_OPT_MEMORY),
    FUSE_OPT_KEY("-l %s", FUSE_OPT_LOGGER),
    FUSE_OPT_KEY("-c %s", FUSE_OPT_COWFILE),
    FUSE_OPT_END
};

int clic_opt_proc(void *data, const char *arg, int key, struct fuse_args *outargs)
{
    (void)data;
    (void)outargs;

    switch (key) {
	case FUSE_OPT_KEY_NONOPT:
	    if (packfilename == NULL) {
		packfilename = strdup(arg);
		return 0;
	    }
	    break;
	case FUSE_OPT_MEMORY:
	     sparse_memory = atoi(arg+2);
	     return 0;
	     break;
	case FUSE_OPT_LOGGER:
	     logfile = strdup(arg+2);
	     return 0;
	     break;
	case FUSE_OPT_COWFILE:
	     cowfilename = strdup(arg+2);
	     return 0;
	     break;
    }
	
    return 1;
}

int main(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    if (fuse_opt_parse(&args, NULL, clic_opt, clic_opt_proc) == -1) {
      perror("fuse_opt_part");
      return 1;
    }

    if (logfile) {
      if (!strcmp(logfile, "-"))
	logger = stderr;
      else
	logger = fopen(logfile, "w");
      if (!logger) {
	perror("open");
        return 1;
      }
      free(logfile);
    }

    // not sure why but multiple threads make it slower
    fuse_opt_add_arg(&args, "-s");

    if (!packfilename || (cowfilename && sparse_memory)) {
	fprintf(stderr, "usage: [-m <mb>] [-l <logfile|->] [-c <cowfile>] <packfile> <mntpoint>\n");
	if (cowfilename && sparse_memory) {
	    fprintf(stderr, "writes can go either into cowfile or memory\n");
	}
        return 1;
    }

    if (clicfs_read_pack(packfilename)) {
      perror("read_pack");
      return 1;
    }

    free(packfilename);

    if (cowfilename) {
	if (access(cowfilename, W_OK)) {
	    FILE *cow = fopen(cowfilename, "w");
	    uint32_t stringlen = (thefilesize / pagesize * pagesize) + 512 * 1024 * 1024;
	    fwrite((char*)&stringlen, 1, sizeof(uint32_t), cow);
	    stringlen = 0;
	    // there are 0 blocks
	    fwrite((char*)&stringlen, 1, sizeof(uint32_t), cow);
	    // the whole index is 8 bytes long
	    stringlen = sizeof(uint32_t) * 2;
	    fwrite((char*)&stringlen, 1, sizeof(uint32_t), cow);
	    fclose(cow);
	}
	if (clicfs_read_cow(cowfilename))
	    return 1;
    }

    // fake for write
    if (sparse_memory) {
      thefilesize = (thefilesize / pagesize * pagesize) + sparse_memory * 1024 * 1024;
      write_pages = thefilesize / pagesize;
      blockmap = realloc(blockmap, sizeof(unsigned char*)*write_pages);
    } else
      write_pages = num_pages;

    uint32_t i;
 
    for (i = num_pages; i < write_pages; ++i)
	blockmap[i] = 0;

    com_count = 6000000 / (bsize*pagesize); // get 6MB of cache
    coms = malloc(sizeof(struct buffer_combo) * com_count);
    for (i = 0; i < com_count; ++i)
	clic_init_buffer(i);

    gettimeofday(&start, 0);
    int ret = fuse_main(args.argc, args.argv, &clic_oper, NULL);
    clic_write_cow();
    close(cowfilefd);
    
    if (logger) fclose(logger);

    free(blockmap);
    for (i = 0; i < com_count; ++i)
	free(coms[i].out_buffer);
    free(coms);
    free(sizes);
    free(offs);
    fclose(packfile);

    if (cowfilename)
	free(cowfilename);
    if (cows)
	free(cows);

    fuse_opt_free_args(&args);

    return ret;
}
