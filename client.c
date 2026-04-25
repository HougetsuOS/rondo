/*
 * rondo — client management: manage, unmanage, focus, helpers
 */
#include "wm.h"
#include <limits.h>

/* ── client helpers ─────────────────────────────────────────────────── */

Client *wintoclient(Window w) {
    for (Client *c = clients; c; c = c->next) {
        if (c->win == w) return c;
        if (XtWindow(c->frame_shell) == w) return c;
        if (XtWindow(c->frame_form) == w) return c;
    }
    return NULL;
}

Client *nexttiled(Client *c) {
    for (; c; c = c->next)
        if (c->ws == curws && !c->is_floating && !c->is_fullscreen && !c->is_minimized) return c;
    return NULL;
}

int tiledcount(void) {
    int n = 0;
    for (Client *c = clients; c; c = c->next)
        if (c->ws == curws && !c->is_floating && !c->is_fullscreen && !c->is_minimized) n++;
    return n;
}

/* ── window name ─────────────────────────────────────────────────────── */

void updatewindowname(Client *c) {
    XTextProperty prop;
    /* Try _NET_WM_NAME (UTF-8) first */
    if (XGetTextProperty(dpy, c->win, &prop, net_wm_name_atom) && prop.value) {
        strncpy(c->name, (char *)prop.value, sizeof(c->name) - 1);
        c->name[sizeof(c->name) - 1] = '\0';
        XFree(prop.value);
    } else if (XGetWMName(dpy, c->win, &prop) && prop.value) {
        strncpy(c->name, (char *)prop.value, sizeof(c->name) - 1);
        c->name[sizeof(c->name) - 1] = '\0';
        XFree(prop.value);
    } else {
        c->name[0] = '\0';
    }
}

/* ── ICCCM helpers ────────────────────────────────────────────────────── */

int client_supports_protocol(Client *c, Atom protocol) {
    Atom *protocols;
    int count, found = 0;
    if (XGetWMProtocols(dpy, c->win, &protocols, &count)) {
        for (int i = 0; i < count; i++)
            if (protocols[i] == protocol) { found = 1; break; }
        XFree(protocols);
    }
    return found;
}

void set_wm_state(Client *c, int state) {
    long icon = (state == IconicState && c->icon_window != None) ? (long)c->icon_window : None;
    long data[2] = { state, icon };
    XChangeProperty(dpy, c->win, wm_state, wm_state, 32,
                    PropModeReplace, (unsigned char *)data, 2);
}

void send_configure_notify(Client *c) {
    Window child;
    int cx_root, cy_root;
    XTranslateCoordinates(dpy, c->win, root, 0, 0, &cx_root, &cy_root, &child);
    int cx, cy, cw, ch;
    frame_to_client(c->w, c->h, &cx, &cy, &cw, &ch, c->no_decor);
    XConfigureEvent ce = {
        .type          = ConfigureNotify,
        .event         = c->win,
        .window        = c->win,
        .x             = cx_root,
        .y             = cy_root,
        .width         = cw,
        .height        = ch,
        .border_width  = 0,
        .above         = None,
        .override_redirect = False
    };
    XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

void read_size_hints(Client *c) {
    XSizeHints *sh = XAllocSizeHints();
    long supplied = 0;
    /* set safe defaults before reading hints */
    c->size_hints_flags = 0;
    c->min_width = 1;  c->min_height = 1;
    c->max_width = INT_MAX; c->max_height = INT_MAX;
    c->width_inc = 1;   c->height_inc = 1;
    c->base_width = 0;  c->base_height = 0;
    c->min_aspect_x = 1; c->min_aspect_y = 1;
    c->max_aspect_x = 1; c->max_aspect_y = 1;
    if (!XGetWMNormalHints(dpy, c->win, sh, &supplied)) {
        XFree(sh);
        return;
    }
    c->size_hints_flags = (int)supplied;
    if (supplied & PMinSize) {
        c->min_width = sh->min_width;
        c->min_height = sh->min_height;
    }
    if (supplied & PMaxSize) {
        /* Some clients (e.g. xterm) set PMaxSize with max_width=0/max_height=0,
         * which is nonsensical — treat as "no maximum". */
        if (sh->max_width > 0 && sh->max_height > 0) {
            c->max_width = sh->max_width;
            c->max_height = sh->max_height;
        } else {
            c->size_hints_flags &= ~PMaxSize;
        }
    }
    if (supplied & PResizeInc) {
        c->width_inc = sh->width_inc;
        c->height_inc = sh->height_inc;
    }
    if (supplied & PBaseSize) {
        c->base_width = sh->base_width;
        c->base_height = sh->base_height;
    }
    if (supplied & PAspect) {
        c->min_aspect_x = sh->min_aspect.x;
        c->min_aspect_y = sh->min_aspect.y;
        c->max_aspect_x = sh->max_aspect.x;
        c->max_aspect_y = sh->max_aspect.y;
    }
    XFree(sh);
}

void apply_size_hints(Client *c, int *w, int *h) {
    if (c->size_hints_flags & PMinSize) {
        if (*w < c->min_width) *w = c->min_width;
        if (*h < c->min_height) *h = c->min_height;
    }
    if (c->size_hints_flags & PMaxSize) {
        if (*w > c->max_width) *w = c->max_width;
        if (*h > c->max_height) *h = c->max_height;
    }
    if (c->size_hints_flags & PResizeInc) {
        if (c->width_inc > 1)
            *w = c->base_width + ((*w - c->base_width) / c->width_inc) * c->width_inc;
        if (c->height_inc > 1)
            *h = c->base_height + ((*h - c->base_height) / c->height_inc) * c->height_inc;
    }
    if (c->size_hints_flags & PAspect) {
        if (c->min_aspect_y > 0 && c->max_aspect_y > 0) {
            double ratio = (double)(*w) / (double)(*h);
            double min_r = (double)c->min_aspect_x / (double)c->min_aspect_y;
            double max_r = (double)c->max_aspect_x / (double)c->max_aspect_y;
            if (ratio < min_r) *w = (int)((double)(*h) * min_r);
            else if (ratio > max_r) *h = (int)((double)(*w) / max_r);
        }
    }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

/* ── minimize / restore ───────────────────────────────────────────────── */

static void minimize_do_unmap(Client *c) {
    /* only unmap if still minimized (not restored during fade-out) */
    if (!c->is_minimized) return;
    XtUnmapWidget(c->frame_shell);
    if (focused == c) focus(nexttiled(clients));
    iconbar_scroll = 0;
    updateiconbar();
}

void minimize_client(Client *c) {
    if (!c || c->is_minimized) return;
    c->is_minimized = 1;
    btree_remove(c);
    set_wm_state(c, IconicState);
    fade_window_out(c, minimize_do_unmap);
}

void restore_client(Client *c) {
    if (!c || !c->is_minimized) return;
    c->is_minimized = 0;
    if (!c->is_floating && !c->is_hidden)
        btree_add(c);
    set_wm_state(c, NormalState);
    XtMapWidget(c->frame_shell);
    fade_window_in(c);
    focus(c);
    updateiconbar();
}

/* ── floating geometry ───────────────────────────────────────────────── */

void float_default_size(Client *c) {
    BarGeometry g = calc_bar_geometry();
    c->w = g.w / 2;
    c->h = g.h / 2;
}

/* ── focus / unfocus ─────────────────────────────────────────────────── */

void unfocus(Client *c) {
    if (!c) return;
    if (c->cmap != None)
        XUninstallColormap(dpy, c->cmap);
    focused = NULL;
    update_active_window();
    drawframe(c);
}

void focus(Client *c) {
    if (!c || c->ws != curws || c->is_minimized)
        for (c = clients; c && (c->ws != curws || c->is_minimized); c = c->next);
    if (!c) {
        XSetInputFocus(dpy, root, RevertToPointerRoot, last_event_time);
        focused = NULL;
        update_active_window();
        return;
    }
    if (focused == c) return;
    if (focused) unfocus(focused);
    focused = c;
    Time ts = (last_event_time != CurrentTime) ? last_event_time : CurrentTime;
    if (c->take_focus) {
        XEvent ev = { .type = ClientMessage };
        ev.xclient.window = c->win;
        ev.xclient.message_type = wm_protocols;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = (long)wm_take_focus;
        ev.xclient.data.l[1] = (long)ts;
        XSendEvent(dpy, c->win, False, NoEventMask, &ev);
    }
    if (c->input_hint)
        XSetInputFocus(dpy, c->win, RevertToPointerRoot, ts);
    if (c->cmap != None)
        XInstallColormap(dpy, c->cmap);
    update_active_window();
    updateframe(c);
    drawbar();
}

/* ── manage / unmanage ──────────────────────────────────────────────── */

void manage(Window w, XWindowAttributes *wa) {
    if (wintoclient(w)) return;

    Client *c = calloc(1, sizeof(Client));
    if (!c) die("rondo: out of memory\n");

    c->win = w;
    c->ws = curws;
    c->is_floating = 1;
    c->is_fullscreen = 0;
    c->is_minimized = 0;
    c->is_hidden = 0;
    c->no_decor = 0;
    c->no_resize = 0;
    c->no_minimize = 0;
    c->no_maximize = 0;
    c->icon_pixmap = None;
    c->icon_mask = None;
    c->take_focus = 0;
    c->delete_window = 0;
    c->input_hint = 1;  /* ICCCM default */
    c->opacity = 0xFFFFFFFF;
    c->fading = 0;
    c->fade_timer = 0;
    c->fade_done_cb = NULL;

    /* transient windows are floating */
    Window trans = None;
    if (XGetTransientForHint(dpy, w, &trans) && trans != None)
        c->is_floating = 1;

    /* compute frame geometry from client's requested size */
    int fw, fh;
    client_to_frame(wa->width, wa->height, &fw, &fh, c->no_decor);

    /* position floating windows below the bar, cascaded */
    float_default_size(c);
    client_to_frame(c->w, c->h, &fw, &fh, c->no_decor);
    int idx = 0;
    for (Client *p = clients; p; p = p->next)
        if (p != c && p->is_floating && p->ws == curws) idx++;
    BarGeometry g = calc_bar_geometry();
    c->x = g.x + CASCADE_BASE + idx * CASCADE_STEP;
    c->y = g.y + CASCADE_BASE + idx * CASCADE_STEP;
    c->w = fw;
    c->h = fh;
    c->oldx = c->x; c->oldy = c->y; c->oldw = c->w; c->oldh = c->h;

    /* create Motif frame shell */
    c->frame_shell = XtVaAppCreateShell("frame", "RondoWm",
        overrideShellWidgetClass, dpy,
        XtNx, c->x, XtNy, c->y,
        XtNwidth, c->w, XtNheight, c->h,
        XtNoverrideRedirect, True,
        XtNmappedWhenManaged, False,
        NULL);

    /* create XmForm as frame container — no shadow, we draw manually */
    c->frame_form = XtVaCreateManagedWidget("frameForm",
        xmFormWidgetClass, c->frame_shell,
        XmNresizePolicy, XmRESIZE_NONE,
        XmNshadowThickness, 0,
        XmNbackground, col_title_unfocus.pixel,
        NULL);

    /* realize the shell (creates X windows) */
    XtRealizeWidget(c->frame_shell);

    /* create Xft draw context for manual frame drawing */
    c->frame_draw = XftDrawCreate(dpy, XtWindow(c->frame_form), xvisual, xcolormap);

    /* set override_redirect on the realized shell window */
    XSetWindowAttributes swa = { .override_redirect = True };
    XChangeWindowAttributes(dpy, XtWindow(c->frame_shell), CWOverrideRedirect, &swa);

    /* select events on the frame shell window — merge with Xt's existing mask */
    XWindowAttributes fwa;
    XGetWindowAttributes(dpy, XtWindow(c->frame_shell), &fwa);
    XSelectInput(dpy, XtWindow(c->frame_shell),
        fwa.your_event_mask | SubstructureRedirectMask | EnterWindowMask);

    /* select events on frame_form for expose and button clicks */
    XGetWindowAttributes(dpy, XtWindow(c->frame_form), &fwa);
    XSelectInput(dpy, XtWindow(c->frame_form),
        fwa.your_event_mask | ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask);

    /* reparent client into the form's window */
    XSetWindowBorderWidth(dpy, w, 0);
    {
        int reparent_x = c->no_decor ? 0 : FRAME_WIDTH;
        int reparent_y = c->no_decor ? 0 : FRAME_WIDTH + TITLE_HEIGHT;
        XReparentWindow(dpy, w, XtWindow(c->frame_form), reparent_x, reparent_y);
    }
    /* resize client to fit inside the frame */
    {
        int cx, cy, cw, ch;
        frame_to_client(c->w, c->h, &cx, &cy, &cw, &ch, c->no_decor);
        XResizeWindow(dpy, w, cw, ch);
    }

    /* select events on client */
    XSelectInput(dpy, w, EnterWindowMask | FocusChangeMask |
                    PropertyChangeMask | StructureNotifyMask);

    /* grab Alt+Button1/3 on client window so we can handle
     * Alt+click-to-move/resize from anywhere on the window */
    XGrabButton(dpy, Button1, MODKEY, w, False, ButtonPressMask,
                GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button3, MODKEY, w, False, ButtonPressMask,
                GrabModeAsync, GrabModeAsync, None, None);

    /* add event handlers for expose, focus, and button clicks */
    XtAddEventHandler(c->frame_form, ExposureMask, True,
                      frame_expose_cb, c);
    XtAddEventHandler(c->frame_form, EnterWindowMask, False,
                      frame_enter_cb, c);
    XtAddEventHandler(c->frame_form, LeaveWindowMask, False,
                      frame_leave_cb, c);
    XtAddEventHandler(c->frame_form, PointerMotionMask, False,
                      frame_motion_cb, c);
    XtAddEventHandler(c->frame_form,
                      ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                      False, frame_btn_cb, c);

    /* read window title */
    updatewindowname(c);

    /* read WM_HINTS (icon, input hint, initial state) */
    XWMHints *hints = XGetWMHints(dpy, w);
    if (hints) {
        if (hints->flags & InputHint)
            c->input_hint = hints->input;
        if (hints->flags & IconPixmapHint)
            c->icon_pixmap = hints->icon_pixmap;
        if (hints->flags & IconMaskHint)
            c->icon_mask = hints->icon_mask;
        if (hints->flags & IconWindowHint)
            c->icon_window = hints->icon_window;
        if (hints->flags & StateHint && hints->initial_state == IconicState)
            c->is_minimized = 1;
        XFree(hints);
    }

    /* query icon pixmap dimensions */
    c->icon_w = 0;
    c->icon_h = 0;
    if (c->icon_pixmap != None) {
        Window _root;
        int _x, _y;
        unsigned int _bw, _depth;
        XGetGeometry(dpy, c->icon_pixmap, &_root, &_x, &_y,
                     (unsigned int *)&c->icon_w, (unsigned int *)&c->icon_h,
                     &_bw, &_depth);
    }

    /* read client colormap */
    c->cmap = wa->colormap;

    /* read ICCCM protocols and size hints */
    c->take_focus = client_supports_protocol(c, wm_take_focus);
    c->delete_window = client_supports_protocol(c, wm_delete_window);
    read_size_hints(c);

    /* check _NET_WM_WINDOW_TYPE — skip unmanaged types, float dialogs/splash */
    {
        Atom actual_type;
        int actual_format;
        unsigned long nitems, bytes_after;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, w, net_wm_window_type, 0, 1024, False,
                               XA_ATOM, &actual_type, &actual_format,
                               &nitems, &bytes_after, &data) == Success && data) {
            Atom *atoms = (Atom *)data;
            for (unsigned long i = 0; i < nitems; i++) {
                if (atoms[i] == net_wm_window_type_dock ||
                    atoms[i] == net_wm_window_type_toolbar ||
                    atoms[i] == net_wm_window_type_utility ||
                    atoms[i] == net_wm_window_type_popup_menu ||
                    atoms[i] == net_wm_window_type_dropdown_menu ||
                    atoms[i] == net_wm_window_type_tooltip ||
                    atoms[i] == net_wm_window_type_notification) {
                    XFree(data);
                    free(c);
                    return; /* don't manage these window types */
                }
                if (atoms[i] == net_wm_window_type_dialog)
                    c->is_floating = 1;
                if (atoms[i] == net_wm_window_type_splash) {
                    c->is_floating = 1;
                    c->no_decor = 1;
                    c->no_resize = 1;
                }
            }
            XFree(data);
        }
    }

    /* read _MOTIF_WM_HINTS — decorations and functions */
    {
        Atom actual_type;
        int actual_format;
        unsigned long nitems, bytes_after;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, w, motif_wm_hints, 0, 5, False,
                               motif_wm_hints, &actual_type, &actual_format,
                               &nitems, &bytes_after, &data) == Success && data && nitems >= 3) {
            long *hints = (long *)data;
            long flags = hints[0];
            if (flags & MWM_HINTS_FUNCTIONS) {
                long funcs = hints[1];
                if (!(funcs & MWM_FUNC_ALL)) {
                    if (!(funcs & MWM_FUNC_RESIZE))   c->no_resize = 1;
                    if (!(funcs & MWM_FUNC_MINIMIZE))  c->no_minimize = 1;
                    if (!(funcs & MWM_FUNC_MAXIMIZE))  c->no_maximize = 1;
                }
            }
            if (flags & MWM_HINTS_DECORATIONS) {
                long decors = hints[2];
                if (decors == 0)
                    c->no_decor = 1;
            }
            XFree(data);
        }
    }

    /* prepend to list */
    c->next = clients;
    clients = c;

    /* set _NET_WM_DESKTOP on client window */
    long desktop = c->ws;
    XChangeProperty(dpy, c->win, net_wm_desktop, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&desktop, 1);

    /* update EWMH client list */
    update_client_list();

    if (c->is_minimized) {
        set_wm_state(c, IconicState);
        /* don't map — client starts minimized */
        updateiconbar();
    } else if (c->ws == curws) {
        set_wm_state(c, NormalState);
        if (fade_enabled)
            set_opacity(XtWindow(c->frame_shell), 0);
        /* Make frame windows have no background so redirected windows
         * don't flash their X server background on top of composited output */
        XSetWindowBackgroundPixmap(dpy, XtWindow(c->frame_shell), None);
        XSetWindowBackgroundPixmap(dpy, XtWindow(c->frame_form), None);
        XtPopup(c->frame_shell, XtGrabNone);
        XMapWindow(dpy, w);
        compositor_manage_client(c);
        fade_window_in(c);
        if (c->is_floating) {
            moveresizeframe(c);
            send_configure_notify(c);
        }
        if (!c->is_floating)
            btree_add(c);
        arrange();
        updateframe(c);
        drawbar();
        focus(c);
    } else {
        set_wm_state(c, IconicState);
    }
}

/* callback after fade-out on client destroy — finish the unmanage */
void unmanage_destroyed_cb(Client *c) {
    if (c->fade_timer) {
        XtRemoveTimeOut(c->fade_timer);
        c->fade_timer = (XtIntervalId)0;
    }
    compositor_untrack_window(XtWindow(c->frame_shell));
    btree_remove(c);
    /* Destroy frame draw context and widgets */
    if (c->frame_draw) {
        XftDrawDestroy(c->frame_draw);
        c->frame_draw = NULL;
    }
    XtDestroyWidget(c->frame_shell);
    free(c);
    arrange();
    updateiconbar();
}

void unmanage(Client *c, int destroyed) {
    if (!c) return;
    /* cancel any in-progress fade */
    if (c->fade_timer) {
        XtRemoveTimeOut(c->fade_timer);
        c->fade_timer = (XtIntervalId)0;
    }
    /* untrack frame_shell from compositor */
    compositor_untrack_window(XtWindow(c->frame_shell));
    /* remove from btree layout */
    btree_remove(c);
    /* Remove from client list first */
    if (clients == c) {
        clients = c->next;
    } else {
        for (Client *p = clients; p; p = p->next) {
            if (p->next == c) { p->next = c->next; break; }
        }
    }
    /* Refocus before destroying the frame */
    if (focused == c)
        focus(nexttiled(clients));
    /* Update EWMH client list and active window */
    update_client_list();
    update_active_window();
    if (!destroyed) {
        set_wm_state(c, WithdrawnState);
        XReparentWindow(dpy, c->win, root, 0, 0);
        XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
        XDeleteProperty(dpy, c->win, net_wm_desktop);
    }
    /* Destroy frame draw context and widgets */
    if (c->frame_draw) {
        XftDrawDestroy(c->frame_draw);
        c->frame_draw = NULL;
    }
    XtDestroyWidget(c->frame_shell);
    free(c);
    arrange();
    updateiconbar();
}

/* ── keybindings ────────────────────────────────────────────────────── */

void grabkeys(void) {
    KeyCode code;
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    for (int i = 0; i < num_keys; i++) {
        code = XKeysymToKeycode(dpy, keys[i].keysym);
        XGrabKey(dpy, code, keys[i].mod, root, True, GrabModeAsync, GrabModeAsync);
        XGrabKey(dpy, code, keys[i].mod | LockMask, root, True, GrabModeAsync, GrabModeAsync);
        XGrabKey(dpy, code, keys[i].mod | Mod2Mask, root, True, GrabModeAsync, GrabModeAsync);
        XGrabKey(dpy, code, keys[i].mod | Mod2Mask | LockMask, root, True, GrabModeAsync, GrabModeAsync);
    }
}