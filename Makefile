CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2 -std=c99
LDLIBS  := -lpthread

BINARIES := drive_stress_linux stress_test_multi

all: $(BINARIES)

drive_stress_linux: drive_stress_linux.o drive_stress.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

stress_test_multi: stress_test_multi.o drive_stress.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c drive_stress.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(BINARIES) *.o

.PHONY: all clean
