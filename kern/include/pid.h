#ifndef _PID_H_
#define _PID_H_

#include <limits.h>
#include <types.h>
#include <lib.h>
#include <synch.h>


#define PROC_RUNNING 1 /* The process is either in state RUNNING or READY here */
#define PROC_FINISHED 2 /* Process have exited and is waiting to exit status to be collected*/
#define NO_PARENT -1


/*
 * We keep track of each processes exit state in this structure.
 * 
 * status: A process status can either be RUNNING, FINISHED. if the process
 *         exits before its parent requests for its exit code, 
 *         it goes to FINISHED. This structure is deleted when the process has
 *         finished execution and the parent has collected its exitcode.
 * 
 */
struct pidstate {
    pid_t ps_pid; //process ID
    pid_t ps_ppid; //parent process ID

    /*
     * This condition variable is used
     * to sync our waitpid and exit functions
     */
    struct cv * ps_cv;
    struct lock * ps_cv_lk;

    struct lock * ps_lk;
    int ps_exit_code; // Exit code of the process

    /*
     * the status needs to be volatile 
     * as it is accessed accross multiple threads
     */
    volatile int ps_status;
};


/*
 * This structure hold a pidstate structure
 * for all existing processes.
 */
struct pidtable {
    struct lock * pt_lk;
    struct pidstate* pt_states[PID_MAX];
};

struct pidstate* pidstate_create(pid_t pid, pid_t ppid);
void pidstate_destroy(struct pidstate* ps);

/*
 * pid table management
 */
struct pidtable * pidtable_create(void);
void pidtable_destroy(struct pidtable* pt);
struct pidstate * pidtable_insert(char* name);
int pidtable_remove(int pid);

#endif /* _PID_H_ */
