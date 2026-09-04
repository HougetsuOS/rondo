/*
 * rondo — desktop background rendering
 */
#include "wm.h"
#include <Imlib2.h>

/* ── pattern drawing helpers ─────────────────────────────────────────── */

/* Build a 2x2-cell tile and fill the whole pixmap with it — one XPutImage
 * plus a single XFillRectangle instead of thousands of per-cell requests. */
static void fill_with_tile(Pixmap pm, int w, int h, XftColor *c1, XftColor *c2,
                           int cell, int tile_w, int tile_h,
                           void (*paint_tile)(Display *, Drawable, GC, XftColor *, XftColor *, int)) {
    Pixmap tp = XCreatePixmap(dpy, pm, (unsigned)tile_w, (unsigned)tile_h,
                              (unsigned)DefaultDepth(dpy, screen));
    GC tgc = XCreateGC(dpy, tp, 0, NULL);
    XSetForeground(dpy, tgc, c1->pixel);
    XFillRectangle(dpy, tp, tgc, 0, 0, tile_w, tile_h);
    paint_tile(dpy, tp, tgc, c1, c2, cell);
    XSetTile(dpy, tgc, tp);
    XSetFillStyle(dpy, tgc, FillTiled);
    XFillRectangle(dpy, pm, tgc, 0, 0, w, h);
    XSetFillStyle(dpy, tgc, FillSolid);
    XFreeGC(dpy, tgc);
    XFreePixmap(dpy, tp);
}

static void tile_checker(Display *d, Drawable dr, GC g, XftColor *c1, XftColor *c2, int cell) {
    (void)d; (void)dr; (void)g; (void)c1;
    XSetForeground(d, g, c2->pixel);
    XFillRectangle(d, dr, g, 0, 0, cell, cell);
    XFillRectangle(d, dr, g, cell, cell, cell, cell);
}

static void tile_dots(Display *d, Drawable dr, GC g, XftColor *c1, XftColor *c2, int cell) {
    (void)c1;
    int r = cell / 4;
    if (r < 1) r = 1;
    XSetForeground(d, g, c2->pixel);
    XFillArc(d, dr, g, cell / 2 - r, cell / 2 - r, r * 2, r * 2, 0, 360 * 64);
}

static void draw_checkerboard(Pixmap pm, int w, int h, XftColor *c1, XftColor *c2, int cell) {
    fill_with_tile(pm, w, h, c1, c2, cell, cell * 2, cell * 2, tile_checker);
}

static void draw_hstripes(Pixmap pm, int w, int h, XftColor *c1, XftColor *c2, int cell) {
    GC gc1 = XCreateGC(dpy, pm, 0, NULL);
    XSetForeground(dpy, gc1, c1->pixel);
    XFillRectangle(dpy, pm, gc1, 0, 0, w, h);
    XSetForeground(dpy, gc1, c2->pixel);
    for (int y = 0; y < h; y += cell * 2)
        XFillRectangle(dpy, pm, gc1, 0, y, w, cell);
    XFreeGC(dpy, gc1);
}

static void draw_vstripes(Pixmap pm, int w, int h, XftColor *c1, XftColor *c2, int cell) {
    GC gc1 = XCreateGC(dpy, pm, 0, NULL);
    XSetForeground(dpy, gc1, c1->pixel);
    XFillRectangle(dpy, pm, gc1, 0, 0, w, h);
    XSetForeground(dpy, gc1, c2->pixel);
    for (int x = 0; x < w; x += cell * 2)
        XFillRectangle(dpy, pm, gc1, x, 0, cell, h);
    XFreeGC(dpy, gc1);
}

static void draw_diagonal_stripes(Pixmap pm, int w, int h, XftColor *c1, XftColor *c2, int cell) {
    GC gc1 = XCreateGC(dpy, pm, 0, NULL);
    XSetForeground(dpy, gc1, c1->pixel);
    XFillRectangle(dpy, pm, gc1, 0, 0, w, h);
    XSetForeground(dpy, gc1, c2->pixel);
    /* batch all diagonal lines into a single XDrawSegments request */
    int count = 0;
    for (int offset = -(h + w); offset < h + w; offset += cell * 2)
        count += cell;
    XSegment *segs = malloc(sizeof(XSegment) * (size_t)count);
    if (!segs) { XFreeGC(dpy, gc1); return; }
    int i = 0;
    for (int offset = -(h + w); offset < h + w; offset += cell * 2) {
        for (int d = 0; d < cell; d++) {
            int ox = offset + d;
            segs[i].x1 = (short)ox;        segs[i].y1 = 0;
            segs[i].x2 = (short)(ox + h);  segs[i].y2 = (short)h;
            i++;
        }
    }
    XDrawSegments(dpy, pm, gc1, segs, count);
    free(segs);
    XFreeGC(dpy, gc1);
}

static void draw_dots(Pixmap pm, int w, int h, XftColor *c1, XftColor *c2, int cell) {
    fill_with_tile(pm, w, h, c1, c2, cell, cell, cell, tile_dots);
}

static void draw_crosshatch(Pixmap pm, int w, int h, XftColor *c1, XftColor *c2, int cell) {
    GC gc1 = XCreateGC(dpy, pm, 0, NULL);
    XSetForeground(dpy, gc1, c1->pixel);
    XFillRectangle(dpy, pm, gc1, 0, 0, w, h);
    XSetForeground(dpy, gc1, c2->pixel);
    int n = 0;
    for (int offset = -(h + w); offset < h + w; offset += cell) n++;
    XSegment *segs = malloc(sizeof(XSegment) * (size_t)n * 2);
    if (!segs) { XFreeGC(dpy, gc1); return; }
    int i = 0;
    for (int offset = -(h + w); offset < h + w; offset += cell) {
        segs[i].x1 = (short)offset;        segs[i].y1 = 0;
        segs[i].x2 = (short)(offset + h);  segs[i].y2 = (short)h;
        i++;
    }
    for (int offset = -(h + w); offset < h + w; offset += cell) {
        segs[i].x1 = (short)(offset + h);  segs[i].y1 = 0;
        segs[i].x2 = (short)offset;        segs[i].y2 = (short)h;
        i++;
    }
    XDrawSegments(dpy, pm, gc1, segs, i);
    free(segs);
    XFreeGC(dpy, gc1);
}

static void draw_weave(Pixmap pm, int w, int h, XftColor *c1, XftColor *c2, int cell) {
    GC gc1 = XCreateGC(dpy, pm, 0, NULL);
    XSetForeground(dpy, gc1, c1->pixel);
    XFillRectangle(dpy, pm, gc1, 0, 0, w, h);
    XSetForeground(dpy, gc1, c2->pixel);
    int half = cell / 2;
    if (half < 2) half = 2;
    /* horizontal bars with gaps */
    for (int y = 0; y < h; y += cell) {
        XFillRectangle(dpy, pm, gc1, 0, y + half - 1, w, 2);
    }
    /* vertical bars with gaps, offset every other row */
    for (int x = 0; x < w; x += cell) {
        for (int y = 0; y < h; y += cell) {
            int off = ((x / cell) % 2) ? 0 : half;
            XFillRectangle(dpy, pm, gc1, x + half - 1, y + off, 2, half);
            if (off + half < cell)
                XFillRectangle(dpy, pm, gc1, x + half - 1, y, 2, off);
        }
    }
    XFreeGC(dpy, gc1);
}

/* ── image loading ────────────────────────────────────────────────────── */

static void load_image_bg(Pixmap pm, int scr_w, int scr_h) {
    if (!cfg_bg_image_path) return;

    Imlib_Image img = imlib_load_image(cfg_bg_image_path);
    if (!img) {
        fprintf(stderr, "rondo: cannot load image '%s', falling back to solid color\n",
                cfg_bg_image_path);
        cfg_bg_mode = BG_SOLID;
        return;
    }
    imlib_context_set_image(img);
    int img_w = imlib_image_get_width();
    int img_h = imlib_image_get_height();

    /* fill background with root-bg color first */
    GC gc1 = XCreateGC(dpy, pm, 0, NULL);
    XSetForeground(dpy, gc1, col_root_bg.pixel);
    XFillRectangle(dpy, pm, gc1, 0, 0, scr_w, scr_h);
    XFreeGC(dpy, gc1);

    Pixmap img_pm = None;

    switch (cfg_bg_mode_image) {
    case BG_CENTERED: {
        /* center the image, fill rest with bg color */
        Pixmap src_pm = XCreatePixmap(dpy, root, (unsigned)img_w, (unsigned)img_h,
                                       (unsigned)DefaultDepth(dpy, screen));
        imlib_context_set_drawable(src_pm);
        imlib_render_image_on_drawable(0, 0);
        GC gc2 = XCreateGC(dpy, pm, 0, NULL);
        int dx = (scr_w - img_w) / 2;
        int dy = (scr_h - img_h) / 2;
        if (dx < 0) dx = 0;
        if (dy < 0) dy = 0;
        int sx = (img_w > scr_w) ? (img_w - scr_w) / 2 : 0;
        int sy = (img_h > scr_h) ? (img_h - scr_h) / 2 : 0;
        int cw = (img_w > scr_w) ? scr_w : img_w;
        int ch = (img_h > scr_h) ? scr_h : img_h;
        XCopyArea(dpy, src_pm, pm, gc2, sx, sy, (unsigned)cw, (unsigned)ch,
                  dx, dy);
        XFreeGC(dpy, gc2);
        XFreePixmap(dpy, src_pm);
        break;
    }
    case BG_SCALED: {
        /* scale to fit, maintaining aspect ratio */
        double scale_x = (double)scr_w / img_w;
        double scale_y = (double)scr_h / img_h;
        double scale = scale_x < scale_y ? scale_x : scale_y;
        int dw = (int)(img_w * scale);
        int dh = (int)(img_h * scale);
        Pixmap src_pm = XCreatePixmap(dpy, root, (unsigned)dw, (unsigned)dh,
                                       (unsigned)DefaultDepth(dpy, screen));
        imlib_context_set_drawable(src_pm);
        imlib_render_image_on_drawable_at_size(0, 0, dw, dh);
        GC gc2 = XCreateGC(dpy, pm, 0, NULL);
        int dx = (scr_w - dw) / 2;
        int dy = (scr_h - dh) / 2;
        XCopyArea(dpy, src_pm, pm, gc2, 0, 0, (unsigned)dw, (unsigned)dh,
                  dx, dy);
        XFreeGC(dpy, gc2);
        XFreePixmap(dpy, src_pm);
        break;
    }
    case BG_TILED: {
        /* tile the image with XSetTile + a single fill */
        Pixmap src_pm = XCreatePixmap(dpy, root, (unsigned)img_w, (unsigned)img_h,
                                       (unsigned)DefaultDepth(dpy, screen));
        imlib_context_set_drawable(src_pm);
        imlib_render_image_on_drawable(0, 0);
        GC gc2 = XCreateGC(dpy, pm, 0, NULL);
        XSetTile(dpy, gc2, src_pm);
        XSetFillStyle(dpy, gc2, FillTiled);
        XFillRectangle(dpy, pm, gc2, 0, 0, scr_w, scr_h);
        XSetFillStyle(dpy, gc2, FillSolid);
        XFreeGC(dpy, gc2);
        XFreePixmap(dpy, src_pm);
        break;
    }
    case BG_STRETCHED: {
        /* scale to exact screen dimensions */
        img_pm = XCreatePixmap(dpy, root, (unsigned)scr_w, (unsigned)scr_h,
                               (unsigned)DefaultDepth(dpy, screen));
        imlib_context_set_drawable(img_pm);
        imlib_render_image_on_drawable_at_size(0, 0, scr_w, scr_h);
        GC gc2 = XCreateGC(dpy, pm, 0, NULL);
        XCopyArea(dpy, img_pm, pm, gc2, 0, 0, (unsigned)scr_w, (unsigned)scr_h, 0, 0);
        XFreeGC(dpy, gc2);
        break;
    }
    case BG_SCALE_FILLED: {
        /* scale to fill, cropping the overflow — no blank areas */
        double scale_x = (double)scr_w / img_w;
        double scale_y = (double)scr_h / img_h;
        double scale = scale_x > scale_y ? scale_x : scale_y;
        int dw = (int)(img_w * scale);
        int dh = (int)(img_h * scale);
        Pixmap src_pm = XCreatePixmap(dpy, root, (unsigned)dw, (unsigned)dh,
                                       (unsigned)DefaultDepth(dpy, screen));
        imlib_context_set_drawable(src_pm);
        imlib_render_image_on_drawable_at_size(0, 0, dw, dh);
        GC gc2 = XCreateGC(dpy, pm, 0, NULL);
        int sx = (dw - scr_w) / 2;
        int sy = (dh - scr_h) / 2;
        XCopyArea(dpy, src_pm, pm, gc2, sx, sy,
                  (unsigned)scr_w, (unsigned)scr_h, 0, 0);
        XFreeGC(dpy, gc2);
        XFreePixmap(dpy, src_pm);
        break;
    }
    }

    if (img_pm != None) XFreePixmap(dpy, img_pm);
    imlib_context_set_image(img);
    imlib_free_image();
}

/* ── public API ───────────────────────────────────────────────────────── */

void bg_load(void) {
    /* free old pixmap if any */
    if (root_bg_pixmap != None) {
        XFreePixmap(dpy, root_bg_pixmap);
        root_bg_pixmap = None;
    }

    static Atom xrootpmap_cached = None;
    static int atom_ready = 0;
    if (!atom_ready) {
        xrootpmap_cached = XInternAtom(dpy, "_XROOTPMAP_ID", False);
        atom_ready = 1;
    }
    Atom xrootpmap = xrootpmap_cached;

    if (cfg_bg_mode == BG_SOLID) {
        XSetWindowBackground(dpy, root, col_root_bg.pixel);
        XClearWindow(dpy, root);
        /* remove pixmap property */
        XDeleteProperty(dpy, root, xrootpmap);
        /* compositor must re-fetch the cached bg color */
        compositor_bg_reloaded();
        return;
    }

    /* create root-sized pixmap */
    unsigned int pw = (unsigned)mon.w;
    unsigned int ph = (unsigned)mon.h;
    Pixmap pm = XCreatePixmap(dpy, root, pw, ph,
                               (unsigned)DefaultDepth(dpy, screen));
    if (!pm) {
        fprintf(stderr, "rondo: failed to create background pixmap\n");
        XSetWindowBackground(dpy, root, col_root_bg.pixel);
        XClearWindow(dpy, root);
        XDeleteProperty(dpy, root, xrootpmap);
        return;
    }

    if (cfg_bg_mode == BG_PATTERN) {
        /* load pattern colors */
        XftColor col1, col2;
        xftcolor_load(cfg_color_root_bg, &col1);
        xftcolor_load(cfg_color_root_bg2, &col2);

        int cell = cfg_bg_pattern_size;
        if (cell <= 0) cell = 16; /* default cell size */

        switch (cfg_bg_pattern) {
        case PAT_CHECKERBOARD:
            draw_checkerboard(pm, (int)pw, (int)ph, &col1, &col2, cell);
            break;
        case PAT_HORIZONTAL_STRIPES:
            draw_hstripes(pm, (int)pw, (int)ph, &col1, &col2, cell);
            break;
        case PAT_VERTICAL_STRIPES:
            draw_vstripes(pm, (int)pw, (int)ph, &col1, &col2, cell);
            break;
        case PAT_DIAGONAL_STRIPES:
            draw_diagonal_stripes(pm, (int)pw, (int)ph, &col1, &col2, cell);
            break;
        case PAT_DOTS:
            draw_dots(pm, (int)pw, (int)ph, &col1, &col2, cell);
            break;
        case PAT_CROSSHATCH:
            draw_crosshatch(pm, (int)pw, (int)ph, &col1, &col2, cell);
            break;
        case PAT_WEAVE:
            draw_weave(pm, (int)pw, (int)ph, &col1, &col2, cell);
            break;
        }

        XftColorFree(dpy, xvisual, xcolormap, &col1);
        XftColorFree(dpy, xvisual, xcolormap, &col2);
    } else if (cfg_bg_mode == BG_IMAGE) {
        load_image_bg(pm, (int)pw, (int)ph);
        /* if load failed, it sets cfg_bg_mode to BG_SOLID */
        if (cfg_bg_mode == BG_SOLID) {
            XFreePixmap(dpy, pm);
            XSetWindowBackground(dpy, root, col_root_bg.pixel);
            XClearWindow(dpy, root);
            XDeleteProperty(dpy, root, xrootpmap);
            return;
        }
    }

    root_bg_pixmap = pm;
    XSetWindowBackgroundPixmap(dpy, root, pm);
    XClearWindow(dpy, root);

    /* set _XROOTPMAP_ID for other programs */
    XChangeProperty(dpy, root, xrootpmap, XA_PIXMAP, 32,
                    PropModeReplace, (unsigned char *)&pm, 1);

    /* compositor must re-fetch the cached bg picture / color */
    compositor_bg_reloaded();
}

void bg_free(void) {
    if (root_bg_pixmap != None) {
        XFreePixmap(dpy, root_bg_pixmap);
        root_bg_pixmap = None;
    }
}