/*
  FUSE ssd: FUSE ioctl example
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>
  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/
#define FUSE_USE_VERSION 35
#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "ssd_fuse_header.h"
#define SSD_NAME "ssd_file"
enum
{
    SSD_NONE,
    SSD_ROOT,
    SSD_FILE,
};

static size_t physic_size;
static size_t logic_size;
static size_t host_write_size;
static size_t nand_write_size;

typedef union pca_rule PCA_RULE;
union pca_rule
{
    unsigned int pca;
    struct
    {
        unsigned int page : 16;
        unsigned int block : 16;
    } fields;
};

PCA_RULE curr_pca;

unsigned int *L2P;

static int ssd_resize(size_t new_size)
{
    // set logic size to new_size
    if (new_size >= LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024)
    {
        return -ENOMEM;
    }
    else
    {
        logic_size = new_size;
        return 0;
    }
}

static int ssd_expand(size_t new_size)
{
    // logical size must be less than logic limit

    if (new_size > logic_size)
    {
        return ssd_resize(new_size);
    }

    return 0;
}

static int nand_read(char *buf, int pca)
{
    char nand_name[100];
    FILE *fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.block);

    // read from nand
    if ((fptr = fopen(nand_name, "r")))
    {
        fseek(fptr, my_pca.fields.page * 512, SEEK_SET);
        fread(buf, 1, 512, fptr);
        fclose(fptr);
    }
    else
    {
        printf("open file fail at nand read pca = %d\n", pca);
        return -EINVAL;
    }
    return 512;
}
static int nand_write(const char *buf, int pca)
{
    char nand_name[100];
    FILE *fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.block);

    // write to nand
    if ((fptr = fopen(nand_name, "r+")))
    {
        fseek(fptr, my_pca.fields.page * 512, SEEK_SET);
        fwrite(buf, 1, 512, fptr);
        fclose(fptr);
        physic_size++;
    }
    else
    {
        printf("open file fail at nand (%s) write pca = %d, return %d\n", nand_name, pca, -EINVAL);
        return -EINVAL;
    }

    nand_write_size += 512;
    return 512;
}

static int nand_erase(int block)
{
    char nand_name[100];
    int found = 0;
    FILE *fptr;

    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, block);

    // erase nand
    if ((fptr = fopen(nand_name, "w")))
    {
        fclose(fptr);
    }
    else
    {
        printf("open file fail at nand (%s) erase nand = %d, return %d\n", nand_name, block, -EINVAL);
        return -EINVAL;
    }

    if (found == 0)
    {
        printf("nand erase not found\n");
        return -EINVAL;
    }

    printf("nand erase %d pass\n", block);
    return 1;
}

static unsigned int get_next_pca()
{
    /*  TODO: seq A, need to change to seq B */
    // pca has 32 bits with two part, 16 bits for page other 16 bits for block
    // the "block" means the storage (block1 is nand_1)
    if (curr_pca.pca == INVALID_PCA)
    {
        // init
        curr_pca.pca = 0;
        return curr_pca.pca;
    }
    else if (curr_pca.pca == FULL_PCA)
    {
        // ssd is full, no pca can be allocated
        printf("No new PCA\n");
        return FULL_PCA;
    }

    // if current page is the last page in the storage
    if (curr_pca.fields.page == (NAND_SIZE_KB * 1024 / 512))
    {
        //  implement this when making GC
        //  find next empty storage for new pca
        //  int i;
        //  for (i = 0; i < PHYSICAL_NAND_NUM; ++i){
        //      //to do: check if the storage_i is empty
        //  }

        // right shift 1 storage
        curr_pca.fields.block += 1;
        // set current page to 0
        curr_pca.fields.page = 0;
    }
    else
    {
        curr_pca.fields.page += 1;
    }

    // check if the current pca is out of range of storage
    if (curr_pca.fields.block >= PHYSICAL_NAND_NUM)
    {
        printf("No new PCA\n");
        curr_pca.pca = FULL_PCA;
        return FULL_PCA;
    }
    else
    {
        printf("PCA = page %d, nand %d\n", curr_pca.fields.page, curr_pca.fields.block);
        return curr_pca.pca;
    }

    // --------------------old code-------------------------
    // if current block is in the rightest storage
    // if (curr_pca.fields.block == PHYSICAL_NAND_NUM - 1)
    // {
    //     // down shift to the next row of pages
    //     curr_pca.fields.page += 1;
    // }

    // // cyclely right shift 1 storage every time
    // curr_pca.fields.block = (curr_pca.fields.block + 1) % PHYSICAL_NAND_NUM;

    // // check if the pca is full
    // if (curr_pca.fields.page >= (NAND_SIZE_KB * 1024 / 512))
    // {
    //     printf("No new PCA\n");
    //     curr_pca.pca = FULL_PCA;
    //     return FULL_PCA;
    // }
    // else
    // {
    //     printf("PCA = page %d, nand %d\n", curr_pca.fields.page, curr_pca.fields.block);
    //     return curr_pca.pca;
    // }
}

static int ftl_read(char *buf, size_t lba)
{
    PCA_RULE pca;

    pca.pca = L2P[lba];
    if (pca.pca == INVALID_PCA)
    {
        // data has not be written, return 0
        return 0;
    }
    else
    {
        return nand_read(buf, pca.pca);
    }
}

static int ftl_write(const char *buf, size_t lba_rnage, size_t lba)
{
    /*  TODO: only basic write case, need to consider other cases */
    PCA_RULE pca;
    pca.pca = get_next_pca();

    if (nand_write(buf, pca.pca) > 0)
    {
        L2P[lba] = pca.pca;
        return 512;
    }
    else
    {
        printf(" --> Write fail !!!");
        return -EINVAL;
    }
}

static int ssd_file_type(const char *path)
{
    if (strcmp(path, "/") == 0)
    {
        return SSD_ROOT;
    }
    if (strcmp(path, "/" SSD_NAME) == 0)
    {
        return SSD_FILE;
    }
    return SSD_NONE;
}
static int ssd_getattr(const char *path, struct stat *stbuf,
                       struct fuse_file_info *fi)
{
    (void)fi;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_atime = stbuf->st_mtime = time(NULL);
    switch (ssd_file_type(path))
    {
    case SSD_ROOT:
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        break;
    case SSD_FILE:
        stbuf->st_mode = S_IFREG | 0644;
        stbuf->st_nlink = 1;
        stbuf->st_size = logic_size;
        break;
    case SSD_NONE:
        return -ENOENT;
    }
    return 0;
}
static int ssd_open(const char *path, struct fuse_file_info *fi)
{
    (void)fi;
    if (ssd_file_type(path) != SSD_NONE)
    {
        return 0;
    }
    return -ENOENT;
}
static int ssd_do_read(char *buf, size_t size, off_t offset)
{
    int tmp_lba, tmp_lba_range, rst;
    char *tmp_buf;

    // out of limit
    if ((offset) >= logic_size)
    {
        return 0;
    }
    // size too big
    if (size > logic_size - offset)
    {
        // is valid data section
        size = logic_size - offset;
    }

    tmp_lba = offset / 512;
    tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;
    tmp_buf = calloc(tmp_lba_range * 512, sizeof(char));

    for (int i = 0; i < tmp_lba_range; i++)
    {
        rst = ftl_read(tmp_buf + i * 512, tmp_lba++);
        if (rst == 0)
        {
            // data has not be written, return empty data
            memset(tmp_buf + i * 512, 0, 512);
        }
        else if (rst < 0)
        {
            free(tmp_buf);
            return rst;
        }
    }

    memcpy(buf, tmp_buf + offset % 512, size);

    free(tmp_buf);
    return size;
}
static int ssd_read(const char *path, char *buf, size_t size,
                    off_t offset, struct fuse_file_info *fi)
{
    (void)fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_read(buf, size, offset);
}

static void print_buffer(const char *buf, size_t size, off_t offset)
{
    int i;

    printf("Buffer content at offset %ld:\n", offset);

    for (i = 0; i < size; ++i)
    {
        // Print each character directly
        // printf("%c", buf[i]);

        // Print each byte in hexadecimal format
        printf("%02X ", (unsigned char)buf[offset + i]);
    }
    printf("\n\n");
}

// buf: the data need to be written to the storage
// size: the size of the data in bytes
// offset: the logical offset in bytes
static int ssd_do_write(const char *buf, size_t size, off_t offset)
{
    /*  TODO: only basic write case, need to consider other cases */

    int tmp_lba, tmp_lba_range, process_size;
    int idx, curr_size, remain_size, rst;
    int i, read_rst;
    char *read_buf = malloc(512 * sizeof(char));
    memset(read_buf, 0, sizeof(char) * 512);
    char *write_buf = calloc(512, sizeof(char));
    memset(write_buf, 0, sizeof(char) * 512);

    host_write_size += size;
    if (ssd_expand(offset + size) != 0)
    {
        return -ENOMEM;
    }

    // the first lba to be written
    tmp_lba = offset / 512;
    printf("tmp_lba: %d\n", tmp_lba);
    // the number of lba should be written
    tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;
    // printf("tmp_lba_range: %d\n", tmp_lba_range);

    process_size = 0;
    remain_size = size;
    int first_offset = offset % 512;

    // not align at the start of the page
    if (first_offset != 0)
    {
        printf("1. current lba: %d\n", tmp_lba);
        read_rst = ftl_read(read_buf, tmp_lba);
        if (read_rst == 0)
        {
            printf("Don't need to overwrite\n");
        }
        else
        {
            printf("first_offset:%d\n", first_offset);
            printf("\nread_buf:\n");
            print_buffer(read_buf, 512, 0);
        }

        for (i = 0; i < first_offset; ++i)
        {
            write_buf[i] = read_buf[i];
        }

        if (first_offset + remain_size < 512)
        {
            for (i = first_offset; i < first_offset + remain_size; ++i)
            {
                write_buf[i] = buf[i];
            }
            for (i = first_offset + remain_size; i < 512; ++i)
            {
                write_buf[i] = read_buf[i];
            }
            process_size += (512 - (remain_size - first_offset));
            remain_size -= (512 - (remain_size - first_offset));
        }
        else
        {
            for (i = first_offset; i < 512; ++i)
            {
                // bug here
                write_buf[i] = buf[i];
            }
            process_size += (512 - first_offset);
            remain_size -= (512 - first_offset);
        }

        printf("\nbuf to write:\n");
        print_buffer(write_buf, 512, 0);
        rst = ftl_write(write_buf, 1, tmp_lba);

        // Write full, return -enomem;
        if (rst == 0)
            return -ENOMEM;
        // Error
        else if (rst < 0)
            return rst;

        ++tmp_lba;
    }

    // write the whole page
    while (remain_size >= 512)
    {
        printf("2. current lba: %d\n", tmp_lba);
        printf("\nbuf to write:\n");
        print_buffer(buf + process_size, 512, 0);
        rst = ftl_write(buf + process_size, 1, tmp_lba);
        // Write full, return -enomem;
        if (rst == 0)
            return -ENOMEM;
        // Error
        else if (rst < 0)
            return rst;

        process_size += 512;
        remain_size -= 512;
        ++tmp_lba;
    }

    // write the remaining bytes
    if (remain_size > 0)
    {
        printf("3. current lba: %d\n", tmp_lba);
        memset(read_buf, 0, sizeof(char) * 512);
        read_rst = ftl_read(read_buf, tmp_lba);
        if (read_rst == 0)
        {
            printf("Don't need to overwrite\n");
        }
        else
        {
            printf("remain_size:%d\n", remain_size);
            printf("\nread_buf:\n");
            print_buffer(read_buf, 512, 0);
        }

        for (i = 0; i < remain_size; ++i)
        {
            write_buf[i] = buf[process_size + i];
        }
        for (i = remain_size; i < 512; ++i)
        {
            write_buf[i] = read_buf[i];
        }

        printf("\nbuf to write:\n");
        print_buffer(write_buf, 512, 0);
        rst = ftl_write(write_buf, 1, tmp_lba);

        // Write full, return -enomem;
        if (rst == 0)
            return -ENOMEM;
        // Error
        else if (rst < 0)
            return rst;

        process_size += remain_size;
        remain_size -= remain_size;
        ++tmp_lba;
    }

    // for (idx = 0; idx < tmp_lba_range; idx++)
    // {
    // given code
    // if (offset % 512 == 0 && size % 512 == 0)
    // {
    //     rst = ftl_write(buf + process_size, 1, tmp_lba + idx);
    //     if (rst == 0)
    //     {
    //         // write full return -enomem;
    //         return -ENOMEM;
    //     }
    //     else if (rst < 0)
    //     {
    //         // error
    //         return rst;
    //     }
    //     curr_size += 512;
    //     remain_size -= 512;
    //     process_size += 512;
    //     offset += 512;
    // }
    // else
    // {
    //     printf(" --> Not align 512 !!!");
    //     return -EINVAL;
    // }
    // }

    free(read_buf);
    free(write_buf);

    return size;
}
static int ssd_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi)
{
    (void)fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_write(buf, size, offset);
}
static int ssd_truncate(const char *path, off_t size,
                        struct fuse_file_info *fi)
{
    (void)fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }

    return ssd_resize(size);
}
static int ssd_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi,
                       enum fuse_readdir_flags flags)
{
    (void)fi;
    (void)offset;
    (void)flags;
    if (ssd_file_type(path) != SSD_ROOT)
    {
        return -ENOENT;
    }
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    filler(buf, SSD_NAME, NULL, 0, 0);
    return 0;
}
static int ssd_ioctl(const char *path, unsigned int cmd, void *arg,
                     struct fuse_file_info *fi, unsigned int flags, void *data)
{

    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    if (flags & FUSE_IOCTL_COMPAT)
    {
        return -ENOSYS;
    }
    switch (cmd)
    {
    case SSD_GET_LOGIC_SIZE:
        *(size_t *)data = logic_size;
        printf(" --> logic size: %ld\n", logic_size);
        return 0;
    case SSD_GET_PHYSIC_SIZE:
        *(size_t *)data = physic_size;
        printf(" --> physic size: %ld\n", physic_size);
        return 0;
    case SSD_GET_WA:
        *(double *)data = (double)nand_write_size / (double)host_write_size;
        return 0;
    }
    return -EINVAL;
}
static const struct fuse_operations ssd_oper =
    {
        .getattr = ssd_getattr,
        .readdir = ssd_readdir,
        .truncate = ssd_truncate,
        .open = ssd_open,
        .read = ssd_read,
        .write = ssd_write,
        .ioctl = ssd_ioctl,
};
int main(int argc, char *argv[])
{
    int idx;
    char nand_name[100];
    physic_size = 0;
    logic_size = 0;
    nand_write_size = 0;
    host_write_size = 0;
    curr_pca.pca = INVALID_PCA;
    L2P = malloc(LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024 / 512 * sizeof(int));
    memset(L2P, INVALID_PCA, sizeof(int) * LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024 / 512);

    // create nand file
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
    {
        FILE *fptr;
        snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, idx);
        fptr = fopen(nand_name, "w");
        if (fptr == NULL)
        {
            printf("open fail");
        }
        fclose(fptr);
    }
    return fuse_main(argc, argv, &ssd_oper, NULL);
}
