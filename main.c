/*
 * rondo — setup, cleanup, and main entry point
 */
#include "wm.h"
#include <Imlib2.h>

/* ── globals ───────────────────────────────────────────────────────── */

Display *dpy;
int screen;
Window root;
Window barwin;
Window iconbar;
Window checkwin;
int running = 1;
int wm_restarting = 0;
int (*xerrorxlib)(Display *, XErrorEvent *);

XtAppContext app;
Widget toplevel_shell;

Client *clients = NULL;
Client *focused  = NULL;

int curws = 0;
float mfact;
int cur_layout = LAYOUT_MASTER_STACK;

Monitor mon;

GC gc;
GC xor_gc;
GC icon_gc;

Window fb_win;
XftDraw *fb_draw;
int fb_win_w, fb_win_h;
int fb_bevel = 2;

XftFont *xftfont;
XftFont *tooltip_font;
XftDraw *xftdraw;
XftDraw *iconbar_draw;
Pixmap iconbar_buf = None;
XftDraw *iconbar_buf_draw = NULL;
int iconbar_scroll = 0;
Window tooltip_win = None;
XftDraw *tooltip_draw = NULL;
int *ws_x;
int *ws_y;
Visual *xvisual;
Colormap xcolormap;
Visual *argb_visual;
Colormap argb_colormap;
GC argb_gc;

XftColor col_bar_bg, col_bar_fg, col_bar_ws_active, col_bar_ws_occupied, col_bar_ws_idle, col_bar_ws_bg;
XftColor col_bar_border_light, col_bar_border_shadow, col_bar_fill;
XftColor col_title_focus, col_title_unfocus, col_title_fg;
XftColor col_frame_light, col_frame_shadow, col_frame_bg, col_btn_fg;
XftColor col_active_light, col_active_shadow;
XftColor col_fb_bg, col_fb_light, col_fb_shadow, col_fb_fg;
XftColor col_tooltip_bg, col_tooltip_fg, col_tooltip_border;
XftColor col_menu_bg;
XftColor col_iconbar_bg;
XftColor col_root_bg;
XftColor col_root_bg2;
XftColor col_dlg_bg;

Pixmap root_bg_pixmap = None;

/* ICCCM / EWMH atoms */
Atom wm_protocols, wm_delete_window, wm_take_focus;
Atom wm_state, wm_change_state, wm_normal_hints;
Atom wm_colormap_windows;
Atom net_supported, net_client_list, net_number_of_desktops;
Atom net_current_desktop, net_desktop_viewport, net_workarea;
Atom net_active_window, net_close_window;
Atom net_wm_state, net_wm_state_fullscreen;
Atom net_wm_desktop, net_wm_name_atom;
Atom net_wm_window_type, net_wm_window_type_dialog;
Atom net_wm_window_type_dock, net_wm_window_type_toolbar;
Atom net_wm_window_type_utility;
Atom net_wm_window_type_splash;
Atom net_wm_window_type_popup_menu;
Atom net_wm_window_type_dropdown_menu;
Atom net_wm_window_type_tooltip;
Atom net_wm_window_type_notification;
Atom motif_wm_hints;
Atom net_wm_window_opacity;
Atom net_wm_cm_s0;

Time last_event_time = CurrentTime;

Cursor curs_resize[16];
Cursor curs_default;

int sw, sh;

/* ── helpers ────────────────────────────────────────────────────────── */

void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

int xftcolor_load(const char *name, XftColor *xc) {
    if (!XftColorAllocName(dpy, xvisual, xcolormap, name, xc)) {
        fprintf(stderr, "rondo: cannot allocate xft color '%s'\n", name);
        return 0;
    }
    return 1;
}

int xftcolor_load_argb(const char *name, XftColor *xc) {
    /* Try XftColorAllocName first — works for named colors and #RRGGBB */
    if (XftColorAllocName(dpy, argb_visual, argb_colormap, name, xc))
        return 1;

    /* XParseColor doesn't support #RRGGBBAA, so handle it manually:
     * parse the 8-digit hex, allocate the RGB part, then patch alpha
     * and pixel value for the ARGB visual. */
    size_t len = strlen(name);
    if (name[0] == '#' && len == 9) {
        unsigned int r, g, b, a;
        if (sscanf(name + 1, "%2x%2x%2x%2x", &r, &g, &b, &a) != 4)
            goto fail;

        char rgb[8];
        snprintf(rgb, sizeof(rgb), "#%02x%02x%02x", r, g, b);
        if (!XftColorAllocName(dpy, argb_visual, argb_colormap, rgb, xc))
            goto fail;

        /* Set alpha in the color struct */
        xc->color.alpha = (unsigned short)(a * 0xFFFF / 0xFF);

        /* Patch pixel value to include alpha channel */
        if (argb_visual != xvisual) {
            XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, argb_visual);
            if (fmt && fmt->direct.alphaMask) {
                unsigned long amask = (unsigned long)fmt->direct.alphaMask
                                    << fmt->direct.alpha;
                xc->pixel = (xc->pixel & ~amask)
                          | ((unsigned long)(a * fmt->direct.alphaMask / 0xFF)
                             << fmt->direct.alpha);
            }
        }
        return 1;
    }

fail:
    fprintf(stderr, "rondo: cannot allocate ARGB color '%s'\n", name);
    return 0;
}

/* load an xft color into *xc, skipping the XAllocColor round trip when
 * the requested name is unchanged from the previous load */
static int xftcolor_load_cached(const char *name, XftColor *xc, char **prev_name) {
    if (*prev_name && strcmp(*prev_name, name) == 0)
        return 1;  /* unchanged — keep existing allocation */
    XftColor tmp;
    if (!XftColorAllocName(dpy, xvisual, xcolormap, name, &tmp)) {
        fprintf(stderr, "rondo: cannot allocate xft color '%s'\n", name);
        return 0;
    }
    /* free the old pixel when the name actually changed */
    if (*prev_name && xc->pixel)
        XftColorFree(dpy, xvisual, xcolormap, xc);
    *xc = tmp;
    if (!*prev_name) {
        *prev_name = malloc(128);
        if (!*prev_name) return 1;
    }
    snprintf(*prev_name, 128, "%s", name);
    return 1;
}

static int xftcolor_load_argb_cached(const char *name, XftColor *xc, char **prev_name) {
    if (*prev_name && strcmp(*prev_name, name) == 0)
        return 1;
    /* reuse the plain loader for the first allocation, then track names */
    XftColor tmp = {0};
    char *unused = NULL;
    /* temporarily load into tmp via the non-caching path */
    XftColor old = *xc;
    if (!xftcolor_load_argb(name, &tmp)) {
        fprintf(stderr, "rondo: cannot allocate ARGB color '%s'\n", name);
        return 0;
    }
    if (*prev_name && old.pixel)
        XftColorFree(dpy, argb_visual, argb_colormap, &old);
    *xc = tmp;
    (void)unused;
    if (!*prev_name) {
        *prev_name = malloc(128);
        if (!*prev_name) return 1;
    }
    snprintf(*prev_name, 128, "%s", name);
    return 1;
}

void load_colors(void) {
    static char *pn_bar_bg, *pn_bar_fg, *pn_ws_active, *pn_ws_occupied,
                *pn_ws_idle, *pn_ws_bg, *pn_bar_bl, *pn_bar_bs, *pn_bar_fill,
                *pn_menu_bg, *pn_iconbar_bg, *pn_dlg_bg,
                *pn_title_f, *pn_title_u, *pn_title_fg,
                *pn_frame_l, *pn_frame_s, *pn_frame_bg, *pn_btn_fg,
                *pn_active_l, *pn_active_s,
                *pn_fb_bg, *pn_fb_l, *pn_fb_s, *pn_fb_fg,
                *pn_tt_bg, *pn_tt_fg, *pn_tt_b,
                *pn_root_bg, *pn_root_bg2;
    /* ARGB colors (drawn on bar, iconbar, menu, dialog — need alpha channel) */
    xftcolor_load_argb_cached(color_bar_bg,          &col_bar_bg,          &pn_bar_bg);
    xftcolor_load_argb_cached(color_bar_fg,          &col_bar_fg,          &pn_bar_fg);
    xftcolor_load_argb_cached(color_bar_ws_active,   &col_bar_ws_active,   &pn_ws_active);
    xftcolor_load_argb_cached(color_bar_ws_occupied, &col_bar_ws_occupied, &pn_ws_occupied);
    xftcolor_load_argb_cached(color_bar_ws_idle,     &col_bar_ws_idle,     &pn_ws_idle);
    xftcolor_load_argb_cached(color_bar_ws_bg,       &col_bar_ws_bg,       &pn_ws_bg);
    xftcolor_load_argb_cached(color_bar_border_light,  &col_bar_border_light,  &pn_bar_bl);
    xftcolor_load_argb_cached(color_bar_border_shadow, &col_bar_border_shadow, &pn_bar_bs);
    xftcolor_load_argb_cached(color_bar_fill,          &col_bar_fill,          &pn_bar_fill);
    xftcolor_load_argb_cached(color_menu_bg,        &col_menu_bg,        &pn_menu_bg);
    xftcolor_load_argb_cached(color_iconbar_bg,     &col_iconbar_bg,     &pn_iconbar_bg);
    xftcolor_load_argb_cached(color_dialog_bg,      &col_dlg_bg,         &pn_dlg_bg);

    /* Default-visual colors (frames, title, tooltips, root — no alpha needed) */
    xftcolor_load_cached(color_title_focus,     &col_title_focus,     &pn_title_f);
    xftcolor_load_cached(color_title_unfocus,   &col_title_unfocus,   &pn_title_u);
    xftcolor_load_cached(color_title_fg,        &col_title_fg,        &pn_title_fg);
    xftcolor_load_cached(color_frame_light,     &col_frame_light,     &pn_frame_l);
    xftcolor_load_cached(color_frame_shadow,    &col_frame_shadow,    &pn_frame_s);
    xftcolor_load_cached(color_frame_bg,        &col_frame_bg,        &pn_frame_bg);
    xftcolor_load_cached(color_btn_fg,          &col_btn_fg,          &pn_btn_fg);
    xftcolor_load_cached(color_active_light,    &col_active_light,    &pn_active_l);
    xftcolor_load_cached(color_active_shadow,   &col_active_shadow,   &pn_active_s);

    xftcolor_load_cached(color_fb_bg,          &col_fb_bg,          &pn_fb_bg);
    xftcolor_load_cached(color_fb_light,       &col_fb_light,       &pn_fb_l);
    xftcolor_load_cached(color_fb_shadow,      &col_fb_shadow,      &pn_fb_s);
    xftcolor_load_cached(color_fb_fg,          &col_fb_fg,          &pn_fb_fg);
    xftcolor_load_cached(color_tooltip_bg,     &col_tooltip_bg,     &pn_tt_bg);
    xftcolor_load_cached(color_tooltip_fg,     &col_tooltip_fg,     &pn_tt_fg);
    xftcolor_load_cached(color_tooltip_border, &col_tooltip_border, &pn_tt_b);

    xftcolor_load_cached(color_root_bg,        &col_root_bg,        &pn_root_bg);
    xftcolor_load_cached(color_root_bg2,       &col_root_bg2,       &pn_root_bg2);
}

/* ── geometry helpers ─────────────────────────────────────────────────── */

void frame_to_client(int fw, int fh, int *cx, int *cy, int *cw, int *ch, int no_decor) {
    if (no_decor) {
        *cx = 0;
        *cy = 0;
        *cw = fw;
        *ch = fh;
    } else {
        *cx = FRAME_WIDTH;
        *cy = FRAME_WIDTH + TITLE_HEIGHT;
        *cw = fw - 2 * FRAME_WIDTH;
        *ch = fh - 2 * FRAME_WIDTH - TITLE_HEIGHT;
    }
}

void client_to_frame(int cw, int ch, int *fw, int *fh, int no_decor) {
    if (no_decor) {
        *fw = cw;
        *fh = ch;
    } else {
        *fw = cw + 2 * FRAME_WIDTH;
        *fh = ch + 2 * FRAME_WIDTH + TITLE_HEIGHT;
    }
}

/* ── xerror ─────────────────────────────────────────────────────────── */

int xerrorstart(Display *d, XErrorEvent *e) {
    (void)d; (void)e;
    die("rondo: another window manager is already running\n");
}

int xerror(Display *d, XErrorEvent *e) {
    (void)d;
    if (e->error_code == BadWindow ||
        (e->request_code == X_SetInputFocus && e->error_code == BadMatch) ||
        (e->request_code == X_PolyText8 && e->error_code == BadDrawable) ||
        (e->request_code == X_PolyFillRectangle && e->error_code == BadDrawable) ||
        (e->request_code == X_PolySegment && e->error_code == BadDrawable) ||
        (e->request_code == X_ConfigureWindow && e->error_code == BadMatch) ||
        (e->request_code == X_GrabButton && e->error_code == BadAccess) ||
        (e->request_code == X_GrabKey && e->error_code == BadAccess) ||
        (e->request_code == X_ImageText8 && e->error_code == BadDrawable) ||
        (e->request_code == X_PolyFillRectangle && e->error_code == BadDrawable) ||
        (e->request_code == X_PutImage && e->error_code == BadDrawable) ||
        (e->request_code == X_CopyArea && e->error_code == BadDrawable))
        return 0;
    /* BadDamage from stale Damage objects (window destroyed before event processed) */
    if (damage_error_base && e->error_code == (unsigned)damage_error_base)
        return 0;
    /* decode Render extension errors */
    if (render_error_base && e->error_code >= (unsigned)render_error_base &&
        e->error_code < (unsigned)(render_error_base + 4)) {
        static const char *render_err_names[] = {"BadPictFormat", "BadPicture", "BadPictOp", "BadGlyphSet"};
        int idx = e->error_code - render_error_base;
        fprintf(stderr, "rondo: xerror: request=%d Render error: %s (resource=%lu)\n",
                e->request_code, render_err_names[idx], (unsigned long)e->resourceid);
    } else {
        fprintf(stderr, "rondo: xerror: request=%d error=%d\n",
                e->request_code, e->error_code);
    }
    return 0;
}

/* ── setup ──────────────────────────────────────────────────────────── */

#include <fcntl.h>

/* ── entry ──────────────────────────────────────────────────────────── */

/* SIGTERM/SIGINT: perform the same clean handover as an in-WM restart —
 * leave client windows mapped and parented (skip withdraw/reparent) so the
 * next WM instance's startup scan adopts them with full decorations.
 * The handler only writes to a self-pipe (async-signal-safe); the Xt input
 * callback sets running=0 so the main loop exits and runs cleanup(). */
static int term_pipe[2] = { -1, -1 };

static void term_signal_handler(int sig) {
    (void)sig;
    char c = 1;
    if (term_pipe[1] >= 0)
        (void)!write(term_pipe[1], &c, 1);
}

static void term_pipe_cb(XtPointer cl, int *fd, XtInputId *id) {
    (void)cl; (void)fd; (void)id;
    char c;
    while (read(term_pipe[0], &c, 1) == 1) { }
    wm_restarting = 1;
    running = 0;
}

void setup(int *argc, char **argv) {
    XtToolkitInitialize();
    app = XtCreateApplicationContext();
    dpy = XtOpenDisplay(app, NULL, "rondo", "RondoWm", NULL, 0, argc, argv);
    if (!dpy) die("rondo: cannot open display\n");

    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);
    sw = DisplayWidth(dpy, screen);
    sh = DisplayHeight(dpy, screen);

    /* check for existing WM */
    xerrorxlib = XSetErrorHandler(xerrorstart);
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask |
                            ButtonPressMask | PropertyChangeMask | StructureNotifyMask);
    XSync(dpy, False);
    XSetErrorHandler(xerror);

    /* monitor geometry (single monitor or Xinerama primary) */
    mon.x = 0; mon.y = 0; mon.w = sw; mon.h = sh;
    if (XineramaIsActive(dpy)) {
        int n = 0;
        XineramaScreenInfo *info = XineramaQueryScreens(dpy, &n);
        if (info && n > 0) {
            mon.x = info[0].x_org;
            mon.y = info[0].y_org;
            mon.w = info[0].width;
            mon.h = info[0].height;
            XFree(info);
        }
    }

    /* colors */
    xvisual   = DefaultVisual(dpy, screen);
    xcolormap = DefaultColormap(dpy, screen);

    /* ARGB visual for transparent windows */
    {
        XVisualInfo vi_template;
        vi_template.screen = screen;
        vi_template.depth = 32;
        vi_template.class = TrueColor;
        int nvi = 0;
        XVisualInfo *vi = XGetVisualInfo(dpy,
            VisualScreenMask | VisualDepthMask | VisualClassMask,
            &vi_template, &nvi);
        if (vi && nvi > 0) {
            argb_visual = vi->visual;
            argb_colormap = XCreateColormap(dpy, root, argb_visual, AllocNone);
        } else {
            argb_visual = xvisual;
            argb_colormap = xcolormap;
        }
        if (vi) XFree(vi);
    }

    /* Imlib2 context (needed for icon scaling in bar.c) */
    imlib_context_set_display(dpy);
    imlib_context_set_visual(xvisual);
    imlib_context_set_colormap(xcolormap);

    /* load config (sets all cfg_* globals, must happen before color loading) */
    cfg_init();
    mfact = MASTER_RATIO;

    load_colors();

    /* font */
    xftfont = XftFontOpenName(dpy, screen, font_name);
    tooltip_font = cfg_tooltip_font
                 ? XftFontOpenName(dpy, screen, cfg_tooltip_font)
                 : NULL;
    if (!tooltip_font) tooltip_font = xftfont;
    if (!xftfont) die("rondo: cannot load font '%s'\n", font_name);

    /* GC */
    gc = XCreateGC(dpy, root, 0, NULL);

    /* XOR GC for rubber-band outlines (mwm-style: GXinvert + IncludeInferiors) */
    {
        XGCValues gcv;
        gcv.function = GXinvert;
        gcv.plane_mask = BlackPixel(dpy, screen) ^ WhitePixel(dpy, screen);
        gcv.subwindow_mode = IncludeInferiors;
        gcv.line_width = 0;
        gcv.cap_style = CapNotLast;
        xor_gc = XCreateGC(dpy, root,
                           GCFunction | GCPlaneMask | GCSubwindowMode |
                           GCLineWidth | GCCapStyle,
                           &gcv);
    }

    /* workspace x-coordinates array */
    ws_x = calloc((size_t)NUM_WORKSPACES, sizeof(int));
    ws_y = calloc((size_t)NUM_WORKSPACES, sizeof(int));

    /* status bar window */
    BarGeometry g = calc_bar_geometry();
    int bar_depth = (argb_visual != xvisual) ? 32 : DefaultDepth(dpy, screen);
    {
        XSetWindowAttributes swa;
        memset(&swa, 0, sizeof(swa));
        swa.background_pixel = 0;
        swa.border_pixel = 0;
        swa.colormap = argb_colormap;
        barwin = XCreateWindow(dpy, root,
            g.bar_x, g.bar_y,
            g.bar_w > 0 ? (unsigned)g.bar_w : 1,
            g.bar_h > 0 ? (unsigned)g.bar_h : 1,
            0, bar_depth, InputOutput, argb_visual,
            CWBackPixel | CWBorderPixel | CWColormap, &swa);
    }
    XSelectInput(dpy, barwin, ExposureMask | ButtonPressMask |
                 SubstructureNotifyMask | SubstructureRedirectMask);
    if (show_bar)
        XMapWindow(dpy, barwin);

    xftdraw = XftDrawCreate(dpy, barwin, argb_visual, argb_colormap);
    argb_gc = XCreateGC(dpy, barwin, 0, NULL);

    /* icon bar window (position depends on iconbar_position, initially hidden) */
    {
        XSetWindowAttributes swa;
        memset(&swa, 0, sizeof(swa));
        swa.background_pixel = 0;
        swa.border_pixel = 0;
        swa.colormap = argb_colormap;
        iconbar = XCreateWindow(dpy, root,
            g.ibar_x, g.ibar_y,
            g.ibar_w > 0 ? (unsigned)g.ibar_w : 1,
            g.ibar_h > 0 ? (unsigned)g.ibar_h : 1,
            0, bar_depth, InputOutput, argb_visual,
            CWBackPixel | CWBorderPixel | CWColormap, &swa);
    }
    XSelectInput(dpy, iconbar, ExposureMask | ButtonPressMask | PointerMotionMask | EnterWindowMask | LeaveWindowMask);
    /* do NOT map iconbar here — it's mapped only when minimized windows exist */

    iconbar_draw = XftDrawCreate(dpy, iconbar, argb_visual, argb_colormap);
    icon_gc = XCreateGC(dpy, iconbar, 0, NULL);

    /* tooltip window (override-redirect, initially unmapped) */
    tooltip_win = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 1,
                                       col_tooltip_border.pixel, col_tooltip_bg.pixel);
    XSetWindowAttributes tsa;
    tsa.override_redirect = True;
    tsa.save_under = True;
    XChangeWindowAttributes(dpy, tooltip_win, CWOverrideRedirect | CWSaveUnder, &tsa);
    XSelectInput(dpy, tooltip_win, ExposureMask);
    tooltip_draw = XftDrawCreate(dpy, tooltip_win, xvisual, xcolormap);

    /* grab keys */
    grabkeys();
    ipc_init();

    /* self-pipe for signal-safe main-loop wakeup */
    if (pipe(term_pipe) == 0) {
        fcntl(term_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(term_pipe[1], F_SETFL, O_NONBLOCK);
        XtAppAddInput(app, term_pipe[0], (XtPointer)XtInputReadMask,
                      term_pipe_cb, NULL);
    }

    /* cursors for resize handles */
    curs_default   = XCreateFontCursor(dpy, XC_left_ptr);
    XDefineCursor(dpy, root, curs_default);
    curs_resize[EDGE_N]  = XCreateFontCursor(dpy, XC_top_side);
    curs_resize[EDGE_S]  = XCreateFontCursor(dpy, XC_bottom_side);
    curs_resize[EDGE_W]  = XCreateFontCursor(dpy, XC_left_side);
    curs_resize[EDGE_E]  = XCreateFontCursor(dpy, XC_right_side);
    curs_resize[EDGE_NW] = XCreateFontCursor(dpy, XC_top_left_corner);
    curs_resize[EDGE_NE] = XCreateFontCursor(dpy, XC_top_right_corner);
    curs_resize[EDGE_SW] = XCreateFontCursor(dpy, XC_bottom_left_corner);
    curs_resize[EDGE_SE] = XCreateFontCursor(dpy, XC_bottom_right_corner);

    /* grab mouse buttons on root for potential use */
    XGrabButton(dpy, Button1, MODKEY, root, True, ButtonPressMask,
                GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button3, MODKEY, root, True, ButtonPressMask,
                GrabModeAsync, GrabModeAsync, None, None);

    /* EWMH support */
    /* intern all atoms in one round trip (XInternAtoms).
     * XInternAtoms fills a flat Atom array — copy results into the globals. */
    {
        Atom ids[37];
        const char *names[] = {
            "WM_PROTOCOLS", "WM_DELETE_WINDOW", "WM_TAKE_FOCUS",
            "WM_STATE", "WM_CHANGE_STATE", "WM_NORMAL_HINTS",
            "WM_COLORMAP_WINDOWS",
            "_NET_SUPPORTED", "_NET_CLIENT_LIST",
            "_NET_NUMBER_OF_DESKTOPS", "_NET_CURRENT_DESKTOP",
            "_NET_DESKTOP_VIEWPORT", "_NET_WORKAREA",
            "_NET_ACTIVE_WINDOW", "_NET_CLOSE_WINDOW",
            "_NET_WM_STATE", "_NET_WM_STATE_FULLSCREEN",
            "_NET_WM_DESKTOP", "_NET_WM_NAME",
            "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DIALOG",
            "_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_TOOLBAR",
            "_NET_WM_WINDOW_TYPE_UTILITY", "_NET_WM_WINDOW_TYPE_SPLASH",
            "_NET_WM_WINDOW_TYPE_POPUP_MENU",
            "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU",
            "_NET_WM_WINDOW_TYPE_TOOLTIP",
            "_NET_WM_WINDOW_TYPE_NOTIFICATION",
            "_MOTIF_WM_HINTS", "_NET_WM_WINDOW_OPACITY",
            "_NET_WM_CM_S0",
            "_NET_SYSTEM_TRAY_S0", "_NET_SYSTEM_TRAY_VISUAL",
            "_NET_SYSTEM_TRAY_OPCODE", "MANAGER", "_XEMBED",
        };
        int na = (int)(sizeof(names) / sizeof(names[0]));
        if (XInternAtoms(dpy, (char **)names, na, False, ids)) {
            Atom *out[] = {
                &wm_protocols, &wm_delete_window, &wm_take_focus,
                &wm_state, &wm_change_state, &wm_normal_hints,
                &wm_colormap_windows,
                &net_supported, &net_client_list,
                &net_number_of_desktops, &net_current_desktop,
                &net_desktop_viewport, &net_workarea,
                &net_active_window, &net_close_window,
                &net_wm_state, &net_wm_state_fullscreen,
                &net_wm_desktop, &net_wm_name_atom,
                &net_wm_window_type, &net_wm_window_type_dialog,
                &net_wm_window_type_dock, &net_wm_window_type_toolbar,
                &net_wm_window_type_utility, &net_wm_window_type_splash,
                &net_wm_window_type_popup_menu,
                &net_wm_window_type_dropdown_menu,
                &net_wm_window_type_tooltip,
                &net_wm_window_type_notification,
                &motif_wm_hints, &net_wm_window_opacity,
                &net_wm_cm_s0,
                &net_system_tray, &net_system_tray_visual,
                &net_system_tray_opcode, &manager_atom, &xembed,
            };
            for (int i = 0; i < na; i++) *out[i] = ids[i];
        } else {
            fprintf(stderr, "rondo: XInternAtoms failed — atom-based protocols disabled\n");
        }
    }
    Atom net_supporting = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
    Atom utf8_string    = XInternAtom(dpy, "UTF8_STRING", False);
    checkwin = XCreateSimpleWindow(dpy, root, -1, -1, 1, 1, 0, 0, 0);
    XChangeProperty(dpy, checkwin, net_supporting, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&checkwin, 1);
    XChangeProperty(dpy, root, net_supporting, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&checkwin, 1);
    XChangeProperty(dpy, checkwin, net_wm_name_atom,
                    utf8_string, 8,
                    PropModeReplace, (unsigned char *)"rondo", 5);

    /* set _NET_SUPPORTED — announce which EWMH atoms we support */
    {
        Atom supported[] = {
            net_supported, net_client_list, net_number_of_desktops,
            net_current_desktop, net_desktop_viewport, net_workarea,
            net_active_window, net_close_window,
            net_wm_state, net_wm_state_fullscreen,
            net_wm_desktop, net_wm_name_atom,
            net_wm_window_type, net_wm_window_type_dialog,
            net_wm_window_type_dock, net_wm_window_type_toolbar,
            net_wm_window_type_utility,
            net_wm_window_type_splash, net_wm_window_type_popup_menu,
            net_wm_window_type_dropdown_menu, net_wm_window_type_tooltip,
            net_wm_window_type_notification,
            net_system_tray,
            net_supporting, net_wm_name_atom
        };
        XChangeProperty(dpy, root, net_supported, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)supported,
                        (int)(sizeof(supported) / sizeof(Atom)));
    }

    /* create invisible override-redirect shell for Xt */
    toplevel_shell = XtVaAppCreateShell("rondo", "RondoWm",
        overrideShellWidgetClass, dpy,
        XtNmappedWhenManaged, False,
        XtNoverrideRedirect, True,
        XtNwidth, 1,
        XtNheight, 1,
        NULL);
    XtRealizeWidget(toplevel_shell);

    /* adopt already-mapped client windows (WM restart / session handover):
     * unmanaged mapped windows would otherwise sit frameless and dead —
     * MapRequest only fires for (re)maps, not for windows mapped before
     * we selected SubstructureRedirect */
    {
        Window dum_r, dum_p, *kids = NULL;
        unsigned int nk = 0;
        if (XQueryTree(dpy, root, &dum_r, &dum_p, &kids, &nk) && kids) {
            for (unsigned int i = 0; i < nk; i++) {
                if (!kids[i]) continue;
                if (kids[i] == XtWindow(toplevel_shell)) continue;
                XWindowAttributes wa;
                if (!XGetWindowAttributes(dpy, kids[i], &wa)) continue;
                if (wa.override_redirect) continue;
                if (wa.map_state != IsViewable) continue;
                /* skip leftovers from a previous rondo instance (its bar,
                 * icon bar, frames): those carry no WM_CLASS / the RondoWm
                 * class, while every real client has a WM_CLASS */
                XClassHint ch;
                if (!XGetClassHint(dpy, kids[i], &ch))
                    continue;  /* no class at all → WM-internal leftover */
                int is_rondo = (ch.res_class && strcmp(ch.res_class, "RondoWm") == 0);
                if (ch.res_name) XFree(ch.res_name);
                if (ch.res_class) XFree(ch.res_class);
                if (is_rondo) continue;
                manage(kids[i], &wa);
            }
            XFree(kids);
        }
    }

    /* apply background (solid, pattern, or image) */
    bg_load();

    /* set EWMH root window properties */
    update_net_desktops();
    update_workarea();
    update_client_list();
    update_active_window();

    drawbar();

    /* start periodic bar refresh for clock/load/mem/disk */
    start_bar_timer();

    /* initialize system tray if tray widget is configured */
    {
        int has_tray = 0;
        for (int i = 0; i < num_bar_widgets; i++) {
            if (bar_widgets[i].type == BAR_WIDGET_TRAY) {
                has_tray = 1;
                break;
            }
        }
        if (has_tray)
            tray_init();
    }

    /* start built-in compositor if no external compositor is running
     * and compositing is enabled in config */
    if (fade_enabled)
        compositor_start();
}

/* ── cleanup ────────────────────────────────────────────────────────── */

void cleanup(void) {
    tray_cleanup();

    /* cancel all in-progress fades */
    for (Client *c = clients; c; c = c->next) {
        if (c->fade_timer) {
            XtRemoveTimeOut(c->fade_timer);
            c->fade_timer = (XtIntervalId)0;
        }
    }

    /* cancel bar refresh timer */
    if (bar_refresh_timer) {
        XtRemoveTimeOut(bar_refresh_timer);
        bar_refresh_timer = 0;
    }

    /* stop compositor before destroying windows */
    compositor_stop();

    /* Unmanage all clients first — unmanage() calls updateiconbar()
     * and drawbar() which need barwin and iconbar to still exist */
    while (clients) unmanage(clients, 0);

    btree_cleanup();

    XftDrawDestroy(xftdraw);
    XftDrawDestroy(iconbar_draw);
    if (tooltip_draw) { XftDrawDestroy(tooltip_draw); tooltip_draw = NULL; }
    if (iconbar_buf_draw) { XftDrawDestroy(iconbar_buf_draw); iconbar_buf_draw = NULL; }
    if (iconbar_buf != None) { XFreePixmap(dpy, iconbar_buf); iconbar_buf = None; }
    XftFontClose(dpy, xftfont);
    if (tooltip_font != xftfont) XftFontClose(dpy, tooltip_font);
    XFreeGC(dpy, gc);
    XFreeGC(dpy, xor_gc);
    XFreeGC(dpy, icon_gc);
    XFreeGC(dpy, argb_gc);
    if (argb_visual != xvisual && argb_colormap != xcolormap)
        XFreeColormap(dpy, argb_colormap);
    XDestroyWindow(dpy, fb_win);
    if (fb_draw) XftDrawDestroy(fb_draw);
    XFreeCursor(dpy, curs_default);
    for (int i = 0; i < 16; i++)
        if (curs_resize[i]) XFreeCursor(dpy, curs_resize[i]);
    XDestroyWindow(dpy, barwin);
    XDestroyWindow(dpy, iconbar);
    XDestroyWindow(dpy, tooltip_win);
    XDestroyWindow(dpy, checkwin);

    bg_free();

    cfg_cleanup();
    free(ws_x);
    free(ws_y);

    ipc_cleanup();

    /* reset focus while display is still valid (before XtDestroyApplicationContext
     * closes the connection) */
    XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);

    XtDestroyWidget(toplevel_shell);
    XtDestroyApplicationContext(app);
}

/* ── restart ──────────────────────────────────────────────────────── */

static int restart_ioerr(Display *d) { (void)d; return 0; }

void restart_wm(void) {
    char exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, (size_t)(sizeof(exe) - 1));
    if (len < 0) return;
    exe[len] = '\0';
    /* prevent X I/O errors from aborting during cleanup */
    XSetIOErrorHandler(restart_ioerr);
    wm_restarting = 1;
    cleanup();
    execl(exe, exe, (char *)NULL);
    _exit(1);
}

/* ── deferred batching ─────────────────────────────────────────────── */

int defer_dirty = 0;
XtIntervalId defer_timer = 0;

static void defer_cb(XtPointer client_data, XtIntervalId *id) {
    (void)client_data; (void)id;
    defer_timer = 0;
    defer_flush();
}

void defer_schedule(void) {
    if (defer_timer) return;
    defer_dirty = 1;
    defer_timer = XtAppAddTimeOut(app, 0, defer_cb, NULL);
}

void defer_flush(void) {
    if (!defer_dirty) return;
    defer_dirty = 0;
    arrange();
    /* the bar widgets read /proc, sysinfo, statvfs etc. — only refresh
     * them at the timer rate, not on every event-driven flush */
    drawbar();
    updateiconbar();
    update_client_list();
    compositor_repaint();
}

/* ── entry ──────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    signal(SIGTERM, term_signal_handler);
    signal(SIGINT, term_signal_handler);
    if (argc > 1 && strcmp(argv[1], "-v") == 0) {
        puts("rondo-0.1");
        return 0;
    }
    if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
        fprintf(stderr, "rondo: locale not supported\n");

    setup(&argc, argv);
    run();
    cleanup();
    return 0;
}