/*
    Steam Controller XInput Relayer
    Copyright (C) 2017  PhaethonH

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
Designed to work around Steam Controller not showing up in Euro Truck Simulator 2 ("ETS2").

This program uses uinput (the same way Steam client itself does for Steam Controller) to create a new event device that does nothing but replicate the events from the Steam Controller generic XInput controller virtual device (USB Vendor:Product = 28de:11fc), but using a different vendor-id and product-id.

In this case, Vendor:Product = f055:11fc.
(0xf055 is the unofficial vendor-id for FOSS projects)
*/
/*
   Terminology clarification
   "XInput" is the Microsoft gamepad interface based on the Xbox 360 (2005) controller.
   "XInput" is also the name of the X.org subsystem (2009) handling input devices in its X Window System (c.1984) implementation -- no relation to Microsoft's XInput, Microsoft Xbox, nor Microsoft Windows.
*/

#include <assert.h>

#include <linux/input.h>
#include <linux/uinput.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>
#include <dirent.h>
#include <signal.h>
#include <wchar.h>

#include <argp.h>

#include <locale.h>
#include <libintl.h>
#define _(String) String
#define N_(String) String
#define bindtextdomain(Package,Locale)
#define textdomain(Package)


#define PACKAGE "screlay"

#define MODELNAME "XInput Relay (SteamController)"
#define MODELREV 1


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

__inline__
static
void die_on_negative (int wrapped_call)
{
  assert(wrapped_call >= 0);
  if (wrapped_call < 0)
    {
      perror(_("ERROR"));
      exit(EXIT_FAILURE);
    }
}


const char * DEFAULT_MODELNAME = MODELNAME;
const char * DEFAULT_UINPUT_PATH = "/dev/uinput";
const int DEFAULT_TARGET_VENDOR_ID = 0x28de;
const int DEFAULT_TARGET_PRODUCT_ID = 0x11fc;
const int MY_VENDOR_ID = 0xf055;
const int MY_PRODUCT_ID = 0x11fc;


/* State information for the relay. */
struct screlay_s {
    int halt;
    int verbose;

    int opt_scan;

    char modelname[255];    /* Human-readable device name */
    char * uinput_path;  /* path to uinput node. */
    int fd;          /* fd to talk to uinput. */
    char * srcpath;  /* Path of Steam Controller's XInput device. */
    int srcfd;       /* file descriptor after opening srcpath. */

    /* Search by vendor-id and product-id */
    int target_vendor;
    int target_product;

    /* Input types to report as existing. */
    int have_axis[ABS_CNT];
    int have_key[KEY_CNT];

    /* source device identification information. */
    struct input_id idinfo;

    /* UINPUT device descriptor. */
    struct uinput_user_dev uidev;
};

struct screlay_s _inst = { 0, }, *inst = &_inst;

void screlay_init ()
{
  memset(inst, 0, sizeof(struct screlay_s));
  inst->verbose = 1;
  inst->uinput_path = strdup(DEFAULT_UINPUT_PATH);
  inst->fd = -1;
  inst->srcfd = -1;
  inst->target_vendor = DEFAULT_TARGET_VENDOR_ID;
  inst->target_product = DEFAULT_TARGET_PRODUCT_ID;
}

void screlay_destroy ()
{
  if (inst->uinput_path)
    {
      free(inst->uinput_path);
    }
  if (inst->srcpath)
    {
      free(inst->srcpath);
    }
  if (inst->fd >= 0)
    {
      close(inst->fd);
    }
}

/* Open event device. */
int screlay_open (const char * sc_path)
{
  int fd = -1;
  int res;

  fd = open(sc_path, O_RDONLY);
  if (fd < 0)
    {
      return -1;
    }

  /* Get device name; might fail. */
  res = ioctl(fd, EVIOCGNAME(sizeof(inst->modelname)), inst->modelname);
  if (res < 0)
    {
      inst->modelname[0] = 0;
    }

  return fd;
}

/* Determine if (opened) event device has appropriate USB ID. */
int screlay_is_matched_usb_id (int fd)
{
  int res;

  res = ioctl(fd, EVIOCGID, &(inst->idinfo));
  if (res == 0)
    {
      if ((inst->idinfo.vendor == inst->target_vendor)
	  && (inst->idinfo.product == inst->target_product))
	{
	  return 1;
	}
    }

  return 0;
}

/* Find (first) Steam Controller XInput device: first /dev/input/event* device
 which has specific USB ID (default 28de:11fc). */
int screlay_scan ()
{
  const char * basedir = "/dev/input";
  char * scanpath = NULL;
  int scanpathsize = 4096;
  int res;
  DIR * dir;
  struct dirent entry;
  struct dirent * result;

  scanpath = malloc(scanpathsize);

  dir = opendir(basedir);
  while (dir != NULL)
    {
      res = readdir_r(dir, &entry, &result);
      // Assuming Linux kernel.
      if (res == 0)
	{
	  if (result == NULL)
	    {
	      /* end of directory. */
	      closedir(dir);
	      dir = NULL;
	    }
	  else
	    {
	      /* valid entry, check for SteamControllerXInput-ness */
	      int srcfd;
	      /* Require 'event' prefix. */
	      if (0 != strncmp(entry.d_name, "event", 5))
		continue;
	      snprintf(scanpath, scanpathsize, "%s/%s", basedir, entry.d_name);
	      srcfd = screlay_open(scanpath);
	      if (screlay_is_matched_usb_id(srcfd))
		{
		  inst->srcpath = strdup(scanpath);
		  inst->srcfd = srcfd;
		  closedir(dir);
		  dir = NULL;
		}
	    }
	}
      else
	{
	  closedir(dir);
	  dir = NULL;
	  perror(_("Scanning for event devices"));
	  exit(EXIT_FAILURE);
	}
    }

  free(scanpath);

  return 0;
}


/* Set of callbacks applied to a bitvector. */
#define SCRELAY_BV_CB(cbname) static int screlay_cb_##cbname (int idx)
SCRELAY_BV_CB(ioc_set_evbit)
{
  die_on_negative( ioctl(inst->fd, UI_SET_EVBIT, idx) );
}
SCRELAY_BV_CB(ioc_set_absbit)
{
  die_on_negative( ioctl(inst->fd, UI_SET_ABSBIT, idx) );
  inst->have_axis[idx] = 1;
}
SCRELAY_BV_CB(ioc_set_keybit)
{
  die_on_negative( ioctl(inst->fd, UI_SET_KEYBIT, idx) );
  inst->have_key[idx] = 1;
}


/* Walk a bitvector, calling 'cb' with argument of the bit index that is set. */
void screlay_walk_bitvectoridx (char bitvector[], int vecbytes, int (*cb)(int))
{
  int nbyte, nbit;

  for (nbyte = 0; nbyte < vecbytes; nbyte++)
    {
      for (nbit = 0; nbit < 8; nbit++)
	{
	  if (bitvector[nbyte] & (1 << nbit))
	    {
	      int idx = (nbyte * 8) + nbit;
	      cb(idx);
	    }
	}
    }
}

/* Mimick "plugging in" the virtual device. */
int screlay_connect ()
{
  int res;

  if (inst->srcfd < 0)
    {
      return -EBADF;
    }

  /* Open uinput node. */
  inst->fd = open(inst->uinput_path, O_WRONLY | O_NONBLOCK);
  if (inst->fd < 0)
    {
      perror(_("Unable to open uinput device"));
      exit(EXIT_FAILURE);
    }

  int codeidx;
  int syscode;

  int datasize = (KEY_CNT+1)/8;
  char * data = malloc(datasize);
  int nbyte, nbit;

  /* Register device abilities. */
  res = ioctl(inst->srcfd, EVIOCGBIT(0, datasize), data);
  if (res > 0)
    {
      screlay_walk_bitvectoridx(data, res, screlay_cb_ioc_set_evbit);
    }

  res = ioctl(inst->srcfd, EVIOCGBIT(EV_ABS, datasize), data);
  if (res > 0)
    {
      screlay_walk_bitvectoridx(data, res, screlay_cb_ioc_set_absbit);
    }

  res = ioctl(inst->srcfd, EVIOCGBIT(EV_KEY, datasize), data);
  if (res > 0)
    {
      screlay_walk_bitvectoridx(data, res, screlay_cb_ioc_set_keybit);
    }

  free(data);

  /* Prepare the UINPUT device descriptor. */
  memset(&(inst->uidev), 0, sizeof(inst->uidev));

  /* Fill in the fields. */
  snprintf(inst->uidev.name, UINPUT_MAX_NAME_SIZE, MODELNAME);
  inst->uidev.id.bustype = BUS_VIRTUAL;
  inst->uidev.id.vendor = MY_VENDOR_ID;
  inst->uidev.id.product = MY_PRODUCT_ID;
  inst->uidev.id.version = MODELREV;

  /* Copy absinfo from source. */
  struct input_absinfo absinfo;
  for (codeidx = 0; codeidx < ABS_CNT; codeidx++)
    {
      if (inst->have_axis[codeidx])
	{
	  ioctl(inst->srcfd, EVIOCGABS(codeidx), &absinfo);
	  inst->uidev.absmin[codeidx] = absinfo.minimum;
	  inst->uidev.absmax[codeidx] = absinfo.maximum;
	  inst->uidev.absfuzz[codeidx] = absinfo.fuzz;
	  inst->uidev.absflat[codeidx] = absinfo.flat;
	}
    }

  /* Write the device descriptor to the fd. */
  die_on_negative( write(inst->fd, &(inst->uidev), sizeof(inst->uidev)) );

  die_on_negative( ioctl(inst->fd, UI_DEV_CREATE) );

  /* Relay device now created. */

  return 0;
}

/* Mimick disconnecting ("unplugging") the relay device. */
int screlay_disconnect ()
{
  int ret;
  ret = ioctl(inst->fd, UI_DEV_DESTROY);
  return ret;
}

int screlay_test_hang ()
{
  int t = 0;
  while (1)
    {
      sleep(1);
      t++;
    }
}

void on_sigint (int signum)
{
  inst->halt = 1;
}

int screlay_mainloop ()
{
  int res;
  struct input_event ev;
  const int evsize = sizeof(struct input_event);

  struct sigaction act = {
      .sa_handler = on_sigint,
      .sa_mask = 0,
      .sa_flags = SA_NODEFER | SA_RESETHAND,
  };
  sigaction(SIGINT, &act, NULL);

  inst->halt = 0;
  while (! inst->halt)
    {
      res = read(inst->srcfd, &ev, evsize);
      if (res == evsize)
	{
	  write(inst->fd, &ev, evsize);
	}
      else if (res == 0)
	{
	  // file closed.
	  inst->halt = 1;
	}
      else if (res < 0)
	{
	  perror(_("Reading from source device file"));
	  inst->halt = 1;
	}
      else
	{
	  // partial read.
	  fprintf(stderr, _("ERROR: Partial read %d from source device file\n"), res);
	  inst->halt = 1;
	}
    }
}




const char *argp_program_version = N_("screlay");
const char *argp_program_bug_address = "<PhaethonH@gmail.com>";

static char screlay_doc[] = N_("SC XInput Relay");

/* A description of the arguments we accept. */
static char args_doc[] = N_("");

/* The options we understand. */
static struct argp_option options[] = {
      { "auto", 'a', 0, 0, N_("Auto-scan for relay source") },
      { "device", 'd', N_("PATH"), 0, N_("Explicit device path (no scan, no id check)") },
      { "usbid", 'u', N_("USB_ID"), 0, N_("Scan to match USB ID for relay source [28de:11fc]") },
      { "quiet", 'q', 0, 0, N_("Verbose output") },
      { 0 },
};

error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  long i;
  char * p;

  switch (key)
    {
    case 'a':
      inst->opt_scan = 1;
      break;
    case 'd':
      inst->srcpath = strdup(arg);
      break;
    case 'q':
      inst->verbose = 0;
      break;
    case 'u':
      i = strtol(arg, &p, 16);
      inst->target_vendor = i;
      i = strtol(p+1, NULL, 16);
      inst->target_product = i;
      inst->opt_scan = 1;
      break;
    }
  return 0;
}

struct argp argp = { options, parse_opt, args_doc, screlay_doc };


int init_i18n ()
{
  setlocale(LC_ALL, "");
  bindtextdomain(PACKAGE, NULL);
  textdomain(PACKAGE);
  return 0;
}


int main (int argc, char ** argv)
{
  int ret;

  init_i18n();

  screlay_init();

  argp_parse(&argp, argc, argv, 0, 0, inst);

//  inst->srcfd = screlay_open((inst->srcpath = strdup("/dev/input/event16")));
  if (inst->opt_scan)
    {
      screlay_scan();
    }
  else if (inst->srcpath)
    {
      inst->srcfd = screlay_open(inst->srcpath);
    }
  else
    {
      argp_help(&argp, stdout, ARGP_HELP_USAGE, argv[0]);
      exit(EXIT_FAILURE);
    }
  logmsg(1, _("Using relay source %s: [%04x:%04x] \"%s\"\n"), inst->srcpath, inst->idinfo.vendor, inst->idinfo.product, inst->modelname);
  if (inst->srcfd >= 0)
    {
      screlay_connect();
      screlay_mainloop();
    }

  screlay_disconnect();
  screlay_destroy();

  puts(_("Done."));

  return 0;
}

