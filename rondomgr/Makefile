CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -pedantic -O2 \
          $(shell pkg-config --cflags motif 2>/dev/null || echo "-I/usr/include/Xm") \
          $(shell pkg-config --cflags x11 xft imlib2 2>/dev/null)
# X libs + imlib-free deps (Motif linked separately, see MOTIF_LIBS)
LDFLAGS = $(shell pkg-config --libs x11 xft imlib2 2>/dev/null)

# Motif link: prefer static (pkg-config motif-static) so the binary does not
# depend on libXm.so.4 at runtime; falls back to shared -lXm.
# Override with: make MOTIF_LIBS="-lXm -lXt"
MOTIF_LIBS = $(shell pkg-config --libs motif-static 2>/dev/null || pkg-config --libs motif 2>/dev/null || echo "-lXm -lXt -lX11 -lXft")
# libXm.a pulls in XShape (libXext) and other transitive deps
MOTIF_STATIC_EXTRA = $(shell pkg-config --libs xext 2>/dev/null) -lXmu -lSM -lICE

BIN     = rondomgr

all: $(BIN)

$(BIN): rondomgr.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(MOTIF_LIBS) $(MOTIF_STATIC_EXTRA)

clean:
	rm -f $(BIN)

.PHONY: all clean