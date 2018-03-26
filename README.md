# T3-scheduler

1. Prepare

	Please place your vexpress-xxx.img into vexpress/ since that image is
	too large, we do not upload it. And please change the copy-test.sh and
	qemu-start.sh to replace vexpress-xxx.img with correct path.

2. Build

	cd linux-linaro-stable-3.10.62-2014.12
	./make.sh
	to compile kernel.

	./copy-test.sh

	to compile test program and copy it to the first partition of your vexpress-xxx.img.

	./qemu-start.sh
	to start qemu-system-arm.

	Within qemu, your may need mount your /dev/mmblk0p1 to find your copied
	test program.


	Please check kernel/sched/mycfs.c if #define DEBUG_MYCFS is commented out,
	otherwise you may be overwhelmed by printk messages

	Please read report.pdf for more details.
