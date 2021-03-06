/**
 * Copyright (c) 2012 Ma Can <ml.macana@gmail.com>
 *                           <macan@ncic.ac.cn>
 *
 * Armed with EMACS.
 * Time-stamp: <2012-12-11 17:47:06 macan>
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

#include "osd.h"
#include "lib.h"

#ifdef HVFS_TRACING
u32 hvfs_osd_tracing_flags = HVFS_DEFAULT_LEVEL;
#endif

/* global variable */
struct hvfs_osd_info hoi;
struct hvfs_osd_object hoo;

static void osd_sigaction_default(int signo, siginfo_t *info, void *arg)
{
    if (signo == SIGSEGV) {
        hvfs_info(osd, "Recv %sSIGSEGV%s %s\n",
                  HVFS_COLOR_RED,
                  HVFS_COLOR_END,
                  SIGCODES(info->si_code));
        lib_segv(signo, info, arg);
    } else if (signo == SIGBUS) {
        hvfs_info(osd, "Recv %sSIGBUS%s %s\n",
                  HVFS_COLOR_RED,
                  HVFS_COLOR_END,
                  SIGCODES(info->si_code));
        lib_segv(signo, info, arg);
    } else if (signo == SIGHUP) {
        hvfs_info(osd, "Exit OSD Server ...\n");
        osd_destroy();
        exit(0);
    } else if (signo == SIGUSR1) {
        hvfs_info(osd, "Exit some threads ...\n");
        pthread_exit(0);
    }
    
}

/* osd_init_signal()
 */
static int osd_init_signal(void)
{
    struct sigaction ac;
    int err;

    ac.sa_sigaction = osd_sigaction_default;
    err = sigemptyset(&ac.sa_mask);
    if (err) {
        err = errno;
        goto out;
    }
    ac.sa_flags = SA_SIGINFO;

#ifndef UNIT_TEST
    err = sigaction(SIGTERM, &ac, NULL);
    if (err) {
        err = errno;
        goto out;
    }
    err = sigaction(SIGHUP, &ac, NULL);
    if (err) {
        err = errno;
        goto out;
    }
    /* FIXME: mask the SIGINT for testing */
#if 0
    err = sigaction(SIGINT, &ac, NULL);
    if (err) {
        err = errno;
        goto out;
    }
#endif
    err = sigaction(SIGSEGV, &ac, NULL);
    if (err) {
        err = errno;
        goto out;
    }
    err = sigaction(SIGBUS, &ac, NULL);
    if (err) {
        err = errno;
        goto out;
    }
    err = sigaction(SIGQUIT, &ac, NULL);
    if (err) {
        err = errno;
        goto out;
    }
    err = sigaction(SIGUSR1, &ac, NULL);
    if (err) {
        err = errno;
        goto out;
    }
#endif

out:
    return err;
}

static void osd_itimer_default(int signo, siginfo_t *info, void *arg)
{
    u64 cur = time(NULL);
    
    sem_post(&hoo.timer_sem);
    /* Note that, we must check the profiling interval at here, otherwise
     * checking the profiling interval at timer_thread will lost some
     * statistics */
    osd_dump_profiling(cur, &hoo.hp);
    hoo.tick = cur;
    hvfs_verbose(osd, "Did this signal handler called?\n");

    return;
}

static int __gcd(int m, int n)
{
    int r, temp;

    if (!m && !n)
        return 0;
    else if (!m)
        return n;
    else if (!n)
        return m;

    if (m < n) {
        temp = m;
        m = n;
        n = temp;
    }
    r = m;
    while (r) {
        r = m % n;
        m = n;
        n = r;
    }

    return m;
}

static void osd_hb_wrapper(time_t t)
{
    static time_t prev = 0;

    if (!hoo.cb_hb)
        return;

    if (t < prev + hoo.conf.hb_interval)
        return;
    prev = t;
    hoo.cb_hb(&hoo);
}

static void *osd_timer_thread_main(void *arg)
{
    sigset_t set;
    time_t cur;
    int v, err;

    hvfs_debug(osd, "I am running...\n");

    /* first, let us block the SIGALRM */
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &set, NULL); /* oh, we do not care about the
                                             * errs */
    /* then, we loop for the timer events */
    while (!hoo.timer_thread_stop) {
        err = sem_wait(&hoo.timer_sem);
        if (err) {
            if (errno == EINTR)
                continue;
            hvfs_err(osd, "sem_wait() failed w/ %s\n", strerror(errno));
        }
        sem_getvalue(&hoo.timer_sem, &v);
        hvfs_debug(osd, "OK, we receive a SIGALRM event(remain %d).\n", v);
        cur = time(NULL);
        /* should we work now */
        osd_dump_profiling(cur, &hoo.hp);
        /* check the pending IOs */
        //osd_storage_pending_io();
        /* do heart beat */
        osd_hb_wrapper(cur);
        /* trigger a event to reporter thread */
        osd_reporter_schedule();
    }

    hvfs_debug(osd, "Hooo, I am exiting...\n");
    pthread_exit(0);
}

static int osd_setup_timers(void)
{
    struct sigaction ac;
    struct itimerval value, ovalue, pvalue;
    int which = ITIMER_REAL, interval;
    int err;

    /* init the timer semaphore */
    sem_init(&hoo.timer_sem, 0, 0);

    /* ok, we create the timer thread now */
    err = pthread_create(&hoo.timer_thread, NULL, &osd_timer_thread_main,
                         NULL);
    if (err)
        goto out;
    /* then, we setup the itimers */
    memset(&ac, 0, sizeof(ac));
    sigemptyset(&ac.sa_mask);
    ac.sa_flags = 0;
    ac.sa_sigaction = osd_itimer_default;
    err = sigaction(SIGALRM, &ac, NULL);
    if (err) {
        err = errno;
        goto out;
    }
    err = getitimer(which, &pvalue);
    if (err) {
        err = errno;
        goto out;
    }
    interval = __gcd(hoo.conf.profiling_thread_interval,
                     hoo.conf.hb_interval);
    if (interval) {
        value.it_interval.tv_sec = interval;
        value.it_interval.tv_usec = 0;
        value.it_value.tv_sec = interval;
        value.it_value.tv_usec = 0;
        err = setitimer(which, &value, &ovalue);
        if (err) {
            err = errno;
            goto out;
        }
        hvfs_debug(osd, "OK, we have created a timer thread to "
                   " profile events every %d second(s).\n", interval);
    } else {
        hvfs_debug(osd, "Hoo, there is no need to setup itimers based on the"
                   " configration.\n");
        hoo.timer_thread_stop = 1;
    }
    
out:
    return err;
}

void osd_reset_itimer(void)
{
    struct itimerval value, ovalue, pvalue;
    int err, interval;

    err = getitimer(ITIMER_REAL, &pvalue);
    if (err) {
        goto out;
    }
    interval = __gcd(hoo.conf.profiling_thread_interval,
                     hoo.conf.hb_interval);
    if (interval) {
        value.it_interval.tv_sec = interval;
        value.it_interval.tv_usec = 0;
        value.it_value.tv_sec = interval;
        value.it_value.tv_usec = 0;
        err = setitimer(ITIMER_REAL, &value, &ovalue);
        if (err) {
            goto out;
        }
        hvfs_info(osd, "OK, we reset the itimer to %d second(s).\n",
                  interval);
    }
out:
    return;
}

/* reporter_schedule() trigger an object update
 */
void osd_reporter_schedule()
{
    sem_post(&hoo.reporter_sem);
}

void __do_obj_diff_report()
{
    struct xnet_msg *msg;
    struct om_changed_entry *oce = NULL, *n;
    struct obj_report_tx *ort = NULL;
    struct objid *ap, *rp;
    struct list_head *plist;
    int addsize = 0, rmvsize = 0, two_pass = 0;
    LIST_HEAD(tmp);

second_pass:
    msg = xnet_alloc_msg(XNET_MSG_NORMAL);
    if (!msg) {
        hvfs_err(osd, "xnet_alloc_msg() msg failed, retry later.\n");
        return;
    }

    xlock_lock(&hoo.om.changed_lock);
    if (hoo.om.should_send == OM_REPORT_PAUSE) {
        hvfs_info(osd, "Wait for FULL disk store scanning...\n");
        xlock_unlock(&hoo.om.changed_lock);
        return;
    }
    if (!list_empty(&hoo.om.changed_saving)) {
        two_pass = 1;
        plist = &hoo.om.changed_saving;
    } else
        plist = &hoo.om.changed;
    xlock_unlock(&hoo.om.changed_lock);

    do {
        /* iterate the changed list to get valid updates */
        xlock_lock(&hoo.om.changed_lock);
        if (!list_empty(plist)) {
            oce = list_first_entry(plist, struct om_changed_entry, list);
            list_del_init(&oce->list);
            list_add_tail(&oce->list, &tmp);
        } else {
            xlock_unlock(&hoo.om.changed_lock);
            break;
        }
        xlock_unlock(&hoo.om.changed_lock);
        
        switch (oce->upd_or_del) {
        default:
        case OM_UPDATE:
            addsize++;
            break;
        case OM_DELETE:
            rmvsize++;
            break;
        }
    } while (1);

    /* alloc the report array, and deal with the tmp list */
    ort = xzalloc(sizeof(*ort) + sizeof(struct objid) * (addsize + rmvsize));
    if (!ort) {
        hvfs_err(osd, "xzalloc() ORT region failed\n");
        goto recover;
    }
    ort->add_size = addsize;
    ort->rmv_size = rmvsize;
    ap = ort->ids;
    rp = ort->ids + addsize;
    addsize = 0, rmvsize = 0;
    
    list_for_each_entry_safe(oce, n, &tmp, list) {
        switch (oce->upd_or_del) {
        default:
        case OM_UPDATE:
            ap[addsize++] = oce->id;
            break;
        case OM_DELETE:
            rp[rmvsize++] = oce->id;
            break;
        }
        list_del(&oce->list);
        xfree(oce);
    }

    /* do report send here! */
    if (addsize > 0 || rmvsize > 0) {
        hvfs_info(osd, "Prepare object report(diff): ADD %d, DEL %d objs in total.\n",
                  addsize, rmvsize);

#ifdef XNET_EAGER_WRITEV
        xnet_msg_add_sdata(msg, &msg->tx, sizeof(msg->tx));
#endif
        xnet_msg_fill_tx(msg, XNET_MSG_REQ, 0, hoo.site_id,
                         osd_select_ring(&hoo));
        xnet_msg_fill_cmd(msg, HVFS_R2_OREP, 0, 0);
        xnet_msg_add_sdata(msg, ort, sizeof(*ort) + 
                           sizeof(struct objid) * (ort->add_size + ort->rmv_size));

        if (xnet_send(hoo.xc, msg)) {
            hvfs_err(osd, "xnet_send() object report failed.\n");
        }
        xnet_free_msg(msg);
    }

    if (two_pass)
        goto second_pass;
    
    return;
recover:
    list_for_each_entry_safe(oce, n, &tmp, list) {
        list_del_init(&oce->list);
        xlock_lock(&hoo.om.changed_lock);
        list_add(&oce->list, &hoo.om.changed);
        xlock_unlock(&hoo.om.changed_lock);
    }
    sched_yield();
    osd_reporter_schedule();
}

static void *osd_reporter_thread_main(void *arg)
{
    static time_t last_diff = 0, last_scan = 0;
    sigset_t set;
    time_t cur;
    int err;

    hvfs_debug(osd, "I am running...\n");

    /* first, let us block he SIGALRM */
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    /* then we loop for the report events */
    while (!hoo.reporter_thread_stop) {
        err = sem_wait(&hoo.reporter_sem);
        if (err == EINTR)
            continue;
        cur = time(NULL);
        if (!last_diff)
            last_diff = cur;
        if (!last_scan)
            last_scan = cur;
        
        /* FIXME: do report here */
        if (cur - last_diff >= hoo.conf.diff_rep_interval) {
            hvfs_debug(osd, "do diff report now ...\n");
            __do_obj_diff_report();
            last_diff = cur;
        }
        if (cur - last_scan >= hoo.conf.full_rep_interval) {
            hvfs_info(osd, "do full report now ...\n");
            osd_do_report();
            last_scan = cur;
        }
    }
    hvfs_debug(osd, "Hooo, I am exiting...\n");
    pthread_exit(0);
}

static int osd_setup_reporter(void)
{
    int err;

    /* first, we init the reporter_sem */
    sem_init(&hoo.reporter_sem, 0, 0);

    /* then, we create the reporter thread now */
    err = pthread_create(&hoo.reporter_thread, NULL, 
                         &osd_reporter_thread_main, NULL);
    if (err)
        goto out;

out:
    return err;
}

/* osd_pre_init()
 */
void osd_pre_init(void)
{
    /* prepare the hoi & hoo */
    memset(&hoi, 0, sizeof(hoi));
    memset(&hoo, 0, sizeof(hoo));
#ifdef HVFS_DEBUG_LOCK
    lock_table_init();
#endif
    xlock_init(&hoo.om.lock);
    xlock_init(&hoo.om.changed_lock);

    /* setup the state */
    hoo.state = HOO_STATE_INIT;
}

/* osd_verify() hoo.site_id is ready now
 */
int osd_verify(void)
{
    char path[256] = {0, };
    int err = 0, i;

    /* check the OSD_HOME */
    err = osd_storage_dir_make_exist(hoo.conf.osd_home);
    if (err) {
        hvfs_err(osd, "dir %s do not exist.\n", hoo.conf.osd_home);
        goto out;
    }

    /* check the OSD site directory */
    sprintf(path, "%s/%lx", hoo.conf.osd_home, hoo.site_id);
    err = osd_storage_dir_make_exist(path);
    if (err) {
        hvfs_err(osd, "dir %s do not exist.\n", path);
        goto out;
    }

    /* check the OSD store array */
    for (i = 0; i < hoo.conf.osd_storages_array_size; i++) {
        err = osd_storage_dir_make_exist(hoo.conf.osd_storages_array[i]);
        if (err) {
            hvfs_err(osd, "storage dir %s do not exist.\n",
                     hoo.conf.osd_storages_array[i]);
            goto out;
        }
        sprintf(path, "%s/%lx", hoo.conf.osd_storages_array[i], hoo.site_id);
        err = osd_storage_dir_make_exist(path);
        if (err) {
            hvfs_err(osd, "storage dir %s do not exist.\n", path);
            goto out;
        }
    }

    /* check if we need a recovery */
    if (hoo.aux_state) {
        //err = osd_do_recovery();
        if (err) {
            hvfs_err(osd, "OSD do recovery failed w/ %d\n",
                     err);
        }
        hoo.aux_state = 0;
    }

    /* do fully disk scan and generate a obj report */
    err = osd_do_report();
    if (err) {
        hvfs_err(osd, "OSD do obj report failed w/ %d\n", err);
        goto out;
    }

    /* setup running state */
    hoo.state = HOO_STATE_RUNNING;

    /* write down a LOG pair to indicate a new instance */
    osd_startup_normal();

out:
    return err;
}

static inline
void __parse_storage_string(char *orig, char ***out, u8 *size)
{
    char *n, *s = NULL, *p, *r;
    char **q;

    n = strdup(orig);
    if (!n) {
        hvfs_err(osd, "duplicate the original storage string '%s' failed\n",
                 orig);
        return;
    }
    *size = 0;
    *out = NULL;
    do {
        p = strtok_r(n, ",;\n", &s);
        if (!p) {
            /* end */
            break;
        }
        r = strdup(p);
        if (!r) {
            hvfs_err(osd, "duplicate store '%s' failed\n", p);
            continue;
        }
        /* alloc a new pointer */
        q = xrealloc(*out, (*size + 1) * sizeof(char *));
        if (!q) {
            hvfs_err(osd, "failed to alloc new pointer, ignore this "
                     "store '%s'\n", p);
            xfree(r);
            continue;
        }
        q[*size] = r;
        (*size) += 1;
        *out = q;
    } while (!(n = NULL));
}

static inline
char *__get_default_store(char *home)
{
    char path[256];

    sprintf(path, "%s/store", home);
    return strdup(path);
}

/* osd_config()
 *
 * Get configuration from the execution environment
 */
int osd_config(void)
{
    char *value;

    if (hoo.state != HOO_STATE_INIT) {
        hvfs_err(osd, "OSD state is not in launching, please call "
                 "osd_pre_init() firstly\n");
        return -EINVAL;
    }

    HVFS_OSD_GET_ENV_strncpy(dcaddr, value, OSD_DCONF_MAX_NAME_LEN);
    HVFS_OSD_GET_ENV_cpy(osd_home, value);
    HVFS_OSD_GET_ENV_cpy(profiling_file, value);
    HVFS_OSD_GET_ENV_cpy(conf_file, value);
    HVFS_OSD_GET_ENV_cpy(log_file, value);
    HVFS_OSD_GET_ENV_cpy(storages, value);

    HVFS_OSD_GET_ENV_atoi(spool_threads, value);
    HVFS_OSD_GET_ENV_atoi(aio_threads, value);
    HVFS_OSD_GET_ENV_atoi(prof_plot, value);
    HVFS_OSD_GET_ENV_atoi(profiling_thread_interval, value);
    HVFS_OSD_GET_ENV_atoi(stacksize, value);
    HVFS_OSD_GET_ENV_atoi(diff_rep_interval, value);
    HVFS_OSD_GET_ENV_atoi(full_rep_interval, value);

    HVFS_OSD_GET_ENV_option(write_drop, WDROP, value);

    /* set default osd home */
    if (!hoo.conf.osd_home) {
        hoo.conf.osd_home = HVFS_OSD_HOME;
    }
    /* set osd storage array */
    if (!hoo.conf.storages) {
        hoo.conf.storages = __get_default_store(hoo.conf.osd_home);
    }
    __parse_storage_string(hoo.conf.storages, 
                           &hoo.conf.osd_storages_array, 
                           &hoo.conf.osd_storages_array_size);
    {
        int i;

        for (i = 0; i < hoo.conf.osd_storages_array_size; i++) {
            hvfs_info(osd, "use STORE[%d] = '%s'\n", i, 
                      hoo.conf.osd_storages_array[i]);
        }
    }

    if (!hoo.conf.spool_threads)
        hoo.conf.spool_threads = 8; /* double # of CPUs */

    if (!hoo.conf.profiling_thread_interval)
        hoo.conf.profiling_thread_interval = 5;
    if (!hoo.conf.hb_interval)
        hoo.conf.hb_interval = 60;
    if (!hoo.conf.diff_rep_interval)
        hoo.conf.diff_rep_interval = 5;
    if (!hoo.conf.full_rep_interval)
        hoo.conf.full_rep_interval = 30;

    return 0;
}

/* osd_help()
 */
void osd_help(void)
{
    hvfs_plain(osd, "OSD build @ %s on %s\n", CDATE, CHOST);
    hvfs_plain(osd, "Usage: [EV list] osd\n\n");
    hvfs_plain(osd, "General Environment Variables:\n"
               " hvfs_osd_dcaddr               dynamic config addr for "
               "UNIX sockets.\n"
               " hvfs_osd_profiling_file       profiling file name.\n"
               " hvfs_osd_conf_file            config file name.\n"
               " hvfs_osd_log_file             log file name.\n"
               " hvfs_osd_spool_threads        spool threads nr.\n"
               " hvfs_osd_prof_plot            output for gnuplot.\n"
               " hvfs_osd_profiling_thread_interval\n"
               "                                wakeup interval for prof thread.\n"
               " hvfs_osd_opt_write_drop       drop the writes to this OSD.\n"
        );
    hvfs_plain(osd, "Any questions please contacts Ma Can <macan@iie.ac.cn>\n");
}

/* osd_init()
 */
int osd_init(void)
{
    int err;

    /* lib init */
    lib_init();

    /* FIXME: decode the cmdline */

    /* Init the signal handlers */
    err = osd_init_signal();
    if (err)
        goto out_signal;

    /* FIXME: setup the timers */
    err = osd_setup_timers();
    if (err)
        goto out_timers;

    /* FIXME: setup the reporter */
    err = osd_setup_reporter();
    if (err)
        goto out_reporter;
    
    /* init storage */
    err = osd_storage_init();
    if (err)
        goto out_storage;

    /* FIXME: init the service threads' pool */
    err = osd_spool_create();
    if (err)
        goto out_spool;

    /* init the aio threads */
    //err = osd_aio_create();
    if (err)
        goto out_aio;
    
    /* mask the SIGUSR1 signal for main thread */
    {
        sigset_t set;

        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        pthread_sigmask(SIG_BLOCK, &set, NULL);
    }

    /* ok to run */
    hoo.state = HOO_STATE_LAUNCH;

out_aio:
out_spool:
out_storage:
out_reporter:
out_timers:
out_signal:
    return err;
}

void osd_destroy(void)
{
    hvfs_verbose(osd, "OK, stop it now...\n");

    /* unreg w/ the r2 server */
    if (hoo.cb_exit) {
        hoo.cb_exit(&hoo);
    }

    /* stop the timer thread */
    hoo.timer_thread_stop = 1;
    if (hoo.timer_thread)
        pthread_join(hoo.timer_thread, NULL);

    sem_destroy(&hoo.timer_sem);

    /* destroy the service threads' pool */
    osd_spool_destroy();

    /* destroy the storage */
    osd_storage_destroy();

    /* stop the reporter thread */
    hoo.reporter_thread_stop = 1;
    osd_reporter_schedule();
    if (hoo.reporter_thread)
        pthread_join(hoo.reporter_thread, NULL);

    sem_destroy(&hoo.reporter_sem);

    /* you should wait for the storage destroied and exit the AIO threads */
    //osd_aio_destroy();

    /* finally, we write our finish flag to objlog file */
    osd_exit_normal();
}

u64 osd_select_ring(struct hvfs_osd_object *hoo)
{
    if (hoo->ring_site)
        return hoo->ring_site;
    else
        return HVFS_RING(0);
}

void osd_set_ring(u64 site_id)
{
    hoo.ring_site = site_id;
}

int osd_addr_table_update(struct xnet_msg *msg)
{
    if (msg->xm_datacheck) {
        if (hoo.cb_addr_table_update)
            hoo.cb_addr_table_update(msg->xm_data);
    } else {
        hvfs_err(osd, "Invalid addr table update message, incomplete hst!\n");
        return -EINVAL;
    }

    xnet_free_msg(msg);

    return 0;
}

static inline
void __free_oga(struct obj_gather_all *oga)
{
    if (oga->add.psize > 0)
        xfree(oga->add.ids);
    if (oga->rmv.psize > 0)
        xfree(oga->rmv.ids);
    xfree(oga);
}

/* Return value: 0 => not update; 1 => updated
 */
static int osd_update_objinfo(struct obj_gather_all *oga)
{
    int updated = 0;
    
retry:
    if (hoo.om.oga == NULL) {
        xlock_lock(&hoo.om.lock);
        if (!hoo.om.oga) {
            hoo.om.oga = oga;
            updated = 1;
        } else {
            xlock_unlock(&hoo.om.lock);
            /* somebody update the objinfo before us, just retry */
            goto retry;
        }
        xlock_unlock(&hoo.om.lock);
        return updated;
    } else {
        /* should we trigger a fully update now */
        MD5_CTX mdContext1, mdContext2;
        int i = 0, update = 0;

        xlock_lock(&hoo.om.lock);
        MD5Init(&mdContext1);
        if (hoo.om.oga->add.asize > 0)
            MD5Update(&mdContext1, hoo.om.oga->add.ids, 
                      hoo.om.oga->add.asize * sizeof(struct objid));
        if (hoo.om.oga->rmv.asize > 0)
            MD5Update(&mdContext1, hoo.om.oga->rmv.ids,
                      hoo.om.oga->rmv.asize * sizeof(struct objid));
        MD5Final(&mdContext1);
        xlock_unlock(&hoo.om.lock);

        MD5Init(&mdContext2);
        if (oga->add.asize > 0)
            MD5Update(&mdContext2, oga->add.ids, 
                      oga->add.asize * sizeof(struct objid));
        if (oga->rmv.asize > 0)
            MD5Update(&mdContext2, oga->rmv.ids,
                      oga->rmv.asize * sizeof(struct objid));
        MD5Final(&mdContext2);

        for (i = 0; i < 16; i++) {
            if (mdContext1.digest[i] != mdContext2.digest[i]) {
                update = 1;
                break;
            }
        }

        if (update) {
            hvfs_info(osd, "Get a new obj info array, do fully update ...\n");
            xlock_lock(&hoo.om.lock);
            /* free the old OGA */
            __free_oga(hoo.om.oga);
            hoo.om.oga = oga;
            updated = 1;
            xlock_unlock(&hoo.om.lock);
        } else {
            hvfs_info(osd, "Get a dup obj info array, no update ...\n");
        }
    }

    return updated;
}

int osd_om_update_cache(struct obj_gather_all *oga)
{
    int err = 0;
    int i;

    /* setup a new cache */
    for (i = 0; i < oga->add.asize; i++) {
        __om_update_entry(oga->add.ids[i].id, oga->add.ids[i].dev);
        hvfs_info(osd, "Cache OBJID %lx.%x DEV %d\n", oga->add.ids[i].id.uuid,
                  oga->add.ids[i].id.bid, oga->add.ids[i].dev);
    }

    return err;
}

/* Input Args: *ort must be NULL
 */
static int osd_oga2ort(struct obj_gather_all *oga, struct obj_report_tx **ort)
{
    struct obj_report_tx *tmp;
    int addsize = 0, rmvsize = 0;
    int i, j;

    if (*ort != NULL) {
        return -EINVAL;
    }

    for (i = 0; i < oga->add.asize; i++) {
        if (oga->add.ids[i].id.error)
            continue;
        addsize++;
    }
    for (i = 0; i < oga->rmv.asize; i++) {
        if (oga->rmv.ids[i].id.error)
            continue;
        rmvsize++;
    }

    tmp = xzalloc(sizeof(*tmp) + sizeof(struct objid) * (addsize + rmvsize));
    if (!tmp) {
        hvfs_err(osd, "xzalloc() ORT region failed\n");
        return -ENOMEM;
    }
    tmp->add_size = addsize;
    tmp->rmv_size = rmvsize;
    for (i = j = 0; j < oga->add.asize; j++) {
        if (oga->add.ids[j].id.error)
            continue;
        tmp->ids[i++] = oga->add.ids[j].id;
    }
    for (j = 0; j < oga->rmv.asize; j++) {
        if (oga->rmv.ids[j].id.error)
            continue;
        tmp->ids[i++] = oga->rmv.ids[j].id;
    }
    *ort = tmp;
    hvfs_info(osd, "Prepare object report(full): ADD %d, DEL %d objs in total.\n",
              addsize, rmvsize);

    return 0;
}

/* osd_do_report() report all the active objects to R2 server
 */
int osd_do_report()
{
    struct obj_gather_all *oga;
    struct obj_report_tx *ort = NULL;
    struct xnet_msg *msg;
    int err = 0, updated;

    oga = osd_storage_process_report();
    if (IS_ERR(oga)) {
        err = PTR_ERR(oga);
        hvfs_err(osd, "osd_storage_process_report() failed w/ %d\n",
                 err);
        goto out;
    }

#if 0
    {
        int i;
        for (i = 0; i < oga->add.asize; i++) {
            hvfs_info(osd, "OBJID %lx.%x DEV %d\n", oga->add.ids[i].id.uuid,
                      oga->add.ids[i].id.bid, oga->add.ids[i].dev);
        }
    }
#endif


    /* handle the oga, free it if needed */
    updated = osd_update_objinfo(oga);
    if (updated) {
        osd_om_update_cache(oga);
    }

    /* generated the object report */
    err = osd_oga2ort(oga, &ort);
    if (err) {
        hvfs_err(osd, "transform OGA to ORT failed w/ %d\n", err);
        goto out;
    }

#if 0
    {
        int i = 0;
        
        for (; i < ort->add_size + ort->rmv_size; i++) {
            hvfs_info(osd, "ORTOBJ %lx.%x\n",
                      ort->ids[i].uuid, ort->ids[i].bid);
        }
    }
#endif

    /* change this object report to fully update report */
    ort->add_size = -ort->add_size;
        
    /* send the object report */
    {
        msg = xnet_alloc_msg(XNET_MSG_NORMAL);
        if (!msg) {
            hvfs_err(osd, "xnet_alloc_msg() msg failed.\n");
            err = -ENOMEM;
            goto out_free;
        }
#ifdef XNET_EAGER_WRITEV
        xnet_msg_add_sdata(msg, &msg->tx, sizeof(msg->tx));
#endif
        xnet_msg_fill_tx(msg, XNET_MSG_REQ, 0, hoo.site_id,
                         osd_select_ring(&hoo));
        xnet_msg_fill_cmd(msg, HVFS_R2_OREP, 0, 0);
        xnet_msg_add_sdata(msg, ort, sizeof(*ort) + 
                           sizeof(struct objid) * (-ort->add_size + ort->rmv_size));

        if (xnet_send(hoo.xc, msg)) {
            hvfs_err(osd, "xnet_send() object report failed.\n");
        }
        xnet_free_msg(msg);
    }

    /* end scan, enable report now */
    if (osd_om_state_change(OM_REPORT_END_SCAN, OM_REPORT_ACTIVE)) {
        hvfs_info(osd, "OSD store END fully scan (ACTIVE) ...\n");
    }
    /* change into diff mode */
    if (osd_om_state_change(OM_REPORT_DIFF, OM_REPORT_ACTIVE)) {
        hvfs_info(osd, "OSD store into DIFF mode (ACTIVE) ...\n");
    }

out_free:
    if (ort) {
        xfree(ort);
    }
    if (!updated) {
        __free_oga(oga);
    }
out:
    return err;
}
