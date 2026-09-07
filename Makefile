CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -pedantic -O2 $(shell pkg-config --cflags x11 xinerama xft xcomposite xdamage xfixes xrender) $(shell pkg-config --cflags motif 2>/dev/null || echo "-I/usr/include/Xm") $(shell pkg-config --cflags imlib2)
# X extension libs + imlib2 (Motif is linked separately, see MOTIF_LIBS)
LDFLAGS = $(shell pkg-config --libs x11 xinerama xft xcomposite xdamage xfixes xrender) $(shell pkg-config --libs imlib2)

# Motif link: prefer static (pkg-config motif-static) so the binary does not
# depend on libXm.so.4 at runtime; falls back to shared -lXm.
# Override with: make MOTIF_LIBS="-lXm -lXt"
MOTIF_LIBS = $(shell pkg-config --libs motif-static 2>/dev/null || pkg-config --libs motif 2>/dev/null || echo "-lXm -lXt -lX11 -lXft")
# libXm.a pulls in XShape (libXext) and other transitive deps
MOTIF_STATIC_EXTRA = $(shell pkg-config --libs xext 2>/dev/null) -lXmu -lSM -lICE

SRCS    = main.c frame.c bar.c feedback.c mouse.c client.c layout.c action.c event.c menu.c config.c bg.c ewmh.c compose.c ipc.c tray.c icon.c
OBJS    = $(SRCS:.c=.o)
BIN     = rondo

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS) $(MOTIF_LIBS) $(MOTIF_STATIC_EXTRA)

%.o: %.c wm.h config.h ipc.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(BIN)

.PHONY: all clean