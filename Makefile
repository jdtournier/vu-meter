CC      := gcc
CFLAGS  := -Wall -Wextra -O2 `pkg-config --cflags gtk+-3.0 libpulse-simple`
LDFLAGS := -pthread -lm `pkg-config --libs gtk+-3.0 libpulse-simple`
PROGS   := vu-bar

all: clean $(PROGS)

.PHONY: clean
clean:
	rm -f *.o $(PROGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $^

vu-bar: gui.o vu.o
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@
