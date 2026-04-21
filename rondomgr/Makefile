CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -pedantic -O2 \
          $(shell pkg-config --cflags motif 2>/dev/null || echo "-I/usr/include/Xm") \
          $(shell pkg-config --cflags x11 xft 2>/dev/null)
LDFLAGS = $(shell pkg-config --libs motif 2>/dev/null || echo "-lXm -lXt -lX11 -lXft") \
          $(shell pkg-config --libs x11 xft 2>/dev/null)

BIN     = rondomgr

all: $(BIN)

$(BIN): rondomgr.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BIN)

.PHONY: all clean