#!/bin/sh

# Example command line:
#   $ SPEAKER_TEST=.../alsa-utils/speaker-test/speaker-test EXTRA_RATES=1 ./test-rates.sh

set -e

SPEAKER_TEST=${SPEAKER_TEST:-speaker-test}
DEVICE=${DEVICE:-hw:1}

EXTRA_RATES=${EXTRA_RATES:-0}

SAMPLE_RATES="44100 48000 88200 96000 176400 192000"
if [ $EXTRA_RATES -eq 1 ];
then
  SAMPLE_RATES="$SAMPLE_RATES 352800 384000"
fi

for rate in $SAMPLE_RATES;
do
  "$SPEAKER_TEST" -l 1 -D "$DEVICE" -c 2 -F S32_LE -r "$rate"
  dmesg | tail -1
done
