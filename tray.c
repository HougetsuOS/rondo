/*
 * rondo — system tray (XEmbed notification area)
 */
#include "wm.h"

TrayIcon *tray_icons = NULL;
int num_tray_icons = 0;
Window tray_win = None;

Atom net_system_tray;
Atom net_system_tray_visual;
Atom net_system_tray_opcode;
Atom manager_atom;
Atom xembed;

int tray_widget_x = 0, tray_widget_y = 0;
int tray_widget_w = 0, tray_widget_h = 0;

int tray_icon_size(void) {
    return BAR_BTN_WIDTH > 0 ? BAR_BTN_WIDTH : 18;
}

void tray_init(void) {
    tray_icons = NULL;
    num_tray_icons = 0;

    tray_win = XCreateWindow(dpy, root, -1, -1, 1, 1, 0,
                             CopyFromParent, CopyFromParent, CopyFromParent,
                             0, NULL);
    XSelectInput(dpy, tray_win, StructureNotifyMask);

    /* advertise the visual for tray icons */
    if (argb_visual) {
        VisualID vid = XVisualIDFromVisual(argb_visual);
        XChangeProperty(dpy, tray_win, net_system_tray_visual,
                        XA_VISUALID, 32, PropModeReplace,
                        (unsigned char *)&vid, 1);
    }

    /* acquire the _NET_SYSTEM_TRAY_S0 selection */
    XSetSelectionOwner(dpy, net_system_tray, tray_win, CurrentTime);

    /* broadcast MANAGER message */
    XClientMessageEvent ev = {0};
    ev.type = ClientMessage;
    ev.window = root;
    ev.message_type = manager_atom;
    ev.format = 32;
    ev.data.l[0] = CurrentTime;
    ev.data.l[1] = (long)net_system_tray;
    ev.data.l[2] = (long)tray_win;
    XSendEvent(dpy, root, False, StructureNotifyMask, (XEvent *)&ev);
}

void tray_dock(Window icon_win) {
    /* check for duplicate */
    for (int i = 0; i < num_tray_icons; i++)
        if (tray_icons[i].icon == icon_win)
            return;

    /* determine icon window's visual/depth */
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, icon_win, &wa)) return;

    Visual *icon_visual = wa.visual;
    int icon_depth = wa.depth;
    Colormap icon_cmap = wa.colormap;

    /* create a wrapper window inside barwin matching the icon's visual */
    int sz = tray_icon_size();
    Window wrapper = XCreateWindow(dpy, barwin, 0, 0, sz, sz, 0,
                                   icon_depth, InputOutput, icon_visual,
                                   CWBackPixel | CWBorderPixel | CWColormap,
                                   &(XSetWindowAttributes){
                                       .background_pixel = 0,
                                       .border_pixel = 0,
                                       .colormap = icon_cmap,
                                   });
    XSelectInput(dpy, wrapper, SubstructureNotifyMask | SubstructureRedirectMask);

    /* reparent icon into wrapper */
    XReparentWindow(dpy, icon_win, wrapper, 0, 0);
    XMoveResizeWindow(dpy, icon_win, 0, 0, sz, sz);
    XSelectInput(dpy, icon_win,
                 StructureNotifyMask | PropertyChangeMask | ResizeRedirectMask);

    /* send XEMBED_EMBEDDED_NOTIFY */
    XEvent ev = {0};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = icon_win;
    ev.xclient.message_type = xembed;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = CurrentTime;
    ev.xclient.data.l[1] = 0; /* XEMBED_EMBEDDED_NOTIFY */
    ev.xclient.data.l[2] = (long)wrapper;
    ev.xclient.data.l[3] = 0; /* XEMBED version */
    XSendEvent(dpy, icon_win, False, NoEventMask, &ev);

    XMapWindow(dpy, icon_win);
    XMapWindow(dpy, wrapper);

    /* add to tray array */
    tray_icons = realloc(tray_icons, sizeof(TrayIcon) * (size_t)(num_tray_icons + 1));
    tray_icons[num_tray_icons].wrapper = wrapper;
    tray_icons[num_tray_icons].icon = icon_win;
    num_tray_icons++;

    tray_update();
}

void tray_remove(Window icon_win) {
    for (int i = 0; i < num_tray_icons; i++) {
        if (tray_icons[i].icon == icon_win || tray_icons[i].wrapper == icon_win) {
            XDestroyWindow(dpy, tray_icons[i].wrapper);
            tray_icons[i] = tray_icons[num_tray_icons - 1];
            num_tray_icons--;
            tray_icons = realloc(tray_icons, sizeof(TrayIcon) * (size_t)(num_tray_icons > 0 ? num_tray_icons : 1));
            tray_update();
            return;
        }
    }
}

void tray_update(void) {
    drawbar();
}

void tray_reposition(void) {
    if (tray_widget_w <= 0 || tray_widget_h <= 0 || num_tray_icons <= 0) return;
    int sz = tray_icon_size();
    int pad = 2;
    if (is_horizontal(bar_position)) {
        int x = tray_widget_x;
        int y = tray_widget_y + (tray_widget_h - sz) / 2;
        for (int i = 0; i < num_tray_icons; i++) {
            XMoveResizeWindow(dpy, tray_icons[i].wrapper, x, y, sz, sz);
            XMoveResizeWindow(dpy, tray_icons[i].icon, 0, 0, sz, sz);
            x += sz + pad;
        }
    } else {
        int x = tray_widget_x + (tray_widget_w - sz) / 2;
        int y = tray_widget_y;
        for (int i = 0; i < num_tray_icons; i++) {
            XMoveResizeWindow(dpy, tray_icons[i].wrapper, x, y, sz, sz);
            XMoveResizeWindow(dpy, tray_icons[i].icon, 0, 0, sz, sz);
            y += sz + pad;
        }
    }
}

void tray_cleanup(void) {
    for (int i = 0; i < num_tray_icons; i++) {
        XUnmapWindow(dpy, tray_icons[i].icon);
        XReparentWindow(dpy, tray_icons[i].icon, root, 0, 0);
        XDestroyWindow(dpy, tray_icons[i].wrapper);
    }
    free(tray_icons);
    tray_icons = NULL;
    num_tray_icons = 0;
    if (tray_win != None) {
        XDestroyWindow(dpy, tray_win);
        tray_win = None;
    }
}