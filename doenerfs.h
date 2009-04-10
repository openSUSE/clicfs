#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include <sys/types.h>

extern int preset;
extern FILE *packfile;
extern FILE *cowfile;

#define DOENER_MAGIC 1

extern char thefile[PATH_MAX];
extern size_t thefilesize;
extern uint64_t *sizes;
extern uint64_t *offs;
extern uint32_t parts;
extern uint32_t pindex;
extern size_t bsize;
extern size_t num_pages;

extern unsigned char **blockmap;

extern int doenerfs_read_pack(const char *packfilename);
extern int doenerfs_read_cow(const char *packfilename);
extern void doener_decompress_part(unsigned char *out, const unsigned char *in, size_t size);
extern size_t doener_readpart(unsigned char *buffer, int part);
extern off_t doener_map_block(off_t block);
extern uint32_t doener_readindex(FILE *f);
