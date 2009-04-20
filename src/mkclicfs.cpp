/* -*- c-file-style: "java"; indent-tabs-mode: nil; fill-column: 78; tab-width: 4 -*-
 *
 * This file is part of Clic FS
 * Copyright (C) 2009 Stephan Kulow (coolo@suse.de)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
*/

//#define _GNU_SOURCE
#include "clicfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <lzma.h>
#include <limits.h>
#include <fcntl.h>

#include <openssl/md5.h>
#include <map>
#include <string>

static std::string calc_md5( unsigned char *d, size_t n )
{
    unsigned char md5[20];
    char md5s[33];
    
    MD5(d, n, md5);
    int j;
    for (j = 0; j < 16; ++j)
        sprintf(md5s+j*2, "%02x", md5[j]);
    md5s[32] = 0;
    return md5s;
}

static bool writeindex( FILE *out, const uint32_t &value )
{
    uint32_t string = value;
    if (fwrite((char*)&string, sizeof(uint32_t), 1, out) != 1) {
        perror("write");
        return false;
    }
    return true;
}

static size_t compress(int preset, unsigned char *in, size_t insize, unsigned char *out, size_t outsize)
{
    lzma_stream strm = LZMA_STREAM_INIT;
    lzma_ret ret = lzma_easy_encoder(&strm, preset, LZMA_CHECK_CRC32);
    assert(ret == LZMA_OK);

    strm.next_in = in;
    strm.avail_in = insize;
    strm.next_out = out;
    strm.avail_out = outsize;

    while (1) {
        ret = lzma_code(&strm, LZMA_RUN);
        //fprintf(stderr, "ret %d\n", ret);
        if (ret != LZMA_OK)
            break;
    }

    ret = lzma_code(&strm, LZMA_FINISH);
    //fprintf(stderr, "ret %d\n", ret);

    assert (ret == LZMA_STREAM_END);
    lzma_end(&strm);

    return strm.total_out;
}

int main(int argc, char **argv)
{
    bool check_dups = true;
    int blocksize = 32;
    int pagesize = pagesize;
    const char *profile = 0;
    int preset = 2;
    bool usage = false;
    int opt;

    while ((opt = getopt(argc, argv, "dp:b:l:c:")) != -1) {
        switch (opt) {
        case 'd':
            check_dups = false;
            break;
        case 'l':
            profile = strdup(optarg);
            break;
        case 'b':
            blocksize = atoi(optarg);
            if (blocksize <= 0)
                usage = true;
            break;
        case 'p':
            pagesize = atoi(optarg);
            if (pagesize <= 0)
                usage = true;
            break;
        case 'c':
            preset = atoi(optarg);
            if (preset < 0 || preset > 9)
                usage = true;
            break;
        default: /* '?' */
            usage = true;
            break;
        }
    }
    
    if (argc != optind + 2 || usage) {
        fprintf(stderr, "Usage: %s [-b <blocks>] [-p <pagesize>] [-d] [-c <preset>] [-l <logfile>] <infile> <outfile>\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    
    const char *infile = argv[optind++];
    const char *outfile = argv[optind++];

    struct stat st;
    stat(infile, &st);

    uint32_t num_pages = st.st_size / pagesize;
    /* ext3 should be X blocks */
    if (num_pages * pagesize != st.st_size)
        num_pages++;

    // the number of original parts - to be saved in header
    uint32_t oparts = num_pages / blocksize;
    if (oparts * blocksize != num_pages)
        oparts++;

    uint32_t *found = ( uint32_t* )malloc(sizeof(uint32_t)*num_pages);
    memset(found, 0, sizeof(int)*num_pages);
    uint32_t *ublocks = ( uint32_t* )malloc(sizeof(uint32_t)*num_pages);
    memset(ublocks, 0, sizeof(int)*num_pages);

    uint32_t pindex = 0;
    // always take the first block to make the algorithm easier
    ublocks[pindex++] = 0;
    found[0] = pindex;
    unsigned long i;
    if (profile) {
        FILE *pr = fopen(profile, "r");
        assert(pr);
        char line[200];
        while (fgets(line, sizeof(line)-1, pr)) {
            unsigned long offset, size;
            if (strncmp(line, "access ", 5))
                continue;
            if (sscanf(line, "access %ld+%ld", &offset, &size) == 2) {
                for (i = 0; i <= size; i++) {
                    if (offset + i < num_pages && found[offset+i] == 0) {
                        ublocks[pindex++] = offset + i;
                        found[offset + i] = pindex;
                    }
                }
            }
        }
        fclose(pr);
    }

    fprintf(stderr, "pindex %ld %ld\n", (long)pindex, (long)num_pages);

    int infd = open(infile, O_RDONLY);
    FILE *out = fopen(outfile, "w");

    unsigned char inbuf[blocksize*pagesize];
    unsigned char outbuf[blocksize*pagesize + 300];

    uint64_t total_in = 0;
    uint64_t total_out = 0;

    uint64_t *sizes = ( uint64_t* )malloc(sizeof(uint64_t)*oparts);
    uint64_t *offs = ( uint64_t* )malloc(sizeof(uint64_t)*oparts);

    off_t index_off = 6;
    int lastpercentage = 0;

    assert( DOENER_MAGIC < 100 );
    fprintf(out, "CLIC%02d", DOENER_MAGIC );

    char fname[PATH_MAX];
    strcpy(fname, basename(infile));
    uint32_t stringlen = strlen(fname);
    if (!writeindex(out,  stringlen )) return 1;
    if (fwrite(fname, stringlen, 1, out) != 1) {
        perror("write"); return 1;
    }
    index_off += sizeof(uint32_t) + stringlen;

    if (!writeindex(out, oparts )) return 1;
    index_off += sizeof(uint32_t);

    if (!writeindex(out, blocksize )) return 1;
    index_off += sizeof(uint32_t);

    if (!writeindex(out, pagesize )) return 1;
    index_off += sizeof(uint32_t);

    if (!writeindex(out, preset )) return 1;
    index_off += sizeof(uint32_t);

    if (!writeindex(out, num_pages )) return 1;
    index_off += sizeof(uint32_t);

    off_t index_blocks = index_off;
    index_off += num_pages * sizeof( uint32_t );
    uint32_t *blockmap = new uint32_t[num_pages];

    off_t index_part = index_off;
    index_off += 2 * oparts * sizeof(uint64_t) + sizeof(uint32_t);
    fseek(out, index_off, SEEK_SET);

    uint32_t rindex = 0; // overall index
    uint32_t uindex = 0; // index for "unused" blocks

    std::map<std::string,uint32_t> dups;

    // the number of really saved parts
    uint32_t parts = 0;
    uint32_t currentblocksperpart = 0; // for debug output
    uint32_t lastparts = 0; // for debug output

    uint32_t usedblock = 0; // overall "mapped" index

    while ( rindex < num_pages )
        {
            uint32_t currentblocks = 0;
            size_t readin = 0;

            while ( currentblocks < blocksize )
                {
                    off_t cindex = 0;
                    if (rindex < pindex) {
                        cindex = ublocks[rindex];
                    } else {
                        while (found[uindex] && uindex < num_pages)  uindex++;
                        assert( uindex < num_pages );
                        if ( uindex < num_pages ) {
                            cindex = uindex;
                            uindex++;
                        }
                    }
                    if ( lseek( infd, cindex * pagesize, SEEK_SET) == -1 ) {
                        perror( "seek" ); return 1;
                    }
                    size_t diff= read( infd, inbuf+readin, pagesize);
                    std::string sm;
                    if (check_dups) 
                        sm = calc_md5( inbuf+readin, diff );
                    if ( check_dups && dups.find( sm ) != dups.end() ) {
                        //fprintf( stderr, "already have %s\n", sm.c_str() );
                        blockmap[cindex] = dups[sm];
                    } else {
                        blockmap[cindex] = usedblock++;
                        dups[sm] = blockmap[cindex];
                        readin += diff;
                        currentblocks++;
                    }
                    //fprintf(stderr, "block %ld in part %ld\n", cindex, parts);
                    rindex++;
                    currentblocksperpart++;
                    if ( rindex == num_pages )
                        break;
                }
            size_t outsize = compress(preset, inbuf, readin, outbuf, blocksize*pagesize + 300);
            sizes[parts] = outsize;
            offs[parts] = total_out + index_off;
            total_in += readin;
            total_out += outsize;
            if (fwrite(outbuf, outsize, 1, out) != 1) {
                perror("write"); return 1;
            }

            parts++;
            if ((int)(rindex * 100. / num_pages) > lastpercentage || rindex >= num_pages - 1) {
                fprintf(stderr, "part blocks:%d%% parts:%ld bpp:%d current:%d%% total:%d%%\n",
                        (int)(rindex * 100. / num_pages), (long)parts,
                        (int)( currentblocksperpart / ( parts - lastparts ) ),
                        (int)(outsize * 100 / readin), (int)(total_out * 100 / total_in));
                lastpercentage++;
                lastparts = parts;
                currentblocksperpart = 0;
            }
        }

    if (fseek(out, index_blocks, SEEK_SET) < 0) {
        perror("seek"); return 1;
    }

    for (i = 0; i < num_pages; ++i)
        if (!writeindex(out, blockmap[i])) 
            return 1;

    if (fseek(out, index_part, SEEK_SET) < 0) {
        perror("seek"); return 1;
    }

    if (!writeindex(out, parts)) return 1;

    for (i = 0; i < parts; ++i) {
        if (fwrite((char*)(sizes + i), sizeof(uint64_t), 1, out) != 1 || 
            fwrite((char*)(offs + i), sizeof(uint64_t), 1, out) != 1) {
            perror("write"); return 1;
        }
    }
    // the remaining array parts (oparts-parts) stay sparse

    fclose(out);
    close( infd );

    delete [] blockmap;

    return 0;
}
