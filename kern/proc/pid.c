
#include <kern/errno.h>
#include <pid.h>
#include <limits.h>
#include <types.h>
#include <current.h>
#include <proc.h>
#include <syscall.h>


/*
 * Helper function to create a process pidstate
 */
struct pidstate *
pidstate_create(pid_t pid, pid_t ppid) 
{
    struct pidstate *p_state;

    p_state = kmalloc(sizeof(struct pidstate));
	if (p_state == NULL) {
		return NULL;
	}

    p_state->ps_lk = lock_create("ps_lk");
    if(p_state->ps_lk == NULL) {
        kfree(p_state);
        return NULL;
    }

    p_state->ps_cv_lk = lock_create("ps_cv_lk");
    if(p_state->ps_cv_lk == NULL) {
        lock_destroy(p_state->ps_lk);
        kfree(p_state);
        return NULL;
    }

    p_state->ps_cv = cv_create("ps_cv");
    if(p_state->ps_cv == NULL) {
        lock_destroy(p_state->ps_cv_lk);
        lock_destroy(p_state->ps_lk);
        kfree(p_state);
        return NULL;
    }

    p_state->ps_pid = pid;
    p_state->ps_ppid = ppid;
    p_state->ps_exit_code = 0;
    p_state->ps_status = PROC_RUNNING;

    return p_state;
}


/*
 * Helper function to get rid of 
 * a process pidstate
 */
void pidstate_destroy(struct pidstate * ps){
    if(ps->ps_lk != NULL) {
        lock_destroy(ps->ps_lk);
    }
    
    if(ps->ps_cv != NULL) {
       cv_destroy(ps->ps_cv); 
    }
    
    if(ps->ps_cv_lk != NULL) {
       lock_destroy(ps->ps_cv_lk); 
    }

    kfree(ps);
}


/*
 * Removes a pidstate entry from the pidtable
 */
int 
pidtable_remove(int pid){
    struct pidtable* pt = NULL;
    struct pidstate *p_state;
    
    pt = get_pidtable();
    if(pt == NULL){
        return -1;
    }

    p_state = pt->pt_states[pid];

    lock_acquire(pt->pt_lk);
    pt->pt_states[pid] = NULL;
    lock_release(pt->pt_lk);

    pidstate_destroy(p_state);

    return 0;
}   

/*
 * Creates a pidstate entry for the process
 * <name> in the pidtable and inserts it into the table
 * and assigns it the lowest pid availble. 
 * 
 * Returns: > NULL if there is no process ids left.
 *          > the created pidstate object otherwise.
 *    
 * No need to lock the table when inserting a pidstate for the kernel
 * process
 */
struct pidstate *
pidtable_insert(char* name){
    struct pidtable* pt = NULL;
    struct pidstate *p_state;
    
    pt = get_pidtable();
    if(pt == NULL){
        return NULL;
    }

    pid_t pid = -1;

    /*Locks are not ready to be acquired on creation of the kernel process*/
    if(strcmp(name, "[kernel]") != 0) {
        lock_acquire(pt->pt_lk);
    }

    for(int i = PID_MIN; i < PID_MAX; i++) {
        if(pt->pt_states[i] == NULL){
            pid = i;
            break;
        }
    }

    if(pid == -1) {
        return NULL;
    }

    /* The kernel process does not have a kernel process*/
    if(strcmp(name, "[kernel]") == 0) {
        p_state = pidstate_create(pid, NO_PARENT);
    }
    else {
        p_state = pidstate_create(pid, curthread->t_proc->p_pidstate->ps_pid);
    }
    
    pt->pt_states[pid] = p_state;
    if(strcmp(name, "[kernel]") != 0) {
        lock_release(pt->pt_lk);
    }

    return p_state;
}

/*
 * Creates the table to keep track of processes' states
 */
struct pidtable *
pidtable_create(void)
{
    struct pidtable* pt;
    pt = kmalloc(sizeof(struct pidtable));
    if(pt == NULL){
        return NULL;
    }

    pt->pt_lk = lock_create("pt_lk");
    if(pt->pt_lk == NULL){
        kfree(pt);
        return NULL;
    }

    for(int i = PID_MIN; i < PID_MAX; i++) {
        pt->pt_states[i] = NULL;
    }

    return pt;
}

/*
 * Gets rid of the kernel process
 * This should only happen on termination of the kernel process
 * 
 */
void 
pidtable_destroy(struct pidtable* pt)
{
    for(int i = PID_MIN; i < PID_MAX; i++){
        if(pt->pt_states[i] != NULL) {
            pidstate_destroy(pt->pt_states[i]);
        } 
    }
    lock_destroy(pt->pt_lk);
    kfree(pt);
}

