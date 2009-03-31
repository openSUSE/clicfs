#include <stdint.h>
#include <stdio.h>
#include <limits.h>

extern int preset;
extern FILE *packfile;

extern char thefile[PATH_MAX];
extern size_t thefilesize;
extern uint64_t *sizes;
extern uint64_t *offs;
extern uint32_t parts;
extern uint32_t pindex;
extern size_t bsize;

struct block {
    uint32_t orig;
    uint32_t mapped;
};
extern struct block *blocks;

/* qsort int comparison function */
static inline int block_cmp(const void *a, const void *b)
{
    const struct block *ba = (const struct block *)a; // casting pointer types
    const struct block *bb = (const struct block *)b;
    return ba->orig  - bb->orig; 
}

/* slightly modified binary_search.
   If target is not found, return the index of the value that's
   in the array before it
*/
static inline int binary_search(struct block *A, int size, uint32_t target)
{
    int lo = 0, hi = size-1;
    if (target > A[hi].orig)
	return hi;
    while (lo <= hi) {
	int mid = lo + (hi-lo)/2;
	if (A[mid].orig == target)
	    return mid;
	else { 
	    if (A[mid].orig < target) 
		lo = mid+1;
	    else
		hi = mid-1;
	}
    }
    
    return hi;
}

extern int doenerfs_read_pack(const char *packfilename);
