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

enum
{
    PCA_VALID,
    PCA_USED,
    PCA_INVALID,
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

int *pca_status;

PCA_RULE table[LOGICAL_NAND_NUM][NAND_SIZE_KB * 1024 / 512], log_block_tail, data_block_tail;
int logBlocks[PHYSICAL_NAND_NUM - LOGICAL_NAND_NUM];

char IsLogBlock(int targetBlock)
{
    for (int index = 0; index < (PHYSICAL_NAND_NUM - LOGICAL_NAND_NUM); index++)
    {
        if (logBlocks[index] == targetBlock)
        {
            return 1;
        }
    }
    return 0;
}

static unsigned int ftl_gc();

PCA_RULE GetNextDataPage(PCA_RULE target)
{
    target.fields.page++;
    if (target.fields.page >= NAND_SIZE_KB * 1024 / 512)
    {
        target.fields.block++;
        target.fields.page = 0;
    }
    // WrapBack
    if (target.fields.block >= PHYSICAL_NAND_NUM)
    {
        printf("> Writing too much data!\n!> This program will now abort.\n");
        abort();
    }
    // If next block is log block -> select next one
    while (IsLogBlock(target.fields.block))
    {
        target.fields.block++;
        // WrapBack
        if (target.fields.block >= PHYSICAL_NAND_NUM)
        {
            printf("> Writing too much data!\n!> This program will now abort.\n");
            abort();
        }
    }
    return target;
}

PCA_RULE GetNextLogPage(PCA_RULE target)
{
    target.fields.page++;
    if (target.fields.page >= NAND_SIZE_KB * 1024 / 512)
    {
        target.fields.block++;
        target.fields.page = 0;
    }
    // WrapBack
    if (target.fields.block >= PHYSICAL_NAND_NUM)
    {
        ftl_gc();
        return log_block_tail;
    }
    // If next block is not log block -> select next one
    while (!IsLogBlock(target.fields.block))
    {
        target.fields.block++;
        // WrapBack
        if (target.fields.block >= PHYSICAL_NAND_NUM)
        {
            ftl_gc();
            return log_block_tail;
        }
    }
    return target;
}

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

static void update_pca_status(PCA_RULE pca, int status)
{

    pca_status[(pca.fields.block * NAND_SIZE_KB * 1024 / 512) + pca.fields.page] = status;
    printf("updating pca_status[%d] = %d\n", pca.fields.block * NAND_SIZE_KB * 1024 / 512 + pca.fields.page, status);
}

static int nand_erase(int block)
{
    char nand_name[100];
    // int found = 0;
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

    // original code, function unknown
    //  if (found == 0)
    //  {
    //      printf("nand erase not found\n");
    //      return -EINVAL;
    //  }

    printf("nand erase %d pass\n", block);
    for (int page_no = 0; page_no < NAND_SIZE_KB * 1024 / 512; page_no++)
    {
        PCA_RULE target;
        target.fields.block = block;
        target.fields.page = page_no;
        update_pca_status(target, PCA_VALID);
    }
    return 1;
}

static void print_pca_status_table()
{
    int i, j;

    for (j = 0; j < NAND_SIZE_KB * 1024 / 512; ++j)
    {
        for (i = 0; i < PHYSICAL_NAND_NUM; ++i)
        {
            printf("%2d", pca_status[(i * NAND_SIZE_KB * 1024 / 512) + j]);
        }
        printf("\n");
    }
}

///全新  江 - 再經 Frisk 修改
// 這是類似 full merge 的東西
static unsigned int ftl_gc(){
    printf("Starting GC\n");
    int s;  //專門用來暫存存了多少的計數器
    int i, j, k;
    PCA_RULE tmp_pca, write_pca;
    curr_pca.pca = INVALID_PCA;  //會用到並且歸0 init
    //////// 計算哪些 block 不需要動  (沒有任何 invalid 的 block)
    char eraseBlock[LOGICAL_NAND_NUM]={0};  // 0: 不需 erase，1: 需 erase
    for(i = 0; i < PHYSICAL_NAND_NUM; ++i){
        for (j = 0; j < NAND_SIZE_KB * 1024 / 512; ++j){
            if(pca_status[(i * NAND_SIZE_KB * 1024 / 512) + j] == PCA_INVALID){
                eraseBlock[i] = 1;
                break;
            }
        }
    }

    // 對每個需要處理的 data block 進行 merge
    for (int block_no = 0; block_no < LOGICAL_NAND_NUM; block_no++)
    {
        if (eraseBlock[block_no])
        {
            // 先備份並整理出有效的資料
            char backup[NAND_SIZE_KB * 1024 / 512][512];
            // 初始化 (該死的 C 語言！)
            for (int page_idx = 0; page_idx < NAND_SIZE_KB * 1024 / 512; page_idx++)
            {
                for (int ch_idx = 0; ch_idx < 512; ch_idx++)
                {
                    backup[page_idx][ch_idx] = '\0';
                }
            }
            
            char page_used[NAND_SIZE_KB * 1024 / 512] = {0}; // 0: 未使用，1: 已使用
            for (int page_no = 0; page_no < NAND_SIZE_KB * 1024 / 512; page_no++)
            {
                if (pca_status[(block_no * NAND_SIZE_KB * 1024 / 512) + page_no] == PCA_USED)
                {
                    PCA_RULE finding;
                    finding.fields.block = block_no;
                    finding.fields.page = page_no;
                    int result = nand_read(backup[page_no], finding.pca);
                    if (result < 0)
                    {
                        printf("> Found broken pca_status when performing GC!\n");
                        abort();
                    }
                    page_used[page_no] = 1;
                }
                else if (pca_status[(block_no * NAND_SIZE_KB * 1024 / 512) + page_no] == PCA_INVALID)
                {
                    PCA_RULE finding;
                    finding = table[block_no][page_no];
                    int result = nand_read(backup[page_no], finding.pca);
                    if (result < 0)
                    {
                        printf("> Found broken pca_status when performing GC!\n");
                        abort();
                    }
                    page_used[page_no] = 1;
                }
            }

            // 然後，將這整個 block erase 掉，再把所有資料寫回去
            nand_erase(block_no);
            for (int page_no = 0; page_no < NAND_SIZE_KB * 1024 / 512; page_no++)
            {
                printf("> Moving block %d, page %d back...\n", block_no, page_no);
                PCA_RULE target;
                target.fields.block = block_no;
                target.fields.page = page_no;
                if (page_used[page_no])
                {
                    int result = nand_write(backup[page_no], target.pca);
                    if (result < 0)
                    {
                        printf("> Error when writing back during GC!\n");
                        abort();
                    }
                    update_pca_status(target, PCA_USED);
                }
                else
                {
                    update_pca_status(target, PCA_VALID);
                }
                table[block_no][page_no] = target;
            }
        }
    }


    // 最後，把 log block 清空，然後 reset log_block_head
    for (int index = 0; index < PHYSICAL_NAND_NUM - LOGICAL_NAND_NUM; index++)
    {
        int target_block = logBlocks[index];
        nand_erase(target_block);
        for (int page_no = 0; page_no < NAND_SIZE_KB * 1024 / 512; page_no++)
        {
            PCA_RULE target;
            target.fields.block = target_block;
            target.fields.page = page_no;
            update_pca_status(target, PCA_VALID);
        }
    }
    log_block_tail.fields.block = LOGICAL_NAND_NUM;
    log_block_tail.fields.page = 0;
    
    
    printf("pca_status after GC:\n");
    print_pca_status_table();
    // return the next avalible pca
    return log_block_tail.pca;
}

// void CleanUpDataBlock()
// {
//     // Find invalid block (all page is invalid or used), and delete them.
//     for (int block_no = 0; block_no < PHYSICAL_NAND_NUM; block_no++)
//     {
//         // Check
//         char allInvalid = 1;
//         for (int page_no = 0; page_no < NAND_SIZE_KB * 1024 / 512; page_no++)
//         {
//             if (pca_status[(block_no * NAND_SIZE_KB * 1024 / 512) + page_no] == PCA_VALID)
//             {
//                 allInvalid = 0;
//                 break;
//             }
//         }

//         if (allInvalid)
//         {
//             nand_erase(block_no);
//             for (int page_no = 0; page_no < NAND_SIZE_KB * 1024 / 512; page_no++)
//             {
//                 PCA_RULE target;
//                 target.fields.block = block_no;
//                 target.fields.page = page_no;
//                 update_pca_status(target, PCA_VALID);
//             }
//         }
//     }
// }

////////////////////// 
static int ftl_read(char *buf, PCA_RULE read_target)
{
    printf("> Trying to read from block %d, page %d\n", read_target.fields.block, read_target.fields.page);
    PCA_RULE nowLoc;
    if (pca_status[(read_target.fields.block * NAND_SIZE_KB * 1024 / 512) + read_target.fields.page] == PCA_INVALID)
    {
        printf("> Data not present on original page\n");
        nowLoc = table[read_target.fields.block][read_target.fields.page];
        printf("> Reading from block %d, page %d\n", nowLoc.fields.block, nowLoc.fields.page);
    }
    else if (pca_status[(read_target.fields.block * NAND_SIZE_KB * 1024 / 512) + read_target.fields.page] == PCA_USED)
    {
        nowLoc = read_target;
    }
    else
    {
        printf("> No value stored!");
        return 0;
    }
    int result = nand_read(buf, nowLoc.pca);
    printf("> The value stored is %s\n", buf);
    return result;
}

static int ftl_write(const char *buf, PCA_RULE write_logical)
{
    PCA_RULE write_target;
    char push_log_block = 0;

    if (pca_status[(write_logical.fields.block * NAND_SIZE_KB * 1024 / 512) + write_logical.fields.page] == PCA_VALID)
    {
        printf("> Normal write\n");
        write_target = write_logical;
        printf("> Preparing to write \"%s\" to block %d, page %d\n", buf, write_target.fields.block, write_target.fields.page);
    }
    else if (pca_status[(write_logical.fields.block * NAND_SIZE_KB * 1024 / 512) + write_logical.fields.page] == PCA_INVALID)
    {
        printf("> Update write\n");
        // Invalid the previous page
        PCA_RULE nowLoc = table[write_logical.fields.block][write_logical.fields.page];
        update_pca_status(nowLoc, PCA_INVALID);
        // Put the data in the next available log page
        write_target = log_block_tail;
        // Register back to the mapping table
        table[write_logical.fields.block][write_logical.fields.page] = log_block_tail;
        printf("> Preparing to write \"%s\" to logical block %d, page %d; Which is now physical block %d, page %d\n",
               buf, write_logical.fields.block, write_logical.fields.page, log_block_tail.fields.block, log_block_tail.fields.page);
        // Push the log block tail back
        push_log_block = 1;
    }
    else if (pca_status[(write_logical.fields.block * NAND_SIZE_KB * 1024 / 512) + write_logical.fields.page] == PCA_USED)
    {
        printf("> 1st Update write\n");
        // Invalid the previous page
        update_pca_status(write_logical, PCA_INVALID);
        // Put the data in the next available log page
        write_target = log_block_tail;
        // Register back to the mapping table
        table[write_logical.fields.block][write_logical.fields.page] = log_block_tail;
        printf("> Preparing to write \"%s\" to logical block %d, page %d; Which is now physical block %d, page %d\n",
               buf, write_logical.fields.block, write_logical.fields.page, log_block_tail.fields.block, log_block_tail.fields.page);
        // Push the log block tail back
        push_log_block = 1;
    }

    printf("writing PCA: page %d, nand %d\n", write_target.fields.page, write_target.fields.block);
    if (nand_write(buf, write_target.pca) > 0)
    {
        update_pca_status(write_target, PCA_USED);
        print_pca_status_table();
        if (push_log_block)
        {
            log_block_tail = GetNextLogPage(log_block_tail);
        }
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
    PCA_RULE now_pca;

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
    now_pca.fields.block = offset / (NAND_SIZE_KB * 1024);
    now_pca.fields.page = (offset / 512) % (NAND_SIZE_KB * 1024 / 512);
    tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;
    tmp_buf = calloc(tmp_lba_range * 512, sizeof(char));

    for (int i = 0; i < tmp_lba_range; i++)
    {
        rst = ftl_read(tmp_buf + i * 512, now_pca);
        now_pca = GetNextDataPage(now_pca);
        tmp_lba++;
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
    int tmp_lba, process_size;
    int remain_size, rst;
    int i, read_rst;
    char *read_buf = malloc(512 * sizeof(char));
    memset(read_buf, 0, sizeof(char) * 512);
    char *write_buf = calloc(512, sizeof(char));
    memset(write_buf, 0, sizeof(char) * 512);
    PCA_RULE now_pca;

    host_write_size += size;
    if (ssd_expand(offset + size) != 0)
    {
        return -ENOMEM;
    }

    // the first lba to be written
    tmp_lba = offset / 512;
    // Find the first PCA to be written
    now_pca.fields.block = offset / (NAND_SIZE_KB * 1024);
    now_pca.fields.page = (offset / 512) % (NAND_SIZE_KB * 1024 / 512);
    // printf("tmp_lba: %d\n", tmp_lba);

    process_size = 0;
    remain_size = size;
    int first_offset = offset % 512;

    // not align at the start of the page
    if (first_offset != 0)
    {
        // printf("1. current lba: %d\n", tmp_lba);
        read_rst = ftl_read(read_buf, now_pca);
        if (read_rst == 0)
        {
            printf("Don't need to overwrite\n");
        }
        else
        {
            printf("first_offset:%d\n", first_offset);
        }

        for (i = 0; i < first_offset; ++i)
        {
            write_buf[i] = read_buf[i];
        }

        if (first_offset + remain_size < 512)
        {
            for (i = first_offset; i < first_offset + remain_size; ++i)
            {
                write_buf[i] = buf[i - first_offset];
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
                write_buf[i] = buf[i - first_offset];
            }
            process_size += (512 - first_offset);
            remain_size -= (512 - first_offset);
        }

        // printf("\nbuf to write:\n");
        // print_buffer(write_buf, 512, 0);
        rst = ftl_write(write_buf, now_pca);

        // Write full, return -enomem;
        if (rst == 0)
            return -ENOMEM;
        // Error
        else if (rst < 0)
            return rst;

        now_pca = GetNextDataPage(now_pca);
        ++tmp_lba;
    }

    // write the whole page
    while (remain_size >= 512)
    {
        // printf("2. current lba: %d\n", tmp_lba);
        // printf("\nbuf to write:\n");
        // print_buffer(buf + process_size, 512, 0);
        rst = ftl_write(buf + process_size, now_pca);
        // Write full, return -enomem;
        if (rst == 0)
            return -ENOMEM;
        // Error
        else if (rst < 0)
            return rst;

        process_size += 512;
        remain_size -= 512;
        now_pca = GetNextDataPage(now_pca);
        ++tmp_lba;
    }

    // write the remaining bytes
    if (remain_size > 0)
    {
        // printf("3. current lba: %d\n", tmp_lba);
        memset(read_buf, 0, sizeof(char) * 512);
        read_rst = ftl_read(read_buf, now_pca);
        if (read_rst == 0)
        {
            printf("Don't need to overwrite\n");
        }
        else
        {
            printf("remain_size:%d\n", remain_size);
        }

        for (i = 0; i < remain_size; ++i)
        {
            write_buf[i] = buf[process_size + i];
        }
        for (i = remain_size; i < 512; ++i)
        {
            write_buf[i] = read_buf[i];
        }

        // printf("\nbuf to write:\n");
        // print_buffer(write_buf, 512, 0);
        rst = ftl_write(write_buf, now_pca);

        // Write full, return -enomem;
        if (rst == 0)
            return -ENOMEM;
        // Error
        else if (rst < 0)
            return rst;

        process_size += remain_size;
        remain_size -= remain_size;
        now_pca = GetNextDataPage(now_pca);
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
    pca_status = malloc(PHYSICAL_NAND_NUM * NAND_SIZE_KB * 1024 / 512 * sizeof(int));
    memset(pca_status, PCA_VALID, PHYSICAL_NAND_NUM * NAND_SIZE_KB * 1024 / 512 * sizeof(int));
    
    // Initialize
    for (short block_no = 0; block_no < PHYSICAL_NAND_NUM; block_no++)
    {
        for (short page_no = 0; page_no < NAND_SIZE_KB * 1024 / 512; page_no++)
        {
            if (block_no < LOGICAL_NAND_NUM)
            {
                PCA_RULE package;
                package.fields.block = block_no;
                package.fields.page = page_no;
                table[block_no][page_no] = package;
            }
            else
            {
                logBlocks[block_no - LOGICAL_NAND_NUM] = block_no;
            }
        }
    }
    log_block_tail.fields.block = LOGICAL_NAND_NUM;
    log_block_tail.fields.page = 0;
    data_block_tail.fields.block = 0;
    data_block_tail.fields.page = 0;

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
