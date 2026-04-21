/*
 * rondo — keybinding action handlers
 */
#include "wm.h"

void spawn(const WmArg *arg) {
    const char **cmd = (const char **)arg->v;
    if (fork() == 0) {
        if (dpy) close(ConnectionNumber(dpy));
        setsid();
        execvp(cmd[0], (char *const *)cmd);
        fprintf(stderr, "rondo: execvp '%s' failed: %s\n", cmd[0], strerror(errno));
        _exit(127);
    }
}

static void kill_do_close(Client *c) {
    if (c->delete_window) {
        XEvent ev = { .type = ClientMessage };
        ev.xclient.window = c->win;
        ev.xclient.message_type = wm_protocols;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = (long)wm_delete_window;
        ev.xclient.data.l[1] = (long)last_event_time;
        XSendEvent(dpy, c->win, False, NoEventMask, &ev);
    } else {
        XKillClient(dpy, c->win);
    }
}

void killclient(const WmArg *arg) {
    (void)arg;
    if (!focused) return;
    if (fade_enabled) {
        fade_window_out(focused, kill_do_close);
    } else {
        kill_do_close(focused);
    }
}

void focusstack(const WmArg *arg) {
    int dir = arg->i;
    if (!focused) return;
    Client *c = focused;
    do {
        c = (dir > 0) ? (c->next ? c->next : clients) : clients;
        if (c == focused) return;
    } while (c->ws != curws || c->is_minimized);
    focus(c);
}

void cyclewindows(const WmArg *arg) {
    int dir = arg->i;
    if (!focused) {
        /* nothing focused — pick first visible window on this workspace */
        for (Client *c = clients; c; c = c->next)
            if (c->ws == curws && !c->is_minimized && !c->is_hidden) {
                focus(c);
                XRaiseWindow(dpy, c->frame_shell ? XtWindow(c->frame_shell) : c->win);
                return;
            }
        return;
    }
    Client *c = focused;
    do {
        if (dir > 0) {
            c = c->next ? c->next : clients;
        } else {
            /* walk backwards: find previous client in list */
            Client *prev = NULL;
            for (Client *p = clients; p; p = p->next)
                if (p->next == c) { prev = p; break; }
            c = prev ? prev : clients;
            /* if we wrapped to the tail, find the last client */
            if (!prev) {
                for (Client *p = clients; p; p = p->next)
                    if (!p->next) { c = p; break; }
            }
        }
        if (c == focused) return;
    } while (c->ws != curws || c->is_minimized || c->is_hidden);
    focus(c);
    XRaiseWindow(dpy, c->frame_shell ? XtWindow(c->frame_shell) : c->win);
}

void lowerwindow(const WmArg *arg) {
    (void)arg;
    if (!focused || focused->ws != curws) return;
    XLowerWindow(dpy, focused->frame_shell ? XtWindow(focused->frame_shell) : focused->win);
}

void togglefloat(const WmArg *arg) {
    (void)arg;
    if (!focused || focused->ws != curws) return;

    /* if fullscreen, un-fullscreen first */
    if (focused->is_fullscreen) {
        focused->is_fullscreen = 0;
        focused->x = focused->oldx; focused->y = focused->oldy;
        focused->w = focused->oldw; focused->h = focused->oldh;
    }

    if (!focused->is_floating) {
        focused->oldx = focused->x;
        focused->oldy = focused->y;
        focused->oldw = focused->w;
        focused->oldh = focused->h;
        focused->is_floating = 1;
        /* give a reasonable floating size — cascade position */
        float_default_size(focused);
        client_to_frame(focused->w, focused->h, &focused->w, &focused->h, focused->no_decor);
        /* count existing floating windows to determine cascade offset */
        int idx = 0;
        for (Client *p = clients; p; p = p->next)
            if (p != focused && p->is_floating && p->ws == curws) idx++;
        BarGeometry g = calc_bar_geometry();
        int max_x = g.x + g.w - focused->w;
        int max_y = g.y + g.h - focused->h;
        int base_x = g.x + CASCADE_BASE;
        int base_y = g.y + CASCADE_BASE;
        /* wrap cascade back inside screen when it would go off-screen */
        focused->x = base_x + (idx * CASCADE_STEP) % (max_x > base_x ? max_x - base_x : CASCADE_STEP + 1);
        focused->y = base_y + (idx * CASCADE_STEP) % (max_y > base_y ? max_y - base_y : CASCADE_STEP + 1);
        moveresizeframe(focused);
        int cx, cy, cw, ch;
        frame_to_client(focused->w, focused->h, &cx, &cy, &cw, &ch, focused->no_decor);
        XMoveResizeWindow(dpy, focused->win, cx, cy, cw, ch);
        send_configure_notify(focused);
        XRaiseWindow(dpy, XtWindow(focused->frame_shell));
    } else {
        focused->is_floating = 0;
    }
    arrange();
    drawbar();
}

void incmaster(const WmArg *arg) {
    float delta = arg->f;
    mfact += delta;
    if (mfact < 0.1) mfact = 0.1;
    if (mfact > 0.9) mfact = 0.9;
    arrange();
}

void zoom(const WmArg *arg) {
    (void)arg;
    if (!focused || focused->is_floating || focused->ws != curws) return;
    Client *master = nexttiled(clients);
    if (!master || master == focused) return;

    /* swap focused and master in the linked list */
    Client *ap = NULL, *bp = NULL;
    for (Client *p = clients; p; p = p->next) {
        if (p->next == master) ap = p;
        if (p->next == focused) bp = p;
    }
    if (bp && bp != master) bp->next = master;
    if (ap && ap != focused) ap->next = focused;
    Client *tmp = master->next;
    master->next = focused->next;
    focused->next = tmp;
    if (clients == master) clients = focused;
    else if (clients == focused) clients = master;

    focus(master);
    arrange();
}

void togglefullscreen(const WmArg *arg) {
    (void)arg;
    if (!focused) return;
    focused->is_fullscreen = !focused->is_fullscreen;
    if (focused->is_fullscreen) {
        focused->oldx = focused->x; focused->oldy = focused->y;
        focused->oldw = focused->w; focused->oldh = focused->h;
        BarGeometry g = calc_bar_geometry();
        focused->x = g.x;
        focused->y = g.y;
        focused->w = g.w;
        focused->h = g.h;
        moveresizeframe(focused);
        int cx, cy, cw, ch;
        frame_to_client(focused->w, focused->h, &cx, &cy, &cw, &ch, focused->no_decor);
        XMoveResizeWindow(dpy, focused->win, cx, cy, cw, ch);
        XRaiseWindow(dpy, XtWindow(focused->frame_shell));
        updateframe(focused);
        send_configure_notify(focused);
        XChangeProperty(dpy, focused->win, net_wm_state, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)&net_wm_state_fullscreen, 1);
    } else {
        focused->x = focused->oldx; focused->y = focused->oldy;
        focused->w = focused->oldw; focused->h = focused->oldh;
        moveresizeframe(focused);
        int cx, cy, cw, ch;
        frame_to_client(focused->w, focused->h, &cx, &cy, &cw, &ch, focused->no_decor);
        XMoveResizeWindow(dpy, focused->win, cx, cy, cw, ch);
        updateframe(focused);
        send_configure_notify(focused);
        arrange();
        XChangeProperty(dpy, focused->win, net_wm_state, XA_ATOM, 32,
                        PropModeReplace, NULL, 0);
    }
}

static void view_hide_cb(Client *c) {
    /* only unmap if still hidden (could be restored during fade-out) */
    if (!c->is_hidden) return;
    XtUnmapWidget(c->frame_shell);
}

void viewworkspace(const WmArg *arg) {
    unsigned int ws = arg->ui - 1;  /* config uses 1-indexed, internal is 0-indexed */
    if (ws == (unsigned)curws) return;

    for (Client *c = clients; c; c = c->next)
        if (c->ws == curws && !c->is_minimized) {
            c->is_hidden = 1;
            set_wm_state(c, IconicState);
            fade_window_out(c, view_hide_cb);
        }

    curws = ws;

    for (Client *c = clients; c; c = c->next)
        if (c->ws == curws && !c->is_minimized) {
            c->is_hidden = 0;
            set_wm_state(c, NormalState);
            XtMapWidget(c->frame_shell);
            fade_window_in(c);
            /* update _NET_WM_DESKTOP for visible clients */
            long desktop = (long)c->ws;
            XChangeProperty(dpy, c->win, net_wm_desktop, XA_CARDINAL, 32,
                            PropModeReplace, (unsigned char *)&desktop, 1);
        }

    /* update _NET_CURRENT_DESKTOP */
    update_net_desktops();
    update_workarea();

    arrange();
    focus(nexttiled(clients));
    updateiconbar();
}

static void move_hide_cb(Client *c) {
    /* only unmap if still hidden (could be restored during fade-out) */
    if (!c->is_hidden) return;
    XtUnmapWidget(c->frame_shell);
}

void movetoworkspace(const WmArg *arg) {
    unsigned int ws = arg->ui - 1;  /* config uses 1-indexed, internal is 0-indexed */
    if (!focused || ws == (unsigned)focused->ws) return;

    focused->ws = ws;
    focused->is_hidden = 1;
    set_wm_state(focused, IconicState);
    fade_window_out(focused, move_hide_cb);
    /* update _NET_WM_DESKTOP for the moved client */
    long desktop = (long)ws;
    XChangeProperty(dpy, focused->win, net_wm_desktop, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&desktop, 1);
    focus(nexttiled(clients));
    arrange();
    updateiconbar();
}

void quit(const WmArg *arg) {
    (void)arg;
    running = 0;
}

void swapbar(const WmArg *arg) {
    (void)arg;
    /* toggle bar visibility */
    show_bar = !show_bar;
    BarGeometry g = calc_bar_geometry();
    if (show_bar) {
        XMoveResizeWindow(dpy, barwin, g.bar_x, g.bar_y,
                          g.bar_w > 0 ? (unsigned)g.bar_w : 1,
                          g.bar_h > 0 ? (unsigned)g.bar_h : 1);
        XMapWindow(dpy, barwin);
        drawbar();
    } else {
        XUnmapWindow(dpy, barwin);
    }
    updateiconbar();
}