# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# nvidia-uvm-freebsd: the clean-room UVM driver (sys/nvidia-uvm.ko).  One Makefile for GNU make and bmake (skill
# project-makefile-contract): recipe lines start with a tab, no conditionals, no GNU functions.
#
# This repository is a kernel module with its own unit tests: purpose library (help, purpose, preflight, build,
# test).  A bare make prints help; make build (or make all) compiles.  Nothing here installs onto a system; the module is staged and loaded by the test boot kits.

SYSDIR?=	/usr/src/sys
REPO_PURPOSE = library
JSON = 0
PREFLIGHT_TOOLS = cc make mandoc

help:
	@printf '%s\n' \
		"purpose: $(REPO_PURPOSE)" \
		"Targets (same names on every OS):" \
		"  make purpose    print REPO_PURPOSE (for machines)" \
		"  make help       this message (default)" \
		"  make preflight  report missing tools (JSON=1 for --json)" \
		"  make build      build sys/nvidia-uvm.ko (SYSDIR=$(SYSDIR): the kernel source); make all is the same" \
		"  make test       the userland unit tests and the amd64 cross-compile check" \
		"  make lint       mandoc -T lint on the manual page" \
		"  make clean" \
		"" \
		"Switches:" \
		"  SYSDIR=DIR   the kernel source tree (default $(SYSDIR))" \
		"  JSON=1       preflight writes the --json contract" \
		"" \
		"Run make from the directory that holds this Makefile."

.PHONY: help purpose preflight build all test lint clean

purpose:
	@printf '%s\n' "$(REPO_PURPOSE)"

preflight:
	@if [ "$(JSON)" = 1 ]; then \
		sh preflight.sh --json --purpose "$(REPO_PURPOSE)" $(PREFLIGHT_TOOLS); \
	else \
		sh preflight.sh --purpose "$(REPO_PURPOSE)" $(PREFLIGHT_TOOLS); \
	fi

# no $(MAKE) in the recipe: make -n would run such a line, and the contract lint dry-runs build in an empty directory.
# The sub-make is the shell variable MAKE when the caller exported it, else make (FreeBSD make: sys/ is a bsd.kmod.mk Makefile).
build:
	cd sys && $${MAKE:-make} SYSDIR="$(SYSDIR)"

all: build

test:
	sh tests/run_unit_tests.sh
	sh tests/amd64_cross_check.sh $(SYSDIR)

lint:
	mandoc -T lint man/nvidia-uvm.4

clean:
	cd sys && $${MAKE:-make} SYSDIR="$(SYSDIR)" clean
