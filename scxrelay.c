/* gcc -o scxrelay scxrelay.c */
/*
   Steam Controller Xpad Minimalist Relayer
   Copyright (C) 2017  PhaethonH <PhaethonH@gmail.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
/*
Purpose: relay xpad (gamepad) events from the Steam Controller's virtual xpad device to another virtual event device with a different vendor ID.

Background: as of 2017-06-24, Euro Truck Simulator 2 (ETS2) does not recognize
Steam Controller's virtual xpad device.  After experimenting with uinput and
virtual event devices, a pattern emerged where devices reporting Valve's
vendorID would not be detected by ETS2, but those reporting vendorID 0xf055
(among others) would.  Simply mirroring the reported events from Steam
Controller's xpad device through another virtual device, changing *only* the
VendorID, resulted in an xpad device usable in ETS2.


Usage (command-line shell):
$ scxrelay /dev/input/eventNN [/dev/uinput]

First argument is the Steam Controller's xpad device from which to copy.

Second argument is optional, an explicit path to the uinput device through
which the program creates a new virtual event device and repeats the xpad
events.  If not specified, defaults to "/dev/uinput".

Use Control-C to terminate.


Usage (no-shell, programmatic POSIX interface):
Open fd 3 for read-write on the Steam Controller xpad device.
Open fd 4 for read-write on the uinput device.
fd 0,1,2 are not significant, and may be closed.
Terminate with SIGINT.


Halt conditions:
Receive SIGINT.
Failure to read from xpad device (e.g. on Steam Controller disconnect).
Faiulre to write to uinput device.

Other notes:
This program pares down functionality to an absolute minimum.
I expect an external program ("front-end") to enhance user experience.
The assumed environment is SteamOS.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>

#define PACKAGE "scxrelay"
#define VERSION "0.01"

/* i18n preparations. */
#define _(String) String
#define N_(String) String

/** Constants. **/
const char *SCXRELAY_MODELNAME = "Xpad Relay (SteamController)";
const int SCXRELAY_MODELREV = 1;
const int SCXRELAY_VENDORID = 0xF055;	/* "FOSS", unofficial vendorID */
const int SCXRELAY_PRODUCTID = 0x11fc;	/* Steam Controller xpad. */
#ifndef PATH_MAX
#define PATH_MAX 4096		/* SteamOS */
#endif

/* Recovery from failure states. */
enum scxstate_e {
    SCXSTATE_INIT,    /* starting up; nothing in progress yet. */
    SCXSTATE_STEADY,  /* the steady state. */
    SCXSTATE_FAILED,  /* read failed; attempt recovery (re-open). */

    SCXSTATE_HALT,    /* terminate process. */
};


/** Logging **/
int logthreshold = 0;

int
vlogmsg (int loglevel, const char *fmt, va_list vp)
{
  int retval = 0;
  if (loglevel > logthreshold)
    {
      retval = vfprintf (stderr, fmt, vp);
      fflush (stderr);
    }
  return retval;
}

int
logmsg (int loglevel, const char *fmt, ...)
{
  va_list vp;
  int retval = 0;
  va_start (vp, fmt);
  retval = vlogmsg (loglevel, fmt, vp);
  va_end (vp);
  return retval;
}

/* Naïve handling of failed system calls: exit immediately with failure. */
static void
die_on_negative (int wrapped_call)
{
  if (wrapped_call < 0)
    {
      perror (_("ERROR"));
      exit (EXIT_FAILURE);
    }
}


/** Run-time state **/
struct scxrelay_s
{
  enum scxstate_e state;	/* Controls main loop behavior; HALT to stop. */
  int srcfd;			/* fd of Steam Controller virtual xpad device; -1 for none. */
  int uinputfd;			/* fd of uinput; -1 for none. */
  /* bit vectors */
#define NBV_EV (1 + EV_CNT/8)
#define NBV_ABS (1 + ABS_CNT/8)
#define NBV_KEY (1 + KEY_CNT/8)
  char have_ev[NBV_EV];		/* bit vector of event types supported by srcfd.  */
  char have_abs[NBV_ABS];	/* bit vector of axes supported by srcfd. */
  char have_key[NBV_KEY];	/* bit vector of keys/buttons, srcfd. */
  struct uinput_user_dev uidev;	/* New virtual device info, for uinput. */
  char event_path[PATH_MAX];	/* Path name used to open srcfd. */
  char uinput_path[PATH_MAX];	/* Path name used to open uinputfd. */
  int filter_sysbutton;
};

typedef struct scxrelay_s scxrelay_t;

scxrelay_t _inst = { 0, },		/* Global single instantiation of run-time state. */
 *inst = &_inst;		/* and pointer to instance. */


/** Events Relay **/

void
scxrelay_init ()
{
  memset (inst, 0, sizeof (*inst));
  inst->srcfd = -1;
  inst->uinputfd = -1;
  snprintf (inst->uinput_path, sizeof (inst->uinput_path), "/dev/uinput");
}

/* Tell uinput of supported input features (copied from source event device) */
static void
scxrelay_register_features_by_code ()
{
  int res;
  int nbyte, nbit, idx;

  /* Convoluted syntax to allow simple curly braces after FOREACH_SET_BIT()
     Abuses shortcut evaluation and side effect of assignment-as-expression.
   */
#define FOREACH_SET_BIT(idxvar, bv, bytecount) \
  for (nbyte = 0, idxvar = 0; nbyte < bytecount; nbyte++) \
  for (nbit = 0; nbit < 8; nbit++, idxvar++) \
  if ((bv)[nbyte] & (1 << nbit))

  /* Query source device for supported events (bitvector). */
  res = ioctl (inst->srcfd, EVIOCGBIT (0, NBV_EV), inst->have_ev);
  if (res > 0)
    {
      /* Traverse bit vector and replicate features. */
      FOREACH_SET_BIT (idx, inst->have_ev, res)
      {
	die_on_negative (ioctl (inst->uinputfd, UI_SET_EVBIT, idx));
      }
    }

  /* Query (bitvector) - axes */
  res = ioctl (inst->srcfd, EVIOCGBIT (EV_ABS, NBV_ABS), inst->have_abs);
  if (res > 0)
    {
      /* Traverse and replicate. */
      FOREACH_SET_BIT (idx, inst->have_abs, res)
      {
	die_on_negative (ioctl (inst->uinputfd, UI_SET_ABSBIT, idx));
      }
    }

  /* Query (bitvector) - buttons */
  res = ioctl (inst->srcfd, EVIOCGBIT (EV_KEY, NBV_KEY), inst->have_key);
  if (res > 0)
    {
      /* Traverse and replicate. */
      FOREACH_SET_BIT (idx, inst->have_key, res)
      {
	die_on_negative (ioctl (inst->uinputfd, UI_SET_KEYBIT, idx));
      }
    }
}

/* Mimick "plugging in" the virtual device.
   Returns 0 on success, -1 on failure (then see errno). */
int
scxrelay_connect ()
{
  int res;
  struct input_absinfo absinfo;
  int nbyte, nbit, idx;

  /* Open the source event device. */
  if (inst->srcfd < 0)
    {
      inst->srcfd = open (inst->event_path, O_RDWR);
    }
  if (inst->srcfd < 0)
    {
      /* Open read-write failed.  Try read-only (no haptic feedback). */
      inst->srcfd = open (inst->event_path, O_RDONLY);
    }
  if (inst->srcfd < 0)
    {
      /* Cannot open at all. */
      perror (_(inst->event_path));
      return -1;
    }

  /* Open the uinput device. */
  if (inst->uinputfd < 0)
    {
      inst->uinputfd = open (inst->uinput_path, O_RDWR);
    }
  if (inst->uinputfd < 0)
    {
      perror (_(inst->uinput_path));
      return -1;
    }

  /* Register input device features. */
  scxrelay_register_features_by_code ();

  /* Prepare the UINPUT device descriptor. */
  memset (&(inst->uidev), 0, sizeof (inst->uidev));
  snprintf (inst->uidev.name, UINPUT_MAX_NAME_SIZE, "%s", SCXRELAY_MODELNAME);
  inst->uidev.id.bustype = BUS_VIRTUAL;
  inst->uidev.id.vendor = SCXRELAY_VENDORID;
  inst->uidev.id.product = SCXRELAY_PRODUCTID;
  inst->uidev.id.version = SCXRELAY_MODELREV;
  /* Copy absinfo from source (also goes into uidev). */
  FOREACH_SET_BIT (idx, inst->have_abs, NBV_EV)
  {
    if (inst->have_abs[nbyte] & (1 << nbit))
      {
	idx = (nbyte * 8) + nbit;
	die_on_negative (ioctl (inst->srcfd, EVIOCGABS (idx), &absinfo));
	inst->uidev.absmin[idx] = absinfo.minimum;
	inst->uidev.absmax[idx] = absinfo.maximum;
	inst->uidev.absfuzz[idx] = absinfo.fuzz;
	inst->uidev.absflat[idx] = absinfo.flat;
      }
  }

  /* Write the device descriptor to the fd. */
  die_on_negative (write
		   (inst->uinputfd, &(inst->uidev), sizeof (inst->uidev)));

  /* Create ("connect") the relay device. */
  die_on_negative (ioctl (inst->uinputfd, UI_DEV_CREATE));

  /* Relay device now created. */

  return 0;
}

/* Mimick disconnecting ("unplugging") the relay device.
   Returns 0 on success, -1 on error (then see errno).  */
int
scxrelay_disconnect ()
{
  int ret;
  ret = ioctl (inst->uinputfd, UI_DEV_DESTROY);
  return ret;
}

/* Signal handler for SIGINT (Control-C), primary means of ending program. */
static void
on_sigint (int signum)
{
  inst->state = SCXSTATE_HALT;
}

/* Copy one instance of input_event from source device to destination device
   (the relay) */
void
scxrelay_copy_event ()
{
  int res;
  struct input_event ev;
  const int evsize = sizeof (struct input_event);

  res = read (inst->srcfd, &ev, evsize);
  if (res == evsize)
    {
      /* steady state: copy event to relay device. */
      if (inst->filter_sysbutton)
        {
	  /* system ("Home", "Guide", "Steam", ...) button ignored. */
	  if ((ev.type == EV_KEY) && (ev.code == 10))
	    return;
        }
      die_on_negative( write (inst->uinputfd, &ev, evsize));
    }
  else if (res == 0)
    {
      /* source closed/disappeared. */
      inst->state = SCXSTATE_HALT;
    }
  else if (res < 0)
    {
      if (errno != EINTR)
	{
	  /* stay silent for SIGINT. */
	  perror (_("Reading from source device file"));
	}
      inst->state = SCXSTATE_HALT;
    }
  else
    {
      /* partial read. */
      logmsg (1, _("Partial read %d from source device file.\n"), res);
      inst->state = SCXSTATE_HALT;
    }
}

/* Main loop, intended to be terminated with SIGINT (Control-C).
   Returns shell-sense status code (EXIT_SUCCESS, EXIT_FAILURE).
 */
int
scxrelay_mainloop ()
{
  int res;

  /* Trap SIGINT; allow interrupting syscall (poll(2)), to terminate program. */
  struct sigaction act;
  act.sa_handler = on_sigint;
  sigemptyset (&(act.sa_mask));
  act.sa_flags = SA_NODEFER | SA_RESETHAND;
  sigaction (SIGINT, &act, NULL);

  inst->state = SCXSTATE_STEADY;
  /* main loop */
  while (inst->state != SCXSTATE_HALT)
    {
      /* Build array of pollfd in program stack (use heap in future?). */
      switch (inst->state)
	{
	case SCXSTATE_INIT:
	  /* TODO: move initialization to here? */
	  break;
	case SCXSTATE_STEADY:
	    {
	      struct pollfd fds[] = {
		    { inst->srcfd, POLLIN, 0 },
	      };
	      struct pollfd *fdsiter = fds + 0;
	      int nfds = sizeof (fds) / sizeof (fds[0]);

	      res = poll (fds, nfds, 100);    /* SIGINT mostly happens here. */

	      if (res > 0)
		{
		  for (fdsiter = fds + 0; fdsiter < fds + nfds; fdsiter++)
		    {
		      if (fdsiter->fd == inst->srcfd)
			{
			  if (fdsiter->revents & POLLIN)
			    {
			      scxrelay_copy_event ();
			    }
			  if (fdsiter->revents & POLLERR)
			    {
			      /* error in polling; presumably disconnect. */
			      printf("Error in fd %d\n", fdsiter->fd);
			      close (inst->srcfd);
			      inst->state = SCXSTATE_FAILED;
			    }
			}
		    }
		}
	    }
	  break;
	case SCXSTATE_FAILED:
	  /* keep trying to re-open event_path. */
	  if (inst->event_path[0])
	    {
	      inst->srcfd = open (inst->event_path, O_RDWR);
	      if (inst->srcfd < 0)
		{
		  usleep(100000);  /* retry after 0.1s */
		}
	      else
		{
		  printf("Recovered as fd %d\n", inst->srcfd);
		  inst->state = SCXSTATE_STEADY;
		}
	    }
	  else
	    {
	      usleep(200000);  /* no recovery, but process remains alive for
				  sake of wrapper script. */
	    }
	  break;
	default:
	  break;
	}
    }

  /* loop cleanup */

  return 0;
}

/* Runs after resolving event_device and uinput_device (options).
   Return shell-sense status code (EXIT_SUCCESS, EXIT_FAILURE).  */
int
scxrelay_main ()
{
  if (scxrelay_connect () == 0)
    {
      scxrelay_mainloop ();
      scxrelay_disconnect ();
      fputs ("", stdout);
    }
  else
    {
      return -1;
    }

  return 0;
}


/** Command-line interface **/

/* Show usage information. */
void
usage (int argc, char **argv)
{
  fprintf (stdout, "Usage: %s source_event_device [UINPUT_PATH]\n\
\n\
Minimalist Steam Controller xpad relay device.\n\
May omit 'source_event_device' if fd 3 is opened for read-write on event device.\n\
If fd 4 is opened, it is treated as read-write fd for uinput device.\n\
Terminate the program by sending signal SIGINT (press Control-C).\n\
", argv[0]);
}

static int
is_fd_open (int probe_fd)
{
  int res = fcntl (probe_fd, F_GETFD);
  return (res == 0);
}

int
main (int argc, char **argv)
{
  int errcode = EXIT_SUCCESS;
  int res;

  scxrelay_init ();

  if (argc < 2)
    {
      /* No command-line arguments.  Assume pass by file descriptors. */
      if (is_fd_open (3))
	{
	  inst->srcfd = 3;
	  strcpy (inst->event_path, "-");
	}

      if (is_fd_open (4))
	{
	  inst->uinputfd = 4;
	  strcpy (inst->uinput_path, "-");
	}

      if (inst->srcfd == -1)
	{
	  /* No event device specified, and insufficient arguments. */
	  usage (argc, argv);
	  return EXIT_FAILURE;
	}
    }

  if (argc > 1)
    {
      /* event device path name. */
      snprintf (inst->event_path, sizeof (inst->event_path), "%s", argv[1]);
    }
  if (argc > 2)
    {
      /* uinput path name. */
      snprintf (inst->uinput_path, sizeof (inst->uinput_path), "%s", argv[2]);
    }

  res = scxrelay_main ();

  return (res == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
