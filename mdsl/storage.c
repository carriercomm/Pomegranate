/**
 * Copyright (c) 2009 Ma Can <ml.macana@gmail.com>
 *                           <macan@ncic.ac.cn>
 *
 * Armed with EMACS.
 * Time-stamp: <2010-03-20 20:57:32 macan>
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

#include "hvfs.h"
#include "xnet.h"
#include "mdsl.h"

/* append_buf_create()
 *
 * @state: fde state w/ OPEN/READ/WRITE
 */
int append_buf_create(struct fdhash_entry *fde, char *name, int state)
{
    size_t buf_len;
    int err = 0;
    
    if (state == FDE_FREE)
        return 0;
    
    if (fde->type == MDSL_STORAGE_ITB) {
        buf_len = hmo.conf.itb_file_chunk;
    } else if (fde->type == MDSL_STORAGE_DATA) {
        buf_len = hmo.conf.data_file_chunk;
    } else {
        buf_len = MDSL_STORAGE_DEFAULT_CHUNK;
    }

    xlock_lock(&fde->lock);
    if (hmo.conf.itb_falloc) {
        fde->abuf.falloc_size = hmo.conf.itb_falloc * buf_len;
    } else {
        hmo.conf.itb_falloc = 1;
        fde->abuf.falloc_size = buf_len;
    }
    
    if (fde->state == FDE_FREE) {
        /* ok, we should open it */
        fde->fd = open(name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
        if (fde->fd < 0) {
            hvfs_err(mdsl, "open file '%s' failed\n", name);
            xlock_unlock(&fde->lock);
            return -EINVAL;
        }
        hvfs_err(mdsl, "open file %s w/ fd %d\n", name, fde->fd);
        fde->state = FDE_OPEN;
    }
    if (state == FDE_ABUF && fde->state == FDE_OPEN) {
        /* get the file end offset */
        fde->abuf.file_offset = lseek(fde->fd, 0, SEEK_END);
        if (fde->abuf.file_offset < 0) {
            hvfs_err(mdsl, "lseek to end of file %s failed w/ %d\n", 
                     name, errno);
            err = -errno;
            goto out_close;
        }
        fde->abuf.falloc_offset = fde->abuf.file_offset;
        err = ftruncate(fde->fd, fde->abuf.falloc_offset + fde->abuf.falloc_size);
        if (err) {
            hvfs_err(mdsl, "ftruncate file %s failed w/ %d\n",
                     name, err);
            goto out_close;
        }
        /* we create the append buf now */
        fde->abuf.addr = mmap(NULL, buf_len, PROT_WRITE | PROT_READ, 
                              MAP_SHARED, fde->fd, fde->abuf.file_offset);
        if (fde->abuf.addr == MAP_FAILED) {
            hvfs_err(mdsl, "mmap file %s in region [%ld,%ld] failed w/ %d\n",
                     name, fde->abuf.file_offset, 
                     fde->abuf.file_offset + buf_len, errno);
            err = -errno;
            goto out_close;
        }
        fde->abuf.len = buf_len;
        fde->state = FDE_ABUF;
    }

    xlock_unlock(&fde->lock);

    return err;
out_close:
    xlock_unlock(&fde->lock);
    /* close the file */
    close(fde->fd);
    fde->state = FDE_FREE;

    return err;
}

/* append_buf_flush()
 *
 * Note: holding the fde->lock
 * Return Value: 0 for no error, 1 for fallback, <0 for error.
 */
static inline
int append_buf_flush(struct fdhash_entry *fde, int flag)
{
    int err = 0;

    if (fde->state == FDE_ABUF) {
        if (flag & ABUF_ASYNC) {
            err = mdsl_aio_submit_request(fde->abuf.addr, fde->abuf.offset,
                                          fde->abuf.len, fde->abuf.file_offset, 
                                          flag, fde->fd);
            if (err) {
                hvfs_err(mdsl, "Submit AIO async request failed w/ %d\n", err);
                err = 1;
                goto fallback;
            }
            hvfs_info(mdsl, "ASYNC FLUSH offset %lx %p, submitted.\n", 
                      fde->abuf.file_offset, fde->abuf.addr);
        } else {
        fallback:
            err = msync(fde->abuf.addr, fde->abuf.offset, MS_SYNC);
            if (err < 0) {
                hvfs_err(mdsl, "msync() fd %d failed w/ %d\n", fde->fd, errno);
                err = -errno;
                goto out;
            }
            if (flag & ABUF_UNMAP) {
                err = munmap(fde->abuf.addr, fde->abuf.offset);
                if (err) {
                    hvfs_err(mdsl, "munmap() fd %d faield w/ %d\n", 
                             fde->fd, errno);
                    err = -errno;
                }
            }
            hvfs_info(mdsl, "sync flush offset %lx\n", fde->abuf.file_offset);
        }
        if (flag & ABUF_UNMAP)
            fde->state = FDE_ABUF_UNMAPPED;
    }
out:
    return err;
}

void append_buf_destroy(struct fdhash_entry *fde)
{
    int err;
    
    /* munmap the region */
    if (fde->state == FDE_ABUF) {
        hvfs_err(mdsl, "begin SYNC .\n");
        append_buf_flush(fde, ABUF_SYNC);
        hvfs_err(mdsl, "end SYNC .\n");
        err = munmap(fde->abuf.addr, fde->abuf.len);
        hvfs_err(mdsl, "end munmap.\n");
        if (err) {
            hvfs_err(mdsl, "munmap fd %d failed w/ %d\n", 
                     fde->fd, err);
        }
        fde->state = FDE_OPEN;
    }
}

/*
 * Note: holding the fde->lock
 */
int append_buf_flush_remap(struct fdhash_entry *fde)
{
    int err = 0;

    err = append_buf_flush(fde, ABUF_ASYNC | ABUF_UNMAP);
    if (err) {
        hvfs_err(mdsl, "ABUF flush failed w/ %d\n", err);
        goto out;
    }

    /* we try to remap another region now */
    switch (fde->state) {
    case FDE_ABUF:
        err = munmap(fde->abuf.addr, fde->abuf.len);
        if (err == -1) {
            hvfs_err(mdsl, "munmap ABUF failed w/ %d\n", errno);
            err = -errno;
            goto out;
        }
        fde->state = FDE_ABUF_UNMAPPED;
    case FDE_ABUF_UNMAPPED:
        fde->abuf.file_offset += fde->abuf.len;
        if (fde->abuf.file_offset + fde->abuf.len > fde->abuf.falloc_offset + 
            fde->abuf.falloc_size) {
            err = ftruncate(fde->fd, fde->abuf.falloc_offset + 
                            (fde->abuf.falloc_size << 1));
            if (err) {
                hvfs_err(mdsl, "fallocate fd %d failed w/ %d\n",
                         fde->fd, err);
                goto out;
            }
            fde->abuf.falloc_offset += fde->abuf.falloc_size;
            hvfs_debug(mdsl, "ftruncate offset %lx\n", fde->abuf.falloc_offset);
        }
        mdsl_aio_start();
        fde->abuf.addr = mmap(NULL, fde->abuf.len, PROT_WRITE | PROT_READ,
                              MAP_SHARED, fde->fd, fde->abuf.file_offset);
        if (fde->abuf.addr == MAP_FAILED) {
            hvfs_err(mdsl, "mmap fd %d in region [%ld,%ld] failed w/ %d\n",
                     fde->fd, fde->abuf.file_offset,
                     fde->abuf.file_offset + fde->abuf.len, errno);
            err = -errno;
            goto out;
        }
        fde->state = FDE_ABUF;
        fde->abuf.offset = 0;
        break;
    default:
        hvfs_err(mdsl, "ABUF flush remap w/ other state %x\n",
                 fde->state);
        err = -EINVAL;
    }

out:
    return err;
}

int append_buf_write(struct fdhash_entry *fde, struct mdsl_storage_access *msa)
{
    int err = 0;

    if (!msa->iov_nr || !msa->iov || fde->state == FDE_FREE) {
        return -EINVAL;
    }

    xlock_lock(&fde->lock);
    /* check the remained length of the Append Buffer */
    if (fde->abuf.offset + msa->iov->iov_len > fde->abuf.len) {
        /* we should mmap another region */
        err = append_buf_flush_remap(fde);
        if (err) {
            hvfs_err(mdsl, "ABUFF flush remap failed w/ %d\n", err);
            goto out_unlock;
        }
    }
    /* ok, we can copy the region to the abuf now */
    memcpy(fde->abuf.addr + fde->abuf.offset, msa->iov->iov_base, msa->iov->iov_len);
    if (fde->type == MDSL_STORAGE_ITB) {
        ((struct itb_info *)msa->arg)->location = fde->abuf.file_offset + 
            fde->abuf.offset;
    }
    ((struct itb_info *)msa->arg)->location = fde->abuf.file_offset +
        fde->abuf.offset;
    fde->abuf.offset += msa->iov->iov_len;
    atomic64_add(msa->iov->iov_len, &hmo.prof.storage.cpbytes);

out_unlock:
    xlock_unlock(&fde->lock);
    
    return err;
}

int mdsl_storage_init(void)
{
    int i;
    
    if (!hmo.conf.storage_fdhash_size) {
        hmo.conf.storage_fdhash_size = MDSL_STORAGE_FDHASH_SIZE;
    }

    hmo.storage.fdhash = xmalloc(hmo.conf.storage_fdhash_size *
                                 sizeof(struct regular_hash));
    if (!hmo.storage.fdhash) {
        hvfs_err(mdsl, "alloc fd hash table failed.\n");
        return -ENOMEM;
    }
    /* init the hash table */
    for (i = 0; i < hmo.conf.storage_fdhash_size; i++) {
        INIT_HLIST_HEAD(&(hmo.storage.fdhash + i)->h);
        xlock_init(&(hmo.storage.fdhash + i)->lock);
    }

    return 0;
}

void mdsl_storage_destroy(void)
{
    struct fdhash_entry *fde;
    struct hlist_node *pos, *n;
    time_t begin, current;
    int i, notdone, force_close = 0;

    begin = time(NULL);
    do {
        current = time(NULL);
        if (current - begin > 30) {
            hvfs_err(mdsl, "30 seconds passed, we will close all pending "
                     "fils forcely.\n");
            force_close = 1;
        }
        notdone = 0;
        for (i = 0; i < hmo.conf.storage_fdhash_size; i++) {
            xlock_lock(&(hmo.storage.fdhash + i)->lock);
            hlist_for_each_entry_safe(fde, pos, n, 
                                      &(hmo.storage.fdhash + i)->h, list) {
                if (atomic_read(&fde->ref) == 0 || force_close) {
                    hvfs_debug(mdsl, "Final close fd %d.\n", fde->fd);
                    if (fde->type == MDSL_STORAGE_ITB) {
                        append_buf_destroy(fde);
                    }
                    close(fde->fd);
                    hlist_del(&fde->list);
                    xfree(fde);
                } else {
                    notdone = 1;
                }
            }
            xlock_unlock(&(hmo.storage.fdhash + i)->lock);
        }
    } while (notdone);
}


static inline
struct fdhash_entry *mdsl_storage_fd_lookup(u64 duuid, int ftype, u64 arg)
{
    struct fdhash_entry *fde;
    struct hlist_node *pos;
    int idx;
    
    idx = hvfs_hash_fdht(duuid, ftype) % hmo.conf.storage_fdhash_size;
    xlock_lock(&(hmo.storage.fdhash + idx)->lock);
    hlist_for_each_entry(fde, pos, &(hmo.storage.fdhash + idx)->h, list) {
        if (duuid == fde->uuid && ftype == fde->type && arg == fde->arg) {
            atomic_inc(&fde->ref);
            xlock_unlock(&(hmo.storage.fdhash + idx)->lock);
            return fde;
        }
    }
    xlock_unlock(&(hmo.storage.fdhash + idx)->lock);

    return ERR_PTR(-EINVAL);
}

struct fdhash_entry *mdsl_storage_fd_insert(struct fdhash_entry *new)
{
    struct fdhash_entry *fde;
    struct hlist_node *pos;
    int idx, found = 0;

    idx = hvfs_hash_fdht(new->uuid, new->type) % hmo.conf.storage_fdhash_size;
    xlock_lock(&(hmo.storage.fdhash + idx)->lock);
    hlist_for_each_entry(fde, pos, &(hmo.storage.fdhash + idx)->h, list) {
        if (new->uuid == fde->uuid && new->type == fde->type &&
            new->arg == fde->arg) {
            atomic_inc(&fde->ref);
            found = 1;
            break;
        }
    }
    if (!found) {
        hlist_add_head(&new->list, &(hmo.storage.fdhash + idx)->h);
    }
    xlock_unlock(&(hmo.storage.fdhash + idx)->lock);

    if (found)
        return fde;
    else
        return new;
}

void mdsl_storage_fd_remove(struct fdhash_entry *new)
{
    int idx;

    idx = hvfs_hash_fdht(new->uuid, new->type) % hmo.conf.storage_fdhash_size;
    xlock_lock(&(hmo.storage.fdhash + idx)->lock);
    hlist_del(&new->list);
    xlock_unlock(&(hmo.storage.fdhash + idx)->lock);
}

/* mdsl_stroage_fd_normal()
 *
 * do normal file open.
 */
int mdsl_storage_fd_mdisk(struct fdhash_entry *fde, char *path)
{
    int err = 0;
    int size;
    
    xlock_lock(&fde->lock);
    if (fde->state == FDE_FREE) {
        /* ok, we should open it */
        fde->fd = open(path, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
        if (fde->fd < 0) {
            hvfs_err(mdsl, "open file '%s' failed\n", path);
            err = -EINVAL;
            goto out_unlock;
        }
        hvfs_err(mdsl, "open file %s w /fd %d\n", path, fde->fd);
        fde->state = FDE_OPEN;
    }
    if (fde->state == FDE_OPEN) {
        /* we should load the disk struct to memory */
        int br, bl = 0;

        do {
            br = pread(fde->fd, (void *)(&fde->mdisk) + bl, 
                       sizeof(struct md_disk) - bl, bl);
            if (br < 0) {
                hvfs_err(mdsl, "pread failed w/ %d\n", errno);
                err = -errno;
                goto out_unlock;
            } else if (br == 0) {
                hvfs_err(mdsl, "pread to EOF\n");
                err = -EINVAL;
                goto out_unlock;
            }
            bl += br;
        } while (bl < sizeof(struct md_disk));

        /* we alloc the region for the ranges */
        fde->mdisk.size = (fde->mdisk.range_nr[0] +
                           fde->mdisk.range_nr[1] +
                           fde->mdisk.range_nr[2]);
        size = fde->mdisk.size * sizeof(range_t);
        
        fde->mdisk.ranges = xzalloc(size);
        if (!fde->mdisk.ranges) {
            hvfs_err(mdsl, "xzalloc ranges failed\n");
            err = -ENOMEM;
            goto out_unlock;
        }

        /* load the ranges to memory */
        bl = 0;
        do {
            br = pread(fde->fd, (void *)fde->mdisk.ranges + bl,
                       size - bl, sizeof(struct md_disk) + bl);
            if (br < 0) {
                hvfs_err(mdsl, "pread failed w/ %d\n", errno);
                err = -errno;
                goto out_unlock;
            } else if (br == 0) {
                hvfs_err(mdsl, "pread to EOF\n");
                err = -EINVAL;
                goto out_unlock;
            }
            bl += br;
        } while (bl < size);
        fde->mdisk.new_range = NULL;
        fde->state = FDE_MDISK;
    }
out_unlock:
    xlock_unlock(&fde->lock);
    
    return 0;
}

int __normal_write(struct fdhash_entry *fde, struct mdsl_storage_access *msa)
{
    return 0;
}

int __mdisk_write(struct fdhash_entry *fde, struct mdsl_storage_access *msa)
{
    hvfs_err(mdsl, "You should not call this function, BUG!\n");
    
    return 0;
}

int __mdisk_sync(struct fdhash_entry *fde)
{
    return 0;
}

int __mdisk_lookup(struct fdhash_entry *fde, int op, u64 arg, void *out)
{
    int found = 0;
    int i;
    int err = 0;
    
    xlock_lock(&fde->lock);
    if (fde->state != FDE_MDISK) {
        err = -EINVAL;
        goto out_unlock;
    }

    if (op == MDSL_MDISK_RANGE) {
        if (fde->mdisk.ranges) {
            for (i = 0; i < fde->mdisk.size; i++) {
                if ((fde->mdisk.ranges + i)->begin <= arg && 
                    (fde->mdisk.ranges + i)->end > arg) {
                    found = 1;
                    break;
                }
                if ((fde->mdisk.ranges + i)->begin > arg)
                    break;
            }
            if (found) {
                *(int *)out = i;
                goto out_unlock;
            }
        }
        if (fde->mdisk.new_range) {
            int found = 0;

            for (i = 0; i < fde->mdisk.size; i++) {
                if ((fde->mdisk.new_range + i)->begin <= arg &&
                    (fde->mdisk.new_range + i)->end > arg) {
                    found = 1;
                    break;
                }
                if ((fde->mdisk.new_range + i)->begin > arg)
                    break;
            }
            if (found) {
                *(int *)out = i;
                goto out_unlock;
            }
        }
    }
    
out_unlock:
    xlock_unlock(&fde->lock);
    
    return err;
}

/* mdsl_storage_fd_mmap()
 *
 * @win: the window size of the mmap region
 */
int mdsl_storage_fd_mmap(struct fdhash_entry *fde, char *path, 
                         struct mmap_args *ma)
{
    int err = 0;
    
    xlock_lock(&fde->lock);
    if (fde->state == FDE_FREE) {
        /* ok, we should open it */
        fde->fd = open(path, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
        if (fde->fd < 0) {
            hvfs_err(mdsl, "open file '%s' failed\n", path);
            err = -EINVAL;
            goto out_unlock;
        }
        hvfs_err(mdsl, "open file %s w/ fd %d\n", path, fde->fd);
        fde->state = FDE_OPEN;
    }
    if (fde->state == FDE_OPEN) {
        /* do mmap on the region */
        err = lseek(fde->fd, 0, SEEK_SET);
        if (err < 0) {
            hvfs_err(mdsl, "fd %d mmap win created failed w/ %d.\n",
                     fde->fd, errno);
            err = -errno;
            goto out_unlock;
        }
        fde->mwin.addr = mmap(NULL, ma->win, PROT_WRITE | PROT_READ,
                              MAP_SHARED, fde->fd, ma->foffset);
        if (fde->mwin.addr == MAP_FAILED) {
            hvfs_err(mdsl, "mmap fd %d in region [%ld,%ld] failed w/ %d\n",
                     fde->fd, 0UL, ma->win, errno);
            err = -errno;
            goto out_unlock;
        }
        fde->mwin.offset = 0;
        fde->mwin.file_offset = ma->foffset;
        fde->mwin.len = ma->win;
        fde->state = FDE_MEMWIN;
    }
out_unlock:            
    xlock_unlock(&fde->lock);

    return err;
}

static inline
int mdsl_storage_fd_init(struct fdhash_entry *fde)
{
    char path[HVFS_MAX_NAME_LEN] = {0, };
    int err = 0;
    
    /* NOTE:
     *
     * 1. itb/data file should be written with self buffering through the mem
     *    window or not
     *
     * 2. itb/data file should be read through the mem window or direct read.
     *
     * 3. md/range file should be read/written with mem window
     */

    /* NOTE2: you should consider the concurrent access here!
     */

    /* make sure the duuid dir exist */
    sprintf(path, "%s/%ld/%ld", HVFS_MDSL_HOME, hmo.site_id, fde->uuid);
    err = mdsl_storage_dir_make_exist(path);
    if (err) {
        hvfs_err(mdsl, "duuid dir %s do not exist %d.\n", path, err);
        goto out;
    }

    /* please consider the concurrent access here in your subroutine! */

    switch (fde->type) {
    case MDSL_STORAGE_MD:
        sprintf(path, "%s/%ld/%ld/md", HVFS_MDSL_HOME, hmo.site_id, fde->uuid);
        err = mdsl_storage_fd_mdisk(fde, path);
        if (err) {
            hvfs_err(mdsl, "change state to open failed w/ %d\n", err);
            goto out;
        }
        break;
    case MDSL_STORAGE_ITB:
        sprintf(path, "%s/%ld/%ld/itb-%ld", HVFS_MDSL_HOME, hmo.site_id, 
                fde->uuid, fde->arg);
        err = append_buf_create(fde, path, FDE_ABUF);
        if (err) {
            hvfs_err(mdsl, "append buf create failed w/ %d\n", err);
            goto out;
        }
        break;
    case MDSL_STORAGE_RANGE:
    {
        struct mmap_args *ma = (struct mmap_args *)fde->arg;
        
        sprintf(path, "%s/%ld/%ld/range-%ld", HVFS_MDSL_HOME, hmo.site_id, 
                fde->uuid, ma->range_id);
        err = mdsl_storage_fd_mmap(fde, path, ma);
        if (err) {
            hvfs_err(mdsl, "mmap window created failed w/ %d\n", err);
            goto out;
        }
        break;
    }
    case MDSL_STORAGE_DATA:
        sprintf(path, "%s/%ld/%ld/data-%ld", HVFS_MDSL_HOME, hmo.site_id, 
                fde->uuid, fde->arg);
        break;
    case MDSL_STORAGE_DIRECTW:
        sprintf(path, "%s/%ld/%ld/directw", HVFS_MDSL_HOME, hmo.site_id, 
                fde->uuid);
        break;
    case MDSL_STORAGE_LOG:
        sprintf(path, "%s/%ld/log", HVFS_MDSL_HOME, hmo.site_id);
        break;
    case MDSL_STORAGE_SPLIT_LOG:
        sprintf(path, "%s/%ld/split_log", HVFS_MDSL_HOME, hmo.site_id);
        break;
    case MDSL_STORAGE_TXG:
        sprintf(path, "%s/%ld/txg", HVFS_MDSL_HOME, hmo.site_id);
        break;
    case MDSL_STORAGE_TMP_TXG:
        sprintf(path, "%s/%ld/tmp_txg", HVFS_MDSL_HOME, hmo.site_id);
        break;
    default:
        hvfs_err(mdsl, "Invalid file type provided, check your codes.\n");
        err = -EINVAL;
        goto out;
    }

out:
    return err;
}

struct fdhash_entry *mdsl_storage_fd_lookup_create(u64 duuid, int fdtype, u64 arg)
{
    struct fdhash_entry *fde;
    int err = 0;
    
    fde = mdsl_storage_fd_lookup(duuid, fdtype, arg);
    if (!IS_ERR(fde)) {
        if (fde->state <= FDE_OPEN) {
            goto reinit;
        } else 
            return fde;
    }

    /* Step 1: create a new fdhash_entry */
    fde = xzalloc(sizeof(*fde));
    if (!fde) {
        hvfs_err(mdsl, "xzalloc struct fdhash_entry failed.\n");
        return ERR_PTR(-ENOMEM);
    } else {
        struct fdhash_entry *inserted;
        
        /* init it */
        INIT_HLIST_NODE(&fde->list);
        xlock_init(&fde->lock);
        atomic_set(&fde->ref, 1);
        fde->uuid = duuid;
        fde->arg = arg;
        fde->type = fdtype;
        fde->state = FDE_FREE;
        /* insert into the fdhash table */
        inserted = mdsl_storage_fd_insert(fde);
        if (inserted != fde) {
            hvfs_warning(mdsl, "someone insert this fde before us.\n");
            xfree(fde);
            fde = inserted;
        }
    }

    /* Step 2: we should open the file now */
reinit:
    err = mdsl_storage_fd_init(fde);
    if (err) {
        goto out_clean;
    }
    
    return fde;
out_clean:
    /* we should release the fde on error */
    if (atomic_dec_return(&fde->ref) == 0) {
        mdsl_storage_fd_remove(fde);
        xfree(fde);
    }
    
    return ERR_PTR(err);
}

int mdsl_storage_fd_write(struct fdhash_entry *fde, 
                          struct mdsl_storage_access *msa)
{
    int err = 0;

retry:
    if (fde->state == FDE_ABUF) {
        err = append_buf_write(fde, msa);
        if (err) {
            hvfs_err(mdsl, "append_buf_write failed w/ %d\n", err);
            goto out_failed;
        }
    } else if (fde->state == FDE_MEMWIN) {
    } else if (fde->state == FDE_NORMAL) {
        err = __normal_write(fde, msa);
        if (err) {
            hvfs_err(mdsl, "__normal_write faield w/ %d\n", err);
            goto out_failed;
        }
    } else if (fde->state == FDE_MDISK) {
        err = __mdisk_write(fde, msa);
        if (err) {
            hvfs_err(mdsl, "__mdisk_write failed w/ %d\n", err);
            goto out_failed;
        }
    } else if (fde->state == FDE_OPEN) {
        /* we should change to ABUF or MEMWIN or NORMAL access mode */
        err = mdsl_storage_fd_init(fde);
        if (err) {
            hvfs_err(mdsl, "try to change state failed w/ %d\n", err);
            goto out_failed;
        }
        goto retry;
    } else {
        /* we should (re-)open the file */
    }

    atomic64_inc(&hmo.prof.storage.wreq);
out_failed:
    return err;
}

int mdsl_storage_dir_make_exist(char *path)
{
    int err;
    
    err = mkdir(path, 0755);
    if (err) {
        err = -errno;
        if (errno == EEXIST) {
            err = 0;
        } else if (errno == EACCES) {
            hvfs_err(mdsl, "Failed to create the dir %s, no permission.\n",
                     path);
        } else {
            hvfs_err(mdsl, "mkdir %s failed w/ %d\n", path, errno);
        }
    }
    
    return err;
}