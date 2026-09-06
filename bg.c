/*
 * rondo — desktop background rendering
 */
#include "wm.h"
#include <Imlib2.h>
#include "xmplat_seam.h"

/* ── pattern drawing helpers ─────────────────────────────────────────── */

/* Build a 2x2-cell tile and fill the whole pixmap with it — one fill of
 * the tile plus a single tiled fill instead of thousands of per-cell
 * requests.  All drawing goes through the XmPlat seam (MIGRATION_GUIDE §4). */
static void fill_with_tile(Pixmap pm, int w, int h, XftColor *c1, XftColor *c2,
                           int cell, int tile_w, int tile_h,
                           void (*paint_tile)(XmPlatDrawCtx, XftColor *, XftColor *, int)) {
    Pixmap tp = XCreatePixmap(dpy, pm, (unsigned)tile_w, (unsigned)tile_h,
                              (unsigned)DefaultDepth(dpy, screen));
    GC tgc = XCreateGC(dpy, tp, 0, NULL);
    /* ctx targets the DESTINATION pixmap pm; the tile (tp) is attached to
     * the GC via _XmPlatSetTile and repeated by the tiled fill */
    XmPlatDrawCtx c = _XmPlatCtx(dpy, pm, tgc);
    _XmPlatSetForeground(c, c1->pixel);
    {
        /* paint the tile contents on the tile pixmap */
        XmPlatDrawCtx tc = _XmPlatCtx(dpy, tp, tgc);
        _XmPlatFillRect(tc, 0, 0, (unsigned)tile_w, (unsigned)tile_h);
        paint_tile(tc, c1, c2, cell);
        _XmPlatCtxFree(tc);
    }
    _XmPlatSetTile(c, _XmPlatSurface(dpy, tp));
    _XmPlatFillRectangleTiled(c, 0, 0, (unsigned)w, (unsigned)h);
    _XmPlatCtxFree(c);
    XFreeGC(dpy, tgc);
    XFreePixmap(dpy, tp);
}

static void tile_checker(XmPlatDrawCtx c, XftColor *c1, XftColor *c2, int cell) {
    (void)c1;
    _XmPlatSetForeground(c, c2->pixel);
    _XmPlatFillRect(c, 0, 0, (unsigned)cell, (unsigned)cell);
    _XmPlatFillRect(c, cell, cell, (unsigned)cell, (unsigned)cell);
}

static void tile_dots(XmPlatDrawCtx c, XftColor *c1, XftColor *c2, int cell) {
    (void)c1;
    int r = cell / 4;
    if (r < 1) r = 1;
    _XmPlatSetForeground(c, c2->pixel);
    _XmPlatFillArc(c, cell / 2 - r, cell / 2 - r, (unsigned)(r * 2), (unsigned)(r * 2), 0, 360 * 64);
}

static void draw_checkerboard(Pixmap pm, int w, int h, XftColor *c1, XftColor *c2, int cell) {
    fill_with_tile(pm, w, h, c1, c2, cell, cell * 2, cell * 2, tile_checker);
}

static void draw_hstripes(Pixmap pm, int w, int h, XftColor *c1, XftColor *c2, int cell) {
    GC gc1 = XCreateGC(dpy, pm, 0, NULL);
    XmPlatDrawCtx c = _XmPlatCtx(dpy, pm, gc1);
    _XmPlatSetForeground(c, c1->pixel);
    _XmPlatFillRect(c, 0, 0, (unsigned)w, (unsigned)h);
    _XmPlatSetForeground(c, c2->pixel);
    for (int y = 0; y < h; y += cell * 2)
        _XmPlatFillRect(c, 0, y, (unsigned)w, (unsigned)cell);
    _XmPlatCtxFree(c);
    XFreeGC(dpy, gc1);
}

static void draw_vstripes(Pixmap pm, int w, int h, XftColor *c1, XftColor *c2, int cell) {
    GC gc1 = XCreateGC(dpy, pm, 0, NULL);
    XmPlatDrawCtx c = _XmPlatCtx(dpy, pm, gc1);
    _XmPlatSetForeground(c, c1->pixel);
    _XmPlatFillRect(c, 0, 0, (unsigned)w, (unsigned)h);
    _XmPlatSetForeground(c, c2->pixel);
    for (int x = 0; x < w; x += cell * 2)
        _XmPlatFillRect(c, x, 0, (unsigned)cell, (unsigned)h);
    _XmPlatCtxFree(c);
    XFreeGC(dpy, gc1);
}

static void draw_diagonal_stripes(Pixmap pm, int w, int h, XftColor *c1, XftColor *c2, int cell) {
    GC gc1 = XCreateGC(dpy, pm, 0, NULL);
    XmPlatDrawCtx c = _XmPlatCtx(dpy, pm, gc1);
    _XmPlatSetForeground(c, c1->pixel);
    _XmPlatFillRect(c, 0, 0, (unsigned)w, (unsigned)h);
    _XmPlatSetForeground(c, c2->pixel);
    /* batch all diagonal lines into a single segments request */
    int count = 0;
    for (int offset = -(h + w); offset < h + w; offset += cell * 2)
        count += cell;
    XmPlatSegment *segs = malloc(sizeof(XmPlatSegment) * (size_t)count);
    if (!segs) { _XmPlatCtxFree(c); XFreeGC(dpy, gc1); return; }
    int i = 0;
    for (int offset = -(h + w); offset < h + w; offset += cell * 2) {
        for (int d = 0; d < cell; d++) {
            int ox = offset + d;
            segs[i].x1 = ox;        segs[i].y1 = 0;
            segs[i].x2 = ox + h;    segs[i].y2 = h;
            i++;
        }
    }
    _XmPlatDrawSegments(c, segs, count);
    free(segs);
    _XmPlatCtxFree(c);
    XFreeGC(dpy, gc1);
}

static void draw_dots(Pixmap pm, int w, int h, XftColor *c1, XftColor *c2, int cell) {
    fill_with_tile(pm, w, h, c1, c2, cell, cell, cell, tile_dots);
}

static void draw_crosshatch(Pixmap pm, int w, int h, XftColor *c1, XftColor *c2, int cell) {
    GC gc1 = XCreateGC(dpy, pm, 0, NULL);
    XmPlatDrawCtx c = _XmPlatCtx(dpy, pm, gc1);
    _XmPlatSetForeground(c, c1->pixel);
    _XmPlatFillRect(c, 0, 0, (unsigned)w, (unsigned)h);
    _XmPlatSetForeground(c, c2->pixel);
    int n = 0;
    for (int offset = -(h + w); offset < h + w; offset += cell) n++;
    XmPlatSegment *segs = malloc(sizeof(XmPlatSegment) * (size_t)n * 2);
    if (!segs) { _XmPlatCtxFree(c); XFreeGC(dpy, gc1); return; }
    int i = 0;
    for (int offset = -(h + w); offset < h + w; offset += cell) {
        segs[i].x1 = offset;        segs[i].y1 = 0;
        segs[i].x2 = offset + h;    segs[i].y2 = h;
        i++;
    }
    for (int offset = -(h + w); offset < h + w; offset += cell) {
        segs[i].x1 = offset + h;    segs[i].y1 = 0;
        segs[i].x2 = offset;        segs[i].y2 = h;
        i++;
    }
    _XmPlatDrawSegments(c, segs, i);
    free(segs);
    _XmPlatCtxFree(c);
    XFreeGC(dpy, gc1);
}

static void draw_weave(Pixmap pm, int w, int h, XftColor *c1, XftColor *c2, int cell) {
    GC gc1 = XCreateGC(dpy, pm, 0, NULL);
    XmPlatDrawCtx c = _XmPlatCtx(dpy, pm, gc1);
    _XmPlatSetForeground(c, c1->pixel);
    _XmPlatFillRect(c, 0, 0, (unsigned)w, (unsigned)h);
    _XmPlatSetForeground(c, c2->pixel);
    int half = cell / 2;
    if (half < 2) half = 2;
    /* horizontal bars with gaps */
    for (int y = 0; y < h; y += cell) {
        _XmPlatFillRect(c, 0, y + half - 1, (unsigned)w, 2);
    }
    /* vertical bars with gaps, offset every other row */
    for (int x = 0; x < w; x += cell) {
        for (int y = 0; y < h; y += cell) {
            int off = ((x / cell) % 2) ? 0 : half;
            _XmPlatFillRect(c, x + half - 1, y + off, 2, (unsigned)half);
            if (off + half < cell)
                _XmPlatFillRect(c, x + half - 1, y, 2, (unsigned)off);
        }
    }
    _XmPlatCtxFree(c);
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
    {
        XmPlatDrawCtx c = _XmPlatCtx(dpy, pm, gc1);
        _XmPlatSetForeground(c, col_root_bg.pixel);
        _XmPlatFillRect(c, 0, 0, (unsigned)scr_w, (unsigned)scr_h);
        _XmPlatCtxFree(c);
    }
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
        {
            XmPlatDrawCtx c = _XmPlatCtx(dpy, pm, gc2);
            int dx = (scr_w - img_w) / 2;
            int dy = (scr_h - img_h) / 2;
            if (dx < 0) dx = 0;
            if (dy < 0) dy = 0;
            int sx = (img_w > scr_w) ? (img_w - scr_w) / 2 : 0;
            int sy = (img_h > scr_h) ? (img_h - scr_h) / 2 : 0;
            int cw = (img_w > scr_w) ? scr_w : img_w;
            int ch = (img_h > scr_h) ? scr_h : img_h;
            _XmPlatBlit(c, _XmPlatSurface(dpy, src_pm), sx, sy, dx, dy,
                        (unsigned)cw, (unsigned)ch);
            _XmPlatCtxFree(c);
        }
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
        {
            XmPlatDrawCtx c = _XmPlatCtx(dpy, pm, gc2);
            int dx = (scr_w - dw) / 2;
            int dy = (scr_h - dh) / 2;
            _XmPlatBlit(c, _XmPlatSurface(dpy, src_pm), 0, 0, dx, dy,
                        (unsigned)dw, (unsigned)dh);
            _XmPlatCtxFree(c);
        }
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
        {
            XmPlatDrawCtx c = _XmPlatCtx(dpy, pm, gc2);
            _XmPlatSetTile(c, _XmPlatSurface(dpy, src_pm));
            _XmPlatFillRectangleTiled(c, 0, 0, (unsigned)scr_w, (unsigned)scr_h);
            _XmPlatCtxFree(c);
        }
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
        {
            XmPlatDrawCtx c = _XmPlatCtx(dpy, pm, gc2);
            _XmPlatBlit(c, _XmPlatSurface(dpy, img_pm), 0, 0, 0, 0,
                        (unsigned)scr_w, (unsigned)scr_h);
            _XmPlatCtxFree(c);
        }
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
        {
            XmPlatDrawCtx c = _XmPlatCtx(dpy, pm, gc2);
            int sx = (dw - scr_w) / 2;
            int sy = (dh - scr_h) / 2;
            _XmPlatBlit(c, _XmPlatSurface(dpy, src_pm), sx, sy, 0, 0,
                        (unsigned)scr_w, (unsigned)scr_h);
            _XmPlatCtxFree(c);
        }
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
        {
            XmPlatSurface s = _XmPlatSurfaceOfWindow(dpy, root);
            _XmPlatClearWindow(s);
            _XmPlatSurfaceFree(s);
        }
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
        {
            XmPlatSurface s = _XmPlatSurfaceOfWindow(dpy, root);
            _XmPlatClearWindow(s);
            _XmPlatSurfaceFree(s);
        }
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
            {
            XmPlatSurface s = _XmPlatSurfaceOfWindow(dpy, root);
            _XmPlatClearWindow(s);
            _XmPlatSurfaceFree(s);
        }
            XDeleteProperty(dpy, root, xrootpmap);
            return;
        }
    }

    root_bg_pixmap = pm;
    XSetWindowBackgroundPixmap(dpy, root, pm);
    {
            XmPlatSurface s = _XmPlatSurfaceOfWindow(dpy, root);
            _XmPlatClearWindow(s);
            _XmPlatSurfaceFree(s);
        }

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