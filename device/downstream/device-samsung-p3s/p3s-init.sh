#!/bin/busybox sh
# Samsung Galaxy S21 Ultra (p3s) pre-init, started via rdinit= in the DTB bootargs. Loads the
# vendor modules in Android order (pmOS stage 2 never loads modules-initfs), then brings up
# USB networking (172.16.42.1) early, then runs pmOS /init.
B=/bin/busybox
$B mount -t proc proc /proc
$B mount -t sysfs sysfs /sys
$B mount -t devtmpfs devtmpfs /dev
$B mkdir -p /dev/pts && $B mount -t devpts devpts /dev/pts
$B mount -t configfs configfs /sys/kernel/config
log() { echo "p3s-init: $*" > /dev/kmsg; echo "$($B cut -d' ' -f1 /proc/uptime) $*" >> /p3s-init.log; }
log "start"
n=0; nfail=0
for m in $($B grep -v '^#' /lib/modules/initramfs.load); do
	n=$((n+1))
	/sbin/modprobe "$m" 2>/tmp/p3s-err || { nfail=$((nfail+1)); log "failed $m: $($B cat /tmp/p3s-err)"; }
done
i=0
while [ $i -lt 100 ] && ! [ -e /sys/class/udc/10e00000.usb ]; do $B sleep 0.1; i=$((i+1)); done
log "$n modules, $nfail failed, udc: $($B ls /sys/class/udc | $B tr '\n' ' ')"
G=/sys/kernel/config/usb_gadget/g1
$B mkdir -p $G/strings/0x409 $G/functions/rndis.usb0 $G/configs/c.1/strings/0x409
echo 0x18D1 > $G/idVendor; echo 0xD001 > $G/idProduct
echo postmarketOS > $G/strings/0x409/manufacturer
echo p3s-debug > $G/strings/0x409/product
echo 1221 > $G/strings/0x409/serialnumber
echo rndis > $G/configs/c.1/strings/0x409/configuration
$B ln -s $G/functions/rndis.usb0 $G/configs/c.1/
echo 10e00000.usb > $G/UDC && log "gadget bound"
$B sleep 1
$B ifconfig usb0 172.16.42.1 netmask 255.255.255.0 up && log "usb0 up"
log "handing over to /init"
exec /init "$@"
