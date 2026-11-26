# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# nvidia-uvm-freebsd: the clean-room UVM driver (sys/nvidia-uvm.ko).  One Makefile for GNU make and bmake (skill
# project-makefile-contract): recipe lines start with a tab, no conditionals, no GNU functions.
#
# A bare make prints help; make build (or make all) compiles.  Install and deinstall use the manifest-keeping
# shell engine in scripts/make.sh, so a normal user can stage a DESTDIR or elevate only the install step.

SYSDIR?=	/usr/src/sys
REPO_PURPOSE = application
JSON = 0
YES = 0
ASSUME_YES = 0
PREFIX ?= /usr/local
DESTDIR ?=
MK_KO =
PREFLIGHT_TOOLS = cc make mandoc
# The path to NVIDIA's MIT uvm_ioctl.h.  It is only needed to regenerate or
# to check the request table (linux/uvm_linux_request_table.h); the table is
# committed, so a build works without it.
NVIDIA_UVM_IOCTL_H ?=

help:
	@printf '%s\n' \
		"purpose: $(REPO_PURPOSE)" \
		"Targets (same names on every OS):" \
		"  make purpose    print REPO_PURPOSE (for machines)" \
		"  make help       this message (default)" \
		"  make preflight  report missing tools (JSON=1 for --json)" \
		"  make install-prereqs  check cc, make and mandoc (aliases: deps, install-deps)" \
		"  make configure  nothing to configure for this module" \
		"  make build      build sys/nvidia-uvm.ko and linux/nvidia_uvm_linux.ko (SYSDIR=$(SYSDIR): the kernel source); make all is the same" \
		"  make test       the userland unit tests and the amd64 cross-compile check" \
		"  make lint       mandoc -T lint on the manual pages" \
		"  make install    install both modules and the man pages (YES=1 skips the question)" \
		"  make deinstall  remove exactly what install wrote (alias: uninstall)" \
		"  make regen-ioctl-table  rewrite the request table from NVIDIA's uvm_ioctl.h (NVIDIA_UVM_IOCTL_H=...)" \
		"  make clean" \
		"" \
		"Switches:" \
		"  SYSDIR=DIR    the kernel source tree (default $(SYSDIR))" \
		"  YES=1         do not ask before install or deinstall" \
		"  ASSUME_YES=1  the same confirmation switch" \
		"  PREFIX=DIR    man page prefix (default $(PREFIX))" \
		"  DESTDIR=DIR   stage as the calling user instead of using root" \
		"  JSON=1        preflight writes the --json contract" \
		"  NVIDIA_UVM_IOCTL_H=FILE  NVIDIA's uvm_ioctl.h, for the request table" \
		"" \
		"Run make from the directory that holds this Makefile."

.PHONY: help purpose preflight install-prereqs deps install-deps configure build all test lint install deinstall uninstall regen-ioctl-table clean

purpose:
	@printf '%s\n' "$(REPO_PURPOSE)"

preflight:
	@if [ "$(JSON)" = 1 ]; then \
		sh preflight.sh --json --purpose "$(REPO_PURPOSE)" $(PREFLIGHT_TOOLS); \
	else \
		sh preflight.sh --purpose "$(REPO_PURPOSE)" $(PREFLIGHT_TOOLS); \
	fi

install-prereqs:
	sh scripts/make.sh install-prereqs

deps: install-prereqs

install-deps: install-prereqs

configure:
	sh scripts/make.sh configure

# no $(MAKE) in the recipe: make -n would run such a line, and the contract lint dry-runs build in an empty directory.
# The sub-make is the shell variable MAKE when the caller exported it, else make (FreeBSD make: sys/ and linux/ are bsd.kmod.mk Makefiles).
# The request table is generated from NVIDIA's uvm_ioctl.h; the committed table is checked against it when the
# header is given, and used as it is when it is not (a build needs no NVIDIA source).
build:
	sh tools/gen-uvm-request-table.sh --check "$(NVIDIA_UVM_IOCTL_H)"
	cd sys && $${MAKE:-make} SYSDIR="$(SYSDIR)"
	cd linux && $${MAKE:-make} SYSDIR="$(SYSDIR)"

all: build

# the module tests read the built modules, so the build comes first
test: build
	sh tests/run_unit_tests.sh
	sh tests/amd64_cross_check.sh $(SYSDIR)

lint:
	cd man && mandoc -T lint nvidia-uvm.4 nvidia-uvm-linux.4

# rewrite the one request table from NVIDIA's uvm_ioctl.h
regen-ioctl-table:
	sh tools/gen-uvm-request-table.sh --write "$(NVIDIA_UVM_IOCTL_H)"

install:
	MK_YES="$(YES)" MK_ASSUME_YES="$(ASSUME_YES)" MK_PREFIX="$(PREFIX)" MK_DESTDIR="$(DESTDIR)" MK_SYSDIR="$(SYSDIR)" MK_KO="$(MK_KO)" sh scripts/make.sh install

deinstall:
	MK_YES="$(YES)" MK_ASSUME_YES="$(ASSUME_YES)" MK_PREFIX="$(PREFIX)" MK_DESTDIR="$(DESTDIR)" MK_SYSDIR="$(SYSDIR)" MK_KO="$(MK_KO)" sh scripts/make.sh deinstall

uninstall: deinstall

clean:
	cd sys && $${MAKE:-make} SYSDIR="$(SYSDIR)" clean
	cd linux && $${MAKE:-make} SYSDIR="$(SYSDIR)" clean
