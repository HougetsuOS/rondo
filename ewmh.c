/*
 * rondo — EWMH helpers
 */
#include "wm.h"
#include "xmplat_seam.h"

void update_client_list(void) {
    /* stack buffer for typical client counts — avoids malloc/free churn */
    Window stack_buf[128] = {0};
    int n = 0;
    for (Client *c = clients; c; c = c->next) n++;
    Window *wins = stack_buf;
    if (n > 128) {
        wins = malloc(sizeof(Window) * (size_t)n);
        if (!wins) return;
    }
    int i = 0;
    for (Client *c = clients; c; c = c->next)
        wins[i++] = c->win;
    _XmPlatChangeProperty(dpy, (unsigned long)root, net_client_list, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)wins, n);
    if (wins != stack_buf) free(wins);
}

void update_active_window(void) {
    Window active = focused ? focused->win : None;
    _XmPlatChangeProperty(dpy, (unsigned long)root, net_active_window, XA_WINDOW, 32, PropModeReplace, (const unsigned char *)&active, 1);
}

void update_net_desktops(void) {
    long ndesktops = NUM_WORKSPACES;
    _XmPlatChangeProperty(dpy, (unsigned long)root, net_number_of_desktops, XA_CARDINAL, 32, PropModeReplace, (const unsigned char *)&ndesktops, 1);

    /* viewport: always [0, 0] for single-monitor no-viewport */
    long viewport[2] = { 0, 0 };
    _XmPlatChangeProperty(dpy, (unsigned long)root, net_desktop_viewport, XA_CARDINAL, 32, PropModeReplace, (const unsigned char *)viewport, 2);

    /* current desktop */
    long cur = curws;
    _XmPlatChangeProperty(dpy, (unsigned long)root, net_current_desktop, XA_CARDINAL, 32, PropModeReplace, (const unsigned char *)&cur, 1);
}

void update_workarea(void) {
    BarGeometry g = calc_bar_geometry();
    long workarea[4] = {
        g.x, g.y, g.w, g.h
    };
    _XmPlatChangeProperty(dpy, (unsigned long)root, net_workarea, XA_CARDINAL, 32, PropModeReplace, (const unsigned char *)workarea, 4);
}