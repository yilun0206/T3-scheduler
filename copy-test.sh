#!/bin/sh

arm-linux-gnueabi-gcc -static test.c -o test
sudo mount -o loop,offset=$((63*512)) vexpress/vexpress-raring_developer_20130723-408.img /mnt
sudo cp test /mnt
sudo umount /mnt
