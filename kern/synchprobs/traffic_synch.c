#include <types.h>
#include <lib.h>
#include <synchprobs.h>
#include <synch.h>
#include <opt-A1.h>

/* 
 * This simple default synchronization mechanism allows only vehicle at a time
 * into the intersection.   The intersectionSem is used as a a lock.
 * We use a semaphore rather than a lock so that this code will work even
 * before locks are implemented.
 */

/* 
 * Replace this default synchronization mechanism with your own (better) mechanism
 * needed for your solution.   Your mechanism may use any of the available synchronzation
 * primitives, e.g., semaphores, locks, condition variables.   You are also free to 
 * declare other global variables if your solution requires them.
 */

/*
 * replace this with declarations of any synchronization and other variables you need here
 */
// static struct semaphore *intersectionSem;
static struct lock *state_lock;
static struct cv *state_cv; 
static volatile Direction state;
static volatile int enter_count;
static volatile int exit_count;
static const int MAX_COUNT = 5;


/* 
 * The simulation driver will call this function once before starting
 * the simulation
 *
 * You can use it to initialize synchronization and other variables.
 * 
 */
void
intersection_sync_init(void)
{
  /* replace this default implementation with your own implementation */

/*
  intersectionSem = sem_create("intersectionSem",1);
  if (intersectionSem == NULL) {
    panic("could not create intersection semaphore");
  }
  return;
*/
        state_lock = lock_create("state_lock");
        if (state_lock == NULL) {
                panic("could not create state lock");
        } 
        state_cv = cv_create("state_cv");
        if (state_cv == NULL) {
                panic("could not create state cv");
        }
        state = north;
        enter_count = 0;
        exit_count = 0;
}

/* 
 * The simulation driver will call this function once after
 * the simulation has finished
 *
 * You can use it to clean up any synchronization and other variables.
 *
 */
void
intersection_sync_cleanup(void)
{
  /* replace this default implementation with your own implementation */
/*
  KASSERT(intersectionSem != NULL);
  sem_destroy(intersectionSem);
*/
        
        KASSERT(state_lock != NULL);
        lock_destroy(state_lock);
        cv_destroy(state_cv);
}


/*
 * The simulation driver will call this function each time a vehicle
 * tries to enter the intersection, before it enters.
 * This function should cause the calling simulation thread 
 * to block until it is OK for the vehicle to enter the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle is arriving
 *    * destination: the Direction in which the vehicle is trying to go
 *
 * return value: none
 */

void
intersection_before_entry(Direction origin, Direction destination) 
{
  /* replace this default implementation with your own implementation */
  // (void)origin;  /* avoid compiler complaint about unused parameter */
  (void)destination; /* avoid compiler complaint about unused parameter */
/*
  KASSERT(intersectionSem != NULL);
  P(intersectionSem);
*/
        lock_acquire(state_lock);
        bool occupied = (enter_count != exit_count);
        while (occupied && (state != origin || enter_count >= MAX_COUNT)) {
                DEBUG(DB_THREADS, "Waiting for state change to allow %d to %d.\n", origin, destination);
                cv_wait(state_cv, state_lock);
                occupied = (enter_count != exit_count);
        }
        if (!occupied && state != origin) {
                DEBUG(DB_THREADS, "Intersection is empty, changing state from %d to %d.\n", state, origin);
                enter_count = 0;
                exit_count = 0;
                state = origin;
                // cv_broadcast(state_cv, state_lock);
        }
        DEBUG(DB_THREADS, "Entering intersection from %d to %d.\n", origin, destination);
        enter_count ++; 
        lock_release(state_lock);
}


/*
 * The simulation driver will call this function each time a vehicle
 * leaves the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle arrived
 *    * destination: the Direction in which the vehicle is going
 *
 * return value: none
 */

void
intersection_after_exit(Direction origin, Direction destination) 
{
  /* replace this default implementation with your own implementation */
  (void)origin;  /* avoid compiler complaint about unused parameter */
  (void)destination; /* avoid compiler complaint about unused parameter */
/*
  KASSERT(intersectionSem != NULL);
  V(intersectionSem);
*/
        lock_acquire(state_lock);
        DEBUG(DB_THREADS, "Leaving intersection.\n");
        exit_count ++;
        if (exit_count == enter_count) {
                cv_broadcast(state_cv, state_lock);
        }
        lock_release(state_lock);        
}
