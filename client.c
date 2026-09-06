/*
 * rondo — client management: manage, unmanage, focus, helpers
 */
#include "wm.h"
#include <limits.h>

/* ── client helpers ─────────────────────────────────────────────────── */

/* Window→Client hash: every event that needs a client lookup goes through
 * wintoclient, so the O(N) list scan is replaced with O(1) probes.
 * Each client registers its client window, frame shell window and
 * frame form window. */
#define WC_HASH_BITS 10
#define WC_HASH_SIZE (1 << WC_HASH_BITS)
#define WC_SENTINEL  0x1  /* mark deleted slots (open addressing) */

typedef struct { Window win; Client *c; } WcEntry;
static WcEntry wc_table[WC_HASH_SIZE];
static int wc_count = 0;

static unsigned int wc_hash(Window w) {
    unsigned int h = (unsigned int)(w ^ (w >> 16));
    h *= 2654435761u;
    return (h >> (32 - WC_HASH_BITS)) & (WC_HASH_SIZE - 1);
}

static void wc_insert(Window w, Client *c) {
    unsigned int i = wc_hash(w);
    while (wc_table[i].win && wc_table[i].win != WC_SENTINEL)
        i = (i + 1) & (WC_HASH_SIZE - 1);
    wc_table[i].win = w;
    wc_table[i].c = c;
    wc_count++;
}

static void wc_remove(Window w) {
    unsigned int i = wc_hash(w);
    while (wc_table[i].win) {
        if (wc_table[i].win == w) {
            wc_table[i].win = WC_SENTINEL;
            wc_table[i].c = NULL;
            wc_count--;
            return;
        }
        i = (i + 1) & (WC_HASH_SIZE - 1);
    }
}

static void wc_rehash(void) {
    memset(wc_table, 0, sizeof(wc_table));
    wc_count = 0;
    for (Client *c = clients; c; c = c->next) {
        if (c->win) wc_insert(c->win, c);
        if (c->frame_shell) wc_insert(XtWindow(c->frame_shell), c);
        if (c->frame_form)  wc_insert(XtWindow(c->frame_form), c);
    }
}

Client *wintoclient(Window w) {
    if (!w) return NULL;
    /* rebuild lazily after bulk changes (reload etc.) */
    if (wc_count < 0) wc_rehash();
    unsigned int i = wc_hash(w);
    while (wc_table[i].win) {
        if (wc_table[i].win == w) return wc_table[i].c;
        i = (i + 1) & (WC_HASH_SIZE - 1);
    }
    /* not found — fall back to a full scan and refresh the table;
     * handles widgets realized after registration (rare) */
    for (Client *c = clients; c; c = c->next) {
        if (c->win == w) { wc_insert(c->win, c); return c; }
        if (c->frame_shell && XtWindow(c->frame_shell) == w) { wc_insert(w, c); return c; }
        if (c->frame_form && XtWindow(c->frame_form) == w)   { wc_insert(w, c); return c; }
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
    char newname[sizeof(c->name)];
    newname[0] = '\0';
    /* Try _NET_WM_NAME (UTF-8) first */
    if (XGetTextProperty(dpy, c->win, &prop, net_wm_name_atom) && prop.value) {
        strncpy(newname, (char *)prop.value, sizeof(newname) - 1);
        newname[sizeof(newname) - 1] = '\0';
        XFree(prop.value);
    } else if (XGetWMName(dpy, c->win, &prop) && prop.value) {
        strncpy(newname, (char *)prop.value, sizeof(newname) - 1);
        newname[sizeof(newname) - 1] = '\0';
        XFree(prop.value);
    }
    /* skip redraw when the title is unchanged — chatty apps rewrite
     * WM_NAME constantly (progress, cwd, etc.) */
    if (strcmp(newname, c->name) == 0)
        return;
    strncpy(c->name, newname, sizeof(c->name) - 1);
    c->name[sizeof(c->name) - 1] = '\0';
    /* invalidate cached name extents */
    c->name_ext_font = NULL;
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
    /* client window sits at a fixed offset inside the frame — compute
     * root coordinates arithmetically (no XTranslateCoordinates round trip) */
    int cx, cy, cw, ch;
    frame_to_client(c->w, c->h, &cx, &cy, &cw, &ch, c->no_decor);
    XConfigureEvent ce = {
        .type          = ConfigureNotify,
        .event         = c->win,
        .window        = c->win,
        .x             = c->x + cx,
        .y             = c->y + cy,
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
    defer_schedule();
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
}

/* ── floating geometry ───────────────────────────────────────────────── */

/* Compute the frame size for making a window floating.
 * Uses the client size in c->req_width/req_height (set by the caller:
 * the initial map size in manage, the current tiled size on un-float),
 * clamped to the app's WM_SIZE_HINTS min/max and capped at half the
 * usable area so a huge requested size doesn't swallow the screen. */
void float_default_size(Client *c) {
    BarGeometry g = calc_bar_geometry();
    int cw = c->req_width, ch = c->req_height;

    /* clamp to the app's own min/max hints */
    if (c->size_hints_flags & PMinSize) {
        if (cw < c->min_width)  cw = c->min_width;
        if (ch < c->min_height) ch = c->min_height;
    }
    if (c->size_hints_flags & PMaxSize) {
        if (cw > c->max_width)  cw = c->max_width;
        if (ch > c->max_height) ch = c->max_height;
    }
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;

    /* cap at half the usable area (but never below the app's minimum) */
    int cap_w = g.w / 2, cap_h = g.h / 2;
    if (cap_w < c->min_width)  cap_w = c->min_width;
    if (cap_h < c->min_height) cap_h = c->min_height;
    if (cw > cap_w) cw = cap_w;
    if (ch > cap_h) ch = cap_h;

    /* convert client size to frame size and store */
    client_to_frame(cw, ch, &c->w, &c->h, c->no_decor);
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
    defer_schedule();
}

/* ── manage / unmanage ──────────────────────────────────────────────── */

/* window-type flags set at the top of manage() before widgets are built */
static int wtype_dialog, wtype_splash;

void manage(Window w, XWindowAttributes *wa) {
    if (wintoclient(w)) return;

    /* check _NET_WM_WINDOW_TYPE FIRST — skip unmanaged types before
     * creating any widgets/windows (was previously done after the frame
     * was built, leaking the frame on early return) */
    {
        Atom actual_type;
        int actual_format;
        unsigned long nitems, bytes_after;
        unsigned char *data = NULL;
        int is_dialog = 0, is_splash = 0;
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
                    return; /* don't manage these window types */
                }
                if (atoms[i] == net_wm_window_type_dialog)
                    is_dialog = 1;
                if (atoms[i] == net_wm_window_type_splash)
                    is_splash = 1;
            }
            XFree(data);
        }
        if (is_splash) is_dialog = 1;
        wtype_dialog = is_dialog;
        wtype_splash = is_splash;
    }

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

    /* apply window-type flags now — before size hints / geometry so a
     * splash's no_decor is reflected in the initial frame size */
    if (wtype_dialog)
        c->is_floating = 1;
    if (wtype_splash) {
        c->is_floating = 1;
        c->no_decor = 1;
        c->no_resize = 1;
    }

    /* transient windows are floating */
    Window trans = None;
    if (XGetTransientForHint(dpy, w, &trans) && trans != None)
        c->is_floating = 1;

    /* read size hints BEFORE computing geometry — the requested floating
     * size must respect the app's min/max (GTK apps like Inkscape set
     * PMinSize to their natural content size; squeezing below it or
     * forcing a bigger window than wanted breaks their layout) */
    read_size_hints(c);

    /* remember the app's requested client size for float_default_size */
    c->req_width = wa->width;
    c->req_height = wa->height;

    /* position floating windows below the bar, cascaded.
     * The app's requested size is honored, clamped to its own size
     * hints and capped at half the usable area; size hints were read
     * above so min/max are known here. */
    float_default_size(c);
    int fw = c->w, fh = c->h;
    int idx = 0;
    for (Client *p = clients; p; p = p->next)
        if (p != c && p->is_floating && p->ws == curws) idx++;
    BarGeometry g = calc_bar_geometry();
    int base_x = g.x + CASCADE_BASE;
    int base_y = g.y + CASCADE_BASE;
    int max_x = g.x + g.w - fw;
    int max_y = g.y + g.h - fh;
    /* wrap cascade so windows stay within the usable area */
    c->x = base_x + (idx * CASCADE_STEP) % (max_x > base_x ? max_x - base_x : CASCADE_STEP + 1);
    c->y = base_y + (idx * CASCADE_STEP) % (max_y > base_y ? max_y - base_y : CASCADE_STEP + 1);
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

    /* read ICCCM protocols and size hints — one XGetWMProtocols fetch
     * covers both take_focus and delete_window */
    {
        Atom *protocols;
        int count;
        c->take_focus = 0;
        c->delete_window = 0;
        if (XGetWMProtocols(dpy, c->win, &protocols, &count)) {
            for (int i = 0; i < count; i++) {
                if (protocols[i] == wm_take_focus)   c->take_focus = 1;
                if (protocols[i] == wm_delete_window) c->delete_window = 1;
            }
            XFree(protocols);
        }
    }
    /* size hints were already read before geometry computation above;
     * window-type flags were also applied before geometry (splash
     * no_decor must be known before frame sizing) */

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
                /* decors == 0 with MWM_DECOR_ALL unset: MWM says "none",
                 * but GTK/toolkits write decorations=0 to mean "no
                 * opinion — use the WM default" (Inkscape and friends
                 * map frameless otherwise). Only honor an explicit
                 * request when MWM_DECOR_ALL is NOT the reason... i.e.
                 * treat decors==0 as no_decor only when the FUNCTIONS
                 * flag is also present and asked for something — real
                 * frameless requests (splash screens, dock panels) set
                 * both. Plain {DECORATIONS,0} alone is ignored. */
                if (decors == 0 && (flags & MWM_HINTS_FUNCTIONS))
                    c->no_decor = 1;
            }
            XFree(data);
        }
    }

    /* prepend to list */
    c->next = clients;
    clients = c;
    wc_insert(c->win, c);
    wc_insert(XtWindow(c->frame_shell), c);
    wc_insert(XtWindow(c->frame_form), c);

    /* set _NET_WM_DESKTOP on client window */
    long desktop = c->ws;
    XChangeProperty(dpy, c->win, net_wm_desktop, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&desktop, 1);

    /* update EWMH client list */
    defer_schedule();

    if (c->is_minimized) {
        set_wm_state(c, IconicState);
        /* don't map — client starts minimized */
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
        /* discard stale UnmapNotify/ReparentNotify queued before we took
         * over (e.g. from a previous WM's handover) — otherwise the stale
         * UnmapNotify unmanages the freshly adopted client */
        {
            XEvent discard;
            while (XCheckTypedWindowEvent(dpy, w, UnmapNotify, &discard)) { }
        }
        compositor_manage_client(c);
        fade_window_in(c);
        if (c->is_floating) {
            moveresizeframe(c);
            send_configure_notify(c);
        }
        if (!c->is_floating)
            btree_add(c);
        defer_schedule();
        updateframe(c);
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
    wc_remove(c->win);
    wc_remove(XtWindow(c->frame_shell));
    wc_remove(XtWindow(c->frame_form));
    XtDestroyWidget(c->frame_shell);
    free(c);
    defer_schedule();
}

void unmanage(Client *c, int destroyed) {
    if (!c) return;
    /* free cached scaled icon pixmaps */
    if (c->icon_scaled_pm)   XFreePixmap(dpy, c->icon_scaled_pm);
    if (c->icon_scaled_mask) XFreePixmap(dpy, c->icon_scaled_mask);
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
    update_active_window();
    /* wm_restarting: hand the window over to the next WM instance —
     * reparent to root (otherwise destroying the frame would destroy the
     * client too — XDestroyWindow takes the whole subtree) but keep it
     * MAPPED and with WM_STATE intact so the startup scan adopts it into
     * a fresh frame seamlessly. Destroy the frame X window DIRECTLY —
     * XtDestroyWidget only queues destruction for the next event loop
     * pass, which never runs during a restart, leaking the frame as an
     * override-redirect orphan that blocks the next WM. */
    if (wm_restarting) {
        if (c->frame_draw) {
            XftDrawDestroy(c->frame_draw);
            c->frame_draw = NULL;
        }
        XReparentWindow(dpy, c->win, root, c->x, c->y);
        XDestroyWindow(dpy, XtWindow(c->frame_shell));
        wc_remove(c->win);
        wc_remove(XtWindow(c->frame_shell));
        wc_remove(XtWindow(c->frame_form));
        free(c);
        return;
    }
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
    wc_remove(c->win);
    wc_remove(XtWindow(c->frame_shell));
    wc_remove(XtWindow(c->frame_form));
    XtDestroyWidget(c->frame_shell);
    free(c);
    defer_schedule();
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