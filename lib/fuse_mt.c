/*
    FUSE: Filesystem in Userspace
    Copyright (C) 2001  Miklos Szeredi (mszeredi@inf.bme.hu)

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#include "fuse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <sys/time.h>

#define FUSE_NUM_WORKERS 5

struct fuse_worker {
    struct fuse *f;
    void *data;
    fuse_processor_t proc;
};

static void *do_work(void *data)
{
    struct fuse_worker *w = (struct fuse_worker *) data;
    
    while(1) {
        struct fuse_cmd *cmd = __fuse_read_cmd(w->f);
        if(cmd == NULL)
            exit(1);

        w->proc(w->f, cmd, w->data);

    }

    return NULL;
}

static void start_thread(struct fuse_worker *w)
{
    pthread_t thrid;
    sigset_t oldset;
    sigset_t newset;
    int res;
    
    /* Disallow signal reception in worker threads */
    sigfillset(&newset);
    pthread_sigmask(SIG_SETMASK, &newset, &oldset);
    res = pthread_create(&thrid, NULL, do_work, w);
    pthread_sigmask(SIG_SETMASK, &oldset, NULL);
    if(res != 0) {
        fprintf(stderr, "Error creating thread: %s\n", strerror(res));
        exit(1);
    }
    pthread_detach(thrid);
}

void __fuse_loop_mt(struct fuse *f, fuse_processor_t proc, void *data)
{
    struct fuse_worker *w;
    int i;

    w = malloc(sizeof(struct fuse_worker));    
    w->f = f;
    w->data = data;
    w->proc = proc;

    for(i = 1; i < FUSE_NUM_WORKERS; i++)
        start_thread(w);

    do_work(w);
}

void fuse_loop_mt(struct fuse *f)
{
    __fuse_loop_mt(f, (fuse_processor_t) __fuse_process_cmd, NULL);
}
