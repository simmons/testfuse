/*
 * testfuse - a FUSE filesystem driver for deterministic pseudo-random
 *            files of various sizes.
 *
 * Copyright 2013 David Simmons
 * http://cafbit.com/
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * This program presents a filesystem containing files of various sizes
 * composed of deterministic, but pseudorandom data.  The goal is to
 * provide test files for various network speed testing, without being
 * bottlenecked by the I/O overhead of hard drives, SSDs, or traditional
 * filesystem drivers.  Tests show that testfuse can deliver file data
 * at about 7 Gbps on a circa-2009 Core i7 machine.
 *
 * The contents of these files are 100% deterministic -- two instances
 * of testfuse with the same parameters running on different machines
 * should result in identical filesystems.  This could be useful, for
 * instance, when staging a BitTorrent swarm.
 *
 * Usage:
 *     ./testfuse <file-spec-list> -f <mount-point>
 *
 * Example:
 *  $ ./testfuse testfile_1M,1M,1/testfile_1G,1G,0x02 -f /mnt/testfuse
 *  $ sha1sum /mnt/testfuse/testfile_1M
 *  1625df500068aa8b85370ba8d488fd4233d59ec1  /mnt/testfuse/testfile_1M
 *  $ sha1sum /mnt/testfuse/testfile_1M
 *  1625df500068aa8b85370ba8d488fd4233d59ec1  /mnt/testfuse/testfile_1M
 *  $ sha1sum /mnt/testfuse/testfile_1G
 *  d4c8ecd333785fcae74d11747d8e32bf066500b0  /mnt/testfuse/testfile_1G
 *  $ sha1sum /mnt/testfuse/testfile_1G
 *  d4c8ecd333785fcae74d11747d8e32bf066500b0  /mnt/testfuse/testfile_1G
 *  $ dd if=/mnt/testfuse/testfile_1G > /dev/null
 *  2097152+0 records in
 *  2097152+0 records out
 *  1073741824 bytes (1.1 GB) copied, 1.2609 s, 852 MB/s
 *
 * The file-spec-list argument is a slash-delimited list of file
 * specifications, each of which is a comma-delimited tuple indicating
 * the file name, the file size, and a 32-bit seed value.  Files of the
 * same size and seed value will always be identical.
 */

#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>

/* uncomment this only to debug edge conditions dealing with blocks */
//#define SMALL_BLOCK_TEST

#ifdef SMALL_BLOCK_TEST
 #define BLOCK_SIZE (16)
 #define BLOCK_SHIFT 4
 #define OFFSET_MASK (BLOCK_SIZE-1)
#else
 #define BLOCK_SIZE (64*1024)
 #define BLOCK_SHIFT 16
 #define OFFSET_MASK (BLOCK_SIZE-1)
#endif

/*
 * A testfile_t structure details a specific test file which will be
 * visible in the filesystem.
 */
typedef struct testfile_s {
    char *name;
    uint64_t size;
    uint32_t seed;
    struct testfile_s *next;
} testfile_t;
testfile_t *testfile_list = NULL;

/*
 * FUSE operation for delivering stat(2) data about our files.
 */
static int fop_getattr(const char *path, struct stat *st) {
    int ret = 0;

    memset(st, 0, sizeof(struct stat));
    if (strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        return 0;
    }

    // skip the leading slash
    if (path[0] == '/') {
        path++;
    }

    testfile_t *testfile;
    for (testfile = testfile_list; testfile!=NULL; testfile=testfile->next) {
        if (strcmp(path, testfile->name) == 0) {
            st->st_mode = S_IFREG | 0444;
            st->st_nlink = 1;
            st->st_size = testfile->size;
            return ret;
        }
    }

    return -ENOENT;
}

/*
 * The FUSE operation for delivering the directory list.
 */
static int fop_readdir(
    const char *path,
    void *buf,
    fuse_fill_dir_t filler,
    off_t offset,
    struct fuse_file_info *fi
) {
    if (strcmp(path, "/") != 0) {
        return -ENOENT;
    }

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    testfile_t *testfile;
    for (testfile = testfile_list; testfile!=NULL; testfile=testfile->next) {
        filler(buf, testfile->name, NULL, 0);
    }

    return 0;
}

/*
 * The FUSE operation for open().
 */
static int fop_open(const char *path, struct fuse_file_info *fi) {
    // skip the leading slash
    if (path[0] == '/') {
        path++;
    }

    testfile_t *testfile;
    for (testfile = testfile_list; testfile!=NULL; testfile=testfile->next) {
        if (strcmp(path, testfile->name) == 0) {
            if ((fi->flags & 3) != O_RDONLY) {
                return -EACCES;
            } else {
                return 0;
            }
        }
    }
    return -ENOENT;
}

/*
 * Combine the global seed, file seed, and the block number using a
 * CRC32 technique such that even a small change in one of the values
 * (i.e., file_seed=1 vs. file_seed=2, or the block number incrementing)
 * will result in a radical change to the output.
 */
static uint32_t crc(uint32_t global_seed, uint32_t file_seed, uint32_t block) {
    static const uint32_t polynomial = 0x04C11DB7U;
    static const uint32_t msb_mask = 0x80000000U;
    uint32_t input = global_seed;
    uint32_t input_next = file_seed;
    uint32_t divisor = msb_mask | (polynomial>>1);
    uint32_t divisor_next = (polynomial&0x01)<<31;
    int i;
    for (i=0; i<(3*sizeof(uint32_t)*8); i++) {
        // refill next
        if (i == 1*sizeof(uint32_t)*8) {
            input_next = block;
        }
        // xor
        if (input & msb_mask) {
            input ^= divisor;
            input_next ^= divisor_next;
        }
        // shift
        input <<= 1;
        if (input_next & msb_mask) {
            input |= 0x01;
        }
        input_next <<= 1;
    }
    return input;
}

/*
 * Use a xorshift algorithm to produce a deterministic pseudo-random
 * block of data.
 */
static void get_block(int block, char *buf, uint32_t file_seed) {
    static const uint32_t global_seed = 123456789;
    uint32_t x = crc(global_seed, file_seed, block);
    uint32_t y = 362436069;
    uint32_t z = 521288629;
    uint32_t w = 88675123;
    uint32_t *buf32 = (uint32_t*)buf;
    int i;
    for (i=0; i<(BLOCK_SIZE/sizeof(uint32_t)); i++) {
        uint32_t t = x ^ (x << 11);
        x = y; y = z; z = w;
        w = w ^ (w >> 19) ^ (t ^ (t >> 8));
        buf32[i] = w;
    }
}

/*
 * FUSE operation for fulfilling read() requests.
 */
static int fop_read(
    const char *path,
    char *buf,
    size_t size,
    off_t abs_offset,
    struct fuse_file_info *fi
) {
    // skip the leading slash
    if (path[0] == '/') {
        path++;
    }

    // lookup the file
    testfile_t *testfile;
    for (testfile = testfile_list; testfile!=NULL; testfile=testfile->next) {
        if (strcmp(path, testfile->name) == 0) {
            break;
        }
    }
    if (testfile == NULL) {
        return -ENOENT;
    }

    // limit to the size of the file
    if (abs_offset + size > testfile->size) {
        size = testfile->size - abs_offset;
    }

    size_t orig_size = size;
    while (size) {
        // consider the file to be made up of 64K blocks, each with its
        // own predictable pseudorandom context.  The use of uint32_t's
        // here limit the total size to 256TB.
        uint32_t block = abs_offset>>BLOCK_SHIFT;
        uint32_t offset = abs_offset & OFFSET_MASK;

        if (offset==0 && size>=BLOCK_SIZE) {
            // ideal case -- aligned buffer of our block size
            get_block(block, buf, testfile->seed);
            buf += BLOCK_SIZE;
            size -= BLOCK_SIZE;
            abs_offset += BLOCK_SIZE;
        } else {
            // fulfill partial-block reads
            char block_buffer[BLOCK_SIZE];
            get_block(block, block_buffer, testfile->seed);
            int bytes;
            if (size < (BLOCK_SIZE-offset)) {
                bytes = size;
            } else {
                bytes = BLOCK_SIZE-offset;
            }
            memcpy(buf, block_buffer+offset, bytes);
            buf += bytes;
            size -= bytes;
            abs_offset += bytes;
        }
    }

    return orig_size;
}

/*
 * Define our basic FUSE file operations.
 */
static struct fuse_operations fops = {
    .getattr        = fop_getattr,
    .readdir        = fop_readdir,
    .open           = fop_open,
    .read           = fop_read,
};

void usage() {
    fprintf(stderr, "usage: testfuse filename,size,seed[/...] /mnt/mntpoint\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        exit(EXIT_FAILURE);
    }

    // parse the test file parameters
    char *save_files;
    char *save_fields;
    char *file = strtok_r(argv[1], "/", &save_files);
    do {
        char *name = strtok_r(file, ",", &save_fields);
        char *size_str = strtok_r(NULL, ",", &save_fields);
        char *seed_str = strtok_r(NULL, ",", &save_fields);

        if ((! name) || (!size_str) || (!seed_str)) {
            usage();
            exit(EXIT_FAILURE);
        }

        // parse name
        if (name == '\0') {
            fprintf(stderr, "error: invalid name\n");
            exit(EXIT_FAILURE);
        }

        // parse size
        char *endptr;
        uint64_t size = strtoll(size_str, &endptr, 0);
        if (*endptr == 'k' || *endptr == 'K') {
            size *= 1024;
        } else if (*endptr == 'm' || *endptr == 'M') {
            size *= 1024*1024;
        } else if (*endptr == 'g' || *endptr == 'G') {
            size *= 1024*1024*1024;
        }
        if (size == 0) {
            fprintf(stderr, "error: invalid size\n");
            exit(EXIT_FAILURE);
        }

        // parse seed
        uint32_t seed = strtoll(seed_str, &endptr, 0);
        if (seed == 0 || *endptr != '\0') {
            fprintf(stderr, "error: invalid seed\n");
            exit(EXIT_FAILURE);
        }

        // add this <name,size,seed> tuple to the list
        testfile_t *testfile = malloc(sizeof(testfile_t));
        if (testfile == NULL) {
            fprintf(stderr, "error: out of memory\n");
            exit(EXIT_FAILURE);
        }
        testfile->name = strdup(name);
        if (testfile->name == NULL) {
            fprintf(stderr, "error: out of memory\n");
            exit(EXIT_FAILURE);
        }
        testfile->size = size;
        testfile->seed = seed;
        testfile->next = testfile_list;
        testfile_list = testfile;

    } while ((file=strtok_r(NULL, "/", &save_files)) != NULL);
    if (testfile_list == NULL) {
        fprintf(stderr, "error: no test files specified\n");
        exit(EXIT_FAILURE);
    }
    argc--;
    argv++;

    return fuse_main(argc, argv, &fops, NULL);
}

