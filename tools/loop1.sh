#!/bin/sh
# Loopback self-test: P1_A7 (gpio39) and P1_B0 (gpio40) shorted by a dupont wire.
# The injector drives gpio39 only (--no-dp so it never touches gpio40); edgecap
# reads gpio40's pad straight out of the GPIO bank, so there is exactly one
# driver and one listener, and no resource contention.
#
# Two passes:
#   A) triggered on the 250 us preamble block -> preamble length, bit period,
#      and a full frame decode (start / 8 data / odd parity / stop).
#   B) no trigger, random start -> lands in the inter-record carrier most of the
#      time (~83% of a group is gap), which measures the 125 us carrier.
#
# Expected: 60 preamble blocks @250.0us, bit @20.833us, carrier @125.0us,
#           first frame byte 0x0A (SET_UNLOCK begins with length=10, LE).
pkill -f edgecap 2>/dev/null
pkill -f stage1_inject 2>/dev/null
sleep 1
echo 1 > /proc/sys/kernel/printk      # mute the vop2/drm console spam
rm -f /tmp/edge.log /tmp/edge2.log /tmp/inj8.log

/tmp/stage1_inject_v8 --payload /tmp/selfproof.bin --seconds 8 \
    --no-dp --dn-pushpull --settle-ms 50 --no-reset > /tmp/inj8.log 2>&1 &
INJ=$!
sleep 0.25

# pass A: start early enough to catch the whole first preamble
/tmp/edgecap --pin 40 --seconds 8 --max 3000 --window-us 20000 > /tmp/edge.log 2>&1

# pass B: carrier only
/tmp/edgecap --pin 40 --seconds 3 --no-trigger --window-us 6000 --max 200 \
    > /tmp/edge2.log 2>&1

kill $INJ 2>/dev/null
sleep 1

echo "=== injector ==="
head -14 /tmp/inj8.log
echo ""
echo "=== pass A: preamble + first frame ==="
cat /tmp/edge.log
echo ""
echo "=== pass B: carrier ==="
head -14 /tmp/edge2.log
echo ""
echo "=== end ==="
