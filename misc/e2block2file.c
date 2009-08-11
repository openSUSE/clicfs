#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>

#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <et/com_err.h>
#include <ext2fs/ext2fs.h>
#include <asm/byteorder.h>
#include <linux/hdreg.h>

#define INDIRECT_TO_OFF(offset, level) (-(((offset) << 2) | (level)))

#define LINEBUFLEN 4096
#define BLKTRACE_BLOCK_SIZE 512
#define MAX_PATH_LEN 32768

#define FL_NODIRNAMES 1
#define FL_DUMPTIMES 2

int options;
int blktrace_block_size = BLKTRACE_BLOCK_SIZE;
loff_t partition_start;
loff_t partition_end;

/* Structure containing one extent from the input */
struct input_extent_map {
	blk_t block;
	unsigned int len;
};

struct inode_blocks;

/* Possible type of mapped data */
enum block_map_type {
	BT_DATA,
	BT_SB,
	BT_GRPDESC,
	BT_ITABLE,
	BT_IBITMAP,
	BT_BBITMAP
};

/* Names of mapped data types */
#define MAPPED_DATA_NAMES { "data or indirect blocks", "superblock or its copy",\
"group descriptor", "inode table", "inode bitmap", "block bitmap" }

/* Structure mapping block extent to ino+offset. For indirect blocks we use
 * negative offsets. */
struct block_map {
	enum block_map_type type;
	blk_t block;
	unsigned int len;
	loff_t offset;
	union {
		struct block_map *next;
		struct inode_blocks *inode;
	} u;
};

/* Structure with internal data for block traversal */
struct traverse_data {
	ext2_ino_t ino;
	int tree_ind[2];	/* Index in the INDIRECT and DOUBLE_INDIRECT levels */
	int depth;	/* Depth of the tree we are currently searching */
	struct inode_blocks *inode;
};

/* Structure for keeping extents belonging to an inode */
struct inode_blocks {
	ext2_ino_t ino;
	struct block_map *blocks;
	struct block_map *last_block;
	char *name;
	struct inode_blocks *next;
	struct inode_blocks *next_hash;
};

FILE *block_file;			/* File with block numbers */
ext2_filsys fs;				/* Filesystem to work on */
unsigned int extent_count;		/* Number of extents to map */
struct input_extent_map *iextents;	/* Extents given in the input */
unsigned int map_count;			/* Number of mapped extents */
struct block_map *bmap;			/* Mapping of extents to ino+off */
unsigned int inode_count;		/* Number of inodes */
struct inode_blocks *inodes;		/* List of inodes */
struct inode_blocks **inode_hash;	/* Hash table of inodes */
int inode_hash_size;
char path[MAX_PATH_LEN];		/* Path to the current file */
int pathlen;
int has_filetype;	/* Does filesystem support the filetype feature? */
FILE *out;		/* Output file */

int log2i(unsigned num)
{
	int i;

	for (i = 0; i < sizeof(num)*8; i++)
		if ((1 << i) > num)
			return i-1;
	return 31;
}

#define IEXTENTS_ALLOC_ENTRIES 8192
#define BMAP_ALLOC_ENTRIES 8192

int input_extent_cmp(const void *ap, const void *bp)
{
	return ((struct input_extent_map *)ap)->block - ((struct input_extent_map *)bp)->block;
}

/* Load blocks to map into memory */
void load_blocks(void)
{
	long long blocknum;
	int shift = log2i(fs->blocksize/blktrace_block_size), i;
	int allocated = BMAP_ALLOC_ENTRIES;
	unsigned int len;
	char linebuf[LINEBUFLEN];

	iextents = malloc(sizeof(struct input_extent_map)*IEXTENTS_ALLOC_ENTRIES);
	if (!iextents) {
		fputs("Cannot get memory for block map.\n", stderr);
		exit(1);
	}
	while (fgets(linebuf, LINEBUFLEN, block_file) != NULL) {
			if (sscanf(linebuf, "%Ld+%u", &blocknum, &len) != 2)
				continue;
			if (extent_count >= allocated) {
				allocated += BMAP_ALLOC_ENTRIES;
				iextents = realloc(iextents,
					sizeof(struct input_extent_map)*allocated);
				if (!iextents) {
					fputs("Cannot get memory for block map.\n", stderr);
					exit(1);
				}
			}
			if (blocknum < partition_start) {
				fprintf(stderr, "Block %Ld before partition start. Ignoring.\n", blocknum);
				continue;
			}
			if (blocknum >= partition_end) {
				fprintf(stderr, "Block %Ld after partition end. Ignoring.\n", blocknum);
				continue;
			}
			iextents[extent_count].block = (blocknum-partition_start) >> shift;
			iextents[extent_count++].len = len >> shift;
	}
	if (!feof(block_file)) {
		fputs("Unable to parse input file with blocks.\n", stderr);
		exit(1);
	}
	qsort(iextents, extent_count, sizeof(struct input_extent_map), input_extent_cmp);
	/* Merge overlapping extents */
	shift = 0;
	for (i = 0; i < extent_count-shift; i++) {
		iextents[i] = iextents[i+shift];
		while (i+shift+1 < extent_count && iextents[i].block+iextents[i].len
		    >= iextents[i+shift+1].block) {
			if (iextents[i].block+iextents[i].len <
			    iextents[i+shift+1].block+iextents[i+shift+1].len)
				iextents[i].len = iextents[i+shift+1].block +
					iextents[i+shift+1].len - iextents[i].block;
			shift++;
		}
	}
	extent_count -= shift;
}

/* Offsets of block trees in the inode */
#define EXT2_IND_OFFSET(fs) (EXT2_NDIR_BLOCKS)
#define EXT2_DIND_OFFSET(fs) (EXT2_IND_OFFSET(fs)+((fs)->blocksize>>2))
#define EXT2_TIND_OFFSET(fs) (EXT2_DIND_OFFSET(fs)+\
				((fs)->blocksize>>2)*((fs)->blocksize>>2))

loff_t index_to_offset(ext2_filsys fs, int depth, int *index)
{
	if (depth == 0)
		return EXT2_IND_OFFSET(fs);
	if (depth == 1)
		return EXT2_DIND_OFFSET(fs) + index[0]*(fs->blocksize>>2);
	if (depth == 2)
		return EXT2_TIND_OFFSET(fs) + index[0]*(fs->blocksize>>2) +
			index[1]*(fs->blocksize>>2)*(fs->blocksize>>2);
	return 0;
}

struct input_extent_map *find_block(blk_t blocknr)
{
	int start = 0, end = extent_count-1, mid;

	while (start <= end) {
		mid = (start+end)/2;
		if (iextents[mid].block <= blocknr &&
		   iextents[mid].block+iextents[mid].len > blocknr)
			return iextents+mid;
		if (iextents[mid].block < blocknr)
			start = mid+1;
		else
			end = mid-1;
		if (blocknr < iextents[start].block || blocknr >=
		    iextents[end].block+iextents[end].len)
			return NULL;
	}
	return NULL;
}

struct inode_blocks *insert_new_inode(void)
{
	struct inode_blocks *inode = malloc(sizeof(struct inode_blocks));

	if (!inode) {
		fputs("Not enough memory for new inode.\n", stderr);
		exit(1);
	}
	inode->name = NULL;
	inode->blocks = NULL;
	inode->last_block = NULL;
	inode->next = inodes;
	inode->next_hash = NULL;
	inodes = inode;
	inode_count++;
	return inode;
}

struct block_map *get_new_block_map(void)
{
	static unsigned int bmap_allocated;

	if (map_count >= bmap_allocated) {
		bmap = realloc(bmap, (bmap_allocated+BMAP_ALLOC_ENTRIES)*sizeof(struct block_map));
		if (!bmap) {
			fputs("Not enough memory for extent mapings.\n", stderr);
			exit(1);
		}
		bmap_allocated += BMAP_ALLOC_ENTRIES;
	}
	return bmap+map_count++;
}

void insert_new_metadata(enum block_map_type type, blk_t block, loff_t offset)
{
	struct block_map *new;
	static struct block_map *last_itable;

	if (type == BT_ITABLE && last_itable && last_itable->offset +
	    last_itable->len == offset) {
		last_itable->len++;
		return;
	}
	new = get_new_block_map();
	new->type = type;
	new->block = block;
	new->len = 1;
	new->offset = offset;
	new->u.next = NULL;
	if (type == BT_ITABLE)
		last_itable = new;
}

void insert_new_block(struct inode_blocks *inode, loff_t offset, blk_t block)
{
	struct block_map *new;

	if (inode->last_block) {
		if (offset >= 0 && inode->last_block->block+inode->last_block->len == block &&
			inode->last_block->offset+inode->last_block->len == offset) {
			inode->last_block->len++;
			return;
		}
	}

	new = get_new_block_map();
	new->type = BT_DATA;
	new->block = block;
	new->offset = offset;
	new->len = 1;
	if (options & FL_DUMPTIMES)
		new->u.inode = inode;
	else {
		new->u.next = NULL;
		if (inode->last_block)
			inode->last_block->u.next = new;
		else
			inode->blocks = new;
	}
	inode->last_block = new;
}

int store_block(ext2_filsys fs, blk_t *blocknr, e2_blkcnt_t offset,
	blk_t parent_blk, int parent_offset, void *priv)
{
	blk_t d_blocknr = __le32_to_cpu(*blocknr);
	struct traverse_data *tdata = priv;
	struct input_extent_map *blk;
	int depth;

	if (offset < 0) {
		switch (offset) {
			case BLOCK_COUNT_IND:
				if (tdata->depth < 0)
					tdata->depth = 0;
				else
					tdata->tree_ind[0] = parent_offset>>2;
				depth = 0;
				break;
			case BLOCK_COUNT_DIND:
				if (tdata->depth < 1) {
					tdata->depth = 1;
					tdata->tree_ind[0] = 0;
				}
				else
					tdata->tree_ind[1] = parent_offset>>2;
				depth = 1;
				break;
			case BLOCK_COUNT_TIND:
				if (tdata->depth < 2) {
					tdata->depth = 2;
					tdata->tree_ind[0] = tdata->tree_ind[1] = 0;
				}
				depth = 2;
				break;
			default:
				fprintf(stderr, "Unknown block offset %Ld in "
						"blocks scan.\n", (long long)offset);
				exit(1);
		}
	}
	blk = find_block(d_blocknr);
	if (!blk)
		return 0;
	if (!tdata->inode)
		tdata->inode = insert_new_inode();
	tdata->inode->ino = tdata->ino;
	insert_new_block(tdata->inode, offset >= 0 ? offset :
		INDIRECT_TO_OFF(index_to_offset(fs, tdata->depth, tdata->tree_ind), depth),
		d_blocknr);
	return 0;
}

int store_block_ext(ext2_filsys fs, blk_t *blocknr, e2_blkcnt_t offset,
	blk_t parent_blk, int parent_offset, void *priv)
{
	blk_t d_blocknr = __le32_to_cpu(*blocknr);
	struct traverse_data *tdata = priv;
	struct input_extent_map *blk;

	if (offset < 0) {
		/* Block with extents */
		offset = -1;
	}

	blk = find_block(d_blocknr);
	if (!blk)
		return 0;
	if (!tdata->inode)
		tdata->inode = insert_new_inode();
	tdata->inode->ino = tdata->ino;
	insert_new_block(tdata->inode, offset, d_blocknr);
	return 0;
}

/* Map blocks to inodes and offsets */
void map_blocks(void)
{
	errcode_t error;
	ext2_inode_scan scan;
	ext2_ino_t inum;
	struct ext2_inode inode;
	char *blockbuf = malloc(fs->blocksize*3);
	struct traverse_data tdata;

	if (!blockbuf) {
		fputs("Not enough memory for block buffer.\n", stderr);
		exit(1);
	}
	if ((error = ext2fs_open_inode_scan(fs, 0, &scan))) {
		fprintf(stderr, "Cannot start inode scan: %s\n", error_message(error));
		exit(1);
	}
	if ((error = ext2fs_get_next_inode(scan, &inum, &inode))) {
		fprintf(stderr, "Cannot get first inode: %s\n", error_message(error));
		exit(1);
	}
	while (inum) {
		if (!inode.i_links_count || !(S_ISREG(__le16_to_cpu(inode.i_mode)) ||
		   S_ISDIR(__le16_to_cpu(inode.i_mode))))
			goto next;

		tdata.depth = -1;
		tdata.ino = inum;
		tdata.inode = NULL;
		tdata.tree_ind[0] = tdata.tree_ind[1] = 0;
		if (!(inode.i_flags & EXT4_EXTENTS_FL))
			error = ext2fs_block_iterate2(fs, inum, 0, blockbuf, store_block, &tdata);
		else
			error = ext2fs_block_iterate2(fs, inum, 0, blockbuf, store_block_ext, &tdata);
		if (error) {
			fprintf(stderr, "Failed to iterate over blocks of ino %d: %s\n", (int)inum, error_message(error));
			printf("%d\n", (int)error);
			exit(1);
		}
next:
		if ((error = ext2fs_get_next_inode(scan, &inum, &inode))) {
			fprintf(stderr, "Cannot get inode: %s\n", error_message(error));
			exit(1);
		}
	}
	ext2fs_close_inode_scan(scan);
}

#define HASH_INO(ino) ((ino) % inode_hash_size)

void dump_inode(struct inode_blocks *inode)
{
	struct block_map *bmap = inode->blocks;

	/* Skip inodes with no blocks. They will be written in inode table dump */
	if (!inode->blocks)
		return;
	if (!inode->name)
		fprintf(out, "Inode %d:\n", inode->ino);
	else
		fprintf(out, "%s (ino %d):\n", inode->name, inode->ino);
	while (bmap) {
		fprintf(out, "  %u+%u %Ld\n", (unsigned)bmap->block, (unsigned)bmap->len,
			(long long)bmap->offset);
		bmap = bmap->u.next;
	}
}

struct inode_blocks *find_inode(ext2_ino_t ino)
{
	struct inode_blocks *inode;
	int hashnum = HASH_INO(ino);

	for (inode = inode_hash[hashnum]; inode; inode = inode->next)
		if (inode->ino == ino)
			return inode;
	return NULL;
}

int scan_dir(struct ext2_dir_entry *dirent, int offset, int blocksize, char *buf, void *priv)
{
	ext2_ino_t ino = __le32_to_cpu(dirent->inode);
	int len = *(unsigned char *)&(dirent->name_len);
	errcode_t error;
	struct inode_blocks *iblocks;
	int dir = 0;

	if (dirent->name[0] == '.' && (len == 1 || (dirent->name[1] == '.'
		&& len == 2)))
		return 0;
	iblocks = find_inode(ino);
	if (has_filetype)
		dir = ((struct ext2_dir_entry_2 *)dirent)->file_type == EXT2_FT_DIR;
	else {
		struct ext2_inode inode;
		int mode;

		error = ext2fs_read_inode(fs, ino, &inode);
		if (error) {
			fprintf(stderr, "Cannot read inode: %s\n", error_message(error));
			exit(1);
		}
		mode = __le16_to_cpu(inode.i_mode);
		dir = S_ISDIR(mode);
	}
	if (iblocks || dir) {
		path[pathlen++] = '/';
		memcpy(path+pathlen, dirent->name, len);
		pathlen += len;
		path[pathlen] = 0;
		if (iblocks) {
			iblocks->name = strndup(path, pathlen);
			if (!iblocks->name) {
				fputs("Cannot allocate memory for file name.\n", stderr);
				exit(1);
			}
		}
		if (dir) {
			error = ext2fs_dir_iterate(fs, ino, 0, NULL, scan_dir, NULL);
			if (error) {
				fprintf(stderr, "Cannot scan directory %s\n", path);
				exit(1);
			}
		}
		pathlen -= len+1;
	}
	return 0;
}

/* Hash inodes for directory lookup */
void prepare_inode_hash(void)
{
	struct inode_blocks *inode;

	inode_hash_size = 32+inode_count+inode_count/2;	/* Make hash at least 32 entries */
	inode_hash = malloc(sizeof(struct inode_blocks *)*inode_hash_size);
	if (!inode_hash) {
		fputs("Cannot allocate enough memory for inode hash table.\n", stderr);
		exit(1);
	}
	memset(inode_hash, 0, sizeof(struct inode_blocks *)*inode_hash_size);
	for (inode = inodes; inode; inode = inode->next) {
		inode->next_hash = inode_hash[HASH_INO(inode->ino)];
		inode_hash[HASH_INO(inode->ino)] = inode;
	}
}

void map_inodes(void)
{
	errcode_t error;

	has_filetype = __le32_to_cpu(fs->super->s_feature_incompat) &
			EXT2_FEATURE_INCOMPAT_FILETYPE;
	prepare_inode_hash();
	error = ext2fs_dir_iterate(fs, EXT2_ROOT_INO, 0, NULL, scan_dir, NULL);
	if (error) {
		fprintf(stderr, "Cannot scan directory: %s\n", error_message(error));
		exit(1);
	}
}

int block_map_cmp(const void *a, const void *b)
{
	return ((struct block_map *)a)->block-((struct block_map *)b)->block;
}

void dump_inodes(void)
{
	struct inode_blocks *inode;
	char *desc[] = MAPPED_DATA_NAMES;
	int i;
	dgrp_t grp;
	ino_t ino, ino_base;

	for (inode = inodes; inode; inode = inode->next)
		dump_inode(inode);
	fputs("Other metadata:\n", out);
	for (i = 0; i < map_count; i++)
		if (bmap[i].type == BT_SB || bmap[i].type == BT_GRPDESC ||
		    bmap[i].type == BT_IBITMAP || bmap[i].type == BT_BBITMAP)
			fprintf(out, "  GRP %u: %s\n", ext2fs_group_of_blk(fs,
				bmap[i].block), desc[bmap[i].type]);
		else if (bmap[i].type == BT_ITABLE) {
			grp = ext2fs_group_of_blk(fs, bmap[i].block);
			ino_base = bmap[i].offset * EXT2_INODES_PER_BLOCK(fs->super)
				   + grp * EXT2_INODES_PER_GROUP(fs->super);
			fprintf(out, "  GRP %u: inode table blocks for inodes %u - %u",
				(unsigned)grp, (unsigned)ino_base,
				(unsigned)ino_base +
				bmap[i].len * EXT2_INODES_PER_BLOCK(fs->super) - 1);
			if (options & FL_NODIRNAMES) {
				fputc('\n', out);
				continue;
			}
			fprintf(out, " files:");
			for (ino = 0; ino < bmap[i].len * EXT2_INODES_PER_BLOCK(fs->super); ino++) {
				if (ext2fs_fast_test_inode_bitmap(fs->inode_map,
								  ino_base + ino)) {
					struct inode_blocks *inode = find_inode(ino + ino_base);

					if (!inode || !inode->name)
						fprintf(out, " #%Lu", (unsigned long long)(ino + ino_base));
					else
						fprintf(out, " %s", inode->name);
				}
			}
			fputc('\n', out);
		}
}

static inline unsigned int min(unsigned int a, unsigned int b)
{
	if (a < b)
		return a;
	return b;
}

struct block_map *find_map_block(blk_t blocknr)
{
	int start = 0, end = map_count-1, mid;

	while (start <= end) {
		mid = (start+end)/2;
		if (bmap[mid].block <= blocknr &&
		   bmap[mid].block+bmap[mid].len > blocknr)
			return bmap+mid;
		if (bmap[mid].block < blocknr)
			start = mid+1;
		else
			end = mid-1;
	}
	if (start >= map_count)
		return NULL;
	return bmap+start;
}

void printf_comment(FILE *f, char *comment, char *fmt, ...)
{
	va_list arg;

	va_start(arg, fmt);
	vfprintf(f, fmt, arg);
	if (comment)
		fprintf(f, " [%s]", comment);
	fputc('\n', f);
	va_end(arg);
}

void dump_times(void)
{
	long long blocknum;
	unsigned int len, cur_len;
	struct block_map *act_map;
	int shift = log2i(fs->blocksize/blktrace_block_size);
	char *desc[] = MAPPED_DATA_NAMES;
	dgrp_t grp;
	char linebuf[LINEBUFLEN], *comment;
	ino_t ino, ino_base;
	
	qsort(bmap, map_count, sizeof(struct block_map), block_map_cmp);
	fseek(block_file, 0, SEEK_SET);
	while (fgets(linebuf, LINEBUFLEN, block_file) != NULL) {
		if (sscanf(linebuf, "%Ld+%u", &blocknum, &len) != 2)
			continue;
		if (blocknum < partition_start || blocknum >= partition_end)
			continue;
		/* Strip \n */
		linebuf[strlen(linebuf) - 1] = 0;
		comment = strchr(linebuf, ' ');
		if (comment)
			comment++;
		blocknum = (blocknum - partition_start) >> shift;
		len >>= shift;
		while (len > 0) {
			act_map = find_map_block(blocknum);
			/* No extent after blocknum? */
			if (!act_map) {
				printf_comment(out, comment, "%Ld+%u unmapped", blocknum, len);
				len = 0;
				break;
			}
			/* Extent starting strictly after blocknum? */
			if (blocknum < act_map->block) {
				cur_len = min(act_map->block-blocknum, len);
				printf_comment(out, comment, "%Ld+%u unmapped", blocknum,
					cur_len);
				len -= cur_len;
				if (!len)
					break;
				blocknum += cur_len;
			}
			cur_len = min(len, act_map->len - (blocknum - act_map->block));
			fprintf(out, "%Ld+%u %Ld ", blocknum, cur_len,
				act_map->offset + (blocknum - act_map->block));
			switch (act_map->type) {
			    case BT_DATA:
				fprintf(out, "INO %u", (unsigned)act_map->u.inode->ino);
				if (act_map->u.inode->name)
					printf_comment(out, comment, " %s", act_map->u.inode->name);
				else
					printf_comment(out, comment, "");
				break;
			    case BT_SB:
			    case BT_GRPDESC:
			    case BT_IBITMAP:
			    case BT_BBITMAP:
				printf_comment(out, comment, "GRP %u: %s\n", ext2fs_group_of_blk(fs,
					act_map->block), desc[act_map->type]);
				break;
			    case BT_ITABLE:
				grp = ext2fs_group_of_blk(fs, act_map->block);
				ino_base = act_map->offset * EXT2_INODES_PER_BLOCK(fs->super)
					   + grp * EXT2_INODES_PER_GROUP(fs->super);
				fprintf(out, "GRP %u: inode table blocks for inodes %u - %u",
				(unsigned)grp, (unsigned)ino_base,
				(unsigned)(ino_base +
					   act_map->len * EXT2_INODES_PER_BLOCK(fs->super) - 1));
				for (ino = 0; ino < act_map->len * EXT2_INODES_PER_BLOCK(fs->super); ino++) {
					if (ext2fs_fast_test_inode_bitmap(fs->inode_map,
									  ino_base + ino)) {
						struct inode_blocks *inode = find_inode(ino + ino_base);

						if (!inode || !inode->name)
							fprintf(out, " #%Lu", (unsigned long long)(ino + ino_base));
						else
							fprintf(out, " %s", inode->name);
					}
				}
				printf_comment(out, comment, "");
				break;
			}
			len -= cur_len;
			blocknum += cur_len;
		}
	}
}

void parse_options(int argc, char **argv)
{
	int c;
	char *endc;
	int endarg, given_part = 0;
	errcode_t error;
	unsigned long long part_beg, part_len;

	while ((c = getopt(argc, argv, "hnb:o:tp:")) != -1) {
		switch (c) {
			case 'b':
				blktrace_block_size = strtol(optarg, &endc, 10);
				if (*endc || blktrace_block_size <= 0 ||
					(blktrace_block_size & (blktrace_block_size-1))) {
					fprintf(stderr, "Illegal block size %s\n", optarg);
					exit(1);
				}
				break;
			case 'p':
				if (sscanf(optarg, "%Lu+%Lu", &part_beg, &part_len) != 2) {
					fprintf(stderr, "Illegal partition description %s\n", optarg);
					exit(1);
				}
				partition_start = part_beg;
				partition_end = partition_start + part_len;
				given_part = 1;
				break;
			case 'o':
				out = fopen(optarg, "w");
				if (!out) {
					perror("Cannot open output file");
					exit(1);
				}
				break;
			case 'n':
				options |= FL_NODIRNAMES;
				break;
			case 't':
				options |= FL_DUMPTIMES;
				break;
			case '?':
			case 'h':
usage:
				fprintf(stderr, "Usage: e2block2file \
[-b device_block_size] [-o output_file] [-n] [-t] [-p partition_start+partition_len] <file-with-extents> <device>\n");
				exit(1);

		}
	}
	if (argc - optind != 2)
		goto usage;
	if (!out)
		out = stdout;
	endarg = optind;
	if (!strcmp(argv[endarg], "-"))
		block_file = stdin;
	else
		if (!(block_file = fopen(argv[endarg], "r"))) {
			perror("Cannot open file with extents");
			exit(1);
		}
	if ((error = ext2fs_open(argv[endarg+1], 0, 0, 0, unix_io_manager, &fs))) {
		fprintf(stderr, "Cannot open device %s: %s\n", argv[endarg+1],
			error_message(error));
		exit(1);
	}
	if (!given_part) {
		struct hd_geometry geom;
		uint64_t part_len;
		int fd = open(argv[endarg+1], O_RDONLY);

		if (fd < 0) {
			fprintf(stderr, "Cannot open device %s: %s\n", argv[endarg+1],
				strerror(errno));
			exit(1);
		}
		if (ioctl(fd, HDIO_GETGEO, &geom) < 0) {
			fprintf(stderr, "Cannot get partition start: %s\n"
				"Please set manually.\n", strerror(errno));
			exit(1);
		}
		if (ioctl(fd, BLKGETSIZE64, &part_len) < 0) {
			fprintf(stderr, "Cannot get partition length: %s\n"
				"Please set manually.\n", strerror(errno));
			exit(1);
		}
		close(fd);
		partition_start = geom.start;
		partition_end = partition_start + (part_len >> 9);
	}
}

/* Map blocks in inode tables, superblocks, bitmaps to inode numbers */
void map_blocks_metadata(void)
{
	unsigned int ext, blk;
	ino_t ino_base, ino;
	dgrp_t grp, cached_grp;
	blk_t sb_block, old_desc_block, new_desc_block;
	int ret;

	old_desc_block = new_desc_block = 0;
	ext2fs_super_and_bgd_loc(fs, 0, &sb_block, &old_desc_block, &new_desc_block, NULL);
	ret = ext2fs_read_inode_bitmap(fs);
	if (ret) {
		fprintf(stderr, "Failed to read inode bitmap: %d\n", ret);
		exit(1);
	}

	for (ext = 0; ext < extent_count; ext++)
		for (blk = iextents[ext].block; blk < iextents[ext].block + iextents[ext].len; blk++) {
			grp = ext2fs_group_of_blk(fs, blk);
			if (grp > cached_grp) {
				old_desc_block = new_desc_block = 0;
				ext2fs_super_and_bgd_loc(fs, 0, &sb_block, &old_desc_block, &new_desc_block, NULL);
			}
			if (sb_block == blk)
				insert_new_metadata(BT_SB, blk, 0);
			if (new_desc_block && new_desc_block == blk)
				insert_new_metadata(BT_GRPDESC, blk, 0);
			if (old_desc_block && blk >= old_desc_block && blk <
			    old_desc_block + fs->desc_blocks)
				insert_new_metadata(BT_GRPDESC, blk, blk - old_desc_block);
			if (fs->group_desc[grp].bg_block_bitmap == blk)
				insert_new_metadata(BT_BBITMAP, blk, 0);
			if (fs->group_desc[grp].bg_inode_bitmap == blk)
				insert_new_metadata(BT_IBITMAP, blk, 0);
			if (fs->group_desc[grp].bg_inode_table <= blk &&
			    fs->group_desc[grp].bg_inode_table + fs->inode_blocks_per_group > blk) {
				insert_new_metadata(BT_ITABLE, blk,
					blk - fs->group_desc[grp].bg_inode_table);
				ino_base = (blk - fs->group_desc[grp].bg_inode_table)
					   * EXT2_INODES_PER_BLOCK(fs->super)
					   + grp * EXT2_INODES_PER_GROUP(fs->super);
				for (ino = 0; ino < EXT2_INODES_PER_BLOCK(fs->super); ino++) {
					if (ext2fs_fast_test_inode_bitmap(fs->inode_map,
									  ino_base + ino)) {
						struct inode_blocks *inode;

						inode = insert_new_inode();
						inode->ino = ino_base + ino;
					}
				}
			}
		}
}

int main(int argc, char **argv)
{
	initialize_ext2_error_table();
	parse_options(argc, argv);

	fputs("Loading blocks to map...\n", stderr);
	load_blocks();
	fputs("Scanning filesystem to map blocks to inodes...\n", stderr);
	map_blocks();
	fputs("Mapping blocks to bitmaps and inode tables...\n", stderr);
	map_blocks_metadata();
	if (!(options & FL_NODIRNAMES)) {
		fputs("Scanning filesystem to map inodes to paths...\n", stderr);
		map_inodes();
	}
	if (!(options & FL_DUMPTIMES))
		dump_inodes();
	else
		dump_times();
	fputs("Done\n", stderr);

	ext2fs_close(fs);
	return 0;
}
