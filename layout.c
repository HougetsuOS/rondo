/*
 * rondo — layout: master-stack tiling
 */
#include "wm.h"

/* Apply only min/max size constraints for tiled windows.
 * PResizeInc and PAspect are for interactive resizing (character grid
 * snapping) and create gaps when applied to tiled layouts. */
static void apply_size_hints_tiled(Client *c, int *w, int *h) {
    if (c->size_hints_flags & PMinSize) {
        if (*w < c->min_width)  *w = c->min_width;
        if (*h < c->min_height) *h = c->min_height;
    }
    if (c->size_hints_flags & PMaxSize) {
        if (*w > c->max_width)  *w = c->max_width;
        if (*h > c->max_height) *h = c->max_height;
    }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

void arrange(void) {
    int n = tiledcount();
    if (n == 0) return;

    BarGeometry g = calc_bar_geometry();

    int mx = g.x;
    int my = g.y;
    int mw = g.w;
    int mh = g.h;

    Client *c = nexttiled(clients);
    if (!c) return;

    if (n == 1) {
        c->x = mx; c->y = my; c->w = mw; c->h = mh;
        moveresizeframe(c);
        int cx, cy, cw, ch;
        frame_to_client(c->w, c->h, &cx, &cy, &cw, &ch, c->no_decor);
        apply_size_hints_tiled(c, &cw, &ch);
        client_to_frame(cw, ch, &c->w, &c->h, c->no_decor);
        moveresizeframe(c);
        frame_to_client(c->w, c->h, &cx, &cy, &cw, &ch, c->no_decor);
        XMoveResizeWindow(dpy, c->win, cx, cy, cw, ch);
        updateframe(c);
        send_configure_notify(c);
        return;
    }

    /* master area */
    int master_w = (int)(mw * mfact);
    int stack_w  = mw - master_w;

    c->x = mx; c->y = my;
    c->w = master_w; c->h = mh;
    {
        int cx, cy, cw, ch;
        frame_to_client(c->w, c->h, &cx, &cy, &cw, &ch, c->no_decor);
        apply_size_hints_tiled(c, &cw, &ch);
        client_to_frame(cw, ch, &c->w, &c->h, c->no_decor);
    }
    moveresizeframe(c);
    {
        int cx, cy, cw, ch;
        frame_to_client(c->w, c->h, &cx, &cy, &cw, &ch, c->no_decor);
        XMoveResizeWindow(dpy, c->win, cx, cy, cw, ch);
    }
    updateframe(c);
    send_configure_notify(c);

    /* stack area */
    c = nexttiled(c->next);
    int sy = my;

    for (; c; c = nexttiled(c->next)) {
        /* each stack window fills its portion of the remaining space */
        int remaining = mh - (sy - my);
        int windows_left = 0;
        for (Client *t = c; t; t = nexttiled(t->next)) windows_left++;
        int h = remaining / windows_left;

        c->x = mx + master_w;
        c->y = sy;
        c->w = stack_w;
        c->h = h;
        int cx, cy, cw, ch;
        frame_to_client(c->w, c->h, &cx, &cy, &cw, &ch, c->no_decor);
        apply_size_hints_tiled(c, &cw, &ch);
        client_to_frame(cw, ch, &c->w, &c->h, c->no_decor);
        moveresizeframe(c);
        frame_to_client(c->w, c->h, &cx, &cy, &cw, &ch, c->no_decor);
        XMoveResizeWindow(dpy, c->win, cx, cy, cw, ch);
        updateframe(c);
        send_configure_notify(c);
        sy += c->h;
    }
}