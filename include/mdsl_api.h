/**
 * Copyright (c) 2009 Ma Can <ml.macana@gmail.com>
 *                           <macan@ncic.ac.cn>
 *
 * Armed with EMACS.
 * Time-stamp: <2011-05-05 11:04:06 macan>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#ifndef __MDSL_API_H__
#define __MDSL_API_H__

struct si_core 
{
    u64 uuid;                   /* the dir UUID */
    /* 
     * for read/write: self uuid or self itbid
     */
    u64 arg0;
};

struct column_req
{
    u64 cno;                    /* requested column number */
    u64 stored_itbid;           /* stored itbid for selecting MDSL */
    u64 file_offset;            /* the offset of the MDSL file */
    u64 req_offset;             /* requested data offset in the logical file
                                 * region */
    u64 req_len;                /* requested data length */
};

/* storage index for mds */
struct si_mds 
{
    u32 cnr;
    u32 padding;
    struct column_req cr[0];
};

/* storage index for client */
struct si_client_data
{
    u32 cnr;                    /* column number */
#define SCD_PROXY       0x01    /* proxy file I/O */
#define SCD_LZO         0x02    /* lzo compressed */
    u32 flag;
    struct column_req cr[0];
};

/* search colump operations */
struct scop 
{
    u32 column;
    u16 len;                    /* key length, max key length: 64KB */
    u16 op;
    char key[0];
};

/* storage index for search */
struct si_search 
{
    u16 num;                    /* number of op[] */
    u16 op;
    int len;                    /* the total length of ops[] */
    struct scop ops[0];
};

struct storage_index 
{
    struct si_core sic;
    union                       /* MAKE SURE that the following structs are
                                 * the same size! */
    {
        struct si_client_data scd;
        struct si_mds sm;
        struct si_search ss;
    };
};

/* the result of the MDSL execution */
struct storage_result_core
{
    int err;
    int len;                    /* the total data length */
#define SR_BGSEARCH     0x01
#define SR_READ         0x02
#define SR_WRITE        0x03
#define SR_ITB          0x04
#define SR_BITMAP       0x05
#define SR_PRECOMMIT    0x06
    u64 flag;
};

struct storage_result 
{
    struct storage_result_core src;
    u8 data[0];
};

/* Region for TXG write back */
#define TXG_BEGIN_MAGIC         0x529be9a8
#define TXG_END_MAGIC           0x529adef8

struct txg_begin
{
    u32 magic;                  /* begin symbol: 0x529be9a8 */
    u32 dir_delta_nr;           /* # of dir deltas */
    u32 bitmap_delta_nr;        /* # of bitmap deltas */
    u32 ckpt_nr;                /* # of checkpoints */

    u64 txg;                    /* committed txg */
    u64 site_id;                /* committer site id */
    u64 session_id;             /* committer session id */

    u32 itb_nr;                 /* itb nr to saved to disk */
    u32 rdd_nr;                 /* # of remote dir deltas */
    u32 rd_nr;                  /* # of removed dir */

    /* backup the hmi */
    u64 mi_txg;
    u64 mi_uuid;
    u64 mi_fnum;
    u64 mi_dnum;
};

struct itb_info
{
    struct list_head list;
    u64 duuid;
    u64 itbid;
    u64 location;
    u32 master;                 /* which itb master file we writen to */
    u32 overwrite:1;            /* should we overwrite the range position? */
};

#define ITB_INFO_DISK_SIZE (sizeof(struct itb_info) - sizeof(struct list_head))

struct txg_open_entry
{
    struct list_head list;
    struct list_head itb;
    struct txg_begin begin;
    void *other_region;
    mcond_t cond;               /* for itb waiting */
    xcond_t wcond;              /* for itb read */
    xlock_t itb_lock;
    atomic_t itb_nr;
    atomic_t ref;
    int osize;
    int state;
};

struct txg_end
{
    u32 magic;                  /* end symbol: 0x529adef8 */
    u32 len;                    /* total length */
    u32 itb_nr;                 /* # of ITBs */
    int err;

    u64 txg;
    u64 site_id;
    u64 session_id;
};

struct bc_commit_core
{
    u64 uuid;
    u64 location;
    u64 itbid;
    u64 size;
};

struct data_file_header
{
    u64 uuid;                   /* this file's uuid */
    u64 length;                 /* file length */
};

#endif
