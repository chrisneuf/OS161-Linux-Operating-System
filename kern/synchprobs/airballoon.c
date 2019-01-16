/*
 * Driver code for airballoon problem
 */
#include <types.h>
#include <lib.h>
#include <thread.h>
#include <test.h>
#include <synch.h>

#define NROPES 16

/*
 * Describe your design and any invariants or locking protocols 
 * that must be maintained. Explain the exit conditions. How
 * do all threads know when they are done?
 * 
 * My design uses a rope structure which contains hooks and stakes for each ropes mappings
 * When ever a rope is switched or severed the rope must be locked by the flower thread trying to 
 * switch or sever. The counter for the amount of ropes left must also be locked before updating
 * after a flower severes a rope.
 * 
 * The main thread creates a semaphore with 0 inital value which the ballon thread must decrment three times to complete
 * Everytime a flower thread completes it increments the semaphore by one. The main thread is also 
 * uses a condion varible to wait for completion from the ballon. Once the ballon has decremented the 
 * semaphore 3 times (meaning all flowers are done) it signals using the cv to the main thread and then the program 
 * completes
 * 
 * Each of the flower threads runs an infite while loop that only breaks when all ropes are severed
 * 
 */

static volatile int ropes_left = NROPES;

//lock for ropes left
static struct lock * ropes_left_lk;

// condition varible for baloon thread to signal finish
static struct cv * balloon_cv;
static struct lock * balloon_cv_lk;

//semaphore for flower thread completion
static struct semaphore * flower_sem;

/*
	Repersents the rope in the air balloon problem
	Contians a hook and stake that the rope is linked to
	and a severed flag. 
	The lock is used to claim access to the rope
	for severing or switching stakes
*/
struct rope {
	unsigned hook;
	volatile unsigned stake;
	volatile unsigned severed;
	struct lock * rope_lk; 
};

//array of all ropes tied to ballon
struct rope ropes[NROPES];

/*
	Assigns every rope in the rope array
	to a hook and stake and creates rope locks
*/
static
void
map_ropes(void) {
	for(int i = 0; i < NROPES; i++) {
		ropes[i].hook = i;
		ropes[i].stake = i;
		ropes[i].severed = 0;
		ropes[i].rope_lk = lock_create("rope lock");
		KASSERT(ropes[i].rope_lk != NULL);
	}
}

/*
	Clean up function to destroy every semaphore, lock
	and condition varible
*/
static
void 
clean_up(void) {
	sem_destroy(flower_sem);
	cv_destroy(balloon_cv);
	lock_destroy(balloon_cv_lk);
	lock_destroy(ropes_left_lk);
	for(int i = 0; i < NROPES; i++) {
		lock_destroy(ropes[i].rope_lk);
	}
}

/*
	Dandelion chooses a random hook and tries to sever
	the rope attached to it if it exists. This function will return 
	when all ropes have been severed.
*/
static
void
dandelion(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;
	
	kprintf("Dandelion thread starting\n");

	int hook;

	while(1) {

		//check if any ropes are left
		lock_acquire(ropes_left_lk);
		if(ropes_left == 0)
		{
			lock_release(ropes_left_lk);
			break;
		}
		lock_release(ropes_left_lk);

		// get a random hook to severe
		hook = random()%NROPES;

		// check that the rope has not been severed
		if(ropes[hook].severed == 1) {
			continue;
		}


		lock_acquire(ropes[hook].rope_lk);

		// check that the rope has not been severed while aquiring the lock
		if(ropes[hook].severed == 1) {
			lock_release(ropes[hook].rope_lk);
			continue;
		}

		//sever the rope
		lock_acquire(ropes_left_lk);
		ropes_left--;
		ropes[hook].severed = 1;
		kprintf("Dandelion severed rope %u\n", hook);
		lock_release(ropes_left_lk);

		lock_release(ropes[hook].rope_lk);
		thread_yield();
	}

	kprintf("Dandelion thread done\n");
	V(flower_sem);
}

/*
	Marigold chooses a random stake and tries to sever
	the rope attached to it if it exists. This function will return 
	when all ropes have been severed.
*/
static
void
marigold(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;
	
	kprintf("Marigold thread starting\n");

	unsigned stake;
	signed rope_index;

	while(1) {

		//check if any ropes are left to sever
		lock_acquire(ropes_left_lk);
		if(ropes_left == 0)
		{
			lock_release(ropes_left_lk);
			break;
		}
		lock_release(ropes_left_lk);

		// get a random stake to severe
		stake = random() % NROPES;

		// get the rope attached to the stake
		rope_index = -1;
		for(int i = 0; i < NROPES; i++) {
			if(ropes[i].stake == stake) {
				rope_index = i;
				break;
			}
		}
	
		// if the stake doesnt have a rope or the rope has been severed we find a new stake
		if(rope_index == -1 || ropes[rope_index].severed == 1) {
			continue;
		}

		lock_acquire(ropes[rope_index].rope_lk);

		// if rope was severed or switched during our lock aquire we find a new rope
		if(ropes[rope_index].severed == 1 || ropes[rope_index].stake != stake) {
			lock_release(ropes[rope_index].rope_lk);
			continue;
		}

		//sever the rope
		lock_acquire(ropes_left_lk);
		ropes_left--;
		ropes[rope_index].severed = 1;
		kprintf("Marigold severed rope %u from stake %u\n", rope_index, stake);
		lock_release(ropes_left_lk);

		lock_release(ropes[rope_index].rope_lk);
		thread_yield();
	}

	kprintf("Marigold thread done\n");
	V(flower_sem);	
}

/*
	Flowerkiller chooses a random stake and if a rope is attached to the stake
	Flowerkiller will try move it to a random different stake
*/
static
void
flowerkiller(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;
	
	kprintf("Lord FlowerKiller thread starting\n");

	unsigned cur_stake;
	unsigned new_stake;
	unsigned rope_index;

	while(1) {

		//check if any ropes are left

		lock_acquire(ropes_left_lk);
		if(ropes_left == 0)
		{
			lock_release(ropes_left_lk);
			break;
		}
		lock_release(ropes_left_lk);

		cur_stake = random() % NROPES;

		// get the rope attached to the stake
		for(int i = 0; i < NROPES; i++) {
			if(ropes[i].stake == cur_stake) {
				rope_index = i;
				break;
			}
		}

		//if rope is severed find new rope
		if(ropes[rope_index].severed == 1) {
			continue;
		}

		lock_acquire(ropes[rope_index].rope_lk);
		// if rope was severed lock aquire we find a new rope
		if(ropes[rope_index].severed == 1) {
			lock_release(ropes[rope_index].rope_lk);
			continue;
		}
		
		// find a different stake to switch the rope to
		new_stake = random() % NROPES;
		if( new_stake == cur_stake) {
			new_stake = (new_stake + 1) % NROPES;
		}

		//swwitch rope to new stake
		ropes[rope_index].stake = new_stake;
		kprintf("Lord FlowerKiller switched rope %u from stake %u to stake %u\n", rope_index, cur_stake, new_stake);
		lock_release(ropes[rope_index].rope_lk);
		thread_yield();
	}

	kprintf("Lord FlowerKiller thread done\n");
	V(flower_sem);
}

/*
	Ballon wait for the flower threads to finish
	and then signals to the main thread that the ballons have been freed
*/

static
void
balloon(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;
	
	kprintf("Balloon thread starting\n");

	int flowers = 3;

	//wait for flower threads
	for(int i = 0; i < flowers; i++) {
		P(flower_sem);
	}

	kprintf("Balloon freed and Prince Dandelion escapes!\n");

	//signal all threads done to main
	lock_acquire(balloon_cv_lk);
	kprintf("Balloon thread done\n");
	cv_signal(balloon_cv, balloon_cv_lk);
	lock_release(balloon_cv_lk);
}


/*
	Airballon is the main thread which starts flowers abd ballon
	and waits for the to complete.
*/
int
airballoon(int nargs, char **args)
{

	int err = 0;

	(void)nargs;
	(void)args;
	(void)ropes_left;

	//initalize locks and semaphores
	ropes_left_lk = lock_create("ropes left lock");
	KASSERT(ropes_left_lk != NULL);
	map_ropes();
	ropes_left = NROPES;

	flower_sem = sem_create("flower sem", 0);
	KASSERT(flower_sem != NULL);

	//create cv for ballon to signal completion
	balloon_cv_lk = lock_create("balloon cv lock");
	KASSERT(balloon_cv_lk != NULL);
	balloon_cv = cv_create("balloon cv");
	KASSERT(balloon_cv != NULL);

	//start flower and balloon threads
	err = thread_fork("Marigold Thread",
			  NULL, marigold, NULL, 0);
	if(err)
		goto panic;
	
	err = thread_fork("Dandelion Thread",
			  NULL, dandelion, NULL, 0);
	if(err)
		goto panic;
	
	err = thread_fork("Lord FlowerKiller Thread",
			  NULL, flowerkiller, NULL, 0);
	if(err)
		goto panic;

	err = thread_fork("Air Balloon",
			  NULL, balloon, NULL, 0);
	if(err)
		goto panic;

	goto done;
panic:

	panic("airballoon: thread_fork failed: %s)\n",
	      strerror(err));
	
done:

	// wait for ballon to be freed
	lock_acquire(balloon_cv_lk);
	cv_wait(balloon_cv, balloon_cv_lk);
	lock_release(balloon_cv_lk);
	clean_up();

	kprintf("Main thread done\n");
	return 0;
}
