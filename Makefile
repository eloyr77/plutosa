# plutosa: a spectrum analyzer for the ADALM-Pluto, a windowed RitterOS app
# (KIND := apps), built with RitterOS's SDK (RITTEROS/sdk/CLAUDE.md).
#
#   make                                 build build/x86_64/plutosa
#   make deploy run HOST=root@ADDRESS    copy it to the RitterOS machine and run it
#                                        (windowd must be running there)
#   make run HOST=... ARGS=ip:ADDRESS    another Pluto than ip:192.168.2.1
#   make stop HOST=root@ADDRESS          end it there
#
# HOST (and SSH_OPTS, e.g. SSH_OPTS="-i ~/.ssh/ritteros") can also be set in
# the environment instead of on every command line.
#
# libiio comes from the submodule libiio/ (git submodule update --init);
# its sources are compiled into the program like our own.

RITTEROS ?= ../RitterOS
APP      := plutosa
KIND     := apps
LIBIIO   := libiio/src/iio.c libiio/src/iioxml.c libiio/src/iionet.c
SRCS     := src/plutosa.c $(LIBIIO)
CFLAGS   += -Werror -Ilibiio/include

ifeq ($(wildcard libiio/include/iio.h),)
$(error libiio/ is empty: git submodule update --init)
endif

RITTEROS_MK := $(firstword $(wildcard $(RITTEROS)/sdk/ritteros.mk /system/sdk/ritteros.mk))
ifeq ($(RITTEROS_MK),)
$(error no RitterOS SDK: neither a checkout at RITTEROS=$(RITTEROS) (git clone https://github.com/eloyr77/RitterOS.git next to this project, or make RITTEROS=/path/to/RitterOS) nor /system/sdk/ritteros.mk (RitterOS itself))
endif
include $(RITTEROS_MK)
