/* gcc -o minrelay minrelay.c */
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
Given an event input device, relays/mirrors events under a different USB ID.

Designed to work around Steam Controller joystick not showing up in
Euro Truck Simulator 2 ("ETS2"), by creating a new virtual input device
and copying all events from the Steam Controller xpad (virtual) device.
For some reason, this works if the USB Vendor ID is not Valve's.
Run before starting ETS2, so the virtual device is visible to the game.

Source code strongly assumes a SteamOS environment to minimize coding.
That is, there is no concern for portability.

Auxiliary/external programs are expected to assist the user (e.g. GUI).
*/

#include <linux/input.h>
#include <linux/uinput.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <signal.h>

#define PACKAGE "scminrelay"

/* i18n preparations. */
#define _(String) String
#define N_(String) String

/** Constants. **/
const char * modelname = "Xpad MiniRelay (SteamController)";
const int modelrev = 1;
const int my_vendor = 0xf055;  /* "FOSS" */
const int my_product = 0x11fc; /* Steam Controller xpad. */
#ifndef PATH_MAX
#define PATH_MAX 4096  /* SteamOS assumption. */
#endif


/** Logging **/
int logthreshold = 0;

int vlogmsg (int loglevel, const char * fmt, va_list vp)
{
  int retval = 0;
  if (loglevel > logthreshold)
    {
      retval = vfprintf(stderr, fmt, vp);
      fflush(stderr);
    }
  return retval;
}

int logmsg (int loglevel, const char * fmt, ...)
{
  va_list vp;
  int retval = 0;
  va_start(vp, fmt);
  retval = vlogmsg(loglevel, fmt, vp);
  va_end(vp);
  return retval;
}

/* Naive handling of failed system calls: exit immediately with failure. */
static
void die_on_negative (int wrapped_call)
{
  if (wrapped_call < 0)
    {
      perror(_("ERROR"));
      exit(EXIT_FAILURE);
    }
}


/** Run-time state **/
int halt = 0;
int srcfd = -1;
int uinputfd = -1;
#define NBV_EV (1 + EV_CNT/8)
#define NBV_ABS (1 + ABS_CNT/8)
#define NBV_KEY (1 + KEY_CNT/8)
char have_ev[NBV_EV] = { 0, };
char have_abs[NBV_ABS] = { 0, };
char have_key[NBV_KEY] = { 0, };
struct input_id idinfo;
struct uinput_user_dev uidev;
char event_path[PATH_MAX] = "";
char uinput_path[PATH_MAX] = "/dev/uinput";  /* SteamOS assumption. */


/** Events Relay **/

/* Tell uinput of supported input features (copied from source event device) */
static
void scxrelay_register_features_by_code ()
{
  int res;
  int nbyte, nbit, idx;

  /* Convoluted syntax to allow simple curly braces after FOREACH_SET_BIT()
     Abuses shortcut evaluation and side effect of assignment-as-expression.
   */
#define FOREACH_SET_BIT(idxvar, bv, bytecount) \
  for (nbyte = 0; nbyte < bytecount; nbyte++) \
    for (nbit = 0; nbit < 8; nbit++) \
      if ( ((bv)[nbyte] & (1 << nbit)) && ((idxvar=nbyte*8+nbit)) )

  /* Query source device for supported events (bitvector). */
  res = ioctl(srcfd, EVIOCGBIT(0, NBV_EV), have_ev);
  if (res > 0)
    {
      /* Traverse bit vector and replicate features. */
      FOREACH_SET_BIT(idx, have_ev, res)
	{
	  die_on_negative( ioctl(uinputfd, UI_SET_EVBIT, idx) );
	}
    }

  /* Query (bitvector) - axes */
  res = ioctl(srcfd, EVIOCGBIT(EV_ABS, NBV_ABS), have_abs);
  if (res > 0)
    {
      /* Traverse and replicate. */
      FOREACH_SET_BIT(idx, have_abs, res)
	{
	  die_on_negative( ioctl(uinputfd, UI_SET_ABSBIT, idx) );
	}
    }

  /* Query (bitvector) - buttons */
  res = ioctl(srcfd, EVIOCGBIT(EV_KEY, NBV_KEY), have_key);
  if (res > 0)
    {
      /* Traverse and replicate. */
      FOREACH_SET_BIT(idx, have_key, res)
	{
	  die_on_negative( ioctl(uinputfd, UI_SET_KEYBIT, idx) );
	}
    }
}

/* Mimick "plugging in" the virtual device. */
int scxrelay_connect ()
{
  int res;
  struct input_absinfo absinfo;
  int nbyte, nbit, idx;

  /* Open the source event device. */
  srcfd = open(event_path, O_RDONLY);
  if (srcfd < 0)
    {
      perror(_(event_path));
      return -1;
    }

  /* Open the uinput device. */
  uinputfd = open(uinput_path, O_WRONLY | O_NONBLOCK);
  if (uinputfd < 0)
    {
      perror(_(uinput_path));
      return -1;
    }

  printf("relay: %s\n", event_path);

  /* Register features. */
  scxrelay_register_features_by_code();

  /* Prepare the UINPUT device descriptor. */
  memset(&(uidev), 0, sizeof(uidev));
  snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "%s", modelname);
  uidev.id.bustype = BUS_VIRTUAL;
  uidev.id.vendor = my_vendor;
  uidev.id.product = my_product;
  uidev.id.version = modelrev;
  /* Copy absinfo from source (also goes into uidev). */
  FOREACH_SET_BIT(idx, have_abs, NBV_EV)
    {
      if (have_abs[nbyte] & (1 << nbit))
	{
	  idx = (nbyte*8) + nbit;
	  die_on_negative( ioctl(srcfd, EVIOCGABS(idx), &absinfo) );
	  uidev.absmin[idx] = absinfo.minimum;
	  uidev.absmax[idx] = absinfo.maximum;
	  uidev.absfuzz[idx] = absinfo.fuzz;
	  uidev.absflat[idx] = absinfo.flat;
	}
    }

  /* Write the device descriptor to the fd. */
  die_on_negative( write(uinputfd, &(uidev), sizeof(uidev)) );

  /* Create ("connect") the relay device. */
  die_on_negative( ioctl(uinputfd, UI_DEV_CREATE) );

  /* Relay device now created. */

  return 0;
}

/* Mimick disconnecting ("unplugging") the relay device. */
int scxrelay_disconnect ()
{
  int ret;
  ret = ioctl(uinputfd, UI_DEV_DESTROY);
  return ret;
}

/* Signal handler for SIGINT (Control-C), primary means of ending program. */
static
void on_sigint (int signum)
{
  halt = 1;
}

/* Main loop, intended to be terminated with SIGINT (Control-C). */
int scxrelay_mainloop ()
{
  int res;
  int nfds;
  long rfds, wfds, efds;
  struct input_event ev;
  const int evsize = sizeof(struct input_event);

  /* Trap SIGINT; allow interrupting syscall (read(2)) to terminate program. */
  struct sigaction act;
  act.sa_handler = on_sigint;
  sigemptyset(&(act.sa_mask));
  act.sa_flags = SA_NODEFER | SA_RESETHAND;
  sigaction(SIGINT, &act, NULL);

  halt = 0;
  /* main loop */
  while (! halt)
    {
      /* Blocking read from source device; SIGINT tends to be here. */
      res = read(srcfd, &ev, evsize);
      if (res == evsize)
	{
	  /* steady state: copy event to relay device. */
	  write(uinputfd, &ev, evsize);
	}
      else if (res == 0)
	{
	  /* source closed/disappeared. */
	  halt = 1;
	}
      else if (res < 0)
	{
	  if (errno != EINTR)
	    {
	      /* stay silent for SIGINT. */
	      perror(_("Reading from source device file"));
	    }
	  halt = 1;
	}
      else
	{
	  /* partial read. */
	  logmsg(1, _("Partial read %d from source device file.\n"), res);
	  halt = 1;
	}
    }

  /* cleanup */

  return 0;
}


/** Command-line interface **/

void usage (int argc, char ** argv)
{
  fprintf(stdout, "Usage: %s source_event_device [UINPUT_PATH]\n\
\n\
Minimalist Steam Controller xpad relay device.\n\
", argv[0]);
}

int main (int argc, char ** argv)
{
  int errcode = EXIT_SUCCESS;

  if (argc < 2)
    {
      usage(argc, argv);
      return EXIT_FAILURE;
    }
  if (argc > 1)
    {
      /* event device. */
      snprintf(event_path, sizeof(event_path), "%s", argv[1]);
    }
  if (argc > 2)
    {
      /* uinput path. */
      snprintf(uinput_path, sizeof(uinput_path), "%s", argv[2]);
    }


  if (scxrelay_connect() == 0)
    {
      scxrelay_mainloop();
      scxrelay_disconnect();
      fputs("", stdout);
    }
  else
    {
      errcode = EXIT_FAILURE;
    }


  return errcode;
}

