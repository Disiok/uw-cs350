/*
 * Copyright (c) 2013
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "opt-A2.h"
#if OPT_A2
#define EXITCODE_NULL -1
#define PID_NULL -1
#define PID_KERN 0
#include <types.h>
#include <limits.h>
#include <current.h>
#include <proc.h>
#include <pid.h>
#include <kern/errno.h>

static struct pid_stat *pid_table[PID_MAX + 1];
static pid_t pid_next_available = PID_MIN - 1;
static pid_t pid_max_assigned = PID_KERN;
static struct lock *pid_lock;

void
pid_bootstrap(void)
{
    // initialize lock
    pid_lock = lock_create("pid_lock");

    // initialize pid table
    for (pid_t pid = PID_MIN; pid <= PID_MAX; ++pid) {
        pid_table[pid] = NULL;
    }

    // initialize next available pid
    pid_next_available = PID_MIN; 
}

void
pid_assign_kern(struct proc *proc_kern)
{
    proc_kern->p_pid = PID_KERN;
}

void
pid_assign_next(struct proc *proc_child)
{
    // allocate pid stat struct for current proc
    struct pid_stat *pid_stat_current = kmalloc(sizeof(struct pid_stat));
    if (pid_stat_current == NULL) {
        panic("Out of memory while attempting to allocate pid stat"); 
    }

    // assign parent pid
    pid_stat_current->p_parent_pid = curproc->p_pid;

    // assign cv
    pid_stat_current->p_cv = cv_create(proc_child->p_name);

    // assign exit code
    pid_stat_current->p_exitcode = EXITCODE_NULL;

    lock_acquire(pid_lock);
    // assign next available pid
    proc_child->p_pid = pid_next_available;

    // assign pid stat struct to pid table
    pid_table[pid_next_available] = pid_stat_current;
    if (pid_next_available > pid_max_assigned) {
        pid_max_assigned = pid_next_available;
    }

    // probe for next available pid
    while (pid_table[pid_next_available] != NULL) {
        pid_next_available ++;
    }
    lock_release(pid_lock);

    // panic if we ran out of pid
    if (pid_next_available > PID_MAX) {
        panic("Out of available process id");
    }
}

int
pid_wait(pid_t pid, int *exitstatus)
{
    pid_t pid_parent = curproc->p_pid;

    lock_acquire(pid_lock);
    if (pid_table[pid] == NULL) {
        lock_release(pid_lock);
        return(ESRCH);
    } else {
        if (pid_table[pid]->p_parent_pid == pid_parent) {
            while (pid_table[pid]->p_exitcode == EXITCODE_NULL) {
                cv_wait(pid_table[pid]->p_cv, pid_lock);            
            }
            *exitstatus = pid_table[pid]->p_exitcode;
            lock_release(pid_lock);
            return (0);
        } else {
            lock_release(pid_lock);
            return(ECHILD);
        } 
    }
}

static
void
pid_destroy(pid_t pid) 
{
    // make sure lock is held
    KASSERT(lock_do_i_hold(pid_lock));
    // make sure pid stat exists
    KASSERT(pid_table[pid] != NULL);
    // make sure process has exited
    KASSERT(pid_table[pid]->p_exitcode != EXITCODE_NULL);
    // make sure process has no alive parent process
    KASSERT(pid_table[pid]->p_parent_pid == PID_NULL);
    
    // remove current pid stat
    kfree(pid_table[pid]);
    pid_table[pid] = NULL;

    if (pid < pid_next_available) {
        pid_next_available = pid;
    }
}

static
void
pid_cleanup(pid_t pid_parent) 
{
    // make sure lock is held
    KASSERT(lock_do_i_hold(pid_lock));
    // make sure parent process has exited
    KASSERT(pid_table[pid_parent] == NULL 
            || pid_table[pid_parent]->p_exitcode != EXITCODE_NULL);

    // unlink children processes and remove interest
    for (pid_t pid_child = PID_MIN; pid_child <= pid_max_assigned; ++pid_child) {
        if (pid_table[pid_child] != NULL && 
                pid_table[pid_child]->p_parent_pid == pid_parent) {
            pid_table[pid_child]->p_parent_pid = PID_NULL;
            if (pid_table[pid_child]->p_exitcode != EXITCODE_NULL) {
                // if child has already exited 
                pid_destroy(pid_child);
            }
        }
    }
}

void
pid_exit(int exitcode)
{
    lock_acquire(pid_lock);
    // retrieve process id
    pid_t pid = curproc->p_pid;

    // assign exit code
    pid_table[pid]->p_exitcode = exitcode;

    if (pid_table[pid]->p_parent_pid == PID_NULL) {
        // if there is no parent process that is interested in this process' exit code
        // destroy pid stat
        pid_destroy(pid);
    } else {
        // if there is a parent process that is interested in this process' exit code
        // signal parent processe waiting on the wait channel
        cv_signal(pid_table[pid]->p_cv, pid_lock);
    }

    // clean up parent children linkages
    pid_cleanup(pid);
    lock_release(pid_lock);
}

void
pid_fail(void)
{
    lock_acquire(pid_lock);

    // retrieve process id
    pid_t pid = curproc->p_pid;

    if (pid_table[pid] != NULL) {
        // remove current pid stat
        kfree(pid_table[pid]);
        pid_table[pid] = NULL;

        if (pid < pid_next_available) {
            pid_next_available = pid;
        }
    }

    lock_release(pid_lock);
}


#endif /* OPT_A2 */
