# plutosa: a spectrum analyzer for the ADALM-Pluto, a windowed RitterOS app
# (KIND := apps). It is built on the RitterOS machine itself, with its own cc
# and SDK (/system/sdk/ritteros.mk); from this computer the same targets copy
# the app's code there and run make there:
#
#   make HOST=root@ADDRESS           copy the code to REMOTE_DIR there and build it
#   make deploy HOST=...             build and install it in /system/ARCH/apps
#   make run HOST=... [ARGS=URI]     run it there (windowd must be running);
#                                    another Pluto than ip:192.168.2.1 with ARGS
#   make stop HOST=...               end it there
#   make check                       the spectrum's arithmetic (src/dsp.c) tested here
#   make check-machine HOST=...      the same test built and run on the machine
#
# HOST and SSH_OPTS (e.g. SSH_OPTS="-i ~/.ssh/ritteros") can also be set in
# the environment. libiio comes from the submodule libiio/
# (git submodule update --init); its sources are compiled into the program.

APP      := plutosa
KIND     := apps
LIBIIO   := libiio/src/iio.c libiio/src/iioxml.c libiio/src/iionet.c
SRCS     := src/plutosa.c src/dsp.c $(LIBIIO)
CFLAGS   += -Werror -Ilibiio/include

ifeq ($(wildcard libiio/include/iio.h),)
$(error libiio/ is empty: git submodule update --init)
endif

ifneq ($(wildcard /system/sdk/ritteros.mk),)
# On the RitterOS machine.
include /system/sdk/ritteros.mk

# The spectrum's test, built with the machine's cc and C library.
build/dsptest: tests/dsptest.c src/dsp.c src/dsp.h
	@mkdir -p build
	cc -Werror -Isrc -o $@ tests/dsptest.c src/dsp.c

.PHONY: check
check: build/dsptest
	build/dsptest
else
# On another computer: only the app's code goes over, and make runs there.
REMOTE_DIR ?= /home/root/plutosa
CODE := Makefile src tests libiio/include libiio/src
SSH  := ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new $(SSH_OPTS)
need_host = $(if $(HOST),,$(error set HOST=user@address, e.g. make $@ HOST=root@192.168.0.195))

.PHONY: all push deploy run stop clean
all: push
	$(SSH) $(HOST) 'cd $(REMOTE_DIR) && make'

# tar xm: every file gets the machine's time now, so a clock that runs ahead
# of this computer's never makes make skip an edited file (sdk/CLAUDE.md).
# (COPYFILE_DISABLE keeps macOS's ._ files out.)
push:
	$(need_host)
	COPYFILE_DISABLE=1 tar cf - $(CODE) | $(SSH) $(HOST) 'mkdir -p $(REMOTE_DIR) && tar xmf - -C $(REMOTE_DIR)'

deploy: push
	$(SSH) $(HOST) 'cd $(REMOTE_DIR) && make deploy'

# A windowed app keeps the session open until its window closes: run it in
# the background here.
run:
	$(need_host)
	$(SSH) $(HOST) 'cd $(REMOTE_DIR) && make run ARGS="$(ARGS)"'

stop:
	$(need_host)
	$(SSH) $(HOST) 'cd $(REMOTE_DIR) && make stop'

clean:
	$(need_host)
	$(SSH) $(HOST) 'cd $(REMOTE_DIR) && make clean'

# The spectrum's arithmetic (src/dsp.c, plain C) checked here, on this
# computer, with its own compiler and sanitizers: no machine needed.
HOST_CC := cc -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror
build/host/dsptest: tests/dsptest.c src/dsp.c src/dsp.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -Isrc -o $@ tests/dsptest.c src/dsp.c -lm

.PHONY: check check-machine
check: build/host/dsptest
	build/host/dsptest

check-machine: push
	$(SSH) $(HOST) 'cd $(REMOTE_DIR) && make check'
endif
