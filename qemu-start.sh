#!/bin/sh

qemu-system-arm -kernel linux-linaro-stable-3.10.62-2014.12/arch/arm/boot/zImage -M vexpress-a9 -cpu cortex-a9 \
-m 1024 -initrd vexpress/initrd \
-append 'root=/dev/mmcblk0p2 rw mem=1024M raid=noautodetect console=ttyAMA0,38400n8 rootwait vmalloc=256MB devtmpfs.mount=0' \
-sd vexpress/vexpress-raring_developer_20130723-408.img  \
-serial stdio \
-display none
