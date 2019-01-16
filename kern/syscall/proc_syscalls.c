#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/limits.h>
#include <kern/seek.h>
#include <kern/stat.h>
#include <lib.h>
#include <uio.h>
#include <proc.h>
#include <current.h>
#include <synch.h>
#include <copyinout.h>
#include <vfs.h>
#include <vnode.h>
#include <openfile.h>
#include <filetable.h>
#include <syscall.h>
#include <pid.h>
#include <addrspace.h>
#include <machine/trapframe.h>
#include <current.h>
#include <kern/wait.h>

/* struct to keep track of process ids*/
struct pidtable * pt; 

/*
 * Bootstrap the pidtable
 */
int
pid_bootstrap(void){
    pt = pidtable_create();
    if(pt == NULL){
        return -1;
    }

    return 0;
}
/*
 * Returns the pidtable struct
 */
struct pidtable*
get_pidtable(){
    return pt;
}

/*
 * fork() - duplicates the currently running process
 * Returns twice, child returns 0, parent returns process ID of the child
 */
int
sys_fork(struct trapframe *tf , pid_t *retval) 
{
    struct proc * newproc;
    struct trapframe * tf_child;

    /* Fork new process from current process */
    proc_fork(&newproc);

    /* Copy address space */
    as_copy(proc_getas(), &(newproc->p_addrspace));
    if(newproc->p_addrspace == NULL) {
        goto nomem;
    }

    /* fork the current thread */
    tf_child = kmalloc(sizeof(struct trapframe));
    if(tf_child == NULL) {
        goto nomem;
    }
    memcpy(tf_child, tf, sizeof(struct trapframe));
    int error = thread_fork(curthread->t_name, newproc, &enter_forked_process, tf_child, 0);

    if(error) {
        proc_destroy(newproc);
        kfree(tf_child);
        return error;
    }

    /* Return to user mode */
    *retval = newproc->p_pidstate->ps_pid;


    return 0;

    nomem: 
        proc_destroy(newproc);
        return ENOMEM;
}

/*
 * execv() -  replaces the currently executing program with a newly loaded program image
 */
int
sys_execv(userptr_t program, userptr_t * args) {
    
    struct addrspace * as;
    struct addrspace * oldas;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;
    int size = 0;
    char ** copyin_args;
    char * copyin_program;
    int arg_count;

    if(program == NULL) {
        return EFAULT; 
    }

    if( args == NULL) {
        return EFAULT;
    }

    /*copyin program*/
    copyin_program = kmalloc(PATH_MAX);
    if(copyin_program == NULL){
        return ENOMEM;         
    }

    result = copyinstr(program, copyin_program, PATH_MAX, NULL);    
    if(result) {
        kfree(copyin_program);
        copyin_program = NULL;
        return result;
    }

    /* check that program is not an empty string */
    if(strlen(copyin_program) == 0){
        kfree(copyin_program);
        copyin_program = NULL;
        return EINVAL;
    }

    /* check args pointer is valid */
    char * testvalid = kmalloc(sizeof(char*));
    result = copyin((const_userptr_t)args, testvalid, sizeof(char*));
    if(result) {  
        return result;
    }
    kfree(testvalid);
    testvalid = NULL;


    /* check size of arguments and make sure pointers are valid pointers */
    for(arg_count = 0; args[arg_count] != NULL; arg_count++){
        char * testarg = kmalloc(sizeof(char*));
        result = copyin((const_userptr_t)args[arg_count], testarg, sizeof(char*));
        if(result) {  
            return result;
        }
        kfree(testarg);
        testarg = NULL;
        size += strlen((char*)args[arg_count]+1);
        if(size > ARG_MAX){
            return E2BIG;
        }
    }

    /* copyin the arguments */
    copyin_args = kmalloc((arg_count + 1) * sizeof(char*));
    if(copyin_args == NULL){
        return ENOMEM;
    }
    for(int i = 0; i < arg_count; i++){
        int arglen = strlen((char *)args[i]) + 1;
        copyin_args[i] = kmalloc(arglen * sizeof(char));
        if(copyin_args[i] == NULL){
            return ENOMEM;
        }
        int result = copyinstr(args[i], copyin_args[i], arglen, NULL);
        if(result) {  
            return result;
        }
    }

    copyin_args[arg_count] = NULL;

	/* Open the file. */
    char * prog;
    prog = kstrdup(copyin_program);
    if(prog == NULL) {
        return ENOMEM;
    }
	result = vfs_open(prog, O_RDONLY, 0, &v);
    kfree(prog);
	if (result) {
		return result;
	}

	/* Create a new address space. */
	as = as_create();
	if (as == NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	oldas = proc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
        proc_setas(oldas);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
        proc_setas(oldas);                                    
		return result;
	}

    /* keep trak of argument location in the stack*/
    vaddr_t * stackptrs = kmalloc(sizeof(vaddr_t)* (arg_count + 1));

    /* null terminate the arguments pointers */
    
    stackptrs[arg_count] = 0;

    /* align the stack pointer */
    while((stackptr % 4) != 0) {
        stackptr--;
    }

    /*copy arguments into the stack and save locations*/
    for(int i = arg_count-1 ; i >= 0; i --){
        int arglen = strlen(copyin_args[i]) + 1;

        /* adjust the stak pointer */
        stackptr -= ROUNDUP(arglen,4);
        
        /*get the argument pointer*/
        stackptrs[i] = stackptr;

        result = copyoutstr(copyin_args[i], (userptr_t) stackptr, arglen, NULL);
        if(result){
            return result;
        }  

    }
    /* Align the stack*/
    while( (stackptr % sizeof(vaddr_t)) != 0){
        stackptr-- ;
    }

    /* copy argument pointers to the stack */
    for(int i = arg_count; i >= 0; i--){
        stackptr -= sizeof(vaddr_t);
        result = copyout(&stackptrs[i], (userptr_t) stackptr, sizeof(vaddr_t));
        if(result){
            return result;
        }
    }

    /* destroy old address space */
    as_destroy(oldas);

    /*clean up*/
    for(int i = 0; i != arg_count; i++) {
        kfree(copyin_args[i]);
        copyin_args[i] = NULL;
    }
    kfree(stackptrs);
    stackptrs = NULL;
    kfree(copyin_args);
    copyin_args = NULL;

	/* Warp to user mode. */
	enter_new_process(arg_count , (userptr_t) stackptr, NULL, stackptr, entrypoint);

	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
    return 0;
}

/*
 * exit() - terminate current process
 */
void
sys_exit(int exitcode, int exittype){
    
    struct pidstate * pstate = curthread->t_proc->p_pidstate;

    lock_acquire(pstate->ps_lk);
    
    /* set the exit code and broadcast to waiting parent*/
    if(pstate->ps_ppid != NO_PARENT) {
        pstate->ps_status = PROC_FINISHED;
        if(exittype == 0) {
            pstate->ps_exit_code = _MKWAIT_EXIT(exitcode);
        } else {
            pstate->ps_exit_code = _MKWAIT_SIG(exitcode);
        }
        lock_acquire(pstate->ps_cv_lk);
        cv_broadcast(pstate->ps_cv, pstate->ps_cv_lk);
        lock_release(pstate->ps_cv_lk);
        lock_release(pstate->ps_lk);
    }
    /*don't care about exit code since parent has exited already*/
    else {
        lock_release(pstate->ps_lk);
        pidtable_remove(pstate->ps_pid);
    }

    /*recycle pid states and change parent pid for orphaned process's*/
    for(int i = PID_MIN; i < PID_MAX; i++) {
        if(pt->pt_states[i] != NULL){
            if(pt->pt_states[i]->ps_ppid == pstate->ps_pid){
                lock_acquire(pt->pt_states[i]->ps_lk);
                pt->pt_states[i]->ps_ppid = NO_PARENT;
                if(pt->pt_states[i]->ps_status == PROC_FINISHED) {
                    lock_release(pt->pt_states[i]->ps_lk);
                    pidtable_remove(i);
                }
                else{
                    lock_release(pt->pt_states[i]->ps_lk);
                }
            }
        }
    }

    /* destroy process */
    struct proc * c_proc = curthread->t_proc;
    proc_remthread(curthread);
    proc_destroy(c_proc);
    thread_exit_detached();

    panic("shouldn't return from thread exit in sys_exit");
}

/*
 * waitpid() - wait for a child process to exit
 */
int
sys_waitpid(pid_t pid, userptr_t status, int options, pid_t *retval){

    struct pidstate * pstate = curthread->t_proc->p_pidstate;
    struct pidstate * pstate_child = pt->pt_states[pid];
    (void) options;
    int exitstatus;
    int err;
    /* error handling */
    if(pstate_child == NULL) {
        return ESRCH;
    }

    if(pstate_child->ps_ppid != pstate->ps_pid) {
        return ECHILD;
    }

    if(options != 0){
        return EINVAL;
    }

    /* wait for child to exit */
    lock_acquire(pstate_child->ps_cv_lk);

    if(pstate_child->ps_status == PROC_RUNNING) {
        cv_wait(pstate_child->ps_cv, pstate_child->ps_cv_lk);
    }

    lock_release(pstate_child->ps_cv_lk);

    /* retrieve exit status from child*/
    exitstatus = pstate_child->ps_exit_code;
    if( status != NULL) {
        err = copyout(&exitstatus, status, sizeof(int));
        if(err) {
            return err;
        }
    }
    *retval = pid;
    return 0;
}
/*
 * getpid() - get process id of the current process
 */
int 
sys_getpid(pid_t *retval){ 
    *retval = curthread->t_proc->p_pidstate->ps_pid;
    return 0;
}
