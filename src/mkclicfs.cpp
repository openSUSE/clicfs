/* -*- c-file-style: "java"; indent-tabs-mode: nil; fill-column: 78; tab-width: 4 -*- */

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
#include <sys/sysinfo.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <pthread.h>

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

    //fprintf( stderr,  "compress %ld %ld %ld\n", insize, outsize, strm.total_out );
    return strm.total_out;
}

int blocksize = 32;
int infd = -1;
uint32_t *ublocks = 0;
uint32_t *found = 0;
bool check_dups = true;
uint32_t *blockindex = 0;
uint32_t num_pages = 0;
size_t pagesize = 4096;
uint32_t pindex = 0;
int preset = 2;

/* most of this logic is from mksquashfs - GPLv2+ */

/* struct describing queues used to pass data between threads */
struct queue {
        int                     size;
        int                     readp;
        int                     writep;
        pthread_mutex_t         mutex;
        pthread_cond_t          empty;
        pthread_cond_t          full;
        void                    **data;
};

static struct queue *from_reader, *to_writer;
static pthread_t *thread;

static int processors = -1;

struct queue *queue_init(int size)
{
        struct queue *queue = (struct queue*)malloc(sizeof(struct queue));

        if(queue == NULL)
                return NULL;

        if((queue->data = (void**)malloc(sizeof(void *) * (size + 1))) == NULL) {
                free(queue);
                return NULL;
        }

        queue->size = size + 1;
        queue->readp = queue->writep = 0;
        pthread_mutex_init(&queue->mutex, NULL);
        pthread_cond_init(&queue->empty, NULL);
        pthread_cond_init(&queue->full, NULL);

        return queue;
}

int queue_length(struct queue *queue)
{
   pthread_mutex_lock(&queue->mutex);
   int ret = (queue->writep - queue->readp + queue->size) % queue->size;
   pthread_mutex_unlock(&queue->mutex);
   return ret;
}

void queue_put(struct queue *queue, void *data)
{
    int nextp;

    pthread_mutex_lock(&queue->mutex);

    while((nextp = (queue->writep + 1) % queue->size) == queue->readp)
        pthread_cond_wait(&queue->full, &queue->mutex);

    queue->data[queue->writep] = data;
    queue->writep = nextp;
    pthread_cond_signal(&queue->empty);
    pthread_mutex_unlock(&queue->mutex);
}


void *queue_get(struct queue *queue)
{
        void *data;
        pthread_mutex_lock(&queue->mutex);

        while(queue->readp == queue->writep)
                pthread_cond_wait(&queue->empty, &queue->mutex);

        data = queue->data[queue->readp];
        queue->readp = (queue->readp + 1) % queue->size;
        pthread_cond_signal(&queue->full);
        pthread_mutex_unlock(&queue->mutex);

        return data;
}

// the number of really saved parts
uint32_t parts = 0;
uint32_t largeparts = 0;

struct inbuf_struct {
    size_t readin, totalin;
    unsigned char *inbuf;
    size_t part, bpp;
    bool lastblock;
};


void *reader(void *arg)
{
    int oldstate;

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldstate);

    uint32_t rindex = 0; // overall index
    uint32_t uindex = 0; // index for "unused" blocks

    std::map<std::string,uint32_t> dups;

    uint32_t currentblocksperpart = 0; // for debug output

    uint32_t usedblock = 0; // overall "mapped" index

    while ( rindex < num_pages ) {

        int currentblocks = 0;

        inbuf_struct *in = new inbuf_struct();
        in->inbuf = new unsigned char[100*blocksize*pagesize];
        in->readin = 0;
        in->totalin = 0;
        in->lastblock = false;

        size_t currentblocksize = blocksize;

        if (rindex + 1 < pindex) {
            currentblocksize = blocksize * 100;
            largeparts++;
        }

        //fprintf( stderr, "cbl %ld %ld %ld\n", currentblocksize, rindex, pindex );

        while ( currentblocks < currentblocksize ) {
            off_t cindex = 0;
            if (rindex < pindex) {
                cindex = ublocks[rindex];
            } else {
                while (found[uindex] && uindex < num_pages) uindex++;
                assert( uindex < num_pages );
                if ( uindex < num_pages ) {
                    cindex = uindex;
                    uindex++;
                }
            }
            if ( lseek( infd, cindex * pagesize, SEEK_SET) == -1 ) {
                perror( "seek" ); exit( 1 );
            }
            size_t diff= read( infd, in->inbuf + in->readin, pagesize);
            in->totalin += diff;
            std::string sm;
            if (check_dups)
                sm = calc_md5( in->inbuf + in->readin, diff );
            //fprintf( stderr, "block %ld %s\n", ( long )cindex, sm.c_str() );
            if ( check_dups && dups.find( sm ) != dups.end() ) {
                //fprintf( stderr, "already have %s\n", sm.c_str() );
                blockindex[cindex] = dups[sm];
            } else {
                blockindex[cindex] = usedblock++;
                dups[sm] = blockindex[cindex];
                in->readin += diff;
                currentblocks++;
            }
            //fprintf(stderr, "block %ld in part %ld\n", cindex, parts);
            rindex++;
            currentblocksperpart++;
            if ( rindex == num_pages ) {
                in->lastblock = true;
                break;
            }
        }
        in->part = parts++;
        in->bpp = currentblocksperpart;
        currentblocksperpart = 0;
        //fprintf( stderr, "put part %d %d\n", in->part, queue_length(from_reader));
        queue_put( from_reader, in );

    }
    thread[0] = 0;

    pthread_exit(NULL);
}


struct outbuf_struct
{
    unsigned char *outbuf;
    size_t insize, outsize, totalin;
    size_t part, bpp;
    bool lastblock;

    ~outbuf_struct() {
        delete [] outbuf;
    }
};

void *deflator(void *arg)
{
        int oldstate;

        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);
        pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldstate);

        while(1) {
            inbuf_struct *in = (inbuf_struct*)queue_get(from_reader);

            outbuf_struct *out = new outbuf_struct();
            out->outbuf = new unsigned char[100*blocksize*pagesize + 300];
//	    fprintf( stderr,  "compress start %ld %d %x %ld\n", pthread_self(), in->part, in->inbuf, in->readin );
            out->outsize = compress(preset, in->inbuf, in->readin, out->outbuf, 100*blocksize*pagesize + 300);
            out->part = in->part;
            out->insize = in->readin;
            out->bpp = in->bpp;
            out->lastblock = in->lastblock;
            out->totalin = in->totalin;

            //fprintf( stderr,  "compress %ld %d %x %ld -> %ld\n", pthread_self(), in->part, in->inbuf, in->readin, out->outsize );
            delete [] in->inbuf;
            delete in;

            queue_put(to_writer, out);
        }

    return 0;
}

void initialise_threads()
{
        int i;
        sigset_t sigmask, old_mask;

        sigemptyset(&sigmask);
        sigaddset(&sigmask, SIGINT);
        sigaddset(&sigmask, SIGQUIT);
        if(sigprocmask(SIG_BLOCK, &sigmask, &old_mask) == -1)
            exit( 1 );

        if(processors == -1) {
#ifndef linux
                int mib[2];
                size_t len = sizeof(processors);

                mib[0] = CTL_HW;
#ifdef HW_AVAILCPU
                mib[1] = HW_AVAILCPU;
#else
                mib[1] = HW_NCPU;
#endif

                if(sysctl(mib, 2, &processors, &len, NULL, 0) == -1) {
                        ERROR("Failed to get number of available processors.  Defaulting to 1\n");
                        processors = 1;
                }
#else
                processors = get_nprocs();
#endif
        }

        if((thread = (pthread_t*)malloc((2 + processors * 2) * sizeof(pthread_t))) == NULL)
            exit( 1 );

        from_reader = queue_init(20);
        to_writer = queue_init(20);
        pthread_create(&thread[0], NULL, reader, NULL);

        for(i = 0; i < processors; i++) {
                if(pthread_create(&thread[2+i], NULL, deflator, NULL) != 0 )
                    exit( 1 );
        }

        fprintf(stderr,  "Parallel mkclicfs: Using %d processor%s\n", processors,
                        processors == 1 ? "" : "s");

        if(sigprocmask(SIG_SETMASK, &old_mask, NULL) == -1)
            exit( 1 );
}

int writer(size_t oparts, off_t index_off, FILE *out, uint64_t *sizes, uint64_t *offs, size_t full_size)
{
    int oldstate;

    uint64_t total_in = 0;
    uint64_t total_out = 0;

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldstate);

    outbuf_struct **comps = new outbuf_struct*[oparts];
    memset( comps, 0, sizeof( void* )*oparts );
    size_t lastpart = -1;

    int lastpercentage = 0;

    struct timeval start;
    gettimeofday(&start, 0);

    while(1) {
        outbuf_struct *comp = (outbuf_struct*)queue_get(to_writer);
        //fprintf(stderr, "got %ld %ld %d %d\n", comp->part, lastpart, queue_length(to_writer), queue_length(from_reader));
        comps[comp->part] = comp;

        while ( comps[lastpart + 1] ) {
            comp = comps[++lastpart];

            fprintf( stderr,  "comp %ld %ld %ld\n", comp->part, ( long )comp->totalin, ( long )comp->outsize );
            sizes[comp->part] = comp->outsize;
            offs[comp->part] = total_out + index_off;
            total_in += comp->totalin;
            total_out += comp->outsize;
            if (fwrite(comp->outbuf, comp->outsize, 1, out) != 1) {
                perror("write"); return 1;
            }

            if ((int)(100 * total_in / full_size) > lastpercentage || comp->lastblock ) {
                struct timeval current;
                gettimeofday(&current, 0);
                fprintf(stderr, "part blocks:%d%% parts:%ld total:%d%% time:%d\n",
                        lastpercentage+1, (long)comp->part,
                        (int)(total_out * 100 / total_in), (current.tv_sec - start.tv_sec) * 1000 + ((current.tv_usec - start.tv_usec) / 1000 ));
                start.tv_sec = current.tv_sec;
                start.tv_usec = current.tv_usec;
                lastpercentage++;
            }

            if ( comp->lastblock ) {
                delete comp;
                goto out;
            }
            comps[comp->part] = 0;
            delete comp;
        }
    }
out:
    delete [] comps;
    return 0;
}

int main(int argc, char **argv)
{
    const char *profile = 0;
    bool usage = false;
    int opt;

    while ((opt = getopt(argc, argv, "dp:b:l:c:n:")) != -1) {
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
        case 'n':
	    processors = atoi(optarg);
	    if (processors < 1)
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
    if (!S_ISREG( st.st_mode )) {
	fprintf(stderr, "expecting regular file as input: %s\n", infile);
        return EXIT_FAILURE;
    }

    num_pages = st.st_size / pagesize;
    /* ext3 should be X blocks */
    if (num_pages * pagesize != st.st_size)
        num_pages++;

    // the number of original parts - to be saved in header
    uint32_t oparts = num_pages / blocksize;
    if (oparts * blocksize != num_pages)
        oparts++;

    found = ( uint32_t* )malloc(sizeof(uint32_t)*num_pages);
    memset(found, 0, sizeof(int)*num_pages);
    ublocks = ( uint32_t* )malloc(sizeof(uint32_t)*num_pages);
    memset(ublocks, 0, sizeof(int)*num_pages);

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

    infd = open(infile, O_RDONLY);
    FILE *out = fopen(outfile, "w");
    if (!out) {
        perror("open output");
        return 1;
    }

    uint64_t *sizes = ( uint64_t* )malloc(sizeof(uint64_t)*oparts);
    uint64_t *offs = ( uint64_t* )malloc(sizeof(uint64_t)*oparts);

    off_t index_off = 6;

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

    if (!writeindex(out, largeparts )) return 1;
    index_off += sizeof(uint32_t);

    if (!writeindex(out, blocksize )) return 1;
    index_off += sizeof(uint32_t);

    if (!writeindex(out, 100 * blocksize )) return 1;
    index_off += sizeof(uint32_t);

    if (!writeindex(out, pagesize )) return 1;
    index_off += sizeof(uint32_t);

    if (!writeindex(out, preset )) return 1;
    index_off += sizeof(uint32_t);

    if (!writeindex(out, num_pages )) return 1;
    index_off += sizeof(uint32_t);

    off_t index_blocks = index_off;
    index_off += num_pages * sizeof( uint32_t );
    blockindex = new uint32_t[num_pages];

    off_t index_part = index_off;
    index_off += 2 * oparts * sizeof(uint64_t) + sizeof(uint32_t);
    fseeko(out, index_off, SEEK_SET);

    initialise_threads();
    if ( writer(oparts, index_off, out, sizes, offs, st.st_size) )
        return 1;

    if (fseeko(out, index_blocks, SEEK_SET) < 0) {
        perror("seek"); return 1;
    }

    for (i = 0; i < num_pages; ++i)
        if (!writeindex(out, blockindex[i]))
            return 1;

    if (fseeko(out, index_part, SEEK_SET) < 0) {
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

    delete [] blockindex;
    free(offs);
    free(sizes);

    return 0;
}
