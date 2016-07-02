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
#include <vfs.h>
#include <kern/fcntl.h>

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

int sys_execv(const char *program, char **uargs) {
    struct addrspace *as_new;
    struct addrspace *as_old;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;
    size_t size;

    char *program_kernel; 
    size_t program_size;
    char **uargs_kernel;
    vaddr_t *uargs_user;
    size_t uargs_size;

    // ensure valid program and arguments
    if (program == NULL || uargs == NULL) {
        return(EFAULT);
    }

    // copy program from user space into kernel space
    program_kernel = (char *) kmalloc(sizeof(char) * PATH_MAX); 
    if (program_kernel == NULL) {
        return(ENOMEM); 
    }
    result = copyinstr((const_userptr_t) program, program_kernel, PATH_MAX, &program_size);
    if (result || program_size <= 1) {
       kfree(program_kernel); 
       return(EINVAL);
    }

    // copy arguments from user space into kernel space
    uargs_size = 0;
    while (uargs[uargs_size] != NULL) {
        uargs_size ++;
    }

    uargs_kernel = (char **) kmalloc(sizeof(char *) * (uargs_size + 1));
    for (size_t i = 0; i < uargs_size; ++i) {
        uargs_kernel[i] = (char *) kmalloc(sizeof(char) * PATH_MAX);
        result = copyinstr((const_userptr_t) uargs[i], uargs_kernel[i], PATH_MAX, &size);
        if (result) {
            kfree(program_kernel);
            kfree(uargs_kernel);
            return(EFAULT);
        }
    }
    uargs_kernel[uargs_size] = NULL;
    
	// open the file
	result = vfs_open(program_kernel, O_RDONLY, 0, &v);
	if (result) {
        kfree(program_kernel);
        kfree(uargs_kernel);
		return result;
	}

    // create new address space
    as_new = as_create();
    if (as_new == NULL) {
        kfree(program_kernel);
        kfree(uargs_kernel);
        vfs_close(v);
        return(ENOMEM);
    }
    
    // set new address space, delete old address space, and activate new address space
    as_old = curproc_setas(as_new);
    as_destroy(as_old);
    as_activate();

    // load the executable
	result = load_elf(v, &entrypoint);
	if (result) {
		// p_addrspace will go away when curproc is destroyed
        kfree(program_kernel);
        kfree(uargs_kernel);
		vfs_close(v);
		return result;
	}

    // done with the file now
    vfs_close(v);

    // define the user stack in the address space
	result = as_define_stack(as_new, &stackptr);
	if (result) {
		// p_addrspace will go away when curproc is destroyed
        kfree(program_kernel);
        kfree(uargs_kernel);
		return result;
	}

    // copy argument strings into user space, and keep track of virtual address
    uargs_user = (vaddr_t *) kmalloc(sizeof(vaddr_t) * (uargs_size + 1));
    for (size_t i = 0; i < uargs_size; ++i) {
        size = ROUNDUP(strlen(uargs_kernel[i]) + 1, 8);
        stackptr -= size;
        result = copyoutstr((const char *) uargs_kernel[i], (userptr_t) stackptr, size, &size);
        if (result) {
            kfree(program_kernel);
            kfree(uargs_kernel);
            kfree(uargs_user);
            return result;
        }

        uargs_user[i] = stackptr;     
    }
    uargs_user[uargs_size] = (vaddr_t) NULL;
    
    // copy argument array of virtual address into user space
    size = sizeof(vaddr_t) * (uargs_size + 1);
    stackptr -= ROUNDUP(size, 8);
    result = copyout((const void *) uargs_user, (userptr_t) stackptr, size);
    if (result) {
        kfree(program_kernel);
        kfree(uargs_kernel);
        kfree(uargs_user);
    }

    // free kernel space program and arguments
    kfree(program_kernel);
    kfree(uargs_kernel);

	// warp to user mode
	enter_new_process(uargs_size /*argc*/, 
            (userptr_t) stackptr /*userspace addr of argv*/,
            stackptr, 
            entrypoint);
	
	// enter_new_process does not return
	panic("enter_new_process returned\n");
	return(EINVAL);
}

#endif /* OPT_A2 */

