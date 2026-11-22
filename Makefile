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

help:
	@printf '%s\n' \
		"purpose: $(REPO_PURPOSE)" \
		"Targets (same names on every OS):" \
		"  make purpose    print REPO_PURPOSE (for machines)" \
		"  make help       this message (default)" \
		"  make preflight  report missing tools (JSON=1 for --json)" \
		"  make install-prereqs  check cc, make and mandoc (aliases: deps, install-deps)" \
		"  make configure  nothing to configure for this module" \
		"  make build      build sys/nvidia-uvm.ko (SYSDIR=$(SYSDIR): the kernel source); make all is the same" \
		"  make test       the userland unit tests and the amd64 cross-compile check" \
		"  make lint       mandoc -T lint on the manual page" \
		"  make install    install the module and man page (YES=1 skips the question)" \
		"  make deinstall  remove exactly what install wrote (alias: uninstall)" \
		"  make clean" \
		"" \
		"Switches:" \
		"  SYSDIR=DIR    the kernel source tree (default $(SYSDIR))" \
		"  YES=1         do not ask before install or deinstall" \
		"  ASSUME_YES=1  the same confirmation switch" \
		"  PREFIX=DIR    man page prefix (default $(PREFIX))" \
		"  DESTDIR=DIR   stage as the calling user instead of using root" \
		"  JSON=1        preflight writes the --json contract" \
		"" \
		"Run make from the directory that holds this Makefile."

.PHONY: help purpose preflight install-prereqs deps install-deps configure build all test lint install deinstall uninstall clean

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
# The sub-make is the shell variable MAKE when the caller exported it, else make (FreeBSD make: sys/ is a bsd.kmod.mk Makefile).
build:
	cd sys && $${MAKE:-make} SYSDIR="$(SYSDIR)"

all: build

test:
	sh tests/run_unit_tests.sh
	sh tests/amd64_cross_check.sh $(SYSDIR)

lint:
	mandoc -T lint man/nvidia-uvm.4

install:
	MK_YES="$(YES)" MK_ASSUME_YES="$(ASSUME_YES)" MK_PREFIX="$(PREFIX)" MK_DESTDIR="$(DESTDIR)" MK_SYSDIR="$(SYSDIR)" MK_KO="$(MK_KO)" sh scripts/make.sh install

deinstall:
	MK_YES="$(YES)" MK_ASSUME_YES="$(ASSUME_YES)" MK_PREFIX="$(PREFIX)" MK_DESTDIR="$(DESTDIR)" MK_SYSDIR="$(SYSDIR)" MK_KO="$(MK_KO)" sh scripts/make.sh deinstall

uninstall: deinstall

clean:
	cd sys && $${MAKE:-make} SYSDIR="$(SYSDIR)" clean
