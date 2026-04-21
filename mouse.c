/*
 * rondo — mouse move/resize interaction
 */
#include "wm.h"

/* Draw a thick rectangle outline on the root window using the XOR GC.
 * Draws OUTLINE_THICKNESS concentric rectangles to create a thick band,
 * matching mwm's SetOutline approach. */
void draw_outline(int x, int y, int w, int h) {
    for (int i = 0; i < OUTLINE_THICKNESS; i++)
        XDrawRectangle(dpy, root, xor_gc, x + i, y + i, w - 2*i, h - 2*i);
}

void mousemove(Client *c, int button, int edge, int x_root, int y_root) {
    if (!c) return;

    /* for tiled windows: make floating first, then drag */
    if (!c->is_floating) {
        c->oldx = c->x; c->oldy = c->y;
        c->oldw = c->w; c->oldh = c->h;
        c->is_floating = 1;
        float_default_size(c);
        client_to_frame(c->w, c->h, &c->w, &c->h, c->no_decor);
        if (button == Button1) {
            /* LClick: position under cursor at title bar */
            c->x = x_root - c->w / 2;
            c->y = y_root - FRAME_WIDTH - TITLE_HEIGHT / 2;
        } else {
            /* RClick: offset slightly and resize */
            c->x = c->x + 20;
            c->y = c->y + 20;
        }
        arrange();
        moveresizeframe(c);
        int cx, cy, cw, ch;
        frame_to_client(c->w, c->h, &cx, &cy, &cw, &ch, c->no_decor);
        XMoveResizeWindow(dpy, c->win, cx, cy, cw, ch);
        drawbar();
    }

    /* if edge is none and button3, default to bottom-right resize */
    if (button == Button3 && edge == EDGE_NONE)
        edge = EDGE_SE;

    /* floating window drag (move or resize) — rubber-band outline mode
     *
     * Follows mwm's approach: grab pointer on the frame window (not root),
     * owner_events=False so all events go to the frame window,
     * confine_to=root so pointer can move anywhere on screen.
     * Use XCheckWindowEvent for motion compression and XWindowEvent
     * for blocking wait. */
    XEvent ev;
    int orig_x = c->x, orig_y = c->y;
    int orig_w = c->w, orig_h = c->h;
    int rx = x_root, ry = y_root;

    /* pick cursor for the resize direction */
    Cursor grab_cursor = None;
    if (edge != EDGE_NONE && edge > 0 && edge < 16 && curs_resize[edge])
        grab_cursor = curs_resize[edge];

    Window grab_win = XtWindow(c->frame_form);
    unsigned int grab_mask = ButtonPressMask | ButtonReleaseMask |
                             PointerMotionMask | PointerMotionHintMask;
    unsigned int config_mask = KeyPressMask | ButtonPressMask |
                              ButtonReleaseMask | PointerMotionMask;

    /* Release any existing active grab (e.g., from XGrabButton passive grab
     * activation on the client window) so our XGrabPointer can take effect */
    XUngrabPointer(dpy, CurrentTime);

    if (XGrabPointer(dpy, grab_win, False,
                     grab_mask,
                     GrabModeAsync, GrabModeAsync,
                     root, grab_cursor, CurrentTime) != GrabSuccess)
        return;

    XGrabKeyboard(dpy, grab_win, False, GrabModeAsync, GrabModeAsync, CurrentTime);

    XGrabServer(dpy);

    /* draw initial rubber-band at current position + show feedback window */
    int cur_x = orig_x, cur_y = orig_y, cur_w = orig_w, cur_h = orig_h;
    int fb_style = (button == Button1) ? 1 : 2;  /* 1=position, 2=size */
    fb_show(cur_x, cur_y, cur_w, cur_h, fb_style);
    draw_outline(cur_x, cur_y, cur_w, cur_h);
    XSync(dpy, False);

    for (;;) {
        /* mwm-style event retrieval: try XCheckWindowEvent first for
         * motion compression, then block with XWindowEvent */
        Bool got_event = False;
        while (XCheckWindowEvent(dpy, grab_win, config_mask, &ev)) {
            got_event = True;
            if (ev.type != MotionNotify)
                break;  /* stop at first non-motion event */
        }
        if (!got_event) {
            /* no events in queue — block until one arrives */
            XWindowEvent(dpy, grab_win, config_mask, &ev);
        }

        switch (ev.type) {
        case MotionNotify:
            /* if hint, query pointer for actual position (mwm-style) */
            if (ev.xmotion.is_hint == NotifyHint) {
                Window dw;
                int di;
                unsigned int du;
                XQueryPointer(dpy, grab_win, &dw, &dw,
                              &ev.xmotion.x_root, &ev.xmotion.y_root,
                              &di, &di, &du);
            }

            /* erase old outline */
            draw_outline(cur_x, cur_y, cur_w, cur_h);

            /* compute new position/size */
            if (button == Button1) {
                /* move */
                cur_x = orig_x + (ev.xmotion.x_root - rx);
                cur_y = orig_y + (ev.xmotion.y_root - ry);
                cur_w = orig_w;
                cur_h = orig_h;
            } else if (button == Button3) {
                /* directional resize */
                int dx = ev.xmotion.x_root - rx;
                int dy = ev.xmotion.y_root - ry;
                cur_x = orig_x; cur_y = orig_y;
                cur_w = orig_w; cur_h = orig_h;
                if (edge & EDGE_E) cur_w = orig_w + dx;
                if (edge & EDGE_W) { cur_x = orig_x + dx; cur_w = orig_w - dx; }
                if (edge & EDGE_S) cur_h = orig_h + dy;
                if (edge & EDGE_N) { cur_y = orig_y + dy; cur_h = orig_h - dy; }
                /* enforce minimum size from size hints */
                int min_cw = (c->size_hints_flags & PMinSize) ? c->min_width : 1;
                int min_ch = (c->size_hints_flags & PMinSize) ? c->min_height : 1;
                int min_fw, min_fh;
                client_to_frame(min_cw, min_ch, &min_fw, &min_fh, c->no_decor);
                if (cur_w < min_fw) {
                    if (edge & EDGE_W) cur_x = orig_x + orig_w - min_fw;
                    cur_w = min_fw;
                }
                if (cur_h < min_fh) {
                    if (edge & EDGE_N) cur_y = orig_y + orig_h - min_fh;
                    cur_h = min_fh;
                }
            }

            /* draw new outline + update feedback window */
            draw_outline(cur_x, cur_y, cur_w, cur_h);
            fb_update(cur_x, cur_y, cur_w, cur_h, fb_style);
            XSync(dpy, False);
            break;

        case ButtonRelease:
            /* erase outline + hide feedback */
            draw_outline(cur_x, cur_y, cur_w, cur_h);
            fb_hide();
            XSync(dpy, False);
            /* release grabs in mwm order */
            XUngrabServer(dpy);
            XUngrabKeyboard(dpy, CurrentTime);
            XUngrabPointer(dpy, CurrentTime);
            XFlush(dpy);
            /* apply final geometry */
            c->x = cur_x; c->y = cur_y; c->w = cur_w; c->h = cur_h;
            moveresizeframe(c);
            {
                int cx2, cy2, cw2, ch2;
                frame_to_client(c->w, c->h, &cx2, &cy2, &cw2, &ch2, c->no_decor);
                XMoveResizeWindow(dpy, c->win, cx2, cy2, cw2, ch2);
            }
            updateframe(c);
            send_configure_notify(c);
            return;
        }
    }
}