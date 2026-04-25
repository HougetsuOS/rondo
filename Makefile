CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -pedantic -O2 $(shell pkg-config --cflags x11 xinerama xft xcomposite xdamage xfixes xrender) $(shell pkg-config --cflags motif 2>/dev/null || echo "-I/usr/include/Xm") $(shell pkg-config --cflags imlib2)
LDFLAGS = $(shell pkg-config --libs x11 xinerama xft xcomposite xdamage xfixes xrender) $(shell pkg-config --libs motif 2>/dev/null || echo "-lXm -lXt -lX11 -lXft") $(shell pkg-config --libs imlib2)

SRCS    = main.c frame.c bar.c feedback.c mouse.c client.c layout.c action.c event.c menu.c config.c bg.c ewmh.c compose.c ipc.c tray.c
OBJS    = $(SRCS:.c=.o)
BIN     = rondo

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c wm.h config.h ipc.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(BIN)

.PHONY: all clean