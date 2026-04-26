/*
 * rondo — X event handlers and main event loop
 */
#include "wm.h"

int handle_buttonpress(XButtonEvent *ev) {
    last_event_time = ev->time;
    /* Right-click on root → context menu (no modifier) */
    if (ev->window == root && ev->button == Button3 && !(ev->state & MODKEY)) {
        show_root_menu(ev->x_root, ev->y_root);
        return 1;
    }

    /* Alt+click on root — find topmost client under cursor */
    if (ev->window == root) {
        Window child;
        int wx, wy;
        unsigned int mask;
        XQueryPointer(dpy, root, &child, &child, &wx, &wy, &wx, &wy, &mask);
        /* child is the topmost window under cursor — find its client */
        Client *c = NULL;
        if (child != None) {
            /* Walk up from child to find which frame it belongs to */
            for (Client *t = clients; t; t = t->next) {
                if (t->ws != curws || t->is_minimized) continue;
                if (child == XtWindow(t->frame_shell) ||
                    child == XtWindow(t->frame_form) ||
                    child == t->win) {
                    c = t;
                    break;
                }
            }
            /* If child is a subwindow (e.g., reparented client content),
             * walk the parent tree to find the frame */
            if (!c) {
                Window root_ret, parent;
                Window *children;
                unsigned int nchildren;
                Window w = child;
                while (XQueryTree(dpy, w, &root_ret, &parent, &children, &nchildren)) {
                    XFree(children);
                    if (parent == root) break;
                    for (Client *t = clients; t; t = t->next) {
                        if (t->ws != curws || t->is_minimized) continue;
                        if (parent == XtWindow(t->frame_shell)) {
                            c = t;
                            goto found;
                        }
                    }
                    w = parent;
                }
            }
        }
        found:
        /* Fallback: if XQueryPointer didn't find a window, check geometry */
        if (!c) {
            for (Client *t = clients; t; t = t->next) {
                if (t->ws != curws || t->is_minimized) continue;
                if (wx >= t->x && wx < t->x + t->w && wy >= t->y && wy < t->y + t->h) {
                    c = t;
                }
            }
        }
        if (c && (ev->button == Button1 || ev->button == Button3)) {
            XRaiseWindow(dpy, XtWindow(c->frame_shell));
            focus(c);
            if (!c->is_floating && ev->button == Button1) {
                /* Alt+LClick on tiled: make floating, then drag from title */
                WmArg arg = {0};
                togglefloat(&arg);
                c->pressed_btn = BTN_TITLE;
                drawframe(c);
                mousemove(c, Button1, EDGE_NONE, ev->x_root, ev->y_root);
                c->pressed_btn = BTN_NONE;
                drawframe(c);
            } else {
                mousemove(c, ev->button, EDGE_NONE, ev->x_root, ev->y_root);
            }
        }
        return 1;  /* event consumed by move/resize or root click */
    }

    Client *c = wintoclient(ev->window);
    if (!c) return 0;

    focus(c);

    /* Alt+click on client window for move/resize */
    if ((ev->state & MODKEY) && (ev->button == Button1 || ev->button == Button3)) {
        mousemove(c, ev->button, EDGE_NONE, ev->x_root, ev->y_root);
        return 1;  /* event consumed by move/resize */
    }
    return 0;
}

void handle_clientmessage(XClientMessageEvent *ev) {
    /* system tray dock request */
    if (ev->message_type == net_system_tray_opcode &&
        ev->data.l[1] == 0 /* SYSTEM_TRAY_REQUEST_DOCK */) {
        Window icon_win = (Window)ev->data.l[2];
        tray_dock(icon_win);
        return;
    }

    if (ev->message_type == net_wm_state &&
        ev->data.l[1] == (long)net_wm_state_fullscreen) {
        Client *c = wintoclient(ev->window);
        if (!c) return;
        if (ev->data.l[0] == 0) {
            /* unset fullscreen */
            if (c->is_fullscreen)
                togglefullscreen(NULL);
        } else if (ev->data.l[0] == 1 || ev->data.l[0] == 2) {
            /* set or toggle fullscreen */
            if (!c->is_fullscreen)
                togglefullscreen(NULL);
        }
    }

    /* EWMH: _NET_CLOSE_WINDOW */
    if (ev->message_type == net_close_window) {
        Client *c = wintoclient(ev->window);
        if (c) killclient(NULL);
    }

    /* ICCCM: WM_CHANGE_STATE for iconification */
    if (ev->message_type == wm_change_state) {
        Client *c = wintoclient(ev->window);
        if (!c) return;
        if (ev->data.l[0] == IconicState)
            minimize_client(c);
        else if (ev->data.l[0] == NormalState)
            restore_client(c);
    }
}

void handle_configurenotify(XConfigureEvent *ev) {
    if (ev->window != root) {
        /* non-root ConfigureNotify: invalidate compositor cached picture */
        compositor_configure_window(ev->window);
        return;
    }
    sw = ev->width;
    sh = ev->height;
    mon.w = sw;
    mon.h = sh;
    BarGeometry g = calc_bar_geometry();
    XMoveResizeWindow(dpy, barwin, g.bar_x, g.bar_y,
                      g.bar_w > 0 ? (unsigned)g.bar_w : 1,
                      g.bar_h > 0 ? (unsigned)g.bar_h : 1);
    if (show_bar)
        XMapWindow(dpy, barwin);
    updateiconbar();
}

void handle_configurerequest(XConfigureRequestEvent *ev) {
    Client *c = wintoclient(ev->window);
    if (c) {
        if (c->is_floating) {
            int fboff = c->no_decor ? 0 : FRAME_WIDTH;
            int toff  = c->no_decor ? 0 : TITLE_HEIGHT;
            int cw = (ev->value_mask & CWWidth) ? ev->width : c->w - 2 * fboff;
            int ch = (ev->value_mask & CWHeight) ? ev->height : c->h - 2 * fboff - toff;
            int fw, fh;
            client_to_frame(cw, ch, &fw, &fh, c->no_decor);
            if (ev->value_mask & CWX)      c->x = ev->x - fboff;
            if (ev->value_mask & CWY)      c->y = ev->y - fboff - toff;
            /* clamp to usable area: avoid bar and keep on screen */
            {
                BarGeometry g = calc_bar_geometry();
                if (c->y < g.y) c->y = g.y;
                if (c->x < g.x) c->x = g.x;
                if (c->x + c->w > g.x + g.w) c->x = g.x + g.w - c->w;
                if (c->y + c->h > g.y + g.h) c->y = g.y + g.h - c->h;
            }
            c->w = fw; c->h = fh;
            moveresizeframe(c);
            int cx, cy, ncw, nch;
            frame_to_client(c->w, c->h, &cx, &cy, &ncw, &nch, c->no_decor);
            XMoveResizeWindow(dpy, c->win, cx, cy, ncw, nch);
            updateframe(c);
            send_configure_notify(c);
        } else {
            arrange();
        }
    } else {
        /* check if this is a tray icon requesting a resize */
        int is_tray_icon = 0;
        for (int i = 0; i < num_tray_icons; i++) {
            if (tray_icons[i].icon == ev->window) {
                is_tray_icon = 1;
                break;
            }
        }
        if (is_tray_icon) {
            int sz = tray_icon_size();
            XWindowChanges wc;
            wc.x = 0; wc.y = 0;
            wc.width = (ev->value_mask & CWWidth) ? MIN(ev->width, sz) : sz;
            wc.height = (ev->value_mask & CWHeight) ? MIN(ev->height, sz) : sz;
            wc.border_width = 0;
            XConfigureWindow(dpy, ev->window,
                            CWX | CWY | CWWidth | CWHeight | CWBorderWidth,
                            &wc);
            return;
        }
        XWindowChanges wc;
        wc.x = ev->x; wc.y = ev->y; wc.width = ev->width; wc.height = ev->height;
        wc.border_width = ev->border_width; wc.sibling = ev->above; wc.stack_mode = ev->detail;
        XConfigureWindow(dpy, ev->window, ev->value_mask, &wc);
    }
}

void handle_destroynotify(XDestroyWindowEvent *ev) {
    /* check if a tray icon was destroyed */
    for (int i = 0; i < num_tray_icons; i++) {
        if (tray_icons[i].icon == ev->window) {
            tray_remove(ev->window);
            return;
        }
    }

    /* untrack from compositor if this window was redirected */
    compositor_untrack_window(ev->window);
    /* Only match on the client window, not the frame */
    for (Client *c = clients; c; c = c->next) {
        if (c->win == ev->window) {
            /* remove from client list */
            if (clients == c) {
                clients = c->next;
            } else {
                for (Client *p = clients; p; p = p->next) {
                    if (p->next == c) { p->next = c->next; break; }
                }
            }
            if (focused == c)
                focus(nexttiled(clients));
            update_client_list();
            update_active_window();
            updateiconbar();
            if (c->is_closing) {
                /* killclient path: already faded out, just clean up */
                fade_cancel(c);
                unmanage_destroyed_cb(c);
            } else if (fade_enabled && compositor_running) {
                c->is_closing = 1;
                fade_window_out(c, unmanage_destroyed_cb);
            } else {
                unmanage(c, 1);
            }
            return;
        }
    }
}

void handle_enternotify(XCrossingEvent *ev) {
    if (ev->mode != NotifyNormal || ev->detail == NotifyInferior) return;
    Client *c = wintoclient(ev->window);
    if (c && c->ws == curws && !c->is_minimized) focus(c);
}

void handle_keypress(XKeyEvent *ev) {
    last_event_time = ev->time;
    KeySym ks = XkbKeycodeToKeysym(dpy, ev->keycode, 0, 0);
    for (int i = 0; i < num_keys; i++) {
        unsigned int mod = keys[i].mod & ~(LockMask | Mod2Mask);
        if (keys[i].keysym == ks && mod == (ev->state & ~(LockMask | Mod2Mask))) {
            keys[i].func(&keys[i].arg);
            break;
        }
    }
}

void handle_maprequest(XMapRequestEvent *ev) {
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, ev->window, &wa)) return;
    if (wa.override_redirect) return;
    manage(ev->window, &wa);
}

void handle_unmapnotify(XUnmapEvent *ev) {
    Client *c = wintoclient(ev->window);
    if (!c) return;
    /* Ignore unmap from intentional minimize or workspace hide */
    if (c->is_minimized || c->is_hidden) return;
    /* Accept unmap from the frame form window or root */
    if (ev->event == XtWindow(c->frame_form) || ev->event == root)
        unmanage(c, 0);
}

/* ── main event loop ────────────────────────────────────────────────── */

void run(void) {
    XEvent ev;
    while (running) {
        XtAppNextEvent(app, &ev);

        /* Check if the event should be handled by the WM.
         * ButtonPress on frame_shell/frame_form is handled by our Xt event handler
         * (frame_btn_cb). ButtonPress on the client window with Alt is handled by
         * the WM for drag. ButtonPress on root is handled by the WM.
         * Expose on frame_form is handled by frame_expose_cb. */
        int wm_event = 1;
        if (ev.type == ButtonPress) {
            Client *c = wintoclient(ev.xany.window);
            if (c && ev.xany.window != c->win &&
                ev.xany.window != XtWindow(c->frame_shell) &&
                ev.xany.window != XtWindow(c->frame_form)) {
                /* Unknown window — skip */
                wm_event = 0;
            } else if (c && (ev.xany.window == XtWindow(c->frame_shell) ||
                            ev.xany.window == XtWindow(c->frame_form))) {
                /* ButtonPress on frame — our Xt handler (frame_btn_cb) processes it */
                wm_event = 0;
            }
        }

        int skip_dispatch = 0;
        if (wm_event) {
            switch (ev.type) {
            case ButtonPress:
                if (ev.xbutton.window == iconbar) {
                    hide_icon_tooltip();
                    /* scroll wheel: Button4 = up/left, Button5 = down/right */
                    int scroll_amt = is_vertical(iconbar_position) ? icon_entry_h() : ICON_W;
                    if (ev.xbutton.button == Button4)
                        iconbar_scroll_by(-scroll_amt);
                    else if (ev.xbutton.button == Button5)
                        iconbar_scroll_by(scroll_amt);
                    else
                        handle_iconbar_click(ev.xbutton.x, ev.xbutton.y);
                    break;
                }
                if (ev.xbutton.window == barwin) {
                    if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)
                        handle_bar_scroll(ev.xbutton.x, ev.xbutton.y, ev.xbutton.button);
                    else
                        handle_bar_click(ev.xbutton.x, ev.xbutton.y);
                    break;
                }
                if (handle_buttonpress(&ev.xbutton)) {
                    skip_dispatch = 1;  /* event consumed by move/resize */
                    break;
                }
                break;
            case ClientMessage:    handle_clientmessage(&ev.xclient);              break;
            case ConfigureNotify:  handle_configurenotify(&ev.xconfigure);         break;
            case ConfigureRequest: handle_configurerequest(&ev.xconfigurerequest); break;
            case DestroyNotify:    handle_destroynotify(&ev.xdestroywindow);       break;
            case EnterNotify:      handle_enternotify(&ev.xcrossing);              break;
            case MotionNotify:
                if (ev.xmotion.window == iconbar)
                    show_icon_tooltip(ev.xmotion.x, ev.xmotion.y);
                break;
            case LeaveNotify:
                if (ev.xcrossing.window == iconbar)
                    hide_icon_tooltip();
                break;
            case KeyPress:         handle_keypress(&ev.xkey);                       break;
            case MapRequest:       handle_maprequest(&ev.xmaprequest);              break;
            case MapNotify:
                compositor_repaint();
                break;
            case UnmapNotify:
                handle_unmapnotify(&ev.xunmap);
                compositor_repaint();
                break;
            case Expose:
                if (ev.xexpose.window == tooltip_win && ev.xexpose.count == 0) {
                    if (tooltip_text_len > 0) {
                        int pad = 6;
                        XWindowAttributes twa;
                        if (XGetWindowAttributes(dpy, tooltip_win, &twa)) {
                            XftDrawRect(tooltip_draw, &col_tooltip_bg, 0, 0, twa.width, twa.height);
                            bevel_rect(tooltip_draw, 0, 0, twa.width, twa.height,
                                       1, 1, 1, 1, &col_tooltip_border, &col_tooltip_border);
                            XftDrawStringUtf8(tooltip_draw, &col_tooltip_fg, tooltip_font,
                                               pad, pad + tooltip_font->ascent,
                                               (XftChar8 *)tooltip_text, tooltip_text_len);
                        }
                    }
                    break;
                }
                if (ev.xexpose.window == barwin && ev.xexpose.count == 0)
                    drawbar();
                if (ev.xexpose.window == iconbar && ev.xexpose.count == 0)
                    drawiconbar();
                if (ev.xexpose.count == 0) {
                    Client *c = wintoclient(ev.xexpose.window);
                    if (c) {
                        drawframe(c);
                        compositor_repaint();
                    }
                }
                break;
            case PropertyNotify:
                if (ev.xproperty.state == PropertyNewValue) {
                    Client *c = wintoclient(ev.xproperty.window);
                    if (c) {
                        static Atom wm_name = None;
                        if (!wm_name) wm_name = XInternAtom(dpy, "WM_NAME", False);
                        if (ev.xproperty.atom == wm_name ||
                            ev.xproperty.atom == net_wm_name_atom) {
                            updatewindowname(c);
                            updateframe(c);
                        } else if (ev.xproperty.atom == wm_protocols) {
                            c->take_focus = client_supports_protocol(c, wm_take_focus);
                            c->delete_window = client_supports_protocol(c, wm_delete_window);
                        } else if (ev.xproperty.atom == XA_WM_HINTS) {
                            XWMHints *hints = XGetWMHints(dpy, c->win);
                            if (hints) {
                                if (hints->flags & InputHint)
                                    c->input_hint = hints->input;
                                if (hints->flags & IconPixmapHint)
                                    c->icon_pixmap = hints->icon_pixmap;
                                else
                                    c->icon_pixmap = None;
                                if (hints->flags & IconMaskHint)
                                    c->icon_mask = hints->icon_mask;
                                else
                                    c->icon_mask = None;
                                if (hints->flags & IconWindowHint)
                                    c->icon_window = hints->icon_window;
                                else
                                    c->icon_window = None;
                                XFree(hints);
                            }
                        } else if (ev.xproperty.atom == wm_normal_hints) {
                            read_size_hints(c);
                        } else if (ev.xproperty.atom == wm_colormap_windows) {
                            /* read WM_COLORMAP_WINDOWS — use first colormap */
                            Atom actual_type;
                            int actual_format;
                            unsigned long nitems, bytes_after;
                            unsigned char *data = NULL;
                            if (XGetWindowProperty(dpy, c->win, wm_colormap_windows,
                                                   0, 1024, False, XA_WINDOW,
                                                   &actual_type, &actual_format,
                                                   &nitems, &bytes_after,
                                                   &data) == Success && data) {
                                Window *wins = (Window *)data;
                                if (nitems > 0) {
                                    XWindowAttributes cwa;
                                    if (XGetWindowAttributes(dpy, wins[0], &cwa))
                                        c->cmap = cwa.colormap;
                                }
                                XFree(data);
                            }
                        }
                    }
                }
                break;
            case ColormapNotify:
                if (ev.xcolormap.state == ColormapInstalled &&
                    ev.xcolormap.window != None) {
                    Client *c = wintoclient(ev.xcolormap.window);
                    if (c && c == focused)
                        XInstallColormap(dpy, c->cmap);
                }
                break;
            case SelectionClear:
                if (ev.xselectionclear.window == tray_win)
                    tray_cleanup();
                break;
            default:
                /* handle DamageNotify for built-in compositor */
                if (compositor_running && ev.type == damage_event_base + XDamageNotify) {
                    compositor_handle_damage((XDamageNotifyEvent *)&ev);
                    skip_dispatch = 1;
                }
                break;
            }
        }
        if (!skip_dispatch)
            XtDispatchEvent(&ev);
    }
}