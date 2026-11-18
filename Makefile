CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Wpedantic -std=c11 -O2
LDFLAGS ?=

SRCS    = src/capture.c src/cci.c src/spotmeter.c src/colorbar.c src/denoise.c
OBJS    = $(SRCS:.c=.o)
LIB     = libflir_adk.a

ACCEL_GENERIC = platform/accel_generic.o

.PHONY: all clean test

all: flir_grab flir_calibrate cci_probe flir_ctl flir_loop test_accel

%.o: %.c
	$(CC) $(CFLAGS) -Iinclude -c -o $@ $<

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

test: tools/test_accel.o $(LIB) $(ACCEL_GENERIC)
	$(CC) $(LDFLAGS) -o test_accel $^ && ./test_accel

clean:
	rm -f $(OBJS) $(ACCEL_GENERIC) tools/*.o $(LIB) flir_grab flir_calibrate cci_probe flir_ctl flir_loop test_accel
