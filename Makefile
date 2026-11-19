# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# nvidia-uvm-freebsd: the clean-room UVM driver.  Targets:
#   all        build sys/nvidia-uvm.ko (SYSDIR: the kernel source, default /usr/src/sys)
#   test       the userland unit tests and the amd64 cross-compile check
#   lint       mandoc -T lint on the manual page
#   clean

SYSDIR?=	/usr/src/sys

all:
	${MAKE} -C sys SYSDIR=${SYSDIR}

test:
	sh tests/run_unit_tests.sh
	sh tests/amd64_cross_check.sh ${SYSDIR}

lint:
	mandoc -T lint man/nvidia-uvm.4

clean:
	${MAKE} -C sys SYSDIR=${SYSDIR} clean

.PHONY: all test lint clean
