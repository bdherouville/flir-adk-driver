CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Wpedantic -std=c11 -O2
LDFLAGS ?=

SRCS    = src/capture.c src/cci.c src/cci_params.c src/cci_system.c \
          src/cci_symbology.c src/spotmeter.c src/colorbar.c src/denoise.c
OBJS    = $(SRCS:.c=.o)
LIB     = libflir_adk.a

ACCEL_GENERIC = platform/accel_generic.o
TOOL_OBJS = tools/flir_grab.o tools/flir_calibrate.o tools/cci_probe.o \
            tools/flir_ctl.o tools/flir_loop.o tools/test_accel.o tools/test_cci.o

.PHONY: all clean test

all: flir_grab flir_calibrate cci_probe flir_ctl flir_loop test_accel test_cci

%.o: %.c
	$(CC) $(CFLAGS) -Iinclude -c -o $@ $<

# rebuild objects when a header changes
$(OBJS): include/flir_adk.h src/internal.h src/cci_util.h
$(TOOL_OBJS) $(ACCEL_GENERIC): include/flir_adk.h
tools/test_cci.o: src/internal.h

$(LIB): $(OBJS)
	ar rcs $@ $^

flir_grab: tools/flir_grab.o $(LIB) $(ACCEL_GENERIC)
	$(CC) $(LDFLAGS) -o $@ $^

flir_calibrate: tools/flir_calibrate.o $(LIB) $(ACCEL_GENERIC)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

cci_probe: tools/cci_probe.o $(LIB) $(ACCEL_GENERIC)
	$(CC) $(LDFLAGS) -o $@ $^

flir_ctl: tools/flir_ctl.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^

flir_loop: tools/flir_loop.o $(LIB) $(ACCEL_GENERIC)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

test_accel: tools/test_accel.o $(LIB) $(ACCEL_GENERIC)
	$(CC) $(LDFLAGS) -o $@ $^

test_cci: tools/test_cci.o $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^

test: test_accel test_cci
	./test_accel && ./test_cci

clean:
	rm -f $(OBJS) $(ACCEL_GENERIC) tools/*.o $(LIB) flir_grab flir_calibrate cci_probe flir_ctl flir_loop test_accel test_cci
