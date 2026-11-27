#!/bin/sh
# Watch for ANY new USB device appearing on this board while the camera is
# plugged into a normal USB-C data cable.  This is the observer we never used:
# if the injected stage1 payload ever took over, the camera comes up as
# 18d1:4ee0 (fastboot) or a device whose product string says ZVE10-fastboot.
#
# Run this, then the operator swaps the 24-pin breakout for a plain USB-C cable
# between the camera and this board's USB-A port.

DUR=${1:-45}

echo "=== watching /sys/bus/usb for new devices for ${DUR}s ==="
echo "seen so far:"
for d in /sys/bus/usb/devices/*/idVendor; do
    dir=$(dirname "$d")
    v=$(cat "$d" 2>/dev/null)
    p=$(cat "$dir/idProduct" 2>/dev/null)
    n=$(cat "$dir/product" 2>/dev/null)
    [ -n "$v" ] && echo "  $v:$p  $n"
done

echo
echo "=== now plug the camera into a plain USB-C data cable on this board ==="

END=$(( $(date +%s) + DUR ))
LAST=""
while [ "$(date +%s)" -lt "$END" ]; do
    CUR=""
    for d in /sys/bus/usb/devices/*/idVendor; do
        dir=$(dirname "$d")
        v=$(cat "$d" 2>/dev/null)
        p=$(cat "$dir/idProduct" 2>/dev/null)
        [ -n "$v" ] && CUR="$CUR $v:$p"
    done
    if [ "$CUR" != "$LAST" ]; then
        T=$(cat /proc/uptime | cut -d' ' -f1)
        echo "[$T] devices:$CUR"
        LAST="$CUR"
    fi
    sleep 0.3
done

echo
echo "=== final list ==="
for d in /sys/bus/usb/devices/*/idVendor; do
    dir=$(dirname "$d")
    v=$(cat "$d" 2>/dev/null)
    p=$(cat "$dir/idProduct" 2>/dev/null)
    n=$(cat "$dir/product" 2>/dev/null)
    m=$(cat "$dir/manufacturer" 2>/dev/null)
    [ -n "$v" ] && echo "  $v:$p  [$m] $n"
done

echo
echo "=== dmesg for anything that looks like the camera or fastboot ==="
dmesg | grep -i -E "18d1|4ee0|zve10|fastboot|054c" | tail -20
