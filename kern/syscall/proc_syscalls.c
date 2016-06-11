#include "opt-A2.h"
#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include <mips/trapframe.h>
#include <pid.h>

  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;
#if OPT_A2
  // set exit code in pid table
  pid_exit(exitcode);
#else
  /* for now, just include this to keep the compiler from complaining about
     an unused variable */
  (void)exitcode;
#endif /* OPT_A2 */

  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  proc_destroy(p);
  
  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
#if OPT_A2
    *retval = curproc->p_pid;
#else
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
  *retval = 1;
#endif /* OPT_A2 */
  return(0);
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{
  int exitstatus;
  int result;

  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  // do not support any options for now
  if (options != 0) {
    return(EINVAL);
  }
#if OPT_A2
  result = pid_wait(pid, &exitstatus);
  if (result) {
     return(result); 
  }
  exitstatus = _MKWAIT_EXIT(exitstatus);
#else
  /* for now, just pretend the exitstatus is 0 */
  exitstatus = 0;
#endif /* OPT_A2 */
  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}

#if OPT_A2
int
sys_fork(struct trapframe *tf,
        pid_t *retval)
{
    int errno;
    struct proc *proc_child;
    struct addrspace *as_child;
    struct trapframe *tf_cp;

    // create process structure for child process
    proc_child = proc_create_runprogram(curproc->p_name);
    if (proc_child == NULL) {
        return(ENOMEM); 
    }

    // create and copy address space
    errno = as_copy(curproc_getas(), &as_child);
    if (errno) {
        pid_fail();
        proc_destroy(proc_child);
        return(errno);  
    }
    
    // allocate trapframe in heap
    tf_cp = kmalloc(sizeof(struct trapframe));
    if (tf_cp == NULL) {
        as_destroy(as_child); 
        pid_fail();
        proc_destroy(proc_child);
        return(ENOMEM);
    }

    // copy trapframe into heap
    *tf_cp = *tf;

    // create thread for child process 
    errno = thread_fork(curthread->t_name, 
            proc_child, 
            enter_forked_proces, 
            tf_cp, 
            (unsigned long) as_child);
    if (errno) {
        kfree(tf_cp);
        as_destroy(as_child); 
        pid_fail();
        proc_destroy(proc_child);
        return errno;
    }

    // set return value
    *retval = proc_child->p_pid;

    return(0);
}
#endif /* OPT_A2 */

