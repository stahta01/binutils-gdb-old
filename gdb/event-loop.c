/* Event loop machinery for GDB, the GNU debugger.
   Copyright 1999 Free Software Foundation, Inc.
   Written by Elena Zannoni <ezannoni@cygnus.com> of Cygnus Solutions.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA. */

#include "defs.h"
#include "top.h"
#include "event-loop.h"
#include "event-top.h"
#include "inferior.h"   /* For fetch_inferior_event. */
#ifdef HAVE_POLL
#include <poll.h>
#else
#include <sys/types.h>
#endif
#include <errno.h>
#include <setjmp.h>
#include <sys/time.h>

/* Type of the mask arguments to select. */

#ifndef NO_FD_SET
#define SELECT_MASK fd_set
#else
#ifndef _AIX
typedef long fd_mask;
#endif
#if defined(_IBMR2)
#define SELECT_MASK void
#else
#define SELECT_MASK int
#endif
#endif

/* Define "NBBY" (number of bits per byte) if it's not already defined. */

#ifndef NBBY
#define NBBY 8
#endif


/* Define the number of fd_masks in an fd_set */

#ifndef FD_SETSIZE
#ifdef OPEN_MAX
#define FD_SETSIZE OPEN_MAX
#else
#define FD_SETSIZE 256
#endif
#endif
#if !defined(howmany)
#define howmany(x, y) (((x)+((y)-1))/(y))
#endif
#ifndef NFDBITS
#define NFDBITS NBBY*sizeof(fd_mask)
#endif
#define MASK_SIZE howmany(FD_SETSIZE, NFDBITS)


typedef struct gdb_event gdb_event;
typedef void (event_handler_func) (int);

/* Event for the GDB event system.  Events are queued by calling
   async_queue_event and serviced later on by gdb_do_one_event. An
   event can be, for instance, a file descriptor becoming ready to be
   read. Servicing an event simply means that the procedure PROC will
   be called.  We have 2 queues, one for file handlers that we listen
   to in the event loop, and one for the file handlers+events that are
   ready. The procedure PROC associated with each event is always the
   same (handle_file_event).  Its duty is to invoke the handler
   associated with the file descriptor whose state change generated
   the event, plus doing other cleanups adn such. */

struct gdb_event
  {
    event_handler_func *proc;	/* Procedure to call to service this event. */
    int fd;			/* File descriptor that is ready. */
    struct gdb_event *next_event;	/* Next in list of events or NULL. */
  };

/* Information about each file descriptor we register with the event
   loop. */

typedef struct file_handler
  {
    int fd;			/* File descriptor. */
    int mask;			/* Events we want to monitor: POLLIN, etc. */
    int ready_mask;		/* Events that have been seen since
				   the last time. */
    handler_func *proc;	        /* Procedure to call when fd is ready. */
    gdb_client_data client_data;	/* Argument to pass to proc. */
    int error;                          /* Was an error detected on this fd? */
    struct file_handler *next_file;	/* Next registered file descriptor. */
  }
file_handler;

/* PROC is a function to be invoked when the READY flag is set. This
   happens when there has been a signal and the corresponding signal
   handler has 'triggered' this async_signal_handler for
   execution. The actual work to be done in response to a signal will
   be carried out by PROC at a later time, within process_event. This
   provides a deferred execution of signal handlers.
   Async_init_signals takes care of setting up such an
   asyn_signal_handler for each interesting signal. */
typedef struct async_signal_handler
  {
    int ready;			/* If ready, call this handler from the main event loop, 
				   using invoke_async_handler. */
    struct async_signal_handler *next_handler;	/* Ptr to next handler */
    sig_handler_func *proc;	                /* Function to call to do the work */
    gdb_client_data client_data;	/* Argument to async_handler_func */
  }
async_signal_handler;


/* Event queue:  
   - the first event in the queue is the head of the queue. 
   It will be the next to be serviced.
   - the last event in the queue 

   Events can be inserted at the front of the queue or at the end of
   the queue.  Events will be extracted from the queue for processing
   starting from the head.  Therefore, events inserted at the head of
   the queue will be processed in a last in first out fashion, while
   those inserted at the tail of the queue will be processed in a first
   in first out manner.  All the fields are NULL if the queue is
   empty. */

static struct
  {
    gdb_event *first_event;	/* First pending event */
    gdb_event *last_event;	/* Last pending event */
  }
event_queue;

/* Gdb_notifier is just a list of file descriptors gdb is interested in.
   These are the input file descriptor, and the target file
   descriptor. We have two flavors of the notifier, one for platforms
   that have the POLL function, the other for those that don't, and
   only support SELECT. Each of the elements in the gdb_notifier list is
   basically a description of what kind of events gdb is interested
   in, for each fd. */

/* As of 1999-04-30 only the input file descriptor is registered with the
   event loop. */

#ifdef HAVE_POLL
/* Poll based implementation of the notifier. */

static struct
  {
    /* Ptr to head of file handler list. */
    file_handler *first_file_handler;

    /* Ptr to array of pollfd structures. */
    struct pollfd *poll_fds;

    /* Number of file descriptors to monitor. */
    int num_fds;

    /* Timeout in milliseconds for calls to poll(). */
    int timeout;

    /* Flag to tell whether the timeout value shuld be used. */
    int timeout_valid;
  }
gdb_notifier;

#else /* ! HAVE_POLL */

/* Select based implementation of the notifier. */

static struct
  {
    /* Ptr to head of file handler list. */
    file_handler *first_file_handler;

    /* Masks to be used in the next call to select.
       Bits are set in response to calls to create_file_handler. */
    fd_mask check_masks[3 * MASK_SIZE];

    /* What file descriptors were found ready by select. */
    fd_mask ready_masks[3 * MASK_SIZE];

    /* Number of valid bits (highest fd value + 1). */
    int num_fds;

    /* Time structure for calls to select(). */
    struct timeval timeout;

    /* Flag to tell whether the timeout struct should be used. */
    int timeout_valid;
   }
gdb_notifier;

#endif /* HAVE_POLL */

/* Structure associated with a timer. PROC will be executed at the
   first occasion after WHEN. */
struct gdb_timer
  {
    struct timeval when;
    int timer_id;
    struct gdb_timer *next;
    timer_handler_func *proc;         /* Function to call to do the work */
    gdb_client_data client_data;      /* Argument to async_handler_func */
  }
gdb_timer;

/* List of currently active timers. It is sorted in order of
   increasing timers.*/
static struct
  {
    /* Pointer to first in timer list. */
    struct gdb_timer *first_timer;

    /* Length of timer list. */
    int num_timers;
  }
timer_list;

/* All the async_signal_handlers gdb is interested in are kept onto
   this list. */
static struct
  {
    /* Pointer to first in handler list. */
    async_signal_handler *first_handler;

    /* Pointer to last in handler list. */
    async_signal_handler *last_handler;
  }
sighandler_list;

/* Is any of the handlers ready?  Check this variable using
   check_async_ready. This is used by process_event, to determine
   whether or not to invoke the invoke_async_signal_handler
   function. */
static int async_handler_ready = 0;

static void create_file_handler (int fd, int mask, handler_func *proc, gdb_client_data client_data);
static void invoke_async_signal_handler (void);
static void handle_file_event (int event_file_desc);
static int gdb_wait_for_event (void);
static int gdb_do_one_event (void);
static int check_async_ready (void);
static void async_queue_event (gdb_event *event_ptr, queue_position position);
static gdb_event * create_file_event (int fd);
static int process_event (void);
static void handle_timer_event (int dummy);
static void poll_timers (void);


/* Insert an event object into the gdb event queue at 
   the specified position.
   POSITION can be head or tail, with values TAIL, HEAD.
   EVENT_PTR points to the event to be inserted into the queue.
   The caller must allocate memory for the event. It is freed
   after the event has ben handled.
   Events in the queue will be processed head to tail, therefore,
   events inserted at the head of the queue will be processed
   as last in first out. Event appended at the tail of the queue
   will be processed first in first out. */
static void
async_queue_event (gdb_event *event_ptr, queue_position position)
{
  if (position == TAIL)
    {
      /* The event will become the new last_event. */

      event_ptr->next_event = NULL;
      if (event_queue.first_event == NULL)
	event_queue.first_event = event_ptr;
      else
	event_queue.last_event->next_event = event_ptr;
      event_queue.last_event = event_ptr;
    }
  else if (position == HEAD)
    {
      /* The event becomes the new first_event. */

      event_ptr->next_event = event_queue.first_event;
      if (event_queue.first_event == NULL)
	event_queue.last_event = event_ptr;
      event_queue.first_event = event_ptr;
    }
}

/* Create a file event, to be enqueued in the event queue for
   processing. The procedure associated to this event is always
   handle_file_event, which will in turn invoke the one that was
   associated to FD when it was registered with the event loop. */
static gdb_event *
create_file_event (int fd)
{
  gdb_event *file_event_ptr;

  file_event_ptr = (gdb_event *) xmalloc (sizeof (gdb_event));
  file_event_ptr->proc = handle_file_event;
  file_event_ptr->fd = fd;
  return (file_event_ptr);
}

/* Process one event.
   The event can be the next one to be serviced in the event queue,
   or an asynchronous event handler can be invoked in response to
   the reception of a signal.
   If an event was processed (either way), 1 is returned otherwise
   0 is returned.   
   Scan the queue from head to tail, processing therefore the high
   priority events first, by invoking the associated event handler
   procedure. */
static int
process_event (void)
{
  gdb_event *event_ptr, *prev_ptr;
  event_handler_func *proc;
  int fd;

  /* First let's see if there are any asynchronous event handlers that
     are ready. These would be the result of invoking any of the
     signal handlers. */

  if (check_async_ready ())
    {
      invoke_async_signal_handler ();
      return 1;
    }

  /* Look in the event queue to find an event that is ready
     to be processed. */

  for (event_ptr = event_queue.first_event; event_ptr != NULL;
       event_ptr = event_ptr->next_event)
    {
      /* Call the handler for the event. */

      proc = event_ptr->proc;
      fd = event_ptr->fd;

      /* Let's get rid of the event from the event queue.  We need to
         do this now because while processing the event, the proc
         function could end up calling 'error' and therefore jump out
         to the caller of this function, gdb_do_one_event. In that
         case, we would have on the event queue an event wich has been
         processed, but not deleted. */

      if (event_queue.first_event == event_ptr)
	{
	  event_queue.first_event = event_ptr->next_event;
	  if (event_ptr->next_event == NULL)
	    event_queue.last_event = NULL;
	}
      else
	{
	  prev_ptr = event_queue.first_event;
	  while (prev_ptr->next_event != event_ptr)
	    prev_ptr = prev_ptr->next_event;

	  prev_ptr->next_event = event_ptr->next_event;
	  if (event_ptr->next_event == NULL)
	    event_queue.last_event = prev_ptr;
	}
      free ((char *) event_ptr);

      /* Now call the procedure associted with the event. */
      (*proc) (fd);
      return 1;
    }

  /* this is the case if there are no event on the event queue. */
  return 0;
}

/* Process one high level event.  If nothing is ready at this time,
   wait for something to happen (via gdb_wait_for_event), then process
   it.  Returns 1 if something was done otherwise returns 0 (this can
   happen if there are no event sources to wait for). */
static int
gdb_do_one_event (void)
{
  int result = 0;

  while (1)
    {
      if (!SET_TOP_LEVEL ())
	{
	  /* Any events already waiting in the queue? */
	  if (process_event ())
	    {
	      result = 1;
	      break;
	    }

	  /* Are any timers that are ready? If so, put an event on the queue.*/
	  poll_timers ();

	  /* Wait for a new event.  If gdb_wait_for_event returns -1,
	     we should get out because this means that there are no
	     event sources left. This will make the event loop stop,
	     and the application exit. */

	  result = gdb_wait_for_event ();
	  if (result < 0)
	    {
	      result = 0;
	      break;
	    }

	  /* Handle any new events occurred while waiting. */
	  if (process_event ())
	    {
	      result = 1;
	      break;
	    }

	  /* If gdb_wait_for_event has returned 1, it means that one
	     event has been handled. We break out of the loop. */
	  if (result)
	    break;
	}			/* end of if !set_top_level */
      else
	{
	  /* FIXME: this should really be a call to a hook that is
	     interface specific, because interfaces can display the
	     prompt in their own way. */
	  display_gdb_prompt (0);
	  /* Maybe better to set a flag to be checked somewhere as to
	     whether display the prompt or not. */
	}
    }
  return result;
}


/* Start up the event loop. This is the entry point to the event loop
   from the command loop. */
void
start_event_loop (void)
{
  /* Loop until there is something to do. This is the entry point to
     the event loop engine. gdb_do_one_event will process one event
     for each invocation.  It always returns 1, unless there are no
     more event sources registered. In this case it returns 0.  */
  while (gdb_do_one_event () != 0)
    ;

  /* We are done with the event loop. There are no more event sources
     to listen to.  So we exit GDB. */
  return;
}


/* Wrapper function for create_file_handler, so that the caller
   doesn't have to know implementation details about the use of poll
   vs. select. */
void
add_file_handler (int fd, handler_func *proc, gdb_client_data client_data)
{
#ifdef HAVE_POLL
  create_file_handler (fd, POLLIN, proc, client_data);
#else
  create_file_handler (fd, GDB_READABLE | GDB_EXCEPTION, proc, client_data);
#endif
}

/* Add a file handler/descriptor to the list of descriptors we are
   interested in.  
   FD is the file descriptor for the file/stream to be listened to.  
   For the poll case, MASK is a combination (OR) of
   POLLIN, POLLRDNORM, POLLRDBAND, POLLPRI, POLLOUT, POLLWRNORM,
   POLLWRBAND: these are the events we are interested in. If any of them 
   occurs, proc should be called.
   For the select case, MASK is a combination of READABLE, WRITABLE, EXCEPTION.
   PROC is the procedure that will be called when an event occurs for
   FD.  CLIENT_DATA is the argument to pass to PROC. */
static void
create_file_handler (int fd, int mask, handler_func *proc, gdb_client_data client_data)
{
  file_handler *file_ptr;

#ifndef HAVE_POLL
  int index, bit;
#endif

  /* Do we already have a file handler for this file? (We may be
     changing its associated procedure). */
  for (file_ptr = gdb_notifier.first_file_handler; file_ptr != NULL;
       file_ptr = file_ptr->next_file)
    {
      if (file_ptr->fd == fd)
	break;
    }

  /* It is a new file descriptor. Add it to the list. Otherwise, just
     change the data associated with it.*/
  if (file_ptr == NULL)
    {
      file_ptr = (file_handler *) xmalloc (sizeof (file_handler));
      file_ptr->fd = fd;
      file_ptr->ready_mask = 0;
      file_ptr->next_file = gdb_notifier.first_file_handler;
      gdb_notifier.first_file_handler = file_ptr;
#ifdef HAVE_POLL
      gdb_notifier.num_fds++;
#endif
    }
  file_ptr->proc = proc;
  file_ptr->client_data = client_data;
  file_ptr->mask = mask;

#ifdef HAVE_POLL

  if (gdb_notifier.poll_fds)
    gdb_notifier.poll_fds =
      (struct pollfd *) realloc (gdb_notifier.poll_fds,
			   (gdb_notifier.num_fds) * sizeof (struct pollfd));
  else
    gdb_notifier.poll_fds =
      (struct pollfd *) xmalloc (sizeof (struct pollfd));
  (gdb_notifier.poll_fds + gdb_notifier.num_fds - 1)->fd = fd;
  (gdb_notifier.poll_fds + gdb_notifier.num_fds - 1)->events = mask;
  (gdb_notifier.poll_fds + gdb_notifier.num_fds - 1)->revents = 0;

#else /* ! HAVE_POLL */

  index = fd / (NBBY * sizeof (fd_mask));
  bit = 1 << (fd % (NBBY * sizeof (fd_mask)));

  if (mask & GDB_READABLE)
    gdb_notifier.check_masks[index] |= bit;
  else
    gdb_notifier.check_masks[index] &= ~bit;

  if (mask & GDB_WRITABLE)
    (gdb_notifier.check_masks + MASK_SIZE)[index] |= bit;
  else
    (gdb_notifier.check_masks + MASK_SIZE)[index] &= ~bit;

  if (mask & GDB_EXCEPTION)
    (gdb_notifier.check_masks + 2 * (MASK_SIZE))[index] |= bit;
  else
    (gdb_notifier.check_masks + 2 * (MASK_SIZE))[index] &= ~bit;

  if (gdb_notifier.num_fds <= fd)
    gdb_notifier.num_fds = fd + 1;

#endif /* HAVE_POLL */
}

/* Remove the file descriptor FD from the list of monitored fd's: 
   i.e. we don't care anymore about events on the FD. */
void
delete_file_handler (int fd)
{
  file_handler *file_ptr, *prev_ptr = NULL;
  int i, j;
  struct pollfd *new_poll_fds;
#ifndef HAVE_POLL
  int index, bit;
  unsigned long flags;
#endif

  /* Find the entry for the given file. */

  for (file_ptr = gdb_notifier.first_file_handler; file_ptr != NULL;
       file_ptr = file_ptr->next_file)
    {
      if (file_ptr->fd == fd)
	break;
    }

  if (file_ptr == NULL)
    return;

#ifdef HAVE_POLL
  /* Create a new poll_fds array by copying every fd's information but the
     one we want to get rid of. */

  new_poll_fds =
    (struct pollfd *) xmalloc ((gdb_notifier.num_fds - 1) * sizeof (struct pollfd));

  for (i = 0, j = 0; i < gdb_notifier.num_fds; i++)
    {
      if ((gdb_notifier.poll_fds + i)->fd != fd)
	{
	  (new_poll_fds + j)->fd = (gdb_notifier.poll_fds + i)->fd;
	  (new_poll_fds + j)->events = (gdb_notifier.poll_fds + i)->events;
	  (new_poll_fds + j)->revents = (gdb_notifier.poll_fds + i)->revents;
	  j++;
	}
    }
  free (gdb_notifier.poll_fds);
  gdb_notifier.poll_fds = new_poll_fds;
  gdb_notifier.num_fds--;

#else /* ! HAVE_POLL */

  index = fd / (NBBY * sizeof (fd_mask));
  bit = 1 << (fd % (NBBY * sizeof (fd_mask)));

  if (file_ptr->mask & GDB_READABLE)
    gdb_notifier.check_masks[index] &= ~bit;
  if (file_ptr->mask & GDB_WRITABLE)
    (gdb_notifier.check_masks + MASK_SIZE)[index] &= ~bit;
  if (file_ptr->mask & GDB_EXCEPTION)
    (gdb_notifier.check_masks + 2 * (MASK_SIZE))[index] &= ~bit;

  /* Find current max fd. */

  if ((fd + 1) == gdb_notifier.num_fds)
    {
      for (gdb_notifier.num_fds = 0; index >= 0; index--)
	{
	  flags = gdb_notifier.check_masks[index]
	    | (gdb_notifier.check_masks + MASK_SIZE)[index]
	    | (gdb_notifier.check_masks + 2 * (MASK_SIZE))[index];
	  if (flags)
	    {
	      for (i = (NBBY * sizeof (fd_mask)); i > 0; i--)
		{
		  if (flags & (((unsigned long) 1) << (i - 1)))
		    break;
		}
	      gdb_notifier.num_fds = index * (NBBY * sizeof (fd_mask)) + i;
	      break;
	    }
	}
    }
#endif /* HAVE_POLL */

  /* Deactivate the file descriptor, by clearing its mask, 
     so that it will not fire again. */

  file_ptr->mask = 0;

  /* Get rid of the file handler in the file handler list. */
  if (file_ptr == gdb_notifier.first_file_handler)
    gdb_notifier.first_file_handler = file_ptr->next_file;
  else
    {
      for (prev_ptr = gdb_notifier.first_file_handler;
	   prev_ptr->next_file != file_ptr;
	   prev_ptr = prev_ptr->next_file)
	;
      prev_ptr->next_file = file_ptr->next_file;
    }
  free ((char *) file_ptr);
}

/* Handle the given event by calling the procedure associated to the
   corresponding file handler.  Called by process_event indirectly,
   through event_ptr->proc.  EVENT_FILE_DESC is file descriptor of the
   event in the front of the event queue. */
static void
handle_file_event (int event_file_desc)
{
  file_handler *file_ptr;
  int mask;
#ifdef HAVE_POLL
  int error_mask;
  int error_mask_returned;
#endif

  /* Search the file handler list to find one that matches the fd in
     the event. */
  for (file_ptr = gdb_notifier.first_file_handler; file_ptr != NULL;
       file_ptr = file_ptr->next_file)
    {
      if (file_ptr->fd == event_file_desc)
	{
	  /* With poll, the ready_mask could have any of three events
	     set to 1: POLLHUP, POLLERR, POLLNVAL. These events cannot
	     be used in the requested event mask (events), but they
	     can be returned in the return mask (revents). We need to
	     check for those event too, and add them to the mask which
	     will be passed to the handler. */

	  /* See if the desired events (mask) match the received
	     events (ready_mask). */

#ifdef HAVE_POLL
	  error_mask = POLLHUP | POLLERR | POLLNVAL;
	  mask = (file_ptr->ready_mask & file_ptr->mask) |
	    (file_ptr->ready_mask & error_mask);
	  error_mask_returned = mask & error_mask;

	  if (error_mask_returned != 0)
	    {
	      /* Work in progress. We may need to tell somebody what
                 kind of error we had. */
	      /*if (error_mask_returned & POLLHUP)
		printf_unfiltered ("Hangup detected on fd %d\n", file_ptr->fd);
	      if (error_mask_returned & POLLERR)
		printf_unfiltered ("Error detected on fd %d\n", file_ptr->fd);
	      if (error_mask_returned & POLLNVAL)
		printf_unfiltered ("Invalid fd %d\n", file_ptr->fd);*/
	      file_ptr->error = 1;
           }
	  else 
	    file_ptr->error = 0;
#else /* ! HAVE_POLL */
	  if (file_ptr->ready_mask & GDB_EXCEPTION)
	    {
	      printf_unfiltered ("Exception condition detected on fd %d\n", file_ptr->fd);
	      file_ptr->error = 1;
	    }
	  else
	    file_ptr->error = 0;
	  mask = file_ptr->ready_mask & file_ptr->mask;
#endif /* HAVE_POLL */

	  /* Clear the received events for next time around. */
	  file_ptr->ready_mask = 0;

	  /* If there was a match, then call the handler. */
	  if (mask != 0)
	    (*file_ptr->proc) (file_ptr->error, file_ptr->fd, file_ptr->client_data);
	  break;
	}
    }
}

/* Called by gdb_do_one_event to wait for new events on the 
   monitored file descriptors. Queue file events as they are 
   detected by the poll. 
   If there are no events, this function will block in the 
   call to poll.
   Return -1 if there are no files descriptors to monitor, 
   otherwise return 0. */
static int
gdb_wait_for_event (void)
{
  file_handler *file_ptr;
  gdb_event *file_event_ptr;
  int num_found = 0;
  int i;

#ifndef HAVE_POLL
  int mask, bit, index;
#endif

  /* Make sure all output is done before getting another event. */
  gdb_flush (gdb_stdout);
  gdb_flush (gdb_stderr);

  if (gdb_notifier.num_fds == 0)
    return -1;

#ifdef HAVE_POLL
  num_found =
    poll (gdb_notifier.poll_fds, 
	  (unsigned long) gdb_notifier.num_fds, 
	  gdb_notifier.timeout_valid ? gdb_notifier.timeout : -1);

  /* Don't print anything if we get out of poll because of a
     signal.*/
  if (num_found == -1 && errno != EINTR)
    perror_with_name ("Poll");

#else /* ! HAVE_POLL */
  memcpy (gdb_notifier.ready_masks,
	  gdb_notifier.check_masks,
	  3 * MASK_SIZE * sizeof (fd_mask));
  num_found = select (gdb_notifier.num_fds,
		      (SELECT_MASK *) & gdb_notifier.ready_masks[0],
		      (SELECT_MASK *) & gdb_notifier.ready_masks[MASK_SIZE],
		      (SELECT_MASK *) & gdb_notifier.ready_masks[2 * MASK_SIZE],
		      gdb_notifier.timeout_valid ? gdb_notifier.timeout : NULL);

  /* Clear the masks after an error from select. */
  if (num_found == -1)
    {
      memset (gdb_notifier.ready_masks,
	      0, 3 * MASK_SIZE * sizeof (fd_mask));
      /* Dont print anything is we got a signal, let gdb handle it. */
      if (errno != EINTR)
	perror_with_name ("Select");
    }
#endif /* HAVE_POLL */

  /* Enqueue all detected file events. */

#ifdef HAVE_POLL

  for (i = 0; (i < gdb_notifier.num_fds) && (num_found > 0); i++)
    {
      if ((gdb_notifier.poll_fds + i)->revents)
	num_found--;
      else
	continue;

      for (file_ptr = gdb_notifier.first_file_handler;
	   file_ptr != NULL;
	   file_ptr = file_ptr->next_file)
	{
	  if (file_ptr->fd == (gdb_notifier.poll_fds + i)->fd)
	    break;
	}

      if (file_ptr)
	{
	  /* Enqueue an event only if this is still a new event for
	     this fd. */
	  if (file_ptr->ready_mask == 0)
	    {
	      file_event_ptr = create_file_event (file_ptr->fd);
	      async_queue_event (file_event_ptr, TAIL);
	    }
	}

      file_ptr->ready_mask = (gdb_notifier.poll_fds + i)->revents;
    }

#else /* ! HAVE_POLL */
  for (file_ptr = gdb_notifier.first_file_handler;
       (file_ptr != NULL) && (num_found > 0);
       file_ptr = file_ptr->next_file)
    {
      index = file_ptr->fd / (NBBY * sizeof (fd_mask));
      bit = 1 << (file_ptr->fd % (NBBY * sizeof (fd_mask)));
      mask = 0;

      if (gdb_notifier.ready_masks[index] & bit)
	mask |= GDB_READABLE;
      if ((gdb_notifier.ready_masks + MASK_SIZE)[index] & bit)
	mask |= GDB_WRITABLE;
      if ((gdb_notifier.ready_masks + 2 * (MASK_SIZE))[index] & bit)
	mask |= GDB_EXCEPTION;

      if (!mask)
	continue;
      else
	num_found--;

      /* Enqueue an event only if this is still a new event for
         this fd. */

      if (file_ptr->ready_mask == 0)
	{
	  file_event_ptr = create_file_event (file_ptr->fd);
	  async_queue_event (file_event_ptr, TAIL);
	}
      file_ptr->ready_mask = mask;
    }
#endif /* HAVE_POLL */

  return 0;
}


/* Create an asynchronous handler, allocating memory for it. 
   Return a pointer to the newly created handler.
   This pointer will be used to invoke the handler by 
   invoke_async_signal_handler.
   PROC is the function to call with CLIENT_DATA argument 
   whenever the handler is invoked. */
async_signal_handler *
create_async_signal_handler (sig_handler_func *proc, gdb_client_data client_data)
{
  async_signal_handler *async_handler_ptr;

  async_handler_ptr =
    (async_signal_handler *) xmalloc (sizeof (async_signal_handler));
  async_handler_ptr->ready = 0;
  async_handler_ptr->next_handler = NULL;
  async_handler_ptr->proc = proc;
  async_handler_ptr->client_data = client_data;
  if (sighandler_list.first_handler == NULL)
    sighandler_list.first_handler = async_handler_ptr;
  else
    sighandler_list.last_handler->next_handler = async_handler_ptr;
  sighandler_list.last_handler = async_handler_ptr;
  return async_handler_ptr;
}

/* Mark the handler (ASYNC_HANDLER_PTR) as ready. This information will
   be used when the handlers are invoked, after we have waited for
   some event.  The caller of this function is the interrupt handler
   associated with a signal. */
void
mark_async_signal_handler (async_signal_handler *async_handler_ptr)
{
  ((async_signal_handler *) async_handler_ptr)->ready = 1;
  async_handler_ready = 1;
}

/* Call all the handlers that are ready. */
static void
invoke_async_signal_handler (void)
{
  async_signal_handler *async_handler_ptr;

  if (async_handler_ready == 0)
    return;
  async_handler_ready = 0;

  /* Invoke ready handlers. */

  while (1)
    {
      for (async_handler_ptr = sighandler_list.first_handler;
	   async_handler_ptr != NULL;
	   async_handler_ptr = async_handler_ptr->next_handler)
	{
	  if (async_handler_ptr->ready)
	    break;
	}
      if (async_handler_ptr == NULL)
	break;
      async_handler_ptr->ready = 0;
      (*async_handler_ptr->proc) (async_handler_ptr->client_data);
    }

  return;
}

/* Delete an asynchronous handler (ASYNC_HANDLER_PTR). 
   Free the space allocated for it.  */
void
delete_async_signal_handler (async_signal_handler **async_handler_ptr)
{
  async_signal_handler *prev_ptr;

  if (sighandler_list.first_handler == (*async_handler_ptr))
    {
      sighandler_list.first_handler = (*async_handler_ptr)->next_handler;
      if (sighandler_list.first_handler == NULL)
	sighandler_list.last_handler = NULL;
    }
  else
    {
      prev_ptr = sighandler_list.first_handler;
      while (prev_ptr->next_handler != (*async_handler_ptr) && prev_ptr)
	prev_ptr = prev_ptr->next_handler;
      prev_ptr->next_handler = (*async_handler_ptr)->next_handler;
      if (sighandler_list.last_handler == (*async_handler_ptr))
	sighandler_list.last_handler = prev_ptr;
    }
  free ((char *) (*async_handler_ptr));
  (*async_handler_ptr) = NULL;
}

/* Is it necessary to call invoke_async_signal_handler? */
static int
check_async_ready (void)
{
  return async_handler_ready;
}

/* FIXME: where does this function belong? */
/* General function to handle events in the inferior. So far it just
   takes care of detecting errors reported by select() or poll(),
   otherwise it assumes that all is OK, and goes on reading data from
   the fd. This however may not always be what we want to do. */
void
inferior_event_handler (int error, gdb_client_data client_data, int fd)
{
  if (error == 1)
    {
      printf_unfiltered ("error detected on fd %d\n", fd);
      delete_file_handler (fd);
      discard_all_continuations ();
    }
  else
    fetch_inferior_event (client_data);
}

/* Create a timer that will expire in MILLISECONDS from now. When the
   timer is ready, PROC will be executed. At creation, the timer is
   aded to the timers queue.  This queue is kept sorted in order of
   increasing timers. Return a handle to the timer struct.*/
int
create_timer (int milliseconds, timer_handler_func *proc, gdb_client_data client_data)
{
  struct gdb_timer *timer_ptr, *timer_index, *prev_timer;
  struct timeval time_now, delta;

  /* compute seconds */
  delta.tv_sec = milliseconds / 1000;
  /* compute microseconds */
  delta.tv_usec = (milliseconds % 1000) * 1000; 
  
  gettimeofday (&time_now, NULL);

  timer_ptr = (struct gdb_timer *) xmalloc (sizeof (gdb_timer));
  timer_ptr->when.tv_sec = time_now.tv_sec + delta.tv_sec;
  timer_ptr->when.tv_usec = time_now.tv_usec + delta.tv_usec;
  /* carry? */
  if (timer_ptr->when.tv_usec >= 1000000 )
    {
      timer_ptr->when.tv_sec += 1;
      timer_ptr->when.tv_usec -= 1000000;
    }
  timer_ptr->proc = proc;
  timer_ptr->client_data = client_data;
  timer_list.num_timers ++;
  timer_ptr->timer_id = timer_list.num_timers;

  /* Now add the timer to the timer queue, making sure it is sorted in
     increasing order of expiration. */

  for (timer_index = timer_list.first_timer; 
       timer_index != NULL; 
       timer_index = timer_index->next)
    {
      /* If the seconds field is greater or if it is the same, but the
         microsecond field is greater. */
      if ((timer_index->when.tv_sec > timer_ptr->when.tv_sec) || 
	  ((timer_index->when.tv_sec == timer_ptr->when.tv_sec)
	   && (timer_index->when.tv_usec > timer_ptr->when.tv_usec)))
	break;
    }
  
  if (timer_index == timer_list.first_timer)
    {
      timer_ptr->next = timer_list.first_timer;
      timer_list.first_timer = timer_ptr;

    }
  else
    {
      for (prev_timer = timer_list.first_timer; 
	   prev_timer->next != timer_index; 
	   prev_timer = prev_timer->next)
	;
      
      prev_timer->next = timer_ptr;
      timer_ptr->next = timer_index;
    }

  gdb_notifier.timeout_valid = 0;
  return timer_ptr->timer_id;
}

/* There is a chance that the creator of the timer wants to get rid of
   it before it expires. */
void
delete_timer (int id)
{
  struct gdb_timer *timer_ptr, *prev_timer = NULL;

  /* Find the entry for the given timer. */

  for (timer_ptr = timer_list.first_timer; timer_ptr != NULL;
       timer_ptr = timer_ptr->next)
    {
      if (timer_ptr->timer_id == id)
	break;
    }

  if (timer_ptr == NULL)
    return;
  /* Get rid of the timer in the timer list. */
  if (timer_ptr == timer_list.first_timer)
    timer_list.first_timer = timer_ptr->next;
  else
    {
      for (prev_timer = timer_list.first_timer;
	   prev_timer->next != timer_ptr;
	   prev_timer = prev_timer->next)
	;
      prev_timer->next = timer_ptr->next;
    }
  free ((char *) timer_ptr);

  gdb_notifier.timeout_valid = 0;
}

/* When a timer event is put on the event queue, it will be handled by
   this function.  Just call the assiciated procedure and delete the
   timer event from the event queue. Repeat this for each timer that
   has expired.*/
static void
handle_timer_event (int dummy)
{
  struct timeval time_now;
  struct gdb_timer *timer_ptr, *saved_timer;
 
  gettimeofday (&time_now, NULL);
  timer_ptr = timer_list.first_timer;

  while (timer_ptr != NULL)
    {
      if ((timer_ptr->when.tv_sec > time_now.tv_sec) || 
	  ((timer_ptr->when.tv_sec == time_now.tv_sec) && 
	   (timer_ptr->when.tv_usec > time_now.tv_usec)))
	break;

      /* Get rid of the timer from the beginning of the list. */
      timer_list.first_timer = timer_ptr->next;
      saved_timer = timer_ptr;
      timer_ptr = timer_ptr->next;
      /* Call the procedure associated with that timer. */
      (*saved_timer->proc) (timer_ptr->client_data);
      free (saved_timer);
    }

  gdb_notifier.timeout_valid = 0;
}
 
/* Check whether any timers in the timers queue are ready. If at least
   one timer is ready, stick an event onto the event queue.  Even in
   case more than one timer is ready, one event is enough, because the
   handle_timer_event() will go through the timers list and call the
   procedures associated with all that have expired. Update the
   timeout for the select() or poll() as well.*/
static void
poll_timers (void)
{
  struct timeval time_now, delta;
  gdb_event *event_ptr;
 
  if (timer_list.num_timers)
    {
      gettimeofday (&time_now, NULL);
      delta.tv_sec = timer_list.first_timer->when.tv_sec - time_now.tv_sec;
      delta.tv_usec = timer_list.first_timer->when.tv_usec - time_now.tv_usec;
      /* borrow? */
      if (delta.tv_usec < 0)
	{
	  delta.tv_sec -= 1;
	  delta.tv_usec += 1000000;
	}
 
      /* Oops it expired already. Tell select / poll to return
	 immediately. */
      if (delta.tv_sec < 0)
	{
	  delta.tv_sec = 0;
	  delta.tv_usec = 0;
	}

      if (delta.tv_sec == 0 && delta.tv_usec == 0)
	{
	  event_ptr = (gdb_event *) xmalloc (sizeof (gdb_event));
	  event_ptr->proc = handle_timer_event;
	  event_ptr->fd = timer_list.first_timer->timer_id;
	  async_queue_event (event_ptr, TAIL);
	}

      /* Now we need to update the timeout for select/ poll, because we
	 don't want to sit there while this timer is expiring. */
#ifdef HAVE_POLL
      gdb_notifier.timeout = delta.tv_sec * 1000; 
#else
      gdb_notifier.timeout.sec = delta.tv_sec;
      gdb_notifier.timeout.usec = delta.tv_usec;
#endif
      gdb_notifier.timeout_valid = 1;
    }
  else 
    gdb_notifier.timeout_valid = 0;
}
