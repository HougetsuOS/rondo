/*
 * rondo — frame drawing and interaction
 */
#include "wm.h"

/* ── 3D bevel drawing ─────────────────────────────────────────────────── */

/* Draw a 3D raised bevel rectangle using 1-pixel scanlines.
 * tw = top bevel width, rw = right, bw = bottom, lw = left.
 * Top/left edges are drawn in light color (highlight).
 * Bottom/right edges are drawn in shadow color.
 * Uses the mwm GE1 staircase mitering algorithm. */
void bevel_rect(XftDraw *xd, int x, int y, int w, int h,
                       int tw, int rw, int bw, int lw,
                       XftColor *light, XftColor *shadow) {
    if (w <= 0 || h <= 0) return;

    /* top side — highlight */
    int join1 = lw, join2 = rw;
    for (int i = 0; i < tw; i++) {
        int x1 = x + (lw > 0 ? lw - join1 : 0);
        int len = w - (lw > 0 ? lw - join1 : 0) - (rw > 0 ? rw - join2 : 0);
        if (len > 0 && y + i < y + h)
            XftDrawRect(xd, light, x1, y + i, len, 1);
        if (join1 > 0) join1--;
        if (join2 > 0) join2--;
    }

    /* left side — highlight */
    join1 = tw; int join2b = bw;
    for (int i = 0; i < lw; i++) {
        int y1 = y + (tw > 0 ? tw - join1 : 0);
        int height = h - (tw > 0 ? tw - join1 : 0) - (bw > 0 ? bw - join2b : 0);
        if (height > 0 && x + i < x + w)
            XftDrawRect(xd, light, x + i, y1, 1, height);
        if (join1 > 0) join1--;
        if (join2b > 0) join2b--;
    }

    /* bottom side — shadow */
    join1 = lw; join2 = rw;
    for (int i = 0; i < bw; i++) {
        int x1 = x + (lw > 0 ? lw - join1 : 0);
        int len = w - (lw > 0 ? lw - join1 : 0) - (rw > 0 ? rw - join2 : 0);
        int y1 = y + h - 1 - i;
        if (len > 0 && y1 >= y)
            XftDrawRect(xd, shadow, x1, y1, len, 1);
        if (join1 > 0) join1--;
        if (join2 > 0) join2--;
    }

    /* right side — shadow */
    join1 = tw; join2b = bw;
    for (int i = 0; i < rw; i++) {
        int y1 = y + (tw > 0 ? tw - join1 : 0);
        int height = h - (tw > 0 ? tw - join1 : 0) - (bw > 0 ? bw - join2b : 0);
        int x1 = x + w - 1 - i;
        if (height > 0 && x1 >= x)
            XftDrawRect(xd, shadow, x1, y1, 1, height);
        if (join1 > 0) join1--;
        if (join2b > 0) join2b--;
    }
}

/* Draw a 3D recessed bevel (shadow on top/left, light on bottom/right) */
void bevel_rect_inv(XftDraw *xd, int x, int y, int w, int h,
                           int tw, int rw, int bw, int lw,
                           XftColor *light, XftColor *shadow) {
    /* Inverted: swap light and shadow */
    bevel_rect(xd, x, y, w, h, tw, rw, bw, lw, shadow, light);
}

/* ── StretcherCorner — ported from mwm WmGraphics.c ────────────────── */

/* Draw an L-shaped corner piece that seamlessly miter-joins two
 * adjacent border edges.  Ported from mwm's StretcherCorner()
 * (WmGraphics.c line 499).  Uses only 1-pixel bevels — hard coded.
 *
 * swidth = border width (stretcher width)
 * cwidth, cheight = corner rectangle dimensions
 * light = top/left shadow colour (highlight)
 * shadow = bottom/right shadow colour
 */
void stretcher_corner(XftDraw *xd, int x, int y, int cnum,
                             int swidth, int cwidth, int cheight,
                             XftColor *light, XftColor *shadow) {
    /* helper: draw a rect only if it has positive dimensions */
    #define R(col, rx, ry, rw, rh) do { \
        if ((rw) > 0 && (rh) > 0) XftDrawRect(xd, col, rx, ry, rw, rh); \
    } while(0)

    switch (cnum) {
    case STRETCH_NW:
        /* top shadow (highlight) */
        R(light, x,          y,         cwidth, 1);           /* row 1 */
        R(light, x+1,        y+1,       cwidth-2, 1);         /* row 2 */
        R(light, x,          y+1,       1, cheight-1);        /* col 1 */
        R(light, x+1,        y+2,       1, cheight-3);        /* col 2 */
        /* bottom shadow */
        R(shadow, x+1,       y+cheight-1, swidth-1, 1);      /* bottom end */
        R(shadow, x+swidth-1, y+swidth-1, 1, cheight-swidth);/* right inside */
        R(shadow, x+swidth,   y+swidth-1, cwidth-swidth, 1); /* bottom inside */
        R(shadow, x+cwidth-1, y+1,       1, swidth-2);       /* right end */
        break;

    case STRETCH_NE:
        /* top shadow (highlight) */
        R(light, x,              y,         cwidth, 1);       /* row 1 */
        R(light, x+1,            y+1,       cwidth-2, 1);     /* row 2 */
        R(light, x,              y+1,       1, swidth-1);     /* left end */
        R(light, x+cwidth-swidth, y+swidth, 1, cheight-swidth); /* left inside */
        /* bottom shadow */
        R(shadow, x+cwidth-swidth+1, y+cheight-1, swidth-1, 1);  /* bottom end */
        R(shadow, x+cwidth-1,   y+1,       1, cheight-2);    /* right col 2 */
        R(shadow, x+cwidth-2,   y+2,       1, cheight-3);    /* right col 1 */
        R(shadow, x+1,          y+swidth-1, cwidth-swidth, 1); /* bottom inside */
        break;

    case STRETCH_SE:
        /* top shadow (highlight) — WmRECESSED inside lines first */
        R(light, x,                y+cheight-swidth, cwidth-swidth+1, 1);  /* top inside */
        R(light, x+cwidth-swidth,  y,               1, cheight-swidth);   /* left inside */
        R(light, x+cwidth-swidth+1, y,              swidth-2, 1);         /* top end */
        R(light, x,                y+cheight-swidth+1, 1, swidth-2);      /* left end */
        /* bottom shadow */
        R(shadow, x+1,       y+cheight-2, cwidth-1, 1);      /* bottom row 1 */
        R(shadow, x,         y+cheight-1, cwidth, 1);         /* bottom row 2 */
        R(shadow, x+cwidth-2, y+1,       1, cheight-3);      /* right col 1 */
        R(shadow, x+cwidth-1, y,         1, cheight-2);      /* right col 2 */
        break;

    case STRETCH_SW:
        /* top shadow (highlight) */
        R(light, x,        y,         swidth, 1);             /* top end */
        R(light, x,        y+1,       1, cheight-1);          /* left col 1 */
        R(light, x+1,      y+1,       1, cheight-2);          /* left col 2 */
        R(light, x+swidth, y+cheight-swidth, cwidth-swidth, 1); /* top inside */
        /* bottom shadow — WmRECESSED inside line first */
        R(shadow, x+swidth-1, y+1,       1, cheight-swidth);  /* right inside */
        R(shadow, x+cwidth-1, y+cheight-swidth+1, 1, swidth-1); /* right end */
        R(shadow, x+2,       y+cheight-2, cwidth-3, 1);       /* bottom row 1 */
        R(shadow, x+1,       y+cheight-1, cwidth-2, 1);       /* bottom row 2 */
        break;
    }
    #undef R
}

/* ── frame hit-testing ───────────────────────────────────────────────── */

int frame_button_hit(Client *c, int x, int y) {
    /* no_decor: no buttons or title bar */
    if (c->no_decor) return BTN_NONE;

    /* Title bar area: from (FRAME_WIDTH, FRAME_WIDTH) with height TITLE_HEIGHT */
    int tb_y = FRAME_WIDTH;
    int tb_h = TITLE_HEIGHT;
    if (y < tb_y || y >= tb_y + tb_h) return BTN_NONE;
    if (x < FRAME_WIDTH || x >= c->w - FRAME_WIDTH) return BTN_NONE;

    /* Menu button on the left side of title bar */
    if (x >= FRAME_WIDTH && x < FRAME_WIDTH + BTN_WIDTH) return BTN_MENU;

    /* Buttons at the right side of title bar — skip disabled buttons */
    int btn_w = BTN_WIDTH;
    int right_edge = c->w - FRAME_WIDTH;
    if (x >= right_edge - btn_w && x < right_edge) return BTN_CLOSE;
    right_edge -= btn_w;
    if (!c->no_maximize) {
        if (x >= right_edge - btn_w && x < right_edge) return BTN_MAX;
        right_edge -= btn_w;
    }
    if (!c->no_minimize) {
        if (x >= right_edge - btn_w && x < right_edge) return BTN_MIN;
        right_edge -= btn_w;
    }
    if (x >= right_edge - btn_w && x < right_edge) return BTN_FLOAT;

    return BTN_TITLE;
}

int frame_edge_hit(Client *c, int x, int y) {
    /* Only floating windows have user-grabbable borders */
    if (!c->is_floating) return EDGE_NONE;
    /* Windows with no_resize flag can't be resized via border handles */
    if (c->no_resize) return EDGE_NONE;

    /* Buttons take priority over border — don't show resize cursor on buttons */
    if (frame_button_hit(c, x, y) != BTN_NONE) return EDGE_NONE;

    int fw = FRAME_WIDTH;
    int cs = fw + TITLE_HEIGHT; /* corner size (matches stretcher corners) */
    int w = c->w, h = c->h;

    /* Corners */
    if (x < cs && y < cs)                  return EDGE_NW;
    if (x >= w - cs && y < cs)             return EDGE_NE;
    if (x < cs && y >= h - cs)             return EDGE_SW;
    if (x >= w - cs && y >= h - cs)        return EDGE_SE;

    /* Edges */
    if (y < fw)          return EDGE_N;
    if (y >= h - fw)     return EDGE_S;
    if (x < fw)          return EDGE_W;
    if (x >= w - fw)     return EDGE_E;

    return EDGE_NONE;
}

/* ── draw frame ──────────────────────────────────────────────────────── */

void drawframe(Client *c) {
    if (!c || !c->frame_draw) return;

    int is_focused = (c == focused);
    XftColor *title_col = is_focused ? &col_title_focus : &col_title_unfocus;
    XftColor *title_light = is_focused ? &col_active_light : &col_frame_light;
    XftColor *title_shadow = is_focused ? &col_active_shadow : &col_frame_shadow;
    XftColor *border_light = title_light;
    XftColor *border_shadow = title_shadow;
    int fw = c->w, fh = c->h;
    XftDraw *xd = c->frame_draw;

    /* no_decor: no title bar, no border decorations, no buttons */
    if (c->no_decor) {
        /* draw a thin 1px border for visual distinction */
        XftDrawRect(xd, &col_frame_bg, 0, 0, fw, fh);
        XftDrawRect(xd, border_shadow, 0, 0, fw, 1);       /* top */
        XftDrawRect(xd, border_shadow, 0, fh - 1, fw, 1);  /* bottom */
        XftDrawRect(xd, border_shadow, 0, 0, 1, fh);        /* left */
        XftDrawRect(xd, border_shadow, fw - 1, 0, 1, fh);   /* right */
        XSync(dpy, False);
        return;
    }

    int bw = FRAME_WIDTH;

    /* 1. Fill entire frame with title color (border + title bar use same color) */
    XftDrawRect(xd, title_col, 0, 0, fw, fh);

    /* 2. Border — mwm WmRECESSED style: 4 StretcherCorner pieces + 4 edge
     * BevelRectangles with per-edge asymmetric widths:
     *   N: (2,1,1,1)  E: (1,2,1,1)  S: (1,1,2,1)  W: (1,1,1,2)
     * This produces the characteristic thick-outer-edge look. */

    /* 4 corner pieces — mwm L-shaped corners
     * cornerSize = TITLE_HEIGHT + FRAME_WIDTH (border + title bar height) */
    int cs = TITLE_HEIGHT + bw;  /* corner width/height */
    stretcher_corner(xd, 0, 0, STRETCH_NW, bw, cs, cs, border_light, border_shadow);
    stretcher_corner(xd, fw - cs, 0, STRETCH_NE, bw, cs, cs, border_light, border_shadow);
    stretcher_corner(xd, fw - cs, fh - cs, STRETCH_SE, bw, cs, cs, border_light, border_shadow);
    stretcher_corner(xd, 0, fh - cs, STRETCH_SW, bw, cs, cs, border_light, border_shadow);

    /* 4 edge strips — mwm per-edge bevel widths.
     * N/S edges fill between corners (border height only).
     * E/W edges fill between corners (border width only). */
    if (fw > 2 * cs)
        bevel_rect(xd, cs, 0, fw - 2*cs, bw, 2, 1, 1, 1, border_light, border_shadow);  /* N */
    if (fh > 2 * cs)
        bevel_rect(xd, fw - bw, cs, bw, fh - 2*cs, 1, 2, 1, 1, border_light, border_shadow);  /* E */
    if (fw > 2 * cs)
        bevel_rect(xd, cs, fh - bw, fw - 2*cs, bw, 1, 1, 2, 1, border_light, border_shadow);  /* S */
    if (fh > 2 * cs)
        bevel_rect(xd, 0, cs, bw, fh - 2*cs, 1, 1, 1, 2, border_light, border_shadow);  /* W */

    /* 3. Title row — fill entire row with title bg color */
    int tb_x = bw;
    int tb_y = bw;
    int tb_w = fw - 2 * bw;
    int tb_h = TITLE_HEIGHT;

    /* Build button list — skip disabled buttons based on MWM hints */
    int btn_count = 0;
    int btn_ids[4];
    if (1)                              btn_ids[btn_count++] = BTN_CLOSE;
    if (!c->no_maximize)                btn_ids[btn_count++] = BTN_MAX;
    if (!c->no_minimize)                btn_ids[btn_count++] = BTN_MIN;
    if (1)                              btn_ids[btn_count++] = BTN_FLOAT;

    /* Button positions (right-aligned in title row) */
    int btn_y = tb_y;
    int btn_h = tb_h;
    int btn_w = BTN_WIDTH;
    int right_edge = fw - bw;

    int btn_positions[4];
    for (int i = 0; i < btn_count; i++)
        btn_positions[i] = right_edge - (i + 1) * btn_w;

    if (tb_w > 0 && tb_h > 0) {
        /* Fill entire title row with title bg color */
        XftDrawRect(xd, title_col, tb_x, tb_y, tb_w, tb_h);
    }

    /* Title text area — raised (or depressed) button */
    int title_btn_x = tb_x + BTN_WIDTH;  /* skip menu button */
    int title_btn_w = btn_positions[btn_count - 1] - title_btn_x;
    if (title_btn_w > 0 && tb_h > 0) {
        if (c->pressed_btn == BTN_TITLE)
            bevel_rect_inv(xd, title_btn_x, tb_y, title_btn_w, tb_h,
                           1, 1, 1, 1, title_light, title_shadow);
        else
            bevel_rect(xd, title_btn_x, tb_y, title_btn_w, tb_h,
                       1, 1, 1, 1, title_light, title_shadow);
    }

    /* 4. Menu button on left side of title bar */
    {
        int bx = tb_x;  /* left-aligned */
        if (c->pressed_btn == BTN_MENU)
            bevel_rect_inv(xd, bx, btn_y, btn_w, btn_h, 1, 1, 1, 1, title_light, title_shadow);
        else
            bevel_rect(xd, bx, btn_y, btn_w, btn_h, 1, 1, 1, 1, title_light, title_shadow);
        /* Minus sign icon */
        int ix = bx + 4;
        int iy = btn_y + (btn_h - 2) / 2;
        int iw = btn_w - 8;
        if (iw > 0) {
            if (c->pressed_btn == BTN_MENU)
                bevel_rect_inv(xd, ix, iy, iw, 2, 1, 1, 1, 1, title_light, title_shadow);
            else
                bevel_rect(xd, ix, iy, iw, 2, 1, 1, 1, 1, title_light, title_shadow);
        }
    }

    /* 5. Title bar buttons (right side) — each is its own raised (or depressed) button */
    for (int i = 0; i < btn_count; i++) {
        int bx = btn_positions[i];
        if (c->pressed_btn == btn_ids[i])
            bevel_rect_inv(xd, bx, btn_y, btn_w, btn_h, 1, 1, 1, 1, title_light, title_shadow);
        else
            bevel_rect(xd, bx, btn_y, btn_w, btn_h, 1, 1, 1, 1, title_light, title_shadow);
    }

    /* Button icons — mwm-style beveled rectangles */
    for (int i = 0; i < btn_count; i++) {
        int bx = btn_positions[i];
        switch (btn_ids[i]) {
        case BTN_CLOSE: {
            /* Close: raised outer square with sunken inner square */
            int outer_off = 3;
            int outer_dim = btn_h - 6;
            int inner_off = 6;
            int inner_dim = btn_h - 12;
            if (outer_dim > 0) {
                if (c->pressed_btn == BTN_CLOSE)
                    bevel_rect_inv(xd, bx + outer_off, btn_y + outer_off,
                                   outer_dim, outer_dim, 1, 1, 1, 1,
                                   title_light, title_shadow);
                else
                    bevel_rect(xd, bx + outer_off, btn_y + outer_off,
                               outer_dim, outer_dim, 1, 1, 1, 1,
                               title_light, title_shadow);
                if (inner_dim > 0)
                    bevel_rect_inv(xd, bx + inner_off, btn_y + inner_off,
                                   inner_dim, inner_dim, 1, 1, 1, 1,
                                   title_light, title_shadow);
            }
            break;
        }
        case BTN_MAX: {
            /* Maximize: large centered square (sunken when fullscreen) */
            int offset = 4;
            int idim = btn_h - 8;
            if (idim > 0) {
                int inverted = (c->is_fullscreen || c->pressed_btn == BTN_MAX);
                if (inverted)
                    bevel_rect_inv(xd, bx + offset, btn_y + offset,
                                   idim, idim, 1, 1, 1, 1,
                                   title_light, title_shadow);
                else
                    bevel_rect(xd, bx + offset, btn_y + offset,
                               idim, idim, 1, 1, 1, 1,
                               title_light, title_shadow);
            }
            break;
        }
        case BTN_MIN: {
            /* Minimize: small centered square */
            int offset = (btn_h - 4) / 2;
            int idim = 4;
            if (c->pressed_btn == BTN_MIN)
                bevel_rect_inv(xd, bx + offset, btn_y + offset,
                               idim, idim, 1, 1, 1, 1, title_light, title_shadow);
            else
                bevel_rect(xd, bx + offset, btn_y + offset,
                           idim, idim, 1, 1, 1, 1, title_light, title_shadow);
            break;
        }
        case BTN_FLOAT: {
            /* Float: two overlapping small squares (like CDE restore icon) */
            int s = 6;
            int off = 3;
            int pressed = (c->pressed_btn == BTN_FLOAT);
            if (pressed)
                bevel_rect_inv(xd, bx + off, btn_y + off,
                               s, s, 1, 1, 1, 1, title_light, title_shadow);
            else
                bevel_rect(xd, bx + off, btn_y + off,
                           s, s, 1, 1, 1, 1, title_shadow, title_light);
            XftDrawRect(xd, title_col, bx, btn_y, s, s);
            if (pressed)
                bevel_rect_inv(xd, bx, btn_y,
                               s, s, 1, 1, 1, 1, title_light, title_shadow);
            else
                bevel_rect(xd, bx, btn_y,
                           s, s, 1, 1, 1, 1, title_light, title_shadow);
            break;
        }
        }
    }

    /* 6. Title text — inside the title button, inset by bevel + padding */
    {
        int text_left = title_btn_x + 3;
        int text_right = title_btn_x + title_btn_w - 3;
        int max_w = text_right - text_left;
        if (max_w > 0) {
            int namelen = (int)strlen(c->name);
            XGlyphInfo ext;
            XftTextExtents8(dpy, xftfont, (XftChar8 *)c->name, namelen, &ext);
            while (namelen > 0 && ext.xOff > max_w) {
                namelen--;
                XftTextExtents8(dpy, xftfont, (XftChar8 *)c->name, namelen, &ext);
            }
            if (namelen > 0) {
                int text_y = tb_y + (tb_h + xftfont->ascent - xftfont->descent) / 2;
                XftDrawStringUtf8(xd, &col_title_fg, xftfont,
                                  text_left, text_y,
                                  (XftChar8 *)c->name, namelen);
            }
        }
    }

    XSync(dpy, False);
}

/* ── update frame ──────────────────────────────────────────────────── */

/* ── move/resize frame ────────────────────────────────────────────────── */

/* Move and resize the frame shell AND its XmForm child.
 * XMoveResizeWindow alone only resizes the shell's X window — the XmForm
 * child stays at its initial (small) size, causing tiled windows to appear
 * tiny.  We must also resize the form's X window to match. */
void moveresizeframe(Client *c) {
    if (!c || !c->frame_shell) return;
    XMoveResizeWindow(dpy, XtWindow(c->frame_shell), c->x, c->y, c->w, c->h);
    if (c->frame_form)
        XResizeWindow(dpy, XtWindow(c->frame_form), c->w, c->h);
}

void updateframe(Client *c) {
    if (!c || !c->frame_shell) return;
    moveresizeframe(c);
    drawframe(c);
}

/* ── frame event callbacks ─────────────────────────────────────────── */

void frame_expose_cb(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont) {
    (void)w; (void)cont;
    Client *c = (Client *)client_data;
    if (ev->type == Expose && ev->xexpose.count == 0)
        drawframe(c);
}

void frame_enter_cb(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont) {
    (void)w; (void)cont;
    Client *c = (Client *)client_data;
    if (c->ws != curws) return;
    if (ev->type == EnterNotify) {
        focus(c);
        /* set cursor based on where the pointer entered */
        int edge = frame_edge_hit(c, ev->xcrossing.x, ev->xcrossing.y);
        if (edge != EDGE_NONE && edge > 0 && edge < 16 && curs_resize[edge])
            XDefineCursor(dpy, XtWindow(c->frame_form), curs_resize[edge]);
        else
            XDefineCursor(dpy, XtWindow(c->frame_form), curs_default);
    }
}

void frame_leave_cb(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont) {
    (void)w; (void)cont;
    Client *c = (Client *)client_data;
    if (ev->type == LeaveNotify)
        XDefineCursor(dpy, XtWindow(c->frame_form), curs_default);
}

void frame_motion_cb(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont) {
    (void)w; (void)cont;
    Client *c = (Client *)client_data;
    if (ev->type == MotionNotify && c->pressed_btn == BTN_NONE) {
        int edge = frame_edge_hit(c, ev->xmotion.x, ev->xmotion.y);
        if (edge != EDGE_NONE && edge > 0 && edge < 16 && curs_resize[edge])
            XDefineCursor(dpy, XtWindow(c->frame_form), curs_resize[edge]);
        else
            XDefineCursor(dpy, XtWindow(c->frame_form), curs_default);
    }
}

void frame_btn_cb(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont) {
    (void)w;
    Client *c = (Client *)client_data;

    if (ev->type == ButtonPress) {
        XButtonEvent *bev = &ev->xbutton;

        /* Alt+click — handle move/resize (immediate, no press/release) */
        if (bev->state & MODKEY) {
            if (bev->button == Button1 || bev->button == Button3) {
                *cont = False;
                /* Release Xt's implicit grab from ButtonPress dispatch
                 * so our XGrabPointer in mousemove() can take effect */
                XtUngrabPointer(w, bev->time);
                XRaiseWindow(dpy, XtWindow(c->frame_shell));
                focus(c);
                if (!c->is_floating && bev->button == Button1) {
                    /* Alt+LClick on tiled: make floating, then drag from title */
                    WmArg arg = {0};
                    togglefloat(&arg);
                    c->pressed_btn = BTN_TITLE;
                    drawframe(c);
                    mousemove(c, Button1, EDGE_NONE, bev->x_root, bev->y_root);
                    c->pressed_btn = BTN_NONE;
                    drawframe(c);
                } else {
                    mousemove(c, bev->button, EDGE_NONE, bev->x_root, bev->y_root);
                }
                return;
            }
        }

        /* Determine which button/title was hit — check before border,
         * since buttons sit inside the border area on floating windows */
        int hit = frame_button_hit(c, bev->x, bev->y);
        if (hit != BTN_NONE) {
            *cont = False;

            if (hit == BTN_MENU) {
                /* Show mwm-style window menu on press */
                c->pressed_btn = BTN_MENU;
                drawframe(c);
                /* Release Xt's implicit grab so run_menu's XGrabPointer can take effect */
                XtUngrabPointer(w, bev->time);
                int menu_x = c->x + FRAME_WIDTH;
                int menu_y = c->y + FRAME_WIDTH + TITLE_HEIGHT;
                show_window_menu(c, menu_x, menu_y);
                c->pressed_btn = BTN_NONE;
                drawframe(c);
                return;
            }

            if (hit == BTN_TITLE) {
                /* Show pressed state, then handle drag/release */
                c->pressed_btn = hit;
                drawframe(c);
                XRaiseWindow(dpy, XtWindow(c->frame_shell));
                focus(c);
                if (c->is_floating) {
                    XtUngrabPointer(w, bev->time);
                    mousemove(c, Button1, EDGE_NONE, bev->x_root, bev->y_root);
                    c->pressed_btn = BTN_NONE;
                    drawframe(c);
                }
                return;
            }

            /* Set pressed state and redraw */
            c->pressed_btn = hit;
            drawframe(c);

            /* Grab pointer so we track release even outside the window */
            XGrabPointer(dpy, XtWindow(c->frame_form), True,
                          ButtonReleaseMask | PointerMotionMask,
                          GrabModeAsync, GrabModeAsync, None, None, bev->time);
            return;
        }

        /* Check for border resize handle (floating only) */
        int edge = frame_edge_hit(c, bev->x, bev->y);
        if (edge != EDGE_NONE) {
            *cont = False;
            XtUngrabPointer(w, bev->time);
            XRaiseWindow(dpy, XtWindow(c->frame_shell));
            focus(c);
            mousemove(c, Button3, edge, bev->x_root, bev->y_root);
            return;
        }

        /* Click in border area that's not a button or resize handle — ignore */
        *cont = False;
    }
    else if (ev->type == ButtonRelease) {
        int hit = c->pressed_btn;
        c->pressed_btn = BTN_NONE;
        XUngrabPointer(dpy, ev->xbutton.time);
        drawframe(c);

        /* Check if release is still over the same button */
        int cur_hit = frame_button_hit(c, ev->xbutton.x, ev->xbutton.y);
        if (cur_hit != hit) return;  /* released outside the button — cancel */

        switch (hit) {
        case BTN_CLOSE:
            if (!focused || focused != c) focus(c);
            killclient(NULL);
            break;
        case BTN_MAX:
            togglefullscreen(NULL);
            break;
        case BTN_MIN:
            minimize_client(c);
            break;
        case BTN_FLOAT:
            if (!focused || focused != c) focus(c);
            togglefloat(NULL);
            break;
        case BTN_TITLE:
            break;  /* handled on press */
        }
    }
    else if (ev->type == MotionNotify) {
        /* While button is held, update pressed state based on position */
        if (c->pressed_btn == BTN_NONE) return;
        int hit = frame_button_hit(c, ev->xmotion.x, ev->xmotion.y);
        if (hit != c->pressed_btn) {
            /* Pointer moved off the pressed button — show unpressed */
            c->pressed_btn = BTN_NONE;
            drawframe(c);
        }
        /* Note: we don't re-set pressed_btn if pointer comes back,
         * matching standard Motif pushbutton behavior (once you leave, it cancels) */
    }
}