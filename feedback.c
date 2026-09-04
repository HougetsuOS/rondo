/*
 * rondo — mwm-style feedback window (coordinate/size display)
 */
#include "wm.h"

/* cached per-show text metrics — reused by fb_update every motion event */
static int fb_line_w[2];
static int fb_last_max_w;
static int fb_nlines_cached;

/* Show the feedback window centered on screen.
 * style: 1 = position (move), 2 = size (resize). */
void fb_show(int x, int y, int w, int h, int style) {
    /* format text */
    char line1[32], line2[32];
    line1[0] = line2[0] = '\0';
    int nlines = 0;

    if (style & 1) {  /* FB_POSITION */
        snprintf(line1, sizeof line1, "(%4d,%-4d)", x, y);
        nlines = 1;
    }
    if (style & 2) {  /* FB_SIZE */
        if (nlines)
            snprintf(line2, sizeof line2, "%4dx%-4d", w, h);
        else
            snprintf(line1, sizeof line1, "%4dx%-4d", w, h);
        nlines = (style & 1) ? 2 : 1;
    }

    /* measure text using Xft — use xOff (advance width) for sizing/centering.
     * Metrics are cached in fb_line_w[] / fb_last_max_w because the fixed-width
     * format makes them identical on every fb_update. */
    XGlyphInfo ext;
    int max_w = 0;
    const char *lines[2] = { line1, line2 };
    for (int i = 0; i < nlines; i++) {
        XftTextExtentsUtf8(dpy, xftfont, (const FcChar8 *)lines[i],
                           (int)strlen(lines[i]), &ext);
        fb_line_w[i] = ext.xOff;
        if (ext.xOff > max_w) max_w = ext.xOff;
    }
    fb_last_max_w = max_w;
    int line_h = xftfont->ascent + xftfont->descent;
    int bw = max_w + 2 * FB_PAD + 2 * fb_bevel;
    int bh = nlines * line_h + 2 * FB_PAD + 2 * fb_bevel;
    fb_nlines_cached = nlines;
    fb_win_w = bw;
    fb_win_h = bh;

    /* center on monitor */
    int wx = mon.x + (mon.w - bw) / 2;
    int wy = mon.y + (mon.h - bh) / 2;

    if (!fb_win) {
        /* create override-redirect + save-under window */
        XSetWindowAttributes wa;
        wa.override_redirect = True;
        wa.save_under = True;
        wa.background_pixel = col_fb_bg.pixel;
        wa.event_mask = ExposureMask;
        fb_win = XCreateWindow(dpy, root, wx, wy, bw, bh, 0,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWSaveUnder |
                               CWBackPixel | CWEventMask, &wa);
        fb_draw = XftDrawCreate(dpy, fb_win, xvisual, xcolormap);
    } else {
        XMoveResizeWindow(dpy, fb_win, wx, wy, bw, bh);
    }

    /* paint background + bevel + text */
    XftDrawRect(fb_draw, &col_fb_bg, 0, 0, bw, bh);
    bevel_rect(fb_draw, 0, 0, bw, bh, fb_bevel, fb_bevel, fb_bevel, fb_bevel,
               &col_fb_light, &col_fb_shadow);

    for (int i = 0; i < nlines; i++) {
        XGlyphInfo lext;
        XftTextExtentsUtf8(dpy, xftfont, (const FcChar8 *)lines[i],
                           (int)strlen(lines[i]), &lext);
        int tx = fb_bevel + FB_PAD + (max_w - lext.xOff) / 2;
        int ty = fb_bevel + FB_PAD + xftfont->ascent + i * line_h;
        XftDrawStringUtf8(fb_draw, &col_fb_fg, xftfont,
                          tx, ty,
                          (const FcChar8 *)lines[i], (int)strlen(lines[i]));
    }

    XRaiseWindow(dpy, fb_win);
    XMapWindow(dpy, fb_win);
    XFlush(dpy);
}

/* Update text in the feedback window (no recreate, just repaint). */
void fb_update(int x, int y, int w, int h, int style) {
    if (!fb_win) return;

    char line1[32], line2[32];
    line1[0] = line2[0] = '\0';
    int nlines = 0;

    if (style & 1) {
        snprintf(line1, sizeof line1, "(%4d,%-4d)", x, y);
        nlines = 1;
    }
    if (style & 2) {
        if (nlines)
            snprintf(line2, sizeof line2, "%4dx%-4d", w, h);
        else
            snprintf(line1, sizeof line1, "%4dx%-4d", w, h);
        nlines = (style & 1) ? 2 : 1;
    }

    const char *lines[2] = { line1, line2 };

    /* clear interior and repaint.
     * The text format is fixed-width ("(%4d,%-4d)" / "%4dx%-4d"), so
     * metrics measured at fb_show time are exact for every update —
     * measure once, reuse every motion event. */
    XftDrawRect(fb_draw, &col_fb_bg,
                fb_bevel, fb_bevel,
                fb_win_w - 2 * fb_bevel, fb_win_h - 2 * fb_bevel);
    bevel_rect(fb_draw, 0, 0, fb_win_w, fb_win_h,
               fb_bevel, fb_bevel, fb_bevel, fb_bevel,
               &col_fb_light, &col_fb_shadow);

    int fb_max_w = fb_last_max_w;
    int line_h = xftfont->ascent + xftfont->descent;

    for (int i = 0; i < nlines; i++) {
        int tx = fb_bevel + FB_PAD + (fb_max_w - fb_line_w[i]) / 2;
        int ty = fb_bevel + FB_PAD + xftfont->ascent + i * line_h;
        XftDrawStringUtf8(fb_draw, &col_fb_fg, xftfont,
                          tx, ty,
                          (const FcChar8 *)lines[i], (int)strlen(lines[i]));
    }
    XFlush(dpy);
}

/* Hide the feedback window. */
void fb_hide(void) {
    if (fb_win) {
        XUnmapWindow(dpy, fb_win);
        XFlush(dpy);
    }
}