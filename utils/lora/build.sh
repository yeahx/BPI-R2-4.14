#!/bin/bash
make CROSS_COMPILE=arm-linux-gnueabihf- ARCH=arm CFLAGS="-D__LINUX_ARM_ARCH__=7"
#make
