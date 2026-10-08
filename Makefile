CC ?= gcc
ARCH ?= $(shell uname -m)

CFLAGS = -O3 -fPIC -Wall -Wextra -pthread
ifeq ($(ARCH),aarch64)
CFLAGS += -march=armv8-a
endif
ifeq ($(ARCH),armv7l)
CFLAGS += -march=armv7-a
endif

LDFLAGS = -shared -lasound

TARGET = libcdda_engine.so
SRCS = cdda_engine.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(SRCS) cdda_engine.h
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o

.PHONY: all clean
