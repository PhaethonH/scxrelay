#!/bin/bash
# Wrapper around scxrelay to automatically find and use (first) Steam Controller virtual xpad device while game is running.
# This script is in the public domain.

# Path to SteamController Xpad Relay.
SCXRELAY=$(PATH=.:"$PATH" which scxrelay || which scxrelay.x86)

# Paths to system utilities.
UDEVADM=udevadm
GREP=grep
KILL=kill

# Default paths.
UINPUT_PATH=/dev/uinput
EVENT_PREFIX=/dev/input/event

EVENT_PATH=
RELAYPID=0

for evdev in "$EVENT_PREFIX"*; do
  # Search for the two lines "P: [...]/virtual/[...]" and "E: ID_INPUT_JOYSTICK=1", which are characteristic of the Steam Controller virtual xpad device.
  if [ `$UDEVADM info -n "$evdev" | $GREP -c '^\(P: .*/virtual/\|E: ID_INPUT_JOYSTICK=1\)'` = 2 ]; then
    if [ x"$EVENT_PATH" = "x" ]; then
      EVENT_PATH="$evdev"  # store first match.
    fi
  fi
done

# Start up the relay in background.
if [ x"$EVENT_PATH" != "x" ]; then
  $SCXRELAY "$EVENT_PATH" "$UINPUT_PATH" &
  RELAYPID=$!
  # Short sleep for udev to settle.
  sleep 0.5
fi

# Run rest of command line (e.g. the actual game)
"$@"

if [ "$RELAYPID" -gt 0 ]; then
  # Terminate backgrounded relay.
  $KILL -INT $RELAYPID
fi
