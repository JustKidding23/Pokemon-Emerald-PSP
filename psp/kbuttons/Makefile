# The kernel module that reads the music note button: make -C psp/kbuttons
TARGET = kbuttons
OBJS = kbuttons.o
BUILD_PRX = 1
USE_KERNEL_LIBS = 1
CFLAGS = -O2 -G0 -Wall
LIBS = -lpspctrl_driver

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
