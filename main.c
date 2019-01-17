
/* Copyright (c) 2014 Red Hat Inc.

   Written by Carlos O'Donell <codonell@redhat.com>

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.  */

/* Example: How to use vfork safely from a multi-threaded application.

   This example is intended to show the safe usage of vfork by a multi-threaded
   application.  The example does not use any advanced features like clone
   without CLONE_VFORK to avoid parent suspension.  The example can also be
   rewritten slightly to be used in a non-multithreaded environment and it still
   remains safe since the latter is just a degenerate case of the former with
   one main thread.

   The example is only valid on Linux with the GNU C Library as the core
   runtime.  Other runtimes may require other actions to call vfork safely from
   a multi-threaded application.

   The inline comments in the code will explain each of the steps taken and
   why.  Justification for some steps is rather complicated so please read it
   twice before asking questions.

   Any questions should go to libc-help@sourceware.org where the GNU C Library
   community can assist with interpretations of this code.  */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>

/* The helper thread executes this application.  */
const char *filename = "/bin/ls";
char *const new_argv[2] = { "/bin/ls", NULL };
char *const new_envp[1] = { NULL };
int status;

void *
run_thread (void *arg)
{
  int i, ret;
  pid_t child, waited;
  struct sigaction newsa, oldsa;

  /* Block all signals in the parent before calling vfork. This is
     for the safety of the child which inherits signal dispositions and
     handlers. The child, running in the parent's stack, may be delivered a
     signal. For example on Linux a killpg call delivering a signal to a
     process group may deliver the signal to the vfork-ing child and you want
     to avoid this. The easy way to do this is via: sigemptyset,
     sigaction, and then undo this when you return to the parent. To be
     completely correct the child should set all non-SIG_IGN signals to
     SIG_DFL and the restore the original signal mask, thus allowing the
     vforking child to receive signals that were actually intended for it, but
     without executing any handlers the parent had setup that could corrupt
     state. When using glibc and Linux these functions i.e. sigemtpyset,
     sigaction, etc. are safe to use after vfork. */
  sigset_t signal_mask, old_signal_mask, empty_mask;
  sigfillset (&signal_mask);

  /* One might think we need to block SIGCANCEL (cancellation handling signal)
     and SIGSETXID (set*id handling signal). These signals are a hidden part of
     the implementation, and if delivered to the child would corrupt the parent
     state. The SIGSETXID signal is only sent to threads that the
     implementation knows about and the child of vfork is not known as a thread
     and thus safe from having a set*id handler run. This is a distinct issue
     from the one below regarding calling set*id functions. The SIGCANCEL
     signal is only sent in response to a pthread_cancel call, and since the
     child has no pthread_t it will not receive that signal by any ordinary
     means. Thus it would be undefined for anything to send SIGSETXID or
     SIGCANCEL to the child thread. If you suspect something like this is
     happening you might try adding this code:

	#define SIGCANCEL __SIGRTMIN
	#define SIGSETXID (__SIGRTMIN + 1)
	sigaddset (&signal_mask, SIGCANCEL);
	sigaddset (&signal_mask, SIGSETXID);

     This will prevent cancellation and set*id signals from being acted upon.
     Please report this problem upstream if you encounter it since the child
     running either handler for those signals is an implementation defect. */

  pthread_sigmask (SIG_BLOCK, &signal_mask, &old_signal_mask);

  /* WARNING: Do not call any set*id functions from other threads while
     vfork-ing. Doing this could result in two threads with distinct UIDs
     sharing the same memory space. As a concrete example a thread might be
     running as root, vfork a helper, and then proceed to setuid to a
     lower-priority user, and run some untrusted code. In this case the higher
     priority root uid child shares the same address space as the low-priority
     threads. The low-priority threads might then remap parts of the address
     space to get root uid child, which has not yet exec'd, to execute
     something else entirely. This is why you should be careful about calling
     set*id functions while vforking. You avoid this problem by coordinating
     your credential transitions to happen after you know your vfork is
     complete i.e. the parent is resumed and this tells you the child has
     completed execing. If you can't coordinate the use of set*id, then the
     only option left is to use the posix_spawn* interfaces which serialize
     against set*id in glibc (Sourceware BZ #14750 and BZ #14749 must be
     fixed in your version of glibc for this to work properly). */
  child = vfork ();

  if (child == 0)
    {
      /* In the child.  */

      /* We reset all signal dispositions that aren't SIG_IGN to SIG_DFL.
         This is done because the child may have a legitimate need to
         receive a signal and the default actions should be taken for
         those signals. Those default actions will not corrupt state in
         the parent. */ 
      newsa.sa_handler = SIG_DFL;
      if (sigemptyset (&empty_mask) != 0)
	_exit (1);
      newsa.sa_mask = empty_mask;
      newsa.sa_flags = 0;
      newsa.sa_restorer = 0;
      for (i = 0; i < NSIG; i++)
	{
	  ret = sigaction (i, NULL, &oldsa);
	  /* If the signal doesn't exist it returns an error and we skip it.  */
	  if (ret == 0
	      && oldsa.sa_handler != SIG_IGN
	      && oldsa.sa_handler != SIG_DFL)
	    {
	      ret = sigaction (i, &newsa, NULL);
	      /* POSIX says:
 		 It is unspecified whether an attempt to set the action for a
                 signal that cannot be caught or ignored to SIG_DFL is
                 ignored or causes an error to be returned with errno set to
                 [EINVAL].

		 Ignore errors if it's EINVAL since those are likely
		 signals we can't change.  */
	      if (ret != 0 && errno != EINVAL)
		_exit (2);
	    }
	}
      /* Restore the old signal mask that we inherited from the parent.  */
      pthread_sigmask (SIG_SETMASK, &old_signal_mask, NULL);

      /* At this point you carry out anything else you need to do before exec
         like changing directory etc.  Signals are enabled in the child and
         will do their default actions, and the parent's handlers do not run.
         The caller has ensured not to call set*id functions. The only remaining
         general restriction is not to corrupt the parent's state by calling
         complex functions (the safe functions should be documented by glibc
         but aren't). */

      /* ... */

      /* The last thing we do is execute the helper.  */
      ret = execve (filename, new_argv, new_envp);
      /* Always call _exit in the event of a failure with exec functions.  */
      _exit (3);
    }
  if (child == -1)
    {
      /* Restore the signal masks in the parent as quickly as possible to
         reduce signal handling latency.  */
      pthread_sigmask (SIG_SETMASK, &old_signal_mask, NULL);
      perror ("vfork");
      exit (EXIT_FAILURE);
    }
  else
    {
      /* In the parent. At this point the child has either succeeded at the
         exec or _exit function call. The parent, this thread, which would
         have been suspended is resumed. */

      /* Restore the signal masks in the parent as quickly as possible to
         reduce signal handling latency.  */
      pthread_sigmask (SIG_SETMASK, &old_signal_mask, NULL);

      /* Wait for the child to exit and then pass back the exit code. */
      waited = waitpid (child, &status, 0);

      if (waited == (pid_t) -1)
	{
	  perror ("wait");
	  exit (EXIT_FAILURE);
	}
      if (WIFEXITED(status)) 
        {
          printf("Helper: Exited, status=%d\n", WEXITSTATUS(status));
        }
      else if (WIFSIGNALED(status))
        {
          printf("Helper: Killed by signal %d\n", WTERMSIG(status));
        } 

      return NULL;
    }
}

int
main (void)
{
  int ret;
  pthread_t thread;

  /* The application creates a thread from which to run other processes.
     The thread will immediately attempt to execute the helper process.
     On Linux the vfork system call suspends only the calling thread, not
     the entire process. Therefore it is still useful to use vfork over
     fork for performance, particularly as the process gets larger and
     larger the cost of fork gets more expensive as page table (not
     memory, since it's all copy-on-write) size grows.  */
  ret = pthread_create (&thread, NULL, run_thread, NULL);
  if (ret != 0)
    {
      fprintf (stderr, "pthread_create: %s\n", strerror (ret));
      exit (EXIT_FAILURE);
    }

  /* Do some other work while the helper launches the application,
     waits for it, and sets the global status.  */

  /* ... */

  /* Lastly, wait for the helper thread to terminate.  */
  ret = pthread_join (thread, NULL);
  if (ret != 0)
    {
      fprintf (stderr, "pthread_join: %s\n", strerror (ret));
      exit (EXIT_FAILURE);
    }
  exit (EXIT_SUCCESS);
}
