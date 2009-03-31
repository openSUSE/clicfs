#define FUSE_USE_VERSION  26

#include "doenerfs.h"   
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <stdlib.h>
#include <pthread.h>

FILE *logger = 0;

static int *hits = 0;
static int hit_counter = 0;
static uint32_t wparts = 0;

static size_t detached_allocated = 0;
static size_t sparse_memory = 0;

static unsigned char **detached; 

static int doener_getattr(const char *path, struct stat *stbuf)
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
  
static int doener_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
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
  
static int doener_open(const char *path, struct fuse_file_info *fi)
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
    unsigned char *in_buffer;
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

static unsigned char *static_empty = 0;

static const unsigned char *doener_uncompress(uint32_t part, int detach)
{
    struct buffer_combo *com;

    //fprintf(logger, "doener_uncompress %d %d %d\n", part, parts, wparts);

    if (part >= wparts) {
         return 0;
    }

    if (detached[part])
    {
	return detached[part];
    }

    if (part >= parts)
    {
	// sparse fake
	if (detach) {
	    detached[part] = malloc(bsize);
	    detached_allocated += bsize;
	    fprintf(logger, "detached %.2f\n", detached_allocated / 1000000.);
            fflush(logger);
	    memset(detached[part], 0, bsize);
	    return detached[part];
	}
	if (!static_empty) {
	    static_empty = malloc(bsize);
	    memset(static_empty,0,bsize);
	}
	return static_empty;
    }

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
	if (detach) {
	    detached[part] = malloc(bsize);
	    detached_allocated += bsize;
	    fprintf(logger, "detached %.2f\n", detached_allocated / 1000000.);
            fflush(logger);
	    memcpy(detached[part], com->out_buffer, bsize);
	    // we can reuse it asap
	    com->used = 0;
	    buf = detached[part];
	}
	//fprintf(logger, "cached %d\n", com->index);
	com->free = 1;
	pthread_mutex_unlock(&com->lock);
	return buf;
    }

    com->part = part;

    if (!hits[part]) {
      fprintf(logger, "first hit %d\n", part );
      hits[part] = ++hit_counter;
    }

    pthread_mutex_lock(&seeker);
    size_t readin = doener_readpart(com->in_buffer, part);
#if defined(DEBUG)
    fprintf(logger, "uncompress %d d %ld %ld\n", part, com->index, offs[part], sizes[part] );
#endif
    if (!readin)
      return 0;
    pthread_mutex_unlock(&seeker);

    doener_decompress_part(com->out_buffer, com->in_buffer, readin);

    com->part = part;
    com->free = 1;

    if (detach) {
	detached[part] = malloc(bsize);
	memcpy(detached[part], com->out_buffer, bsize);
	detached_allocated += bsize;
	fprintf(logger, "detached %.3f\n", detached_allocated / 1000000.);
        fflush(logger);
	// we can reuse it asap
	com->used = 0;
	pthread_mutex_unlock(&com->lock);
	return detached[part];
    }

    pthread_mutex_unlock(&com->lock);

    return com->out_buffer;
}

static size_t doener_write_block(const char *buf, off_t block, size_t size)
{
    block = doener_map_block(block);

    off_t part = (int)(block * 4096 / bsize);
    unsigned char *partbuf = (unsigned char*)doener_uncompress(part, 1);
    memcpy(partbuf + 4096 * (block % (bsize / 4096)), buf, size);

    return size;
}

static int doener_write(const char *path, const char *buf, size_t size, off_t offset,
		       struct fuse_file_info *fi)
{
    fprintf(logger, "write %s %ld %ld\n", path, offset, size);
    (void) fi;
    if(path[0] == '/' && strcmp(path + 1, thefile) != 0)
	return -ENOENT;

    off_t part = (int)(offset / bsize);
    if (part >= wparts) {
        return 0;
    }

    off_t block = offset / 4096;
    off_t ioff = offset - block * 4096;

    assert(ioff == 0 || ioff + size <= 4096);

    if (size <= 4096) {

	return doener_write_block(buf+ioff, block, size);

    } else {

	size_t wrote = 0;
	do
	{
	    size_t diff = doener_write_block(buf, block, size > 4096 ? 4096 : size);
	    size -= diff;
	    buf += diff;
	    block++;
	    wrote += diff;
	} while (size > 0);

	return wrote;
    }
}

static size_t doener_read_block(char *buf, off_t block)
{
    block = doener_map_block(block);

    off_t part = (int)(block * 4096 / bsize);

    const unsigned char *partbuf = doener_uncompress(part, 0);
    assert(partbuf);
    memcpy(buf, partbuf + 4096 * (block % (bsize / 4096)), 4096);

    return 4096;
}

static int doener_read(const char *path, char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
    fprintf(logger, "read %s %ld %ld %p\n", path, offset, size, buf);
    (void) fi;
    if(path[0] == '/' && strcmp(path + 1, thefile) != 0)
	return -ENOENT;

    size_t readtotal = 0;

    assert(size % 4096 == 0);
    assert(offset % 4096 == 0);

    do
    {
	size_t diff = doener_read_block(buf, offset / 4096);
	size -= diff;
	buf += diff;
	offset += diff;
	readtotal += diff;
    } while (size > 0);

    return readtotal;
}
  
static int doener_flush(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    fflush(logger);
    return 0;
}

static int doener_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    (void)datasync;
    fflush(logger);
    return 0;
}

static struct fuse_operations doener_oper = {
    .getattr   = doener_getattr,
    .readdir = doener_readdir,
    .open   = doener_open,
    .read   = doener_read,
    .write  = doener_write,
    .flush  = doener_flush,
    .fsync = doener_fsync
};
  
static void doener_init_buffer(int i)
{
    coms[i].part = -1;
    coms[i].used = 0;
    coms[i].index = i + 1;
    coms[i].free = 1;
    coms[i].in_buffer = malloc(bsize);
    coms[i].out_buffer = malloc(bsize + 300);
    pthread_mutex_init(&coms[i].lock, 0);
}

char *packfilename = 0;

enum  { FUSE_OPT_MEMORY };

struct fuse_opt doener_opt[] = {
    FUSE_OPT_KEY("-m %s", FUSE_OPT_MEMORY),
    FUSE_OPT_END
};

int doener_opt_proc(void *data, const char *arg, int key, struct fuse_args *outargs)
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
    }
	
    return 1;
}

int main(int argc, char *argv[])
{
    logger = fopen("/dev/shm/doenerfs.log", "w");
    if (!logger) {
	perror("open /dev/shm/doenerfs.log");
        return 1;
    }
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    if (fuse_opt_parse(&args, NULL, doener_opt, doener_opt_proc) == -1) return 1;

    // not sure why but multiple threads make it slower
    fuse_opt_add_arg(&args, "-s");

    if (doenerfs_read_pack(packfilename))
      return 1;

    // fake for write
    thefilesize += sparse_memory * 1024 * 1024;

    wparts = thefilesize / bsize + 1;
    detached = malloc(sizeof(unsigned char*) * wparts);
    uint32_t i;
    for (i = 0; i < wparts; ++i)
    {
	detached[i] = 0;
    }

    hits = malloc(sizeof(int)*parts);
    for (i = 0; i < parts; ++i)
    {
        hits[i] = 0;
    }
    
    com_count = 6000000 / bsize; // get 6MB of cache
    coms = malloc(sizeof(struct buffer_combo) * com_count);
    for (i = 0; i < com_count; ++i)
	doener_init_buffer(i);

    int ret = fuse_main(args.argc, args.argv, &doener_oper, NULL);
    fclose(logger);
    return ret;
}
