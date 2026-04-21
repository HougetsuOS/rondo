/*
 * rondo — EWMH helpers
 */
#include "wm.h"

void update_client_list(void) {
    int n = 0;
    for (Client *c = clients; c; c = c->next) n++;
    Window *wins = malloc(sizeof(Window) * (size_t)(n > 0 ? n : 1));
    if (!wins) return;
    int i = 0;
    for (Client *c = clients; c; c = c->next)
        wins[i++] = c->win;
    XChangeProperty(dpy, root, net_client_list, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)wins, n);
    free(wins);
}

void update_active_window(void) {
    Window active = focused ? focused->win : None;
    XChangeProperty(dpy, root, net_active_window, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&active, 1);
}

void update_net_desktops(void) {
    long ndesktops = NUM_WORKSPACES;
    XChangeProperty(dpy, root, net_number_of_desktops, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&ndesktops, 1);

    /* viewport: always [0, 0] for single-monitor no-viewport */
    long viewport[2] = { 0, 0 };
    XChangeProperty(dpy, root, net_desktop_viewport, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)viewport, 2);

    /* current desktop */
    long cur = curws;
    XChangeProperty(dpy, root, net_current_desktop, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&cur, 1);
}

void update_workarea(void) {
    BarGeometry g = calc_bar_geometry();
    long workarea[4] = {
        g.x, g.y, g.w, g.h
    };
    XChangeProperty(dpy, root, net_workarea, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)workarea, 4);
}