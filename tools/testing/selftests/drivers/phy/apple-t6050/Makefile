# SPDX-License-Identifier: GPL-2.0-only
# Standalone external test module: make -C <kernel> O=<build> M=$PWD modules
obj-m += apple-t6050-driver-test.o
apple-t6050-driver-test-y := driver-test.o
ccflags-y += -I$(srctree)/drivers/phy/apple
