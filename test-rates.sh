#!/bin/sh

set -e

SPEAKER_TEST=${SPEAKER_TEST:-speaker-test}
DEVICE=${DEVICE:-hw:1}

for rate in 44100 48000 88200 96000 176400 192000 352800 384000;
do
  "$SPEAKER_TEST" -l 1 -D "$DEVICE" -c 2 -F S32_LE -r "$rate"
  dmesg | tail -1
done
