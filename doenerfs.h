#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include <sys/types.h>

extern int preset;
extern FILE *packfile;
extern int cowfilefd;

#define DOENER_MAGIC 1

extern char thefile[PATH_MAX];
extern size_t thefilesize;
extern uint64_t *sizes;
extern uint64_t *offs;
extern uint32_t parts;
extern uint32_t pindex;
extern size_t bsize;
extern uint32_t num_pages;
// the number of pages in the cow index
extern uint32_t cow_pages;

#define DOENER_COW_COUNT 100

// an array
extern uint32_t *cows;
extern unsigned int cows_index;

extern unsigned char **blockmap;

extern int doenerfs_read_pack(const char *packfilename);
extern int doenerfs_read_cow(const char *packfilename);
extern void doener_decompress_part(unsigned char *out, const unsigned char *in, size_t size);
extern size_t doener_readpart(unsigned char *buffer, int part);
extern off_t doener_map_block(off_t block);
extern uint32_t doener_readindex_fd(int fd );
