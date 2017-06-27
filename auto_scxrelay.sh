#!/bin/bash
# Wrapper to scxrelay to automatically find and use Steam Controller virtual xpad device.

# Path to SteamController Xpad Relay.
SCXRELAY=./scxrelay

# Default paths.
UINPUT_PATH=/dev/uinput
EVENT_PREFIX=/dev/input/event

# Paths to system utilities.
UDEVADM=udevadm
GREP=grep
WC=wc

for evdev in "$EVENT_PREFIX"*; do
  if $UDEVADM info -n "$evdev" | $GREP '^\(P: .*/virtual/\|E: ID_INPUT_JOYSTICK=1\)' | $GREP 2 &> /dev/null; then
    # match
    $SCXRELAY "$evdev" "$UINPUT_PATH"
  fi
done
