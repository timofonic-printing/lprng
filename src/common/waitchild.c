/***************************************************************************
 * LPRng - An Extended Print Spooler System
 *
 * Copyright 1988-1995 Patrick Powell, San Diego State University
 *     papowell@sdsu.edu
 * See LICENSE for conditions of use.
 *
 ***************************************************************************
 * MODULE: waitchild.c
 * PURPOSE: wait for children
 **************************************************************************/
/***************************************************************************
 * The following code is incredibly complex.  It does things to guard
 * against race conditions that are incredibly complex.  Kinda makes you
 * long for a simple 'wait' statement...
 *
 * Patrick Powell Sun Apr 16 19:42:18 PDT 1995
 *
 * Sat Feb 10 06:17:00 PST 1996
 *  Apparently not quite complex enough!  There is a wait condition in the
 *  SIGCHLD handler with the malloc() code - you need to preallocate the
 *  data structures and not call malloc in the handler!
 *
 ***************************************************************************/

static char *const _id =
"$Id: waitchild.c,v 3.1 1996/09/09 14:24:41 papowell Exp papowell $";

#include "lp.h"
#include "decodestatus.h"
/*
 *
 * Patrick Powell Sun Aug 13 18:53:14 PDT 1995
 *  This code was reviewed, and ripped to shreds, coded, tightened, bulkheaded
 *  and otherwise put into production shape. Sigh.
 *
 * Commments from PLP4.0 code
 *
 * This should work on every system that implements wait3() or waitpid()
 * and supports SIGCHLD or SIGCLD with any of the semantics.
 * It needs to handle the SIGCHLD/SIGCLD signal itself.
 * -- Justin Mason <jmason@iona.ie> 94.
 *
 * Here's the comment from the original version:
 * 
 *      This procedure emulates the POSIX waitpid kernel call on
 *      BSD systems that don't have waitpid but do have wait3.
 *      This code is based on a prototype version written by
 *      Mark Diekhans and Karl Lehenbauer.
 *
 */

/*
 * A linked list of the following structures is used to keep track
 * of processes for which we received notification from the kernel,
 * but the application hasn't waited for them yet (this can happen
 * because wait may not return the process we really want).  We
 * save the information here until the application finally does
 * wait for the process.
 */

/* maximum number of process to wait for */
#define ALLOC_BLK 100

typedef struct WaitInfo {
	pid_t pid;                          /* Pid of process that exited. */
	plp_status_t status;                /* Status returned when child exited
										 * or suspended. */
	struct WaitInfo *nextPtr;           /* Next in list of exited processes. */
} WaitInfo;

static WaitInfo *DeadHead;       /* First in list of all dead */
static WaitInfo *DeadTail;       /* Last in list of all dead */
static WaitInfo *Freelist;       /* First in list of all free */

static int catching_SIGCHLD = 0;  /* flag */

static void alloc_blocks()
{
	int size, i;
	size = sizeof(Freelist[0]) * ALLOC_BLK;
	malloc_or_die( Freelist, size );
	for( i = 0; i < ALLOC_BLK-1; ++i ){
		Freelist[i].nextPtr = &Freelist[i+1];
	}
	Freelist[i].nextPtr = 0;
}

/* If a child process has changed status, but we're waiting for some
 * particular process, we should store it for later use.
 * Tricky point: first check for an existing entry for the
 * process and overwrite it if it exists (e.g. a previously
 * stopped process might now be dead).
 */

static void waitpid_add_to_list (plp_status_t status, pid_t result)
{
	register WaitInfo *w;

	for (w = DeadHead; w != 0; w = w->nextPtr) {
		if (w->pid == result) {
			w->status = status;
			DEBUG6 ("waitpid_add_to_list: overwriting %d (%s)",
						result, Decode_status (&status));
			return;
		}
	}
again:
	if( (w = Freelist) ){
		Freelist = Freelist->nextPtr;
	} else {
		log( LOG_ERR, "wait_add_to_list: out of wait() blocks!");
		alloc_blocks();
		goto again;
	}
	memset(w, 0, sizeof(w[0])); /* paranoia */
	w->pid = result;
	w->status = status;
	if( DeadTail ){
		DeadTail->nextPtr = w;
	} else {
		DeadHead = w;
	}
	DeadTail = w;
	DEBUG6 ("waitpid_add_to_list: saving %d (%s)",
		result, Decode_status (&status));
}

/*
 * See if there's a suitable process that has already stopped or
 * exited. If so, remove it from the list of exited processes and
 * return its information.
 *
 * Also, store the location of the start of the list before each
 * checking run; if this changes, more entries have been added,
 * and we should re-check.
 */

static pid_t waitpid_check_list (pid_t pid,plp_status_t *statusPtr,int options)
{
	WaitInfo *waitPtr, *prevPtr;
	pid_t result;

	DEBUG6 ("waitpid_check_list: checking savelist for %d", pid);
	for (waitPtr = DeadHead, prevPtr = 0; waitPtr != 0;
			prevPtr = waitPtr, waitPtr = waitPtr->nextPtr){
		DEBUG8 ("waitpid_check_list: list entry %d, status 0x%x",
			waitPtr->pid, waitPtr->status);
		if ((pid != waitPtr->pid) && (pid != -1)){
			continue;
		}
		if (!(options & WUNTRACED) && (WIFSTOPPED (waitPtr->status))) {
			continue;
		}
		result = waitPtr->pid;
		*statusPtr = *((plp_status_t *) &waitPtr->status);

		DeadTail = prevPtr;
		if (prevPtr == 0){
			DeadHead = waitPtr->nextPtr;
		} else {
			prevPtr->nextPtr = waitPtr->nextPtr;
		}
		waitPtr->nextPtr = Freelist;
		Freelist = waitPtr;
		DEBUG6 ("waitpid_check_list: returning '%d'", result);
		return result;
	}
	result = -1;
	DEBUG6 ("waitpid_check_list: returning '%d'", result);
	return(result);
}

/*
 *----------------------------------------------------------------------
 *
 * waitpid --
 *
 *      This procedure emulates the functionality of the POSIX
 *      waitpid kernel call, using the BSD wait3 kernel call.
 *      Note:  it doesn't emulate absolutely all of the waitpid
 *      functionality, in that it doesn't support pid's of 0
 *      or < -1.
 *
 * Results:
 *      -1 is returned if there is an error in the wait kernel call.
 *      Otherwise the pid of an exited or suspended process is
 *      returned and *statusPtr is set to the status value of the
 *      process.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

/* Check to see if any processes have stopped or exited. */

static pid_t
waitpid_wrapper (pid_t pid, plp_status_t *statusPtr, int options)
{
	int result;
	plp_status_t status;
	int err;

retry_waitpid:

#ifdef HAVE_WAITPID
	DEBUG6 ("waitpid_wrapper: calling waitpid");
	result = waitpid (pid, &status, options);
#else
#ifdef HAVE_WAIT3
	DEBUG6 ("waitpid_wrapper: calling wait3");
	result = wait3 (&status, options, 0);
#else
#error	wait not supported!!;
#endif /* HAVE_WAIT3 */
#endif /* HAVE_WAITPID */

	err = errno;
	DEBUG6 ("waitpid_wrapper: waitpid %d returned pid %d, status 0x%x",
		pid, result, status );
	/* interpret the result for debugging purposes */
	if (result == 0) {
		DEBUG6 ("waitpid_wrapper: kid is still alive");

	} else if (result == -1 && err == ECHILD) {
		DEBUG6 ("waitpid_wrapper: no kids left alive");

	} else if (result < 0) {
		DEBUG6 ("waitpid_wrapper: returning error");

	} else if ((pid != result) && (pid != -1)) {
		/* this is not the correct process */
		waitpid_add_to_list (status, result);
		goto retry_waitpid;

	} else if (!(options & WUNTRACED) && (WIFSTOPPED(status))) {
		/* this is not the correct process */
		waitpid_add_to_list (status, result);
		goto retry_waitpid;

	} else {
		*statusPtr = *((plp_status_t *) &status);
		DEBUG6 ("waitpid_wrapper: returning %d (%s)",
					result, Decode_status (statusPtr));
	}
	return result;
}

plp_signal_t
sigchld_handler (int signo)
{
	pid_t pid;
	int status;
	int err;

	err = errno;
	assert (signo == SIGCHLD);
	DEBUG6 ("sigchld_handler: caught SIGCHLD");

	while ((pid = waitpid_wrapper (-1, &status, WNOHANG)) > 0) {
		waitpid_add_to_list (status, pid);
	}

	/* reestablish signal handler */
	plp_signal(SIGCHLD, sigchld_handler);
	errno = err;
}

#ifdef HAVE_SIGPROCMASK
	sigset_t zeromask, newmask, oldmask;
#endif

static void block_sigchld (void)
{
	int err;

	err = errno;
#ifdef HAVE_SIGPROCMASK
	sigemptyset (&zeromask);
	sigemptyset (&newmask); sigaddset (&newmask, SIGCHLD);
	if (sigprocmask (SIG_BLOCK, &newmask, &oldmask) < 0)
		logerr_die (LOG_INFO, "block_sigchld: sigprocmask() failed");
#else
	/* argh -- setting SIGCHLD with signal() can be really unportable.
	 * We just assume the BSD behaviour, as later systems have
	 * sigprocmask implementations (which are much more portable).
	 */
	plp_signal (SIGCHLD, SIG_IGN);
#endif
	errno = err;
}

static void unblock_sigchld (void)
{
	int err;

	err = errno;
#ifdef HAVE_SIGPROCMASK
	if( sigprocmask (SIG_SETMASK, &oldmask, 0) < 0 )
		logerr_die (LOG_INFO, "unblock_sigchld: sigprocmask() failed");
#else
	plp_signal (SIGCHLD, sigchld_handler);
#endif
	errno = err;
}

pid_t plp_waitpid (pid_t pid, plp_status_t *statusPtr, int options)
{
	pid_t result;

	if( catching_SIGCHLD == 0 ){
		(void) plp_signal(SIGCHLD, sigchld_handler);
		catching_SIGCHLD = 1;
	}
	/* we will block for the child process */

	block_sigchld();       /* race condition otherwise */

	result = waitpid_check_list( pid, statusPtr, options );
	if (result == -1) {
		/* now we perform the actual wait if necessary */
		result = waitpid_wrapper (pid, statusPtr, options );
	}

	unblock_sigchld ();

	return result;
}

void Setup_waitpid (void)
{
	/* the portable waitpid() emulation function needs this SIGCHLD handler. */
	if( catching_SIGCHLD == 0 ){
		if( Freelist == 0 ) alloc_blocks();
		/* we suppress getting signals except at well controlled places */
		/*(void) plp_signal(SIGCHLD, sigchld_handler);*/
		catching_SIGCHLD = 1;
	}
}
