/**
 * Copyright (c) 2009 Ma Can <ml.macana@gmail.com>
 *                           <macan@ncic.ac.cn>
 *
 * Armed with EMACS.
 * Time-stamp: <2009-12-14 15:08:27 macan>
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
#include "tx.h"
#include "mds.h"

struct hvfs_txg *txg_alloc(void)
{
    struct hvfs_txg *t;
    
    t = xzalloc(sizeof(struct hvfs_txg));
    if (!t)
        return NULL;
    /* init the lock region */
    xcond_init(&t->cond);
    xlock_init(&t->ckpt_lock);
    xlock_init(&t->delta_lock);
    xlock_init(&t->itb_lock);
    INIT_LIST_HEAD(&t->dirty_list);

    return t;
}

struct hvfs_txg *mds_get_open_txg(struct hvfs_mds_object *hmo)
{
    struct hvfs_txg *t;

retry:
    /* get the txg first */
    t = hmo->txg[TXG_OPEN];     /* atomic read */
    txg_get(t);
    /* checking the txg state */
    if (t->state != TXG_STATE_OPEN) {
        /* oh, txg switched, for correctnetss, retry myself */
        txg_put(t);
        goto retry;
    }

    return t;
}

/* txg_switch()
 *
 * NOTE: only one thread can call this function, and the WB txg should be
 * commited BEFORE calling this function!
 */
int txg_switch(struct hvfs_mds_info *hmi, struct hvfs_mds_object *hmo)
{
    struct hvfs_txg *nt;
    int err = 0;
    
    /* alloc a txg */
    nt = txg_alloc();
    if (!nt) {
        hvfs_err(mds, "xzalloc() struct hvfs_txg failed.\n");
        err = -ENOMEM;
        goto out;
    }

    /* make sure the WB txg is commited */
    ASSERT(hmo->txg[TXG_WB] == NULL, mds);

    /* atomic inc the txg # */
    atomic64_inc(&hmi->mi_txg);
    nt->txg = atomic64_read(&hmi->mi_txg);
    
    /* the current opened txg is going into WB state */
    hmo->txg[TXG_WB] = hmo->txg[TXG_OPEN];
    
    /* atomic swith to the current opened txg */
    hmo->txg[TXG_OPEN]->state = TXG_STATE_WB;
    hmo->txg[TXG_OPEN] = nt;
    
out:
    return err;
}

/* txg_add_itb()
 *
 * NOTE: adding the itb to the txg's dirty list.
 */
void txg_add_itb(struct hvfs_txg *txg, struct itb *i)
{
}

