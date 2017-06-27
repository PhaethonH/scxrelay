#!/bin/bash
# Wrapper around scxrelay to automatically find and use Steam Controller virtual xpad device.
# This script is public domain.

# Path to SteamController Xpad Relay.
SCXRELAY=./scxrelay

# Default paths.
UINPUT_PATH=/dev/uinput
EVENT_PREFIX=/dev/input/event

# Paths to system utilities.
UDEVADM=udevadm
GREP=grep

for evdev in "$EVENT_PREFIX"*; do
  # Search for the two lines "P: [...]/virtual/[...]" and "E: ID_INPUT_JOYSTICK=1", which are characteristic of the Steam Controller virtual xpad device.
  if [ `$UDEVADM info -n "$evdev" | $GREP -c '^\(P: .*/virtual/\|E: ID_INPUT_JOYSTICK=1\)'` = 2 ]; then
    # match
    $SCXRELAY "$evdev" "$UINPUT_PATH"
  fi
done

