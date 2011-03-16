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

#define FUSE_USE_VERSION  26
#define _GNU_SOURCE

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
#include <sys/mman.h>

#define DEBUG 1

FILE *logger = 0;

static size_t detached_allocated = 0;
static size_t sparse_memory = 0;
static char *cowfilename = 0;
static off_t memory_used = 0;
static time_t last_sync = 0;
static time_t last_write = 0;

static struct timeval start;

static uint32_t clic_find_next_cow()
{
  if (cows_index > 0) {
    if (logger) fprintf(logger, "find_next (old): %d\n", cows[cows_index-1]);
    return cows[--cows_index];
  }
  if (logger) fprintf(logger, "find_next (new): %d\n", cow_pages + 1);
  return cow_pages + 1;
}

static int clic_detach(size_t block, int islock);
static int clic_write_cow();

pthread_mutex_t cowfile_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t cowfile_mutex_writer = PTHREAD_MUTEX_INITIALIZER;

static int clic_write_cow(int islocked)
{
    if (!cowfilename || cowfile_ro == 1 || !detached_allocated)
	return 0;

    int ret = 0;

    if (logger) fprintf(logger, "cow detached %dMB\n", (int)(detached_allocated / 1024));
    if (logger) fprintf(logger, "clic_write_cow %ld\n", pthread_self());
    
    uint32_t i;
    for (i = 0; i < num_pages; ++i)
    {
	long ptr = (long)blockmap[i];
	if ( ptr && PTR_CLASS(ptr) == CLASS_MEMORY ) { // detached now
	    off_t cowindex = clic_find_next_cow();
	    ssize_t pret = pwrite(cowfilefd, blockmap[i], pagesize, cowindex * pagesize + cow_pages_start);
	    if (logger) fprintf(logger, "pwrote %ld %ld -> %ld\n", pagesize, cowindex * pagesize + cow_pages_start, pret);
	    if (pret < 0) {
	      if (logger) fprintf(logger, "failed %s\n", strerror(errno));
	      ret = errno;
	      goto exit;
	    }
	    assert(pret == (ssize_t)pagesize);
	    free(blockmap[i]);
	    detached_allocated -= (pagesize / 1024);
	    blockmap[i] = (unsigned char*)(long)(cowindex << 2) + CLASS_COW;
	    uint32_t key = i, value = ptr >> 2;
	    off_t offset = cow_index_start + i * sizeof(uint32_t);
	    pret = pwrite(cowfilefd, (char*)&value, sizeof(uint32_t), offset);
	    if (logger) fprintf(logger, "pwrote2 %ld %ld -> %ld\n", sizeof(uint32_t), offset, pret);
	    cow_pages++;
	}
    }

    assert(!detached_allocated);

exit:
    if (logger) fprintf(logger, "clic_write_cow %ld done %d\n", pthread_self(), ret);
    return ret;
}

/** 
 * fuse callback to get stat informations
 */
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
  
/** 
 * fuse callback to get directory informations. 
 * We only have one file in one dir
 */
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
    // the buffer of the part
    unsigned char *out_buffer;
    off_t out_buffer_size;
    int mmapped;
    uint32_t part;
    time_t last_used;
    struct buffer_combo *next_by_use;
    struct buffer_combo *prev_by_use;
};

// first
struct buffer_combo **coms_by_part = 0;
#define MAX_COMS_SIZE 3000
int32_t coms_sort_by_part_size = 0;
struct buffer_combo *coms_sort_by_use_first = 0;
struct buffer_combo *coms_sort_by_use_last = 0;
static unsigned int com_count = 0;

pthread_mutex_t picker = PTHREAD_MUTEX_INITIALIZER;

FILE *pack;

static void clic_append_by_use(struct buffer_combo *com)
{
    if (!coms_sort_by_use_last) {
       coms_sort_by_use_last = com;
       coms_sort_by_use_first = com; 
       return;
    }
    coms_sort_by_use_last->next_by_use = com;
    com->prev_by_use = coms_sort_by_use_last;
    com->next_by_use = 0;
    coms_sort_by_use_last = com;
}

/** I wrote this while watching TV, I know it sucks */
static void clic_insert_com(struct buffer_combo *com, int32_t after)
{
    if (!coms_sort_by_part_size) {
	assert(!coms_sort_by_use_first);
	assert(!coms_sort_by_use_last);
	coms_by_part[0] = com;
	coms_sort_by_use_first = com;
	coms_sort_by_use_last = com;
	com->next_by_use = 0;
	com->prev_by_use = 0;
	com_count++;
	coms_sort_by_part_size++;
	return;
    }
    if (coms_sort_by_part_size == after + 1) { // just append
	coms_by_part[coms_sort_by_part_size] = com;
    } else {
	// I don't like memmove
	int i; 
	for (i = coms_sort_by_part_size-1; i > after; i--)
	    coms_by_part[i+1] = coms_by_part[i];
	coms_by_part[after+1] = com;
    }
    coms_sort_by_part_size++;
    clic_append_by_use(com);
}

static void clic_dump_use()
{
    if (!logger)
	return;

    struct buffer_combo *c =  coms_sort_by_use_first;
    fprintf(logger, "dump %ldMB ", (long int)memory_used / 1024 / 1024);
    while (c) {
	fprintf(logger, "%ld ", (long)c->part);
	c = c->next_by_use;
    }
    fprintf(logger, "\n");
}

/* slightly modified binary_search.
   If target is not found, return the index of the value that's
   in the array before it
*/
int32_t binary_search(struct buffer_combo **A, size_t size, uint32_t target)
{
    if (!size)
	return -1;
    int lo = 0, hi = size-1;
    if (target > A[hi]->part)
        return hi;
    while (lo <= hi) {
        int mid = lo + (hi-lo)/2;
        if (A[mid]->part == target)
            return mid;
        else {
            if (A[mid]->part < target)
                lo = mid+1;
            else
                hi = mid-1;
        }
    }

    return hi;
}

static void clic_remove_com_from_use(struct buffer_combo *com)
{
    if (coms_sort_by_use_first == com)
	coms_sort_by_use_first = com->next_by_use;
    if (coms_sort_by_use_last == com)
	coms_sort_by_use_last = com->prev_by_use;

    // P->C->N -> P->N
    struct buffer_combo *n = com->next_by_use;
    struct buffer_combo *p = com->prev_by_use;
    if (n)
	n->prev_by_use = p;
    if (p)
	p->next_by_use = n;
}

static void clic_free_com(struct buffer_combo *com)
{
    clic_remove_com_from_use(com);
        
    if (com->mmapped == 1) {
	int ret = munmap(com->out_buffer, com->out_buffer_size);
	if (ret == -1) {
	    perror("munmap");
	    exit(1);
	}
    } else
	free(com->out_buffer);
    if (logger) fprintf(logger, "free block %d\n", com->part);
    memory_used -= com->out_buffer_size;
    int32_t res = binary_search(coms_by_part, coms_sort_by_part_size, com->part);
    assert(coms_by_part[res] == com);
    // I don't like memmove
    while (res < coms_sort_by_part_size - 1)
    {
	coms_by_part[res] = coms_by_part[res+1];
	res++;
    }
    free(com);
    memory_used -= sizeof(struct buffer_combo);
    coms_sort_by_part_size--;
}

static const unsigned char *clic_uncompress(uint32_t part)
{
    //if (logger) fprintf(logger, "clic_uncompress %d %d\n", part, parts);
    time_t now = time(0);

    if (coms_sort_by_use_first) // clean up
    {
	if (0) clic_dump_use();
	// if the oldest is 1m, drop it 
	while (coms_sort_by_use_first && (now - coms_sort_by_use_first->last_used > 40 || (memory_used > 1024 * 1024 * 40 && coms_sort_by_use_first->part != part))) {
	    clic_free_com(coms_sort_by_use_first);
	}
    	//clic_dump_use();
    }

    int32_t res = binary_search(coms_by_part, coms_sort_by_part_size, part);
    if (res >= 0 && coms_by_part[res] && coms_by_part[res]->part == part)
    {
	struct buffer_combo *com = coms_by_part[res];
       	const unsigned char *buf = com->out_buffer;
	com->last_used = now;
	clic_remove_com_from_use(com);
	clic_append_by_use(com);
	return buf;
    }

    // need room?
    if (coms_sort_by_part_size == MAX_COMS_SIZE) {
	// the index moves
	if (coms_sort_by_use_first->part < part)
	    res--;
	clic_free_com(coms_sort_by_use_first);
    }

    struct buffer_combo *com = malloc(sizeof(struct buffer_combo));
    memory_used += sizeof(struct buffer_combo);
    if (part < largeparts) {
	com->out_buffer_size = blocksize_large*pagesize;
	// TODO: round up to the next PAGE_SIZE (no worry for now)
	com->out_buffer = mmap(0, com->out_buffer_size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
	if (com->out_buffer == MAP_FAILED) {
	    perror("mmap");
	    exit(1);
	}
	com->mmapped = 1;
    } else {
	com->out_buffer_size = blocksize_small*pagesize;
	com->out_buffer = malloc(blocksize_small*pagesize);
	com->mmapped = 0;
    }
    memory_used += com->out_buffer_size;
    com->last_used = now;
    com->part = part;

    clic_insert_com(com, res);

    unsigned char *inbuffer = malloc(sizes[part]);
    struct timeval begin, end;
    gettimeofday(&begin, 0);
    size_t readin = clic_readpart(inbuffer, part);
    gettimeofday(&end, 0);
    if (!readin) {
      free(inbuffer);
      return 0;
    }

#if defined(DEBUG)
    if (logger) fprintf(logger, "uncompress %d %ld-%ld %ld (read took %ld - started %ld)\n", part, (long)offs[part], (long)sizes[part], (long)readin, (end.tv_sec - begin.tv_sec) * 1000 + (end.tv_usec - begin.tv_usec) / 1000, (begin.tv_sec - start.tv_sec) * 1000 + (begin.tv_usec - start.tv_usec) / 1000 );
#endif
    if (!clic_decompress_part(com->out_buffer, inbuffer, readin)) {
      if (logger) fprintf(logger, "uncompess of part %d failed - ignoring", part);
    }
    free(inbuffer);

    return com->out_buffer;
}

static void clic_log_access(size_t block)
{
    if (!logger) return;

    static size_t firstblock = 0;
    static ssize_t lastblock = -1;

    if (lastblock >= 0 && block != (size_t)(lastblock + 1))
    {
	fprintf(logger, "access %ld+%ld\n", (long)firstblock*8, (long)(lastblock-firstblock+1)*8);
	firstblock = block;
    }
    lastblock = block;
    if (block > firstblock + 30) 
    {
	fprintf(logger, "access %ld+%ld\n", (long)firstblock*8, (long)(lastblock-firstblock+1)*8);
	firstblock = block;
    }
}

static ssize_t clic_read_block(char *buf, size_t block);

static int clic_detach(size_t block, int islocked)
{
    assert(block < num_pages);

    int ret = 0;

    unsigned char *ptr = blockmap[block];
    if ((PTR_CLASS(ptr) == CLASS_RO ) || (PTR_CLASS(ptr) == CLASS_COW))
    {
	if (PTR_CLASS(ptr) == CLASS_COW) {
	  if (cows_index == CLICFS_COW_COUNT - 1) {
	    ret = clic_write_cow(1);
	    if (logger) fprintf(logger, "detach cow %d\n", ret);
	  }
	}

	char *newptr = malloc(pagesize);
	detached_allocated += (pagesize / 1024);
	if (logger && detached_allocated % 1024 == 0 ) fprintf(logger, "detached %dMB\n", (int)(detached_allocated / 1024));

	clic_read_block(newptr, block);
	if (PTR_CLASS(ptr) == CLASS_COW) { // we need to mark the place in the cow obsolete
	    if (logger) fprintf(logger, "detach block %ld (was %ld)\n", (long)block, (long)ptr >> 2);
	    cows[cows_index++] = (long)ptr >> 2;
	    cow_pages--;
	}
	blockmap[block] = (unsigned char*)newptr;

	goto exit;
    }

    if (!blockmap[block])
    {
	blockmap[block] = malloc(pagesize);
	assert(PTR_CLASS(ptr) == CLASS_MEMORY);
	detached_allocated += (pagesize / 1024);
	if (logger && detached_allocated % 1024 == 0 ) fprintf(logger, "detached %dMB\n", (int)(detached_allocated / 1024));
	memset(blockmap[block],0,pagesize);
    }

exit:
    if (logger) fprintf(logger, "clic_detach done %d\n", ret);
    return ret;
}

static size_t clic_write_block(const char *buf, off_t block, off_t ioff, size_t size)
{
    if (clic_detach(block, 1)) {
      if (logger) fprintf(logger, "clic_detach failed\n");
      return -ENOSPC;
    }
    memcpy(blockmap[block]+ioff, buf, size);
    return size;
}

static int clic_write(const char *path, const char *buf, size_t size, off_t offset,
		       struct fuse_file_info *fi)
{
    if (logger) fprintf(logger, "write %s %ld %ld\n", path, offset, size);
    (void) fi;
    if(path[0] == '/' && strcmp(path + 1, thefile) != 0)
	return -ENOENT;

    if (offset >= (off_t)thefilesize)
        return 0;

    if ((off_t)(offset+size) > (off_t)thefilesize)
	size = thefilesize-offset;

    if (!size)
	return 0;

    off_t block = offset / pagesize;
    off_t ioff = offset - block * pagesize;

    assert(ioff == 0 || ioff + size <= pagesize);

    last_write = time(0);

    int ret = 0;

    if (size <= pagesize) {
        ret = clic_write_block(buf, block, ioff, size);
    } else {
	size_t wrote = 0;
	do
	{
	    size_t diff = clic_write_block(buf, block, ioff, size > pagesize ? pagesize : size);
	    ioff = 0;
	    size -= diff;
	    buf += diff;
	    block++;
	    wrote += diff;
	} while (size > 0);

	ret = wrote;
    }
    return ret;
}

static ssize_t clic_read_block(char *buf, size_t block)
{
    if (block >= num_pages)
	return -EFAULT;

    if (!blockmap[block]) { // sparse block 
        memset(buf, 0, pagesize);
        return pagesize;
    }

    long ptr = (long)blockmap[block];
    if (PTR_CLASS(ptr) == CLASS_MEMORY) {
	// detached
	memcpy(buf, blockmap[block], pagesize);
	return pagesize;
    }

    if (PTR_CLASS(ptr) == CLASS_COW) {
	off_t target = ptr >> 2;
	pthread_mutex_lock(&cowfile_mutex);
	ssize_t haveread = pread(cowfilefd, buf, pagesize, target * pagesize);
	pthread_mutex_unlock(&cowfile_mutex);
	return haveread;
    }

    assert(PTR_CLASS(ptr) == CLASS_RO); // in read only part
    assert(block < num_pages);

    clic_log_access(block);

    off_t mapped_block = clic_map_block(block);
    
    off_t part, off;
    clic_find_block( mapped_block, &part, &off);

    assert(part < parts);

    //if (part >= largeparts && logger)  { fprintf(logger, "big access %ld+8\n", block*8); }

    const unsigned char *partbuf = clic_uncompress(part);
    if (!partbuf) 
    {
      // problems in the, invent 0 block
      memset(buf,0,pagesize);
    } else {
      memcpy(buf, partbuf + pagesize * off, pagesize);
    }

    return pagesize;
}

static int clic_read(const char *path, char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
    //if (logger) fprintf(logger, "read %ld %ld %ld\n", offset, size, thefilesize);
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
	off_t block = offset / pagesize;
	ssize_t diff = clic_read_block(buf, block);
	if (diff < 0) {
	    return diff;
	}
	//if (logger) fprintf(logger, "read block %ld: %ld bytes\n", (long)block, (long)diff);
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
    if (logger)	{ fprintf(logger, "flush\n"); fflush(logger); }
    return clic_write_cow(0);
}

static int clic_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    (void)datasync;
    // TODO write out cow
    if (logger) { fprintf(logger, "sync\n"); fflush(logger); }

    int ret = clic_write_cow(0);
    last_sync = time(0);
    pthread_mutex_lock(&cowfile_mutex);
    if (cowfilefd >= 0) fdatasync(cowfilefd);
    pthread_mutex_unlock(&cowfile_mutex);
    return ret;
}

static void *clic_sync_thread(void *arg)
{
    (void)arg;

    while (1) {
	sleep(1);
	    
	if (last_sync < last_write ) {
	    if (cowfilefd < 0)
		pthread_exit(0);
	    if (logger) fprintf(logger, "Sync thread %d %ld %ld\n", cowfilefd, last_sync, last_write);
	    // assume the paramters are not used
	    clic_fsync(0, 0, 0);
	}
    }

    return 0;
}

static pthread_t clic_sync_tid;

static void* clic_init(struct fuse_conn_info *conn)
{
    // avoid random reads or our profiling will be destroyed
    conn->max_readahead = 0;
    clic_sync_tid = 0;

    //FOR NOWpthread_create(&clic_sync_tid, NULL, clic_sync_thread, 0);
       
    return 0;
}

static void clic_destroy(void *arg)
{
    (void)arg;
    if (logger) fprintf(logger, "destroy\n");
    if (clic_sync_tid > 0)
      pthread_cancel(clic_sync_tid);
}


static struct fuse_operations clic_oper = {
    .init    = clic_init,
    .getattr = clic_getattr,
    .readdir = clic_readdir,
    .open   = clic_open,
    .read   = clic_read,
    .write  = clic_write,
    .flush  = clic_flush,
    .fsync = clic_fsync,
    .destroy = clic_destroy
};
  
char *packfilename = 0;
char *logfile = 0;
int ignore_cow_errors = 0;

enum  { FUSE_OPT_SPARSE, FUSE_OPT_LOGGER, FUSE_OPT_COWFILE, FUSE_OPT_IGNORE_COW_ERRORS };

struct fuse_opt clic_opt[] = {
    FUSE_OPT_KEY("--resevere-sparse %s", FUSE_OPT_SPARSE),
    FUSE_OPT_KEY("--ignore-cow-errors", FUSE_OPT_IGNORE_COW_ERRORS),
    FUSE_OPT_KEY("-m %s", FUSE_OPT_SPARSE),
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
	case FUSE_OPT_SPARSE:
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
        case FUSE_OPT_IGNORE_COW_ERRORS:
	     ignore_cow_errors = 1;
	     return 0;
	     break;
    }
	
    return 1;
}

static int init_cow()
{
    FILE *cow = fopen(cowfilename, "w");
    if (!cow) {
	perror("opening cow");
	return 1;
    }
    uint64_t bigfilesize = (thefilesize / pagesize * pagesize);
    if (bigfilesize < thefilesize)
	thefilesize += pagesize;
    bigfilesize += sparse_memory * 1024 * 1024;
  
    assert( DOENER_MAGIC < 100 );
    int index_len = fprintf(cow, "CLICCOW%02d", DOENER_MAGIC );
    index_len += fwrite((char*)&bigfilesize, 1, sizeof(uint64_t), cow);

    char zeros[sizeof(uint32_t)];
    memset(zeros, 0, sizeof(uint32_t));

    size_t write_pages = bigfilesize / pagesize;
    blockmap = realloc(blockmap, sizeof(unsigned char*)*write_pages);
    unsigned int i;
    for (i = num_pages; i < write_pages; ++i)
      blockmap[i] = 0;
    num_pages = write_pages;

    for (i = 0; i < num_pages; ++i)
	fwrite(zeros, 1, sizeof(uint32_t), cow);
    fclose(cow);

    return 0;
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

    if (!packfilename) {
	fprintf(stderr, "usage: [-m <mb>] [-l <logfile|->] [-c <cowfile>] <packfile> <mntpoint>\n");
        return 1;
    }

    if (clicfs_read_pack(packfilename)) {
	perror("read_pack");
	return 1;
    }

    free(packfilename);

    if (cowfilename) {
      
	if (access(cowfilename, R_OK))
	    init_cow();

	if (clicfs_read_cow(cowfilename)) {
	    if (!ignore_cow_errors)
		return 1;
	
	    init_cow();
	    if (clicfs_read_cow(cowfilename))
		return 1;
	}
	sparse_memory = 0; // ignore the option if we have a cow
    }

    uint32_t i;

    // fake for write
    if (sparse_memory) {
	thefilesize = (thefilesize / pagesize * pagesize) + sparse_memory * 1024 * 1024;
	size_t write_pages = thefilesize / pagesize;
	blockmap = realloc(blockmap, sizeof(unsigned char*)*write_pages);
	for (i = num_pages; i < write_pages; ++i)
	    blockmap[i] = 0;
	num_pages = write_pages;
    }

    for (i = 0; i < largeparts; ++i) {
	posix_fadvise( packfilefd, offs[i], sizes[i], POSIX_FADV_SEQUENTIAL);
    }
    coms_by_part = malloc(sizeof(struct buffer_combo*)*MAX_COMS_SIZE);
    gettimeofday(&start, 0);
    /* MAIN LOOP */
    int ret = fuse_main(args.argc, args.argv, &clic_oper, NULL);
    clic_write_cow(0); // ignored
    if (cowfilefd >= 0) close(cowfilefd);
    
    if (logger) fclose(logger);

    while (coms_sort_by_use_first)
	clic_free_com(coms_sort_by_use_first);

    for (i = 0; i < num_pages; ++i)
    {
	long ptr = (long)blockmap[i];
	if (PTR_CLASS(ptr) == CLASS_MEMORY) { // block
	    free(blockmap[i]);
	}
    }

    free(blockmap);
    free(sizes);
    free(offs);
    close(packfilefd);

    if (cowfilename)
	free(cowfilename);
    if (cows)
	free(cows);
    free(coms_by_part);
    clic_free_lzma();

    fuse_opt_free_args(&args);

    return ret;
}
