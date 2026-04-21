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
int (*xerrorxlib)(Display *, XErrorEvent *);

XtAppContext app;
Widget toplevel_shell;

Client *clients = NULL;
Client *focused  = NULL;

int curws = 0;
float mfact;

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

void load_colors(void) {
    /* ARGB colors (drawn on bar, iconbar, menu, dialog — need alpha channel) */
    xftcolor_load_argb(color_bar_bg,          &col_bar_bg);
    xftcolor_load_argb(color_bar_fg,          &col_bar_fg);
    xftcolor_load_argb(color_bar_ws_active,   &col_bar_ws_active);
    xftcolor_load_argb(color_bar_ws_occupied, &col_bar_ws_occupied);
    xftcolor_load_argb(color_bar_ws_idle,     &col_bar_ws_idle);
    xftcolor_load_argb(color_bar_ws_bg,       &col_bar_ws_bg);
    xftcolor_load_argb(color_bar_border_light,  &col_bar_border_light);
    xftcolor_load_argb(color_bar_border_shadow, &col_bar_border_shadow);
    xftcolor_load_argb(color_bar_fill,          &col_bar_fill);
    xftcolor_load_argb(color_menu_bg,        &col_menu_bg);
    xftcolor_load_argb(color_iconbar_bg,     &col_iconbar_bg);
    xftcolor_load_argb(color_dialog_bg,      &col_dlg_bg);

    /* Default-visual colors (frames, title, tooltips, root — no alpha needed) */
    xftcolor_load(color_title_focus,     &col_title_focus);
    xftcolor_load(color_title_unfocus,   &col_title_unfocus);
    xftcolor_load(color_title_fg,        &col_title_fg);
    xftcolor_load(color_frame_light,     &col_frame_light);
    xftcolor_load(color_frame_shadow,    &col_frame_shadow);
    xftcolor_load(color_frame_bg,        &col_frame_bg);
    xftcolor_load(color_btn_fg,          &col_btn_fg);
    xftcolor_load(color_active_light,    &col_active_light);
    xftcolor_load(color_active_shadow,   &col_active_shadow);

    xftcolor_load(color_fb_bg,          &col_fb_bg);
    xftcolor_load(color_fb_light,       &col_fb_light);
    xftcolor_load(color_fb_shadow,      &col_fb_shadow);
    xftcolor_load(color_fb_fg,          &col_fb_fg);
    xftcolor_load(color_tooltip_bg,     &col_tooltip_bg);
    xftcolor_load(color_tooltip_fg,     &col_tooltip_fg);
    xftcolor_load(color_tooltip_border, &col_tooltip_border);

    xftcolor_load(color_root_bg,        &col_root_bg);
    xftcolor_load(color_root_bg2,       &col_root_bg2);
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
    icon_gc = XCreateGC(dpy, root, 0, NULL);

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
    XSelectInput(dpy, barwin, ExposureMask | ButtonPressMask);
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
    XFreeGC(dpy, icon_gc);
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
    Atom net_supporting = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
    Atom net_wm_name    = XInternAtom(dpy, "_NET_WM_NAME", False);
    checkwin = XCreateSimpleWindow(dpy, root, -1, -1, 1, 1, 0, 0, 0);
    XChangeProperty(dpy, checkwin, net_supporting, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&checkwin, 1);
    XChangeProperty(dpy, root, net_supporting, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&checkwin, 1);
    XChangeProperty(dpy, checkwin, net_wm_name,
                    XInternAtom(dpy, "UTF8_STRING", False), 8,
                    PropModeReplace, (unsigned char *)"rondo", 5);

    /* ICCCM atoms */
    wm_protocols      = XInternAtom(dpy, "WM_PROTOCOLS", False);
    wm_delete_window  = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    wm_take_focus     = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
    wm_state          = XInternAtom(dpy, "WM_STATE", False);
    wm_change_state   = XInternAtom(dpy, "WM_CHANGE_STATE", False);
    wm_normal_hints   = XInternAtom(dpy, "WM_NORMAL_HINTS", False);
    wm_colormap_windows = XInternAtom(dpy, "WM_COLORMAP_WINDOWS", False);

    /* EWMH atoms */
    net_supported      = XInternAtom(dpy, "_NET_SUPPORTED", False);
    net_client_list    = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    net_number_of_desktops = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    net_current_desktop = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    net_desktop_viewport = XInternAtom(dpy, "_NET_DESKTOP_VIEWPORT", False);
    net_workarea       = XInternAtom(dpy, "_NET_WORKAREA", False);
    net_active_window   = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    net_close_window    = XInternAtom(dpy, "_NET_CLOSE_WINDOW", False);
    net_wm_state        = XInternAtom(dpy, "_NET_WM_STATE", False);
    net_wm_state_fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    net_wm_desktop      = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
    net_wm_name_atom    = XInternAtom(dpy, "_NET_WM_NAME", False);
    net_wm_window_type  = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    net_wm_window_type_dialog = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    net_wm_window_type_dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    net_wm_window_type_toolbar = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
    net_wm_window_type_utility = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    net_wm_window_type_splash       = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
    net_wm_window_type_popup_menu   = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
    net_wm_window_type_dropdown_menu = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", False);
    net_wm_window_type_tooltip      = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
    net_wm_window_type_notification  = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
    motif_wm_hints                  = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
    net_wm_window_opacity           = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);
    net_wm_cm_s0                    = XInternAtom(dpy, "_NET_WM_CM_S0", False);

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
            net_supporting, net_wm_name
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

    /* start built-in compositor if no external compositor is running
     * and compositing is enabled in config */
    if (fade_enabled)
        compositor_start();
}

/* ── cleanup ────────────────────────────────────────────────────────── */

void cleanup(void) {
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
    cleanup();
    execl(exe, exe, (char *)NULL);
    _exit(1);
}

/* ── entry ──────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
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