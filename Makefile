# plutosa: a spectrum analyzer for the ADALM-Pluto, a windowed RitterOS app
# (KIND := apps). It is built on the RitterOS machine itself, with its own cc
# and SDK (/system/sdk/ritteros.mk); from this computer the same targets copy
# the app's code there and run make there:
#
#   make HOST=root@ADDRESS           copy the code to REMOTE_DIR there and build it
#   make deploy HOST=...             build and install it in /system/ARCH/apps, as the
#                                    bundle plutosa.program/ (the program, plutosa.info
#                                    as info, plutosa.png as icon.png: the SDK's 42b)
#   make upload HOST=...             install the last deployed program as it is
#   make run HOST=... [ARGS=URI]     run it there (windowd must be running);
#                                    another Pluto than ip:192.168.2.1 with ARGS
#   make stop HOST=...               end it there
#   make check                       the spectrum's arithmetic (src/dsp.c) tested here
#   make check-machine HOST=...      the same test built and run on the machine
#   make check-radio HOST=...        src/radio.c on the real Pluto (receive settings
#                                    changed, read back, then put back)
#
# HOST and SSH_OPTS (e.g. SSH_OPTS="-i ~/.ssh/ritteros") can also be set in
# the environment. libiio comes from the submodule libiio/
# (git submodule update --init); its sources are compiled into the program.

APP      := plutosa
KIND     := apps
LIBIIO   := libiio/src/iio.c libiio/src/iioxml.c libiio/src/iionet.c
SRCS     := src/plutosa.c src/dsp.c src/demod.c src/radio.c src/view.c $(LIBIIO)
CFLAGS   += -Werror -Ilibiio/include

ifeq ($(wildcard libiio/include/iio.h),)
$(error libiio/ is empty: git submodule update --init)
endif

ifneq ($(wildcard /system/sdk/ritteros.mk),)
# On the RitterOS machine.
include /system/sdk/ritteros.mk

# The spectrum's and the demodulator's tests, built with the machine's cc
# and C library.
build/dsptest: tests/dsptest.c src/dsp.c src/dsp.h
	@mkdir -p build
	cc -Werror -Isrc -o $@ tests/dsptest.c src/dsp.c

build/demodtest: tests/demodtest.c src/demod.c src/demod.h
	@mkdir -p build
	cc -Werror -Isrc -o $@ tests/demodtest.c src/demod.c

# radio.c on the real Pluto (ARGS: another URI).
build/radiotest: tests/radiotest.c src/radio.c src/radio.h $(LIBIIO)
	@mkdir -p build
	cc -Werror -Isrc -Ilibiio/include -o $@ tests/radiotest.c src/radio.c $(LIBIIO)

.PHONY: check check-radio
check: build/dsptest build/demodtest
	build/dsptest
	build/demodtest

check-radio: build/radiotest
	build/radiotest $(ARGS)

build/streamtest: tests/streamtest.c src/radio.c src/radio.h src/dsp.c src/dsp.h $(LIBIIO)
	cc -Werror -Isrc -Ilibiio/include -o $@ tests/streamtest.c src/radio.c src/dsp.c $(LIBIIO)

.PHONY: measure
measure: build/streamtest
	build/streamtest $(ARGS)
else
# On another computer: only the app's code goes over, and make runs there.
REMOTE_DIR ?= /home/root/plutosa
ARCH ?= x86_64
CODE := Makefile $(APP).info $(APP).png src tests libiio/include libiio/src
SSH  := ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new $(SSH_OPTS)
need_host = $(if $(HOST),,$(error set HOST=user@address, e.g. make $@ HOST=root@192.168.0.195))

.PHONY: all push deploy upload run stop clean
all: push
	$(SSH) $(HOST) 'cd $(REMOTE_DIR) && make'

# tar xm: every file gets the machine's time now, so a clock that runs ahead
# of this computer's never makes make skip an edited file (sdk/CLAUDE.md).
# (COPYFILE_DISABLE keeps macOS's ._ files out.)
push:
	$(need_host)
	COPYFILE_DISABLE=1 tar cf - $(CODE) | $(SSH) $(HOST) 'mkdir -p $(REMOTE_DIR) && tar xmf - -C $(REMOTE_DIR)'

# deploy also brings the program back to dist/ARCH (git-ignored), for upload.
deploy: push
	$(SSH) $(HOST) 'cd $(REMOTE_DIR) && make deploy'
	@mkdir -p dist/$(ARCH)
	$(SSH) $(HOST) 'cat $(REMOTE_DIR)/build/$(ARCH)/$(APP)' > dist/$(ARCH)/$(APP).new
	mv dist/$(ARCH)/$(APP).new dist/$(ARCH)/$(APP)

# The program last deployed, installed as it is (a fresh system: no sources,
# no build). The shared libraries stay compatible, so it runs on newer
# systems too; on an older one ld.so may refuse it ("needs FUNCTION"):
# make deploy then.
# Installed as deploy does, the bundle $(APP).program/ with its info and
# icon (from here), and an old flat apps/$(APP) from before bundles removed.
upload:
	$(need_host)
	@test -f dist/$(ARCH)/$(APP) || { echo "no dist/$(ARCH)/$(APP): make deploy first"; exit 1; }
	rm -rf dist/stage && mkdir -p dist/stage/$(APP).program
	cp dist/$(ARCH)/$(APP) dist/stage/$(APP).program/$(APP)
	chmod 755 dist/stage/$(APP).program/$(APP)
	cp $(APP).info dist/stage/$(APP).program/info
	cp $(APP).png dist/stage/$(APP).program/icon.png
	COPYFILE_DISABLE=1 tar cf - -C dist/stage $(APP).program | $(SSH) $(HOST) 'rm -f /system/$(ARCH)/apps/$(APP); tar xf - -C /system/$(ARCH)/apps'

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

# The spectrum's and the demodulator's arithmetic (src/dsp.c, src/demod.c,
# plain C) checked here, on this computer, with its own compiler and
# sanitizers: no machine needed.
HOST_CC := cc -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror
build/host/dsptest: tests/dsptest.c src/dsp.c src/dsp.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -Isrc -o $@ tests/dsptest.c src/dsp.c -lm

.PHONY: check check-machine check-radio
build/host/demodtest: tests/demodtest.c src/demod.c src/demod.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -Isrc -o $@ tests/demodtest.c src/demod.c -lm

check: build/host/dsptest build/host/demodtest
	build/host/dsptest
	build/host/demodtest

check-machine: push
	$(SSH) $(HOST) 'cd $(REMOTE_DIR) && make check'

check-radio: push
	$(SSH) $(HOST) 'cd $(REMOTE_DIR) && make check-radio ARGS="$(ARGS)"'

# How many of the Pluto's samples the link carries (tests/streamtest.c).
.PHONY: measure
measure: push
	$(SSH) $(HOST) 'cd $(REMOTE_DIR) && make measure ARGS="$(ARGS)"'
endif
