/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/ext2.h
 * Layer: Kernel / public internal interface
 *
 * Responsibilities:
 * - Declare kernel types, constants, and subsystem contracts.
 * - Keep cross-module ABI and structure expectations explicit.
 *
 * Notes:
 * - Header changes can ripple across kernel and user ABI glue.
 */

#ifndef _KERNEL_EXT2_H
#define _KERNEL_EXT2_H

#include <kernel/types.h>
#include <kernel/task.h>

#define EXT2_MAGIC          0xEF53
#define EXT2_ROOT_INO       2

/* i_mode flags */
#define EXT2_S_IFREG        0x8000
#define EXT2_S_IFDIR        0x4000
#define EXT2_S_IFCHR        0x2000
#define EXT2_S_IFBLK        0x6000
#define EXT2_S_IFIFO        0x1000
#define EXT2_S_IFLNK        0xA000

/* dir_entry file_type */
#define EXT2_FT_UNKNOWN     0
#define EXT2_FT_REG_FILE    1
#define EXT2_FT_DIR         2
#define EXT2_FT_CHRDEV      3
#define EXT2_FT_BLKDEV      4
#define EXT2_FT_FIFO        5
#define EXT2_FT_SYMLINK     7

/* Superblock — only the fields we actually use */
typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;       /* 0  */
    uint32_t s_blocks_count;       /* 4  */
    uint32_t s_r_blocks_count;     /* 8  */
    uint32_t s_free_blocks_count;  /* 12 */
    uint32_t s_free_inodes_count;  /* 16 */
    uint32_t s_first_data_block;   /* 20 — 1 for 1K blocks, 0 otherwise */
    uint32_t s_log_block_size;     /* 24 — block_size = 1024 << this */
    int32_t  s_log_frag_size;      /* 28 */
    uint32_t s_blocks_per_group;   /* 32 */
    uint32_t s_frags_per_group;    /* 36 */
    uint32_t s_inodes_per_group;   /* 40 */
    uint32_t s_mtime;              /* 44 */
    uint32_t s_wtime;              /* 48 */
    uint16_t s_mnt_count;          /* 52 */
    uint16_t s_max_mnt_count;      /* 54 */
    uint16_t s_magic;              /* 56 */
    uint16_t s_state;              /* 58 */
    uint16_t s_errors;             /* 60 */
    uint16_t s_minor_rev_level;    /* 62 */
    uint32_t s_lastcheck;          /* 64 */
    uint32_t s_checkinterval;      /* 68 */
    uint32_t s_creator_os;         /* 72 */
    uint32_t s_rev_level;          /* 76 — 0=old 1=dynamic */
    uint16_t s_def_resuid;         /* 80 */
    uint16_t s_def_resgid;         /* 82 */
    uint32_t s_first_ino;          /* 84 — dynamic rev only */
    uint16_t s_inode_size;         /* 88 — dynamic rev only */
} ext2_superblock_t;

/* Block group descriptor — 32 bytes */
typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} ext2_group_desc_t;

/* On-disk inode — 128 bytes (revision 0) */
typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;      /* in 512-byte units */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];   /* block pointers: [0..11] direct, [12] indirect */
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} ext2_inode_t;

/* Directory entry — variable length, always 4-byte aligned */
typedef struct __attribute__((packed)) {
    uint32_t inode;     /* 0 = deleted entry */
    uint16_t rec_len;   /* total length of this entry */
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];    /* not NUL-terminated */
} ext2_dir_entry_t;

/* Runtime filesystem state */
typedef struct {
    uint64_t lba_start;
    uint32_t block_size;
    uint32_t sectors_per_block;
    uint32_t first_data_block;
    uint32_t blocks_count;
    uint32_t groups_count;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t inode_size;
    uint32_t gdesc_block;   /* block number of the group descriptor table */
    bool     mounted;
} ext2_fs_t;

typedef struct {
    uint32_t mounted;
    uint32_t dirty;
    uint32_t block_size;
    uint32_t blocks_count;
    uint32_t groups_count;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t cache_hits;
    uint32_t cache_misses;
    uint32_t cache_writes;
    uint32_t cache_dirty_blocks;
    uint32_t writeback_batches;
    uint32_t cache_waits;
    uint32_t op_waits;
    uint32_t read_blocks;
    uint32_t write_blocks;
    uint32_t syncs;
    uint32_t sync_errors;
    uint32_t check_errors;
    uint32_t sb_free_blocks;
    uint32_t sb_free_inodes;
    uint32_t gd_free_blocks_sum;
    uint32_t gd_free_inodes_sum;
    uint32_t gd_used_dirs_sum;
} ext2_stats_t;

struct statfs;

inode_t* ext2_mount(uint64_t lba_start);
int ext2_statfs(struct statfs* st);
int ext2_sync(void);
void ext2_get_stats(ext2_stats_t* out);
int ext2_check(char* buf, size_t cap, size_t* len);
inode_t* ext2_create_file(inode_t* parent, const char* name, mode_t mode);
int ext2_link_inode(inode_t* parent, const char* name, inode_t* target);
int ext2_create_symlink(inode_t* parent, const char* name, const char* target);
int ext2_readlink_inode(inode_t* inode, char* buf, size_t bufsiz);
int ext2_truncate_inode(inode_t* inode);
int ext2_update_inode_metadata(inode_t* inode);

#endif
