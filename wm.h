/*
 * rondo — shared header
 */
#ifndef WM_H
#define WM_H

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/Intrinsic.h>
#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>
#include <errno.h>
#include <signal.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <sys/statvfs.h>

#include "config.h"
#include "ipc.h"

/* ── constants ───────────────────────────────────────────────────────── */

/* Button identifiers for hit-testing */
#define BTN_NONE  0
#define BTN_CLOSE 1
#define BTN_MAX   2
#define BTN_MIN   3
#define BTN_FLOAT 4
#define BTN_MENU  5
#define BTN_TITLE 6

/* Resize edge hit constants (bitmask for corners) */
#define EDGE_NONE  0
#define EDGE_N     1
#define EDGE_S     2
#define EDGE_W     4
#define EDGE_E     8
#define EDGE_NW    (EDGE_N|EDGE_W)
#define EDGE_NE    (EDGE_N|EDGE_E)
#define EDGE_SW    (EDGE_S|EDGE_W)
#define EDGE_SE    (EDGE_S|EDGE_E)

/* Stretcher corner identifiers */
#define STRETCH_NW 0
#define STRETCH_NE 2
#define STRETCH_SE 4
#define STRETCH_SW 6

/* Rubber-band outline thickness */
#define OUTLINE_THICKNESS 3

/* Feedback window padding */
#define FB_PAD 6

/* ── types ───────────────────────────────────────────────────────────── */

typedef struct Client Client;

struct Client {
    Window win;           /* client window */
    Widget frame_shell;   /* overrideShell — the frame toplevel */
    Widget frame_form;    /* XmForm — layout container (shadowThickness=0) */
    XftDraw *frame_draw;  /* Xft draw context for manual frame drawing */
    int x, y, w, h;      /* frame geometry (not client) */
    int oldx, oldy, oldw, oldh; /* saved floating geometry */
    int ws;               /* workspace index */
    int is_floating;
    int is_fullscreen;
    int is_minimized;
    int is_hidden;         /* hidden by workspace switch */
    int pressed_btn;       /* button being held down (BTN_NONE..BTN_TITLE), or 0 */
    int no_decor;          /* no frame decorations requested */
    int no_resize;         /* resize disabled */
    int no_minimize;       /* minimize disabled */
    int no_maximize;       /* maximize disabled */
    Pixmap icon_pixmap;   /* WM_HINTS icon pixmap (None if unavailable) */
    Pixmap icon_mask;     /* WM_HINTS icon mask (None if unavailable) */
    int icon_w, icon_h;   /* actual icon pixmap dimensions (0 if none) */
    Window icon_window;   /* WM_HINTS icon window (None if unavailable) */
    Colormap cmap;        /* client's colormap (None if default) */
    int take_focus;        /* client claims WM_TAKE_FOCUS in WM_PROTOCOLS */
    int delete_window;     /* client claims WM_DELETE_WINDOW in WM_PROTOCOLS */
    int input_hint;        /* WM_HINTS input field: True for direct SetInputFocus */
    int size_hints_flags;  /* stored PMinSize/PMaxSize/PResizeInc/PBaseSize/PAspect */
    int min_width, min_height;
    int max_width, max_height;
    int width_inc, height_inc;
    int base_width, base_height;
    int min_aspect_x, min_aspect_y;
    int max_aspect_x, max_aspect_y;
    char name[256];       /* window title (WM_NAME) */
    int is_closing;          /* client is being destroyed, frame kept for fade-out */
    int fading;                    /* 0=none, 1=fading in, -1=fading out */
    unsigned int opacity;         /* current opacity 0..0xFFFFFFFF */
    XtIntervalId fade_timer;      /* active timer ID, 0 if none */
    void (*fade_done_cb)(Client *); /* called when fade-out completes */
    void *btree_node;       /* binary tree leaf node (BTreeNode*, or NULL) */
    Client *next;
};

typedef struct {
    int x, y, w, h;    /* monitor geometry */
} Monitor;

/* bar geometry — computed from bar/iconbar positions and visibility */
typedef struct {
    int x, y, w, h;                     /* tiling area (usable region) */
    int bar_x, bar_y, bar_w, bar_h;     /* status bar window rect */
    int ibar_x, ibar_y, ibar_w, ibar_h; /* icon bar window rect */
} BarGeometry;

/* ── globals (defined in main.c) ───────────────────────────────────── */

extern Display *dpy;
extern int screen;
extern Window root;
extern Window barwin;
extern Window iconbar;
extern Window checkwin;
extern int running;
extern int (*xerrorxlib)(Display *, XErrorEvent *);

extern XtAppContext app;
extern Widget toplevel_shell;

extern Client *clients;
extern Client *focused;

extern int curws;
extern float mfact;
extern int cur_layout;

/* Layout types */
#define LAYOUT_MASTER_STACK 0
#define LAYOUT_BINARY_TREE  1

extern Monitor mon;

extern GC gc;
extern GC xor_gc;
extern GC icon_gc;

extern Window fb_win;
extern XftDraw *fb_draw;
extern int fb_win_w, fb_win_h;
extern int fb_bevel;

extern XftFont *xftfont;
extern XftFont *tooltip_font;
extern XftDraw *xftdraw;
extern XftDraw *iconbar_draw;
extern Pixmap iconbar_buf;
extern XftDraw *iconbar_buf_draw;
extern int iconbar_scroll;
extern Window tooltip_win;
extern XftDraw *tooltip_draw;
extern char tooltip_text[];
extern int tooltip_text_len;
extern int *ws_x;
extern int *ws_y;
extern Visual *xvisual;
extern Colormap xcolormap;
extern Visual *argb_visual;
extern Colormap argb_colormap;
extern GC argb_gc;

extern XftColor col_bar_bg, col_bar_fg, col_bar_ws_active, col_bar_ws_occupied, col_bar_ws_idle, col_bar_ws_bg;
extern XftColor col_bar_border_light, col_bar_border_shadow, col_bar_fill;
extern XftColor col_title_focus, col_title_unfocus, col_title_fg;
extern XftColor col_frame_light, col_frame_shadow, col_frame_bg, col_btn_fg;
extern XftColor col_active_light, col_active_shadow;
extern XftColor col_fb_bg, col_fb_light, col_fb_shadow, col_fb_fg;
extern XftColor col_tooltip_bg, col_tooltip_fg, col_tooltip_border;
extern XftColor col_menu_bg;
extern XftColor col_iconbar_bg;
extern XftColor col_root_bg;
extern XftColor col_root_bg2;
extern XftColor col_dlg_bg;

extern Cursor curs_resize[16];
extern Cursor curs_default;

extern int sw, sh;

/* ICCCM / EWMH atoms (interned once in setup) */
extern Atom wm_protocols, wm_delete_window, wm_take_focus;
extern Atom wm_state, wm_change_state, wm_normal_hints;
extern Atom wm_colormap_windows;
extern Atom net_supported, net_client_list, net_number_of_desktops;
extern Atom net_current_desktop, net_desktop_viewport, net_workarea;
extern Atom net_active_window, net_close_window;
extern Atom net_wm_state, net_wm_state_fullscreen;
extern Atom net_wm_desktop, net_wm_name_atom;
extern Atom net_wm_window_type, net_wm_window_type_dialog;
extern Atom net_wm_window_type_dock, net_wm_window_type_toolbar;
extern Atom net_wm_window_type_utility;
extern Atom net_wm_window_type_splash;
extern Atom net_wm_window_type_popup_menu;
extern Atom net_wm_window_type_dropdown_menu;
extern Atom net_wm_window_type_tooltip;
extern Atom net_wm_window_type_notification;
extern Atom motif_wm_hints;
extern Atom net_wm_window_opacity;
extern Atom net_wm_cm_s0;

/* MWM hints flags */
#define MWM_HINTS_FUNCTIONS    (1L << 0)
#define MWM_HINTS_DECORATIONS (1L << 1)

/* MWM functions */
#define MWM_FUNC_ALL       (1L << 0)
#define MWM_FUNC_RESIZE    (1L << 1)
#define MWM_FUNC_MOVE      (1L << 2)
#define MWM_FUNC_MINIMIZE  (1L << 3)
#define MWM_FUNC_MAXIMIZE  (1L << 4)
#define MWM_FUNC_CLOSE     (1L << 5)

/* MWM decorations */
#define MWM_DECOR_ALL      (1L << 0)
#define MWM_DECOR_BORDER   (1L << 1)
#define MWM_DECOR_RESIZEH  (1L << 2)
#define MWM_DECOR_TITLE    (1L << 3)
#define MWM_DECOR_MENU     (1L << 4)
#define MWM_DECOR_MINIMIZE (1L << 5)
#define MWM_DECOR_MAXIMIZE (1L << 6)

/* timestamp of last user input event (for ICCCM focus/protocol timestamps) */
extern Time last_event_time;

/* bar refresh timer */
extern XtIntervalId bar_refresh_timer;

/* compositing state */
extern int compositor_running;
extern int damage_event_base;
extern int damage_error_base;
extern int render_error_base;

/* ── function prototypes ────────────────────────────────────────────── */

/* main.c */
void die(const char *fmt, ...) __attribute__((noreturn));
int xftcolor_load(const char *name, XftColor *xc);
int xftcolor_load_argb(const char *name, XftColor *xc);
void load_colors(void);
void frame_to_client(int fw, int fh, int *cx, int *cy, int *cw, int *ch, int no_decor);
void client_to_frame(int cw, int ch, int *fw, int *fh, int no_decor);
int xerrorstart(Display *d, XErrorEvent *e);
int xerror(Display *d, XErrorEvent *e);
void setup(int *argc, char **argv);
void cleanup(void);
void restart_wm(void);

/* frame.c */
void bevel_rect(XftDraw *xd, int x, int y, int w, int h,
                int tw, int rw, int bw, int lw,
                XftColor *light, XftColor *shadow);
void bevel_rect_inv(XftDraw *xd, int x, int y, int w, int h,
                    int tw, int rw, int bw, int lw,
                    XftColor *light, XftColor *shadow);
void stretcher_corner(XftDraw *xd, int x, int y, int cnum,
                      int swidth, int cwidth, int cheight,
                      XftColor *light, XftColor *shadow);
int frame_button_hit(Client *c, int x, int y);
int frame_edge_hit(Client *c, int x, int y);
void drawframe(Client *c);
void updateframe(Client *c);
void moveresizeframe(Client *c);
void frame_expose_cb(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont);
void frame_enter_cb(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont);
void frame_leave_cb(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont);
void frame_motion_cb(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont);
void frame_btn_cb(Widget w, XtPointer client_data, XEvent *ev, Boolean *cont);

/* bar.c */
BarGeometry calc_bar_geometry(void);
int bar_effective_thickness(void);
int is_horizontal(BarPosition p);
int is_vertical(BarPosition p);
void drawbar(void);
void start_bar_timer(void);
void handle_bar_click(int x, int y);
int iconbar_visible(void);
int icon_entry_h(void);
int minimized_count(void);
void drawiconbar(void);
void updateiconbar(void);
void handle_iconbar_click(int x, int y);
void iconbar_scroll_by(int delta);
void show_icon_tooltip(int x, int y);
void hide_icon_tooltip(void);

/* menu.c */
void show_window_menu(Client *c, int root_x, int root_y);
void show_root_menu(int root_x, int root_y);
int show_confirm_dialog(const char *message);

/* feedback.c */
void fb_show(int x, int y, int w, int h, int style);
void fb_update(int x, int y, int w, int h, int style);
void fb_hide(void);

/* mouse.c */
void draw_outline(int x, int y, int w, int h);
void mousemove(Client *c, int button, int edge, int x_root, int y_root);

/* client.c */
Client *wintoclient(Window w);
Client *nexttiled(Client *c);
int tiledcount(void);
void manage(Window w, XWindowAttributes *wa);
void unmanage(Client *c, int destroyed);
void unmanage_destroyed_cb(Client *c);
void focus(Client *c);
void unfocus(Client *c);
void minimize_client(Client *c);
void restore_client(Client *c);
void float_default_size(Client *c);
void updatewindowname(Client *c);
void grabkeys(void);
int client_supports_protocol(Client *c, Atom protocol);
void set_wm_state(Client *c, int state);
void send_configure_notify(Client *c);
void read_size_hints(Client *c);
void apply_size_hints(Client *c, int *w, int *h);

/* ewmh.c */
void update_client_list(void);
void update_active_window(void);
void update_net_desktops(void);
void update_workarea(void);

/* layout.c */
void arrange(void);
void btree_add(Client *c);
void btree_remove(Client *c);
void btree_cleanup(void);

/* action.c */
void spawn(const WmArg *arg);
void killclient(const WmArg *arg);
void focusstack(const WmArg *arg);
void cyclewindows(const WmArg *arg);
void lowerwindow(const WmArg *arg);
void setlayout(const WmArg *arg);
void cyclelayout(const WmArg *arg);
void togglefloat(const WmArg *arg);
void incmaster(const WmArg *arg);
void zoom(const WmArg *arg);
void togglefullscreen(const WmArg *arg);
void viewworkspace(const WmArg *arg);
void movetoworkspace(const WmArg *arg);
void quit(const WmArg *arg);
void swapbar(const WmArg *arg);
void reloadconfig(const WmArg *arg);

/* event.c */
int handle_buttonpress(XButtonEvent *ev);
void handle_clientmessage(XClientMessageEvent *ev);
void handle_configurenotify(XConfigureEvent *ev);
void handle_configurerequest(XConfigureRequestEvent *ev);
void handle_destroynotify(XDestroyWindowEvent *ev);
void handle_enternotify(XCrossingEvent *ev);
void handle_keypress(XKeyEvent *ev);
void handle_maprequest(XMapRequestEvent *ev);
void handle_unmapnotify(XUnmapEvent *ev);
void run(void);

/* compose.c */
void set_opacity(Window win, unsigned int opacity);
void fade_window_in(Client *c);
void fade_window_out(Client *c, void (*callback)(Client *));
void fade_cancel(Client *c);
void togglecompositing(const WmArg *arg);
void compositor_start(void);
void compositor_stop(void);
void compositor_untrack_window(Window w);
void compositor_configure_window(Window w);
void compositor_manage_client(Client *c);
void compositor_repaint(void);
int compositor_handle_damage(XDamageNotifyEvent *ev);

#endif /* WM_H */