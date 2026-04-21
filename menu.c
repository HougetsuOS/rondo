/*
 * rondo — menus and dialogs
 */
#include "wm.h"
#include <limits.h>

/* ── window menu items ───────────────────────────────────────────────── */

enum {
    WIN_RESTORE, WIN_MOVE, WIN_SIZE,
    WIN_MINIMIZE, WIN_MAXIMIZE, WIN_LOWER, WIN_CLOSE
};

static const CfgMenuItem win_items[] = {
    { "Restore",    WIN_RESTORE },
    { "Move",       WIN_MOVE },
    { "Size",       WIN_SIZE },
    { "Minimize",   WIN_MINIMIZE },
    { "Maximize",   WIN_MAXIMIZE },
    { "Lower",      WIN_LOWER },
    { NULL,         MENU_SEP },
    { "Close",      WIN_CLOSE },
};
#define NUM_WIN_ITEMS ((int)(sizeof(win_items) / sizeof(win_items[0])))

/* ── generic menu internals ──────────────────────────────────────────── */

#define MENU_ITEM_H   22
#define MENU_PAD_X    8
#define MENU_PAD_Y    2
#define MENU_BORDER   2

typedef int (*MenuEnabledFn)(int action, void *ctx);

static int generic_item_enabled(const CfgMenuItem *items, int num_items,
                                int idx, MenuEnabledFn check, void *ctx) {
    if (idx < 0 || idx >= num_items) return 0;
    if (items[idx].action == MENU_SEP) return 0;
    if (check) return check(items[idx].action, ctx);
    return 1;
}

static int first_enabled(const CfgMenuItem *items, int num_items,
                         MenuEnabledFn check, void *ctx) {
    for (int i = 0; i < num_items; i++)
        if (items[i].action != MENU_SEP && generic_item_enabled(items, num_items, i, check, ctx))
            return i;
    return -1;
}

static int next_enabled(const CfgMenuItem *items, int num_items,
                        MenuEnabledFn check, void *ctx, int from) {
    for (int i = from + 1; i < num_items; i++)
        if (items[i].action != MENU_SEP && generic_item_enabled(items, num_items, i, check, ctx))
            return i;
    for (int i = 0; i < from; i++)
        if (items[i].action != MENU_SEP && generic_item_enabled(items, num_items, i, check, ctx))
            return i;
    return from;
}

static int prev_enabled(const CfgMenuItem *items, int num_items,
                        MenuEnabledFn check, void *ctx, int from) {
    for (int i = from - 1; i >= 0; i--)
        if (items[i].action != MENU_SEP && generic_item_enabled(items, num_items, i, check, ctx))
            return i;
    for (int i = num_items - 1; i > from; i--)
        if (items[i].action != MENU_SEP && generic_item_enabled(items, num_items, i, check, ctx))
            return i;
    return from;
}

static int item_at_y(const CfgMenuItem *items, int num_items, int my) {
    int y = MENU_PAD_Y + MENU_BORDER;
    for (int i = 0; i < num_items; i++) {
        int ih = (items[i].action == MENU_SEP) ? 6 : MENU_ITEM_H;
        if (my >= y && my < y + ih)
            return (items[i].action == MENU_SEP) ? -1 : i;
        y += ih;
    }
    return -1;
}

static void draw_menu(XftDraw *draw, int menu_w, int menu_h,
                      const CfgMenuItem *items, int num_items,
                      int highlight, MenuEnabledFn check, void *ctx) {
    XftDrawRect(draw, &col_menu_bg, 0, 0, menu_w, menu_h);
    bevel_rect(draw, 0, 0, menu_w, menu_h,
               MENU_BORDER, MENU_BORDER, MENU_BORDER, MENU_BORDER,
               &col_frame_light, &col_frame_shadow);

    int y = MENU_PAD_Y + MENU_BORDER;
    for (int i = 0; i < num_items; i++) {
        if (items[i].action == MENU_SEP) {
            int sep_y = y + 2;
            int sep_x = MENU_BORDER + 2;
            int sep_w = menu_w - 2 * MENU_BORDER - 4;
            XftDrawRect(draw, &col_frame_shadow, sep_x, sep_y, sep_w, 1);
            XftDrawRect(draw, &col_frame_light, sep_x, sep_y + 1, sep_w, 1);
            y += 6;
            continue;
        }
        int enabled = generic_item_enabled(items, num_items, i, check, ctx);
        int ix = MENU_BORDER;
        int iw = menu_w - 2 * MENU_BORDER;
        if (i == highlight && enabled) {
            bevel_rect(draw, ix, y, iw, MENU_ITEM_H,
                       2, 2, 2, 2, &col_frame_light, &col_frame_shadow);
        }
        XftColor *text_col;
        if (!enabled)
            text_col = &col_frame_shadow;
        else
            text_col = &col_btn_fg;
        int namelen = (int)strlen(items[i].label);
        int text_y = y + (MENU_ITEM_H + xftfont->ascent - xftfont->descent) / 2;
        XftDrawStringUtf8(draw, text_col, xftfont,
                          ix + MENU_PAD_X, text_y,
                          (XftChar8 *)items[i].label, namelen);
        y += MENU_ITEM_H;
    }
    XSync(dpy, False);
}

static int run_menu(const CfgMenuItem *items, int num_items,
                    MenuEnabledFn check, void *ctx,
                    int root_x, int root_y, int drag_select) {
    /* calculate dimensions */
    int max_text_w = 0;
    for (int i = 0; i < num_items; i++) {
        if (items[i].label) {
            XGlyphInfo ext;
            XftTextExtents8(dpy, xftfont, (XftChar8 *)items[i].label,
                             (int)strlen(items[i].label), &ext);
            if (ext.xOff > max_text_w) max_text_w = ext.xOff;
        }
    }
    int menu_w = max_text_w + 2 * MENU_PAD_X + 2 * MENU_BORDER;
    int menu_h = 0;
    for (int i = 0; i < num_items; i++)
        menu_h += (items[i].action == MENU_SEP) ? 6 : MENU_ITEM_H;
    menu_h += 2 * MENU_PAD_Y + 2 * MENU_BORDER;

    /* clamp position to screen */
    if (root_x + menu_w > mon.x + mon.w)
        root_x = mon.x + mon.w - menu_w;
    if (root_y + menu_h > mon.y + mon.h)
        root_y = mon.y + mon.h - menu_h;
    if (root_x < mon.x) root_x = mon.x;
    if (root_y < mon.y) root_y = mon.y;

    /* create popup window */
    XSetWindowAttributes mwa;
    mwa.background_pixel = 0;
    mwa.border_pixel = 0;
    mwa.colormap = argb_colormap;
    mwa.override_redirect = True;
    mwa.save_under = True;
    int menu_depth = (argb_visual != xvisual) ? 32 : DefaultDepth(dpy, screen);
    Window menu_win = XCreateWindow(dpy, root, root_x, root_y,
                                     (unsigned)menu_w, (unsigned)menu_h, 0,
                                     menu_depth, InputOutput, argb_visual,
                                     CWBackPixel | CWBorderPixel | CWColormap |
                                     CWOverrideRedirect | CWSaveUnder,
                                     &mwa);
    XSelectInput(dpy, menu_win,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask | LeaveWindowMask | KeyPressMask);
    XMapWindow(dpy, menu_win);
    XRaiseWindow(dpy, menu_win);

    XftDraw *menu_draw = XftDrawCreate(dpy, menu_win, argb_visual, argb_colormap);

    if (XGrabPointer(dpy, menu_win, True,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, None, CurrentTime) != GrabSuccess) {
        XftDrawDestroy(menu_draw);
        compositor_untrack_window(menu_win);
        XDestroyWindow(dpy, menu_win);
        return -1;
    }
    XGrabKeyboard(dpy, menu_win, True, GrabModeAsync, GrabModeAsync, CurrentTime);

    /* in drag-select mode: highlight starts at -1, motion tracks, release selects
     * in click-select mode: highlight starts at first enabled, first release consumed */
    int highlight = drag_select ? -1 : first_enabled(items, num_items, check, ctx);
    draw_menu(menu_draw, menu_w, menu_h, items, num_items, highlight, check, ctx);
    compositor_repaint();

    /* event loop */
    XEvent ev;
    int done = 0;
    int action_result = -1;
    int consume_release = !drag_select;  /* click-select: consume the opening release */

    while (!done && running) {
        XNextEvent(dpy, &ev);
        switch (ev.type) {
        case KeyPress: {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            if (ks == XK_Escape) {
                done = 1;
            } else if (ks == XK_Return || ks == XK_KP_Enter) {
                if (highlight >= 0 && generic_item_enabled(items, num_items, highlight, check, ctx))
                    action_result = items[highlight].action;
                done = 1;
            } else if (ks == XK_Down || ks == XK_KP_Down) {
                int new_hl = next_enabled(items, num_items, check, ctx, highlight < 0 ? -1 : highlight);
                if (new_hl != highlight) {
                    highlight = new_hl;
                    draw_menu(menu_draw, menu_w, menu_h, items, num_items, highlight, check, ctx);
                    compositor_repaint();
                }
            } else if (ks == XK_Up || ks == XK_KP_Up) {
                int new_hl = prev_enabled(items, num_items, check, ctx, highlight < 0 ? num_items : highlight);
                if (new_hl != highlight) {
                    highlight = new_hl;
                    draw_menu(menu_draw, menu_w, menu_h, items, num_items, highlight, check, ctx);
                    compositor_repaint();
                }
            }
            break;
        }
        case MotionNotify: {
            if (drag_select) {
                int new_hl = item_at_y(items, num_items, ev.xmotion.y);
                if (new_hl >= 0 && !generic_item_enabled(items, num_items, new_hl, check, ctx))
                    new_hl = -1;
                if (new_hl != highlight) {
                    highlight = new_hl;
                    draw_menu(menu_draw, menu_w, menu_h, items, num_items, highlight, check, ctx);
                    compositor_repaint();
                }
            }
            break;
        }
        case LeaveNotify:
            if (drag_select && highlight != -1) {
                highlight = -1;
                draw_menu(menu_draw, menu_w, menu_h, items, num_items, highlight, check, ctx);
                compositor_repaint();
            }
            break;
        case ButtonPress:
            if (!drag_select) {
                /* click-select mode: click outside dismisses */
                if (ev.xbutton.x < 0 || ev.xbutton.x >= menu_w ||
                    ev.xbutton.y < 0 || ev.xbutton.y >= menu_h) {
                    done = 1;
                }
            }
            break;
        case ButtonRelease:
            if (consume_release) {
                consume_release = 0;
                break;
            }
            if (drag_select) {
                /* release selects highlighted item (or dismisses if none) */
                if (highlight >= 0 && generic_item_enabled(items, num_items, highlight, check, ctx))
                    action_result = items[highlight].action;
                done = 1;
            } else {
                /* click-select: release on an enabled item selects it */
                int idx = item_at_y(items, num_items, ev.xbutton.y);
                if (idx >= 0 && generic_item_enabled(items, num_items, idx, check, ctx))
                    action_result = items[idx].action;
                done = 1;
            }
            break;
        case Expose:
            if (ev.xexpose.count == 0) {
                draw_menu(menu_draw, menu_w, menu_h, items, num_items, highlight, check, ctx);
                compositor_repaint();
            }
            break;
        default:
            if (compositor_running && ev.type == damage_event_base + XDamageNotify) {
                compositor_handle_damage((XDamageNotifyEvent *)&ev);
            }
            break;
        }
    }

    XUngrabKeyboard(dpy, CurrentTime);
    XUngrabPointer(dpy, CurrentTime);
    XftDrawDestroy(menu_draw);
    compositor_untrack_window(menu_win);
    XDestroyWindow(dpy, menu_win);
    XFlush(dpy);
    return action_result;
}

/* ── window menu ─────────────────────────────────────────────────────── */

static int win_menu_check(int action, void *ctx) {
    Client *c = (Client *)ctx;
    if (action == WIN_RESTORE && !c->is_fullscreen && !c->is_minimized)
        return 0;
    if (action == WIN_SIZE && !c->is_floating)
        return 0;
    if (action == WIN_MINIMIZE && c->is_minimized)
        return 0;
    return 1;
}

static void win_menu_exec(Client *c, int action) {
    switch (action) {
    case WIN_RESTORE:
        if (c->is_fullscreen)
            togglefullscreen(NULL);
        else
            restore_client(c);
        break;
    case WIN_MOVE: {
        if (!c->is_floating) {
            WmArg arg = {0};
            togglefloat(&arg);
        }
        Window dw;
        int di, px, py;
        unsigned int du;
        XQueryPointer(dpy, root, &dw, &dw, &px, &py, &di, &di, &du);
        mousemove(c, Button1, EDGE_NONE, px, py);
        break;
    }
    case WIN_SIZE: {
        if (!c->is_floating) {
            WmArg arg = {0};
            togglefloat(&arg);
        }
        Window dw;
        int di, px, py;
        unsigned int du;
        XQueryPointer(dpy, root, &dw, &dw, &px, &py, &di, &di, &du);
        mousemove(c, Button3, EDGE_SE, px, py);
        break;
    }
    case WIN_MINIMIZE:
        minimize_client(c);
        break;
    case WIN_MAXIMIZE:
        togglefullscreen(NULL);
        break;
    case WIN_LOWER:
        lowerwindow(NULL);
        break;
    case WIN_CLOSE:
        if (focused != c) focus(c);
        killclient(NULL);
        break;
    }
}

void show_window_menu(Client *c, int root_x, int root_y) {
    c->pressed_btn = BTN_MENU;
    drawframe(c);
    int action = run_menu(win_items, NUM_WIN_ITEMS, win_menu_check, c, root_x, root_y, 0);
    c->pressed_btn = BTN_NONE;
    drawframe(c);
    if (action >= 0)
        win_menu_exec(c, action);
}

/* ── root menu ───────────────────────────────────────────────────────── */

static int root_menu_check(int action, void *ctx) {
    (void)ctx;
    if ((action == ROOT_SHUFFLE_UP || action == ROOT_SHUFFLE_DOWN) && !focused)
        return 0;
    return 1;
}

static void root_menu_exec(int action) {
    switch (action) {
    case ROOT_NEW: {
        static const char *new_argv[] = { "xterm", NULL };
        (void)cfg_term_cmd; /* use term_cmd as the command to spawn */
        new_argv[0] = cfg_term_cmd;
        WmArg arg = { .v = (const void *)new_argv };
        spawn(&arg);
        break;
    }
    case ROOT_SHUFFLE_UP:
        if (focused && focused->ws == curws)
            XRaiseWindow(dpy, XtWindow(focused->frame_shell));
        break;
    case ROOT_SHUFFLE_DOWN:
        if (focused && focused->ws == curws)
            XLowerWindow(dpy, XtWindow(focused->frame_shell));
        break;
    case ROOT_REFRESH:
        bg_load();
        arrange();
        for (Client *c = clients; c; c = c->next)
            if (!c->is_minimized && !c->is_hidden)
                drawframe(c);
        drawbar();
        updateiconbar();
        break;
    case ROOT_RESTART:
        if (show_confirm_dialog("Restart?"))
            restart_wm();
        break;
    case ROOT_QUIT:
        if (show_confirm_dialog("Quit?"))
            running = 0;
        break;
    }
}

void show_root_menu(int root_x, int root_y) {
    int action = run_menu(cfg_root_menu_items, cfg_num_root_menu_items, root_menu_check, NULL, root_x, root_y, 1);
    if (action >= 0)
        root_menu_exec(action);
}

/* ── confirmation dialog ─────────────────────────────────────────────── */

#define DLG_FW       FRAME_WIDTH
#define DLG_PAD      14
#define DLG_BTN_CORE_W 62   /* button core (raised bevel) width */
#define DLG_BTN_CORE_H 24   /* button core (raised bevel) height */
#define DLG_BTN_GAP  14
#define DLG_HL_THICK 3    /* focus highlight (black outline) thickness */
#define DLG_HL_GAP1  2    /* gap between highlight and sunken bevel */
#define DLG_HL_DARK  1    /* sunken bevel thickness in emphasis */
#define DLG_HL_GAP2  3    /* gap between sunken bevel and button shadow */
#define DLG_EMPHASIS (DLG_HL_THICK + DLG_HL_GAP1 + DLG_HL_DARK + DLG_HL_GAP2)

/* draw_dialog: focus_btn 0=OK,1=Cancel; pressed_btn -1=none,0=OK,1=Cancel */
static void draw_dialog(XftDraw *draw, int dlg_w, int dlg_h,
                        const char *message, int msg_len, int msg_w,
                        int btn_row_w, int focus_btn, int pressed_btn) {
    int cs = DLG_FW * 2;  /* corner size for simple beveled border */

    /* 1. fill border area with active frame color, body with dialog bg */
    XftDrawRect(draw, &col_title_focus, 0, 0, dlg_w, dlg_h);
    XftDrawRect(draw, &col_dlg_bg, DLG_FW, DLG_FW, dlg_w - 2*DLG_FW, dlg_h - 2*DLG_FW);

    /* 2. raised 3D border using active window colors */
    stretcher_corner(draw, 0, 0, STRETCH_NW, DLG_FW, cs, cs, &col_active_light, &col_active_shadow);
    stretcher_corner(draw, dlg_w - cs, 0, STRETCH_NE, DLG_FW, cs, cs, &col_active_light, &col_active_shadow);
    stretcher_corner(draw, dlg_w - cs, dlg_h - cs, STRETCH_SE, DLG_FW, cs, cs, &col_active_light, &col_active_shadow);
    stretcher_corner(draw, 0, dlg_h - cs, STRETCH_SW, DLG_FW, cs, cs, &col_active_light, &col_active_shadow);
    if (dlg_w > 2 * cs) {
        bevel_rect(draw, cs, 0, dlg_w - 2*cs, DLG_FW, 2, 1, 1, 1, &col_active_light, &col_active_shadow);
        bevel_rect(draw, cs, dlg_h - DLG_FW, dlg_w - 2*cs, DLG_FW, 1, 1, 2, 1, &col_active_light, &col_active_shadow);
    }
    if (dlg_h > 2 * cs) {
        bevel_rect(draw, 0, cs, DLG_FW, dlg_h - 2*cs, 1, 1, 1, 2, &col_active_light, &col_active_shadow);
        bevel_rect(draw, dlg_w - DLG_FW, cs, DLG_FW, dlg_h - 2*cs, 1, 2, 1, 1, &col_active_light, &col_active_shadow);
    }

    /* 3. message text */
    int content_y = DLG_FW + DLG_PAD;
    int text_y = content_y + xftfont->ascent;
    int text_x = DLG_FW + DLG_PAD + (dlg_w - 2 * DLG_FW - 2 * DLG_PAD - msg_w) / 2;
    XftDrawStringUtf8(draw, &col_btn_fg, xftfont,
                      text_x, text_y,
                      (XftChar8 *)message, msg_len);

    /* 4. separator line above buttons */
    int sep_y = content_y + xftfont->ascent - xftfont->descent + DLG_PAD;
    int sep_x = DLG_FW + 4;
    int sep_w = dlg_w - 2 * DLG_FW - 8;
    XftDrawRect(draw, &col_frame_shadow, sep_x, sep_y, sep_w, 1);
    XftDrawRect(draw, &col_frame_light, sep_x, sep_y + 1, sep_w, 1);

    /* 5. buttons — core positions (the raised-bevel part) */
    int btn_y = sep_y + 2 + DLG_PAD + DLG_EMPHASIS;

    /* draw default-button emphasis border AROUND a button core */
    /* 3px black → 2px normal → 1px sunken bevel → 3px normal, all outside the core */
    #define DRAW_BTN_EMPHASIS(cx, cy, cw, ch) do { \
        int _ex = (cx) - DLG_EMPHASIS; int _ey = (cy) - DLG_EMPHASIS; \
        int _ew = (cw) + 2*DLG_EMPHASIS; int _eh = (ch) + 2*DLG_EMPHASIS; \
        /* 3px black outline at outer edge */ \
        XftDrawRect(draw, &col_btn_fg, _ex, _ey, _ew, DLG_HL_THICK); \
        XftDrawRect(draw, &col_btn_fg, _ex, _ey+_eh-DLG_HL_THICK, _ew, DLG_HL_THICK); \
        XftDrawRect(draw, &col_btn_fg, _ex, _ey+DLG_HL_THICK, DLG_HL_THICK, _eh-2*DLG_HL_THICK); \
        XftDrawRect(draw, &col_btn_fg, _ex+_ew-DLG_HL_THICK, _ey+DLG_HL_THICK, DLG_HL_THICK, _eh-2*DLG_HL_THICK); \
        /* 2px normal + 1px sunken bevel + 3px normal: fill gap with bg, then sunken bevel */ \
        { int _ix = _ex+DLG_HL_THICK; int _iy = _ey+DLG_HL_THICK; \
          int _iw = _ew-2*DLG_HL_THICK; int _ih = _eh-2*DLG_HL_THICK; \
          XftDrawRect(draw, &col_dlg_bg, _ix, _iy, _iw, _ih); \
          int _dx = _ix + DLG_HL_GAP1; int _dy = _iy + DLG_HL_GAP1; \
          int _dw = _iw - 2*DLG_HL_GAP1; int _dh = _ih - 2*DLG_HL_GAP1; \
          bevel_rect_inv(draw, _dx, _dy, _dw, _dh, \
                         1, 1, 1, 1, &col_frame_light, &col_frame_shadow); \
        } \
    } while(0)

    /* center the button row */
    int row_x = DLG_FW + DLG_PAD + (dlg_w - 2*DLG_FW - 2*DLG_PAD - btn_row_w) / 2;

    /* OK button */
    {
        int ok_x = row_x + DLG_EMPHASIS;
        if (focus_btn == 0)
            DRAW_BTN_EMPHASIS(ok_x, btn_y, DLG_BTN_CORE_W, DLG_BTN_CORE_H);
        if (pressed_btn == 0)
            bevel_rect_inv(draw, ok_x, btn_y, DLG_BTN_CORE_W, DLG_BTN_CORE_H,
                           2, 2, 2, 2, &col_frame_light, &col_frame_shadow);
        else
            bevel_rect(draw, ok_x, btn_y, DLG_BTN_CORE_W, DLG_BTN_CORE_H,
                       2, 2, 2, 2, &col_frame_light, &col_frame_shadow);
        XGlyphInfo be;
        const char *lbl = "OK";
        XftTextExtents8(dpy, xftfont, (XftChar8 *)lbl, 2, &be);
        int lx = ok_x + (DLG_BTN_CORE_W - be.xOff) / 2;
        int ly = btn_y + (DLG_BTN_CORE_H + xftfont->ascent - xftfont->descent) / 2;
        XftDrawStringUtf8(draw, &col_btn_fg, xftfont,
                          lx, ly, (XftChar8 *)lbl, 2);
    }

    /* Cancel button */
    {
        int cancel_x = row_x + DLG_BTN_CORE_W + 2*DLG_EMPHASIS + DLG_BTN_GAP + DLG_EMPHASIS;
        if (focus_btn == 1)
            DRAW_BTN_EMPHASIS(cancel_x, btn_y, DLG_BTN_CORE_W, DLG_BTN_CORE_H);
        if (pressed_btn == 1)
            bevel_rect_inv(draw, cancel_x, btn_y, DLG_BTN_CORE_W, DLG_BTN_CORE_H,
                           2, 2, 2, 2, &col_frame_light, &col_frame_shadow);
        else
            bevel_rect(draw, cancel_x, btn_y, DLG_BTN_CORE_W, DLG_BTN_CORE_H,
                       2, 2, 2, 2, &col_frame_light, &col_frame_shadow);
        XGlyphInfo be;
        const char *lbl = "Cancel";
        int llen = (int)strlen(lbl);
        XftTextExtents8(dpy, xftfont, (XftChar8 *)lbl, llen, &be);
        int lx = cancel_x + (DLG_BTN_CORE_W - be.xOff) / 2;
        int ly = btn_y + (DLG_BTN_CORE_H + xftfont->ascent - xftfont->descent) / 2;
        XftDrawStringUtf8(draw, &col_btn_fg, xftfont,
                          lx, ly, (XftChar8 *)lbl, llen);
    }
    #undef DRAW_BTN_EMPHASIS
    XSync(dpy, False);
}

int show_confirm_dialog(const char *message) {
    /* dialog background color — loaded from config */
    if (!col_dlg_bg.pixel)
        xftcolor_load(cfg_color_dialog_bg, &col_dlg_bg);

    int msg_len = (int)strlen(message);
    XGlyphInfo ext;
    XftTextExtents8(dpy, xftfont, (XftChar8 *)message, msg_len, &ext);
    int msg_w = ext.xOff;

    /* dialog dimensions — WmDEFAULT border + body, no title bar */
    int btn_row_w = 2 * (DLG_BTN_CORE_W + 2*DLG_EMPHASIS) + DLG_BTN_GAP;
    int inner_w = (msg_w > btn_row_w ? msg_w : btn_row_w) + 2 * DLG_PAD;
    int dlg_w = inner_w + 2 * DLG_FW;
    int body_h = DLG_PAD + xftfont->ascent - xftfont->descent + DLG_PAD +
                 2 + DLG_PAD + DLG_BTN_CORE_H + 2*DLG_EMPHASIS + DLG_PAD;
    int dlg_h = 2 * DLG_FW + body_h;

    /* center on screen */
    int dlg_x = mon.x + (mon.w - dlg_w) / 2;
    int dlg_y = mon.y + (mon.h - dlg_h) / 2;

    /* create dialog window */
    XSetWindowAttributes dwa;
    dwa.background_pixel = 0;
    dwa.border_pixel = 0;
    dwa.colormap = argb_colormap;
    dwa.override_redirect = True;
    dwa.save_under = True;
    int dlg_depth = (argb_visual != xvisual) ? 32 : DefaultDepth(dpy, screen);
    Window dlg_win = XCreateWindow(dpy, root, dlg_x, dlg_y,
                                    (unsigned)dlg_w, (unsigned)dlg_h, 0,
                                    dlg_depth, InputOutput, argb_visual,
                                    CWBackPixel | CWBorderPixel | CWColormap |
                                    CWOverrideRedirect | CWSaveUnder,
                                    &dwa);
    XSelectInput(dpy, dlg_win,
                 ExposureMask | ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask | KeyPressMask);
    XMapWindow(dpy, dlg_win);
    XRaiseWindow(dpy, dlg_win);

    XftDraw *dlg_draw = XftDrawCreate(dpy, dlg_win, argb_visual, argb_colormap);

    /* grab pointer — stop cursor outside dialog, normal inside */
    Cursor curs_stop = XCreateFontCursor(dpy, XC_trek);
    Cursor curs_normal = XCreateFontCursor(dpy, XC_left_ptr);
    XGrabPointer(dpy, dlg_win, False,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync, None, curs_stop, CurrentTime);
    XGrabKeyboard(dpy, dlg_win, True, GrabModeAsync, GrabModeAsync, CurrentTime);

    int focus_btn = 0;    /* 0 = OK, 1 = Cancel */
    int pressed_btn = -1; /* -1 = none, 0 = OK pressed, 1 = Cancel pressed */

    draw_dialog(dlg_draw, dlg_w, dlg_h,
               message, msg_len, msg_w, btn_row_w, focus_btn, pressed_btn);
    compositor_repaint();

    /* precompute button positions (must match draw_dialog layout) */
    int content_y = DLG_FW + DLG_PAD;
    int sep_y = content_y + xftfont->ascent - xftfont->descent + DLG_PAD;
    int btn_y_pos = sep_y + 2 + DLG_PAD + DLG_EMPHASIS;
    int row_x = DLG_FW + DLG_PAD + (dlg_w - 2*DLG_FW - 2*DLG_PAD - btn_row_w) / 2;
    int ok_x_pos = row_x + DLG_EMPHASIS;
    int cancel_x_pos = row_x + DLG_BTN_CORE_W + 2*DLG_EMPHASIS + DLG_BTN_GAP + DLG_EMPHASIS;

    /* event loop */
    XEvent ev;
    int done = 0;
    int result = 0;  /* 0 = Cancel */

    while (!done && running) {
        XNextEvent(dpy, &ev);
        switch (ev.type) {
        case KeyPress: {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            if (ks == XK_Escape) {
                done = 1;
            } else if (ks == XK_Return || ks == XK_KP_Enter) {
                result = (focus_btn == 0) ? 1 : 0;
                done = 1;
            } else if (ks == XK_Tab || ks == XK_ISO_Left_Tab) {
                focus_btn = 1 - focus_btn;
                pressed_btn = -1;
                draw_dialog(dlg_draw, dlg_w, dlg_h,
                           message, msg_len, msg_w, btn_row_w, focus_btn, pressed_btn);
                compositor_repaint();
            }
            break;
        }
        case ButtonPress:
            if (ev.xbutton.x >= 0 && ev.xbutton.x < dlg_w &&
                ev.xbutton.y >= 0 && ev.xbutton.y < dlg_h) {
                /* hit zone includes emphasis border around each button */
                int emph_y = btn_y_pos - DLG_EMPHASIS;
                int emph_h = DLG_BTN_CORE_H + 2*DLG_EMPHASIS;
                int ok_emph_x = ok_x_pos - DLG_EMPHASIS;
                int ok_emph_w = DLG_BTN_CORE_W + 2*DLG_EMPHASIS;
                int cancel_emph_x = cancel_x_pos - DLG_EMPHASIS;
                int cancel_emph_w = DLG_BTN_CORE_W + 2*DLG_EMPHASIS;
                if (ev.xbutton.x >= ok_emph_x && ev.xbutton.x < ok_emph_x + ok_emph_w &&
                    ev.xbutton.y >= emph_y && ev.xbutton.y < emph_y + emph_h) {
                    focus_btn = 0;
                    pressed_btn = 0;
                    draw_dialog(dlg_draw, dlg_w, dlg_h,
                               message, msg_len, msg_w, btn_row_w, focus_btn, pressed_btn);
                    compositor_repaint();
                } else if (ev.xbutton.x >= cancel_emph_x && ev.xbutton.x < cancel_emph_x + cancel_emph_w &&
                           ev.xbutton.y >= emph_y && ev.xbutton.y < emph_y + emph_h) {
                    focus_btn = 1;
                    pressed_btn = 1;
                    draw_dialog(dlg_draw, dlg_w, dlg_h,
                               message, msg_len, msg_w, btn_row_w, focus_btn, pressed_btn);
                    compositor_repaint();
                }
            }
            /* clicks outside dialog or outside buttons: ignored */
            break;
        case ButtonRelease:
            if (pressed_btn >= 0) {
                int emph_y = btn_y_pos - DLG_EMPHASIS;
                int emph_h = DLG_BTN_CORE_H + 2*DLG_EMPHASIS;
                int ok_emph_x = ok_x_pos - DLG_EMPHASIS;
                int ok_emph_w = DLG_BTN_CORE_W + 2*DLG_EMPHASIS;
                int cancel_emph_x = cancel_x_pos - DLG_EMPHASIS;
                int cancel_emph_w = DLG_BTN_CORE_W + 2*DLG_EMPHASIS;
                int on_ok = (ev.xbutton.x >= ok_emph_x && ev.xbutton.x < ok_emph_x + ok_emph_w &&
                             ev.xbutton.y >= emph_y && ev.xbutton.y < emph_y + emph_h);
                int on_cancel = (ev.xbutton.x >= cancel_emph_x && ev.xbutton.x < cancel_emph_x + cancel_emph_w &&
                                 ev.xbutton.y >= emph_y && ev.xbutton.y < emph_y + emph_h);
                if (pressed_btn == 0 && on_ok) {
                    result = 1;
                    done = 1;
                } else if (pressed_btn == 1 && on_cancel) {
                    result = 0;
                    done = 1;
                }
                pressed_btn = -1;
                draw_dialog(dlg_draw, dlg_w, dlg_h,
                           message, msg_len, msg_w, btn_row_w, focus_btn, pressed_btn);
                compositor_repaint();
            }
            break;
        case MotionNotify: {
            /* switch cursor: normal inside dialog, stop outside */
            int in_dialog = (ev.xmotion.x >= 0 && ev.xmotion.x < dlg_w &&
                            ev.xmotion.y >= 0 && ev.xmotion.y < dlg_h);
            XChangeActivePointerGrab(dpy,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                in_dialog ? curs_normal : curs_stop, CurrentTime);

            if (pressed_btn >= 0) {
                int old_pressed = pressed_btn;
                int emph_y = btn_y_pos - DLG_EMPHASIS;
                int emph_h = DLG_BTN_CORE_H + 2*DLG_EMPHASIS;
                int ok_emph_x = ok_x_pos - DLG_EMPHASIS;
                int ok_emph_w = DLG_BTN_CORE_W + 2*DLG_EMPHASIS;
                int cancel_emph_x = cancel_x_pos - DLG_EMPHASIS;
                int cancel_emph_w = DLG_BTN_CORE_W + 2*DLG_EMPHASIS;
                int on_ok = (ev.xmotion.x >= ok_emph_x && ev.xmotion.x < ok_emph_x + ok_emph_w &&
                             ev.xmotion.y >= emph_y && ev.xmotion.y < emph_y + emph_h);
                int on_cancel = (ev.xmotion.x >= cancel_emph_x && ev.xmotion.x < cancel_emph_x + cancel_emph_w &&
                                 ev.xmotion.y >= emph_y && ev.xmotion.y < emph_y + emph_h);
                if (pressed_btn == 0 && on_ok)
                    pressed_btn = 0;
                else if (pressed_btn == 1 && on_cancel)
                    pressed_btn = 1;
                else
                    pressed_btn = -1;
                if (pressed_btn != old_pressed) {
                    draw_dialog(dlg_draw, dlg_w, dlg_h,
                               message, msg_len, msg_w, btn_row_w, focus_btn, pressed_btn);
                    compositor_repaint();
                }
            }
            break;
        }
        case Expose:
            if (ev.xexpose.count == 0) {
                draw_dialog(dlg_draw, dlg_w, dlg_h,
                            message, msg_len, msg_w, btn_row_w, focus_btn, pressed_btn);
                compositor_repaint();
            }
            break;
        default:
            if (compositor_running && ev.type == damage_event_base + XDamageNotify) {
                compositor_handle_damage((XDamageNotifyEvent *)&ev);
            }
            break;
        }
    }

    XUngrabKeyboard(dpy, CurrentTime);
    XUngrabPointer(dpy, CurrentTime);
    XFreeCursor(dpy, curs_stop);
    XFreeCursor(dpy, curs_normal);
    XftDrawDestroy(dlg_draw);
    compositor_untrack_window(dlg_win);
    XDestroyWindow(dpy, dlg_win);
    XFlush(dpy);
    return result;
}