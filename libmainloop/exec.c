/*
 * ion/mainloop/exec.c
 *
 * Copyright (c) Tuomo Valkonen 1999-2005. 
 *
 * Ion is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 */

#include <limits.h>
#include <sys/types.h>
#include <sys/signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <libtu/output.h>
#include <libtu/misc.h>
#include <libtu/locale.h>
#include <libtu/types.h>

#include "select.h"
#include "exec.h"


/*{{{ Exec/spawn/fork */

#define SHELL_PATH "/bin/sh"
#define SHELL_NAME "sh"
#define SHELL_ARG "-c"


void mainloop_do_exec(const char *cmd)
{
    char *argv[4];

    argv[0]=SHELL_NAME;
    argv[1]=SHELL_ARG;
    argv[2]=(char*)cmd; /* stupid execve... */
    argv[3]=NULL;
    execvp(SHELL_PATH, argv);
}


static int mypipe(int *fds)
{
    int r=pipe(fds);
    if(r==0){
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    }else{
        warn_err_obj("pipe()");
    }
    return r;
}


static bool unblock(int fd)
{
    int fl=fcntl(fd, F_GETFL);
    if(fl!=-1)
        fl=fcntl(fd, F_SETFL, fl|O_NONBLOCK);
    return (fd!=-1);
}


static void duppipe(int fd, int idx, int *fds)
{
    close(fd);
    dup(fds[idx]);
    close(fds[0]);
    close(fds[1]);
}


pid_t mainloop_fork(void (*fn)(void *p), void *fnp,
                    int *infd, int *outfd, int *errfd)
{
    int pid;
    int infds[2];
    int outfds[2];
    int errfds[2];
    
    if(infd!=NULL){
        if(mypipe(infds)!=0)
            return -1;
    }

    if(outfd!=NULL){
        if(mypipe(outfds)!=0)
            goto err1;
    }

    if(errfd!=NULL){
        if(mypipe(errfds)!=0)
            goto err2;
    }
    

    pid=fork();
    
    if(pid<0)
        goto err3;
    
    if(pid!=0){
        if(outfd!=NULL){
            if(!unblock(outfds[0]))
                goto err3;
            *outfd=outfds[0];
            close(outfds[1]);
        }

        if(errfd!=NULL){
            if(!unblock(errfds[0]))
                goto err3;
            *errfd=errfds[0];
            close(errfds[1]);
        }
        
        if(infd!=NULL){
            *infd=infds[1];
            close(infds[0]);
        }
        
        return pid;
    }

    if(infd!=NULL)
        duppipe(0, 0, infds);
    if(outfd!=NULL)
        duppipe(1, 1, outfds);
    if(errfd!=NULL)
        duppipe(2, 1, errfds);
    
    fn(fnp);

    abort();

err3:
    warn_err();
    if(errfd!=NULL){
        close(errfds[0]);
        close(errfds[1]);
    }
err2:
    if(outfd!=NULL){
        close(outfds[0]);
        close(outfds[1]);
    }
err1:    
    if(infd!=NULL){
        close(infds[0]);
        close(infds[1]);
    }
    return -1;
}


typedef struct{
    const char *cmd;
    void (*initenv)(void *p);
    void *initenvp;
} SpawnP;


static void do_spawn(void *spawnp)
{
    SpawnP *p=(SpawnP*)spawnp;
    
    if(p->initenv)
        p->initenv(p->initenvp);
    mainloop_do_exec(p->cmd);
}


pid_t mainloop_do_spawn(const char *cmd, 
                        void (*initenv)(void *p), void *p,
                        int *infd, int *outfd, int *errfd)
{
    SpawnP spawnp;
    
    spawnp.cmd=cmd;
    spawnp.initenv=initenv;
    spawnp.initenvp=p;
    
    return mainloop_fork(do_spawn, (void*)&spawnp, infd, outfd, errfd);
}


pid_t mainloop_spawn(const char *cmd)
{
    return mainloop_do_spawn(cmd, NULL, NULL, NULL, NULL, NULL);
}


/*}}}*/


/*{{{ popen_bgread */


#define BL 1024

static void process_pipe(int fd, void *p)
{
    char buf[BL];
    int n;
    
    while(1){
        n=read(fd, buf, BL-1);
        if(n<0){
            if(errno==EAGAIN || errno==EINTR)
                return;
            n=0;
            warn_err_obj(TR("reading a pipe"));
        }
        if(n>0){
            buf[n]='\0';
            if(!extl_call(*(ExtlFn*)p, "s", NULL, &buf))
                break;
        }
        if(n==0){
            /* Call with no argument/NULL string to signify EOF */
            extl_call(*(ExtlFn*)p, NULL, NULL);
            break;
        }
        if(n<BL-1)
            return;
    }
    
    /* We get here on EOL or if the handler failed */
    mainloop_unregister_input_fd(fd);
    close(fd);
    extl_unref_fn(*(ExtlFn*)p);
    free(p);
}


static bool setup_bgread(ExtlFn handler, int fd)
{
    ExtlFn *p=ALLOC(ExtlFn);
    if(p!=NULL){
        *(ExtlFn*)p=extl_ref_fn(handler);
        if(mainloop_register_input_fd(fd, p, process_pipe))
            return TRUE;
        extl_unref_fn(*(ExtlFn*)p);
        free(p);
    }
    return FALSE;
}


pid_t mainloop_popen_bgread(const char *cmd, ExtlFn handler,
                            void (*initenv)(void *p), void *p)
{
    pid_t pid;
    int fd;
    
    pid=mainloop_do_spawn(cmd, initenv, p, NULL, &fd, NULL);
    
    if(pid>0){
        if(!setup_bgread(handler, fd)){
            close(fd);
            return -1;
        }
    }
    
    return pid;
}


pid_t mainloop_spawn_merr(const char *cmd, ExtlFn handler,
                          void (*initenv)(void *p), void *p)
{
    pid_t pid;
    int fd;
    
    pid=mainloop_do_spawn(cmd, initenv, p, NULL, NULL, &fd);
    
    if(pid>0){
        if(!setup_bgread(handler, fd)){
            close(fd);
            return -1;
        }
    }
    
    return pid;
}


/*}}}*/