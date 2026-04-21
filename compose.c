/*
 * rondo — compositing: fade-in/fade-out + built-in compositor
 *
 * Fade animation uses _NET_WM_WINDOW_OPACITY + XtAppAddTimeOut timers.
 * Built-in compositor uses Xcomposite+Xdamage+Xfixes+Xrender as fallback
 * when no external compositor (picom, compton) is running.
 *
 * Architecture follows xcompmgr: redirect subwindows manually,
 * track damage per-window, and repaint all windows via Xrender when
 * any damage occurs or opacity changes.
 */
#include "wm.h"

/* ── compositing globals ────────────────────────────────────────────── */

int compositor_running = 0;
int damage_event_base = 0;

/* compositor-internal state */
static int composite_event_base = 0;
static int composite_error_base = 0;
int damage_error_base = 0;
int render_error_base = 0;
static XRenderPictFormat *root_fmt = NULL;  /* format for root visual */
static Picture root_picture = None;         /* picture for root window */
static Picture root_buffer = None;          /* offscreen buffer picture */
static Pixmap root_buffer_pixmap = None;
static int root_buffer_w = 0, root_buffer_h = 0;

/* per-window tracked state */
typedef struct {
    Window  win;
    Damage  damage;
    Picture picture;    /* created lazily in paint from window with IncludeInferiors */
    Visual  *visual;    /* cached window visual */
    int     depth;      /* cached window depth */
    int     width, height;  /* last known size, to detect stale pictures */
    Window  client_win;    /* client window inside the frame (None if not a frame) */
    Damage  client_damage; /* damage for client window */
    Window  form_win;      /* frame_form window inside the frame (None if not a frame) */
    Damage  form_damage;   /* damage for frame_form */
} TrkWin;

static TrkWin *trk = NULL;
static int ntrk = 0;
static int trk_cap = 0;

/* 1x1 alpha pixmap for per-window opacity masking */
static Pixmap alpha_pixmap = None;
static Picture alpha_picture = None;

/* ── opacity property helper ─────────────────────────────────────────── */

void set_opacity(Window win, unsigned int opacity)
{
    XChangeProperty(dpy, win, net_wm_window_opacity, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&opacity, 1);
}

/* ── tracked window management ──────────────────────────────────────── */

static TrkWin *trk_find(Window w)
{
    for (int i = 0; i < ntrk; i++)
        if (trk[i].win == w) return &trk[i];
    return NULL;
}

static TrkWin *trk_add(Window w)
{
    if (trk_find(w)) return trk_find(w);
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa)) return NULL;
    if (wa.class == InputOnly) return NULL;

    if (ntrk >= trk_cap) {
        trk_cap = trk_cap ? trk_cap * 2 : 32;
        trk = realloc(trk, sizeof(TrkWin) * (size_t)trk_cap);
    }
    TrkWin *t = &trk[ntrk++];
    t->win = w;
    t->visual = wa.visual;
    t->depth = wa.depth;
    t->width = wa.width;
    t->height = wa.height;
    t->damage = XDamageCreate(dpy, w, XDamageReportNonEmpty);
    t->picture = None;
    t->client_win = None;
    t->client_damage = None;
    t->form_win = None;
    t->form_damage = None;
    /* if this is a client frame, also track damage on client + form windows */
    for (Client *c = clients; c; c = c->next) {
        if (XtWindow(c->frame_shell) == w) {
            t->client_win = c->win;
            t->client_damage = XDamageCreate(dpy, c->win, XDamageReportNonEmpty);
            t->form_win = XtWindow(c->frame_form);
            t->form_damage = XDamageCreate(dpy, XtWindow(c->frame_form), XDamageReportNonEmpty);
            break;
        }
    }
    return t;
}

/* Invalidate cached Picture for a tracked window.
 * Called when the window is resized — the old Picture may
 * reference stale content or dimensions. */
static void trk_invalidate(TrkWin *t)
{
    if (t->picture) { XRenderFreePicture(dpy, t->picture); t->picture = None; }
}

static void trk_remove(Window w)
{
    for (int i = 0; i < ntrk; i++) {
        if (trk[i].win == w) {
            if (trk[i].damage)        XDamageDestroy(dpy, trk[i].damage);
            if (trk[i].client_damage) XDamageDestroy(dpy, trk[i].client_damage);
            if (trk[i].form_damage)   XDamageDestroy(dpy, trk[i].form_damage);
            if (trk[i].picture)       XRenderFreePicture(dpy, trk[i].picture);
            trk[i] = trk[--ntrk];
            return;
        }
    }
}

static void trk_clear(void)
{
    for (int i = 0; i < ntrk; i++) {
        if (trk[i].damage)        XDamageDestroy(dpy, trk[i].damage);
        if (trk[i].client_damage) XDamageDestroy(dpy, trk[i].client_damage);
        if (trk[i].form_damage)   XDamageDestroy(dpy, trk[i].form_damage);
        if (trk[i].picture)       XRenderFreePicture(dpy, trk[i].picture);
    }
    free(trk);
    trk = NULL; ntrk = 0; trk_cap = 0;
}

/* ── untrack a window (call from DestroyNotify) ─────────────────────── */

void compositor_untrack_window(Window w)
{
    trk_remove(w);
    /* also check if w is a client/form window tracked inside a frame's TrkWin */
    for (int i = 0; i < ntrk; i++) {
        if (trk[i].client_win == w) {
            if (trk[i].client_damage)
                XDamageDestroy(dpy, trk[i].client_damage);
            trk[i].client_win = None;
            trk[i].client_damage = None;
        }
        if (trk[i].form_win == w) {
            if (trk[i].form_damage)
                XDamageDestroy(dpy, trk[i].form_damage);
            trk[i].form_win = None;
            trk[i].form_damage = None;
        }
    }
}

/* ── invalidate cached picture on resize (call from ConfigureNotify) ── */

void compositor_configure_window(Window w)
{
    TrkWin *t = trk_find(w);
    if (t) trk_invalidate(t);
}

/* ── track client window for damage (call from manage) ─────────────── */

void compositor_manage_client(Client *c)
{
    if (!compositor_running) return;
    Window fw = XtWindow(c->frame_shell);
    TrkWin *t = trk_find(fw);
    if (!t) t = trk_add(fw);
    if (!t) return;
    if (c->win != None && t->client_win == None) {
        t->client_win = c->win;
        t->client_damage = XDamageCreate(dpy, c->win, XDamageReportNonEmpty);
    }
    if (t->form_win == None) {
        Window form_w = XtWindow(c->frame_form);
        t->form_win = form_w;
        t->form_damage = XDamageCreate(dpy, form_w, XDamageReportNonEmpty);
    }
}

/* ── get opacity for a root child window ────────────────────────────── */

static unsigned int window_opacity(Window w)
{
    /* if it's a client frame, use tracked Client opacity */
    for (Client *c = clients; c; c = c->next) {
        if (XtWindow(c->frame_shell) == w)
            return c->opacity;
    }
    /* read _NET_WM_WINDOW_OPACITY property */
    Atom actual;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    unsigned int op = 0xFFFFFFFF;
    if (XGetWindowProperty(dpy, w, net_wm_window_opacity, 0, 1, False,
                           XA_CARDINAL, &actual, &fmt,
                           &n, &after, &data) == Success && data && n > 0) {
        op = *(unsigned int *)data;
        XFree(data);
    }
    return op;
}

/* ── ensure alpha mask resources exist ───────────────────────────────── */

static void ensure_alpha_picture(void)
{
    if (alpha_picture) return;
    XRenderPictFormat *a8 = XRenderFindStandardFormat(dpy, PictStandardA8);
    if (!a8) return;
    alpha_pixmap = XCreatePixmap(dpy, root, 1, 1, 8);
    XRenderPictureAttributes pa;
    pa.repeat = True;
    alpha_picture = XRenderCreatePicture(dpy, alpha_pixmap, a8, CPRepeat, &pa);
}

/* ── compositor paint ────────────────────────────────────────────────── */

static void compositor_paint_all(void)
{
    if (!root_fmt || !root_picture) return;
    ensure_alpha_picture();

    /* ensure offscreen buffer is the right size */
    if (!root_buffer_pixmap || root_buffer_w != sw || root_buffer_h != sh) {
        /* detach from root background before freeing the old pixmap */
        XSetWindowBackgroundPixmap(dpy, root, None);
        if (root_buffer) XRenderFreePicture(dpy, root_buffer);
        if (root_buffer_pixmap) XFreePixmap(dpy, root_buffer_pixmap);
        root_buffer_pixmap = XCreatePixmap(dpy, root, (unsigned)sw, (unsigned)sh,
                                           DefaultDepth(dpy, screen));
        root_buffer = XRenderCreatePicture(dpy, root_buffer_pixmap, root_fmt,
                                           0, NULL);
        root_buffer_w = sw;
        root_buffer_h = sh;
    }

    /* paint root background into buffer */
    if (root_bg_pixmap != None) {
        Picture bg = XRenderCreatePicture(dpy, root_bg_pixmap, root_fmt, 0, NULL);
        XRenderComposite(dpy, PictOpSrc, bg, None, root_buffer,
                         0, 0, 0, 0, 0, 0, (unsigned)sw, (unsigned)sh);
        XRenderFreePicture(dpy, bg);
    } else {
        XRenderColor bg;
        XColor exact, screen_c;
        if (XAllocNamedColor(dpy, xcolormap, color_root_bg, &exact, &screen_c)) {
            bg = (XRenderColor){ screen_c.red, screen_c.green, screen_c.blue, 0xFFFF };
            XFreeColors(dpy, xcolormap, &screen_c.pixel, 1, 0);
        } else {
            bg = (XRenderColor){ 0x2222, 0x2222, 0x2222, 0xFFFF };
        }
        XRenderFillRectangle(dpy, PictOpSrc, root_buffer, &bg,
                             0, 0, (unsigned short)sw, (unsigned short)sh);
    }

    /* composite ALL root children in stacking order */
    Window dum_root, dum_parent;
    Window *children = NULL;
    unsigned int nchildren = 0;
    if (!XQueryTree(dpy, root, &dum_root, &dum_parent, &children, &nchildren))
        goto blit;

    for (unsigned int i = 0; i < nchildren; i++) {
        Window w = children[i];
        if (!w) continue;
        if (w == XtWindow(toplevel_shell)) continue;

        XWindowAttributes wa;
        if (!XGetWindowAttributes(dpy, w, &wa)) continue;
        if (wa.class == InputOnly) continue;
        if (wa.map_state != IsViewable) continue;
        if (wa.width == 0 || wa.height == 0) continue;

        unsigned int op = window_opacity(w);
        unsigned short alpha = (unsigned short)(op >> 16);
        if (alpha == 0) continue;

        /* ensure tracked (lazy add for new windows) */
        TrkWin *t = trk_find(w);
        if (!t) t = trk_add(w);
        if (!t) continue;

        /* lazily add client/form window damage if we missed it in trk_add
         * (e.g. client was added to the list after the frame was first tracked) */
        if (t->client_win == None) {
            for (Client *c = clients; c; c = c->next) {
                if (XtWindow(c->frame_shell) == w) {
                    t->client_win = c->win;
                    t->client_damage = XDamageCreate(dpy, c->win, XDamageReportNonEmpty);
                    t->form_win = XtWindow(c->frame_form);
                    t->form_damage = XDamageCreate(dpy, XtWindow(c->frame_form), XDamageReportNonEmpty);
                    break;
                }
            }
        }

        /* invalidate cached picture on resize */
        if (t->picture && (wa.width != t->width || wa.height != t->height))
            trk_invalidate(t);
        t->width = wa.width;
        t->height = wa.height;

        /* lazily create Picture from window with IncludeInferiors.
         * This composites the window and all its children (frame +
         * client content) in a single source picture — same approach
         * as xcompmgr. */
        XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, t->visual);
        if (!fmt) continue;

        if (!t->picture) {
            XRenderPictureAttributes pa;
            pa.subwindow_mode = IncludeInferiors;
            t->picture = XRenderCreatePicture(dpy, w, fmt,
                                              CPSubwindowMode, &pa);
            if (!t->picture) continue;
        }

        /* ARGB windows have per-pixel alpha and must always use PictOpOver.
         * Non-ARGB windows at full opacity can use the faster PictOpSrc. */
        int has_argb = (fmt->type == PictTypeDirect && fmt->direct.alphaMask != 0);
        if (has_argb || alpha != 0xFFFF) {
            XRenderColor ac = { 0, 0, 0, alpha };
            XRenderFillRectangle(dpy, PictOpSrc, alpha_picture, &ac, 0, 0, 1, 1);
            XRenderComposite(dpy, PictOpOver, t->picture, alpha_picture,
                             root_buffer,
                             0, 0, 0, 0, wa.x, wa.y,
                             (unsigned)wa.width, (unsigned)wa.height);
        } else {
            XRenderComposite(dpy, PictOpSrc, t->picture, None, root_buffer,
                             0, 0, 0, 0, wa.x, wa.y,
                             (unsigned)wa.width, (unsigned)wa.height);
        }
    }
    if (children) XFree(children);

blit:
    /* blit buffer to root */
    XRenderComposite(dpy, PictOpSrc, root_buffer, None, root_picture,
                     0, 0, 0, 0, 0, 0, (unsigned)sw, (unsigned)sh);
    /* set root background to our composited buffer pixmap — when the
     * X server handles an exposure event on root (e.g. after a window
     * is unmapped), it paints the buffer as background, which matches
     * our composited output and prevents flashing.  (xcompmgr trick) */
    XSetWindowBackgroundPixmap(dpy, root, root_buffer_pixmap);
    /* flush so each frame is visible immediately (important for fade animation) */
    XFlush(dpy);
}

/* ── compositor start/stop ──────────────────────────────────────────── */

void compositor_start(void)
{
    if (compositor_running) return;

    /* check if an external compositor already owns the selection */
    Window owner = XGetSelectionOwner(dpy, net_wm_cm_s0);
    if (owner != None) return;

    /* check extension availability */
    if (!XCompositeQueryExtension(dpy, &composite_event_base, &composite_error_base))
        return;
    if (!XDamageQueryExtension(dpy, &damage_event_base, &damage_error_base))
        return;
    {
        int rend_ev, rend_err;
        if (!XRenderQueryExtension(dpy, &rend_ev, &rend_err))
            return;
        render_error_base = rend_err;
    }

    /* claim the compositor selection */
    static Window cm_win = None;
    if (cm_win == None) {
        XSetWindowAttributes attr = { .override_redirect = True };
        cm_win = XCreateWindow(dpy, root, -1, -1, 1, 1, 0,
                               CopyFromParent, InputOnly,
                               CopyFromParent, CWOverrideRedirect, &attr);
    }
    XSetSelectionOwner(dpy, net_wm_cm_s0, cm_win, CurrentTime);

    /* create root picture — this is how we paint to the root window */
    XRenderPictureAttributes pa;
    pa.subwindow_mode = IncludeInferiors;
    root_fmt = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen));
    root_picture = XRenderCreatePicture(dpy, root, root_fmt,
                                        CPSubwindowMode, &pa);

    /* redirect ALL subwindows — X server stops rendering them directly */
    XCompositeRedirectSubwindows(dpy, root, CompositeRedirectManual);

    /* track all currently mapped root children */
    XGrabServer(dpy);
    {
        Window dum_root, dum_parent;
        Window *children = NULL;
        unsigned int nchildren = 0;
        if (XQueryTree(dpy, root, &dum_root, &dum_parent, &children, &nchildren)) {
            for (unsigned int i = 0; i < nchildren; i++)
                trk_add(children[i]);
            if (children) XFree(children);
        }
    }
    XUngrabServer(dpy);

    /* initial paint */
    compositor_paint_all();

    compositor_running = 1;
}

void compositor_stop(void)
{
    if (!compositor_running) return;

    /* unredirect — X server resumes direct rendering */
    XCompositeRedirectSubwindows(dpy, root, CompositeRedirectAutomatic);
    XCompositeUnredirectSubwindows(dpy, root, CompositeRedirectManual);

    /* release compositor selection so compositor_start() can reclaim it */
    XSetSelectionOwner(dpy, net_wm_cm_s0, None, CurrentTime);

    /* restore root background — re-apply the configured background
     * so the X server can paint it again now that we're not compositing */
    bg_load();

    trk_clear();

    if (root_picture)      { XRenderFreePicture(dpy, root_picture);      root_picture = None; }
    if (root_buffer)       { XRenderFreePicture(dpy, root_buffer);       root_buffer = None; }
    if (root_buffer_pixmap){ XFreePixmap(dpy, root_buffer_pixmap);        root_buffer_pixmap = None; }
    if (alpha_picture)     { XRenderFreePicture(dpy, alpha_picture);     alpha_picture = None; }
    if (alpha_pixmap)      { XFreePixmap(dpy, alpha_pixmap);             alpha_pixmap = None; }

    root_fmt = NULL;

    for (Client *c = clients; c; c = c->next)
        set_opacity(XtWindow(c->frame_shell), 0xFFFFFFFF);

    compositor_running = 0;
}

/* ── repaint trigger ────────────────────────────────────────────────── */

void compositor_repaint(void)
{
    if (compositor_running) compositor_paint_all();
}

/* ── damage event handler ───────────────────────────────────────────── */

int compositor_handle_damage(XDamageNotifyEvent *ev)
{
    if (!compositor_running) return 0;
    XDamageSubtract(dpy, ev->damage, None, None);
    /* also subtract damage on related tracked windows (form, client) */
    for (int i = 0; i < ntrk; i++) {
        if (trk[i].client_damage)
            XDamageSubtract(dpy, trk[i].client_damage, None, None);
        if (trk[i].form_damage)
            XDamageSubtract(dpy, trk[i].form_damage, None, None);
    }
    compositor_paint_all();
    return 1;
}

/* ── fade animation ─────────────────────────────────────────────────── */

#define FADE_STEP_MS 16

static void fade_step(XtPointer client_data, XtIntervalId *id);

void fade_window_in(Client *c)
{
    if (!fade_enabled) {
        set_opacity(XtWindow(c->frame_shell), 0xFFFFFFFF);
        c->fading = 0;
        c->opacity = 0xFFFFFFFF;
        return;
    }
    if (c->fade_timer) {
        XtRemoveTimeOut(c->fade_timer);
        c->fade_timer = (XtIntervalId)0;
    }
    c->fading = 1;
    c->opacity = 0;
    c->fade_done_cb = NULL;
    set_opacity(XtWindow(c->frame_shell), 0);
    c->fade_timer = XtAppAddTimeOut(app, FADE_STEP_MS, fade_step, (XtPointer)c);
}

void fade_window_out(Client *c, void (*callback)(Client *))
{
    if (!fade_enabled) {
        if (callback) callback(c);
        return;
    }
    if (c->fade_timer) {
        XtRemoveTimeOut(c->fade_timer);
        c->fade_timer = (XtIntervalId)0;
    }
    c->fading = -1;
    c->opacity = 0xFFFFFFFF;
    c->fade_done_cb = callback;
    c->fade_timer = XtAppAddTimeOut(app, FADE_STEP_MS, fade_step, (XtPointer)c);
}

void fade_cancel(Client *c)
{
    if (c->fade_timer) {
        XtRemoveTimeOut(c->fade_timer);
        c->fade_timer = (XtIntervalId)0;
    }
    c->fading = 0;
    c->opacity = 0xFFFFFFFF;
    set_opacity(XtWindow(c->frame_shell), 0xFFFFFFFF);
    c->fade_done_cb = NULL;
}

static void fade_step(XtPointer client_data, XtIntervalId *id)
{
    Client *c = (Client *)client_data;
    (void)id;
    c->fade_timer = (XtIntervalId)0;

    unsigned int step;
    if (c->fading == 1) {
        step = (unsigned int)((double)0xFFFFFFFF /
               ((double)fade_in_ms / FADE_STEP_MS));
        if (step == 0) step = 1;
        if (0xFFFFFFFF - c->opacity <= step) {
            c->opacity = 0xFFFFFFFF;
            c->fading = 0;
            set_opacity(XtWindow(c->frame_shell), 0xFFFFFFFF);
            compositor_repaint();
            return;
        }
        c->opacity += step;
    } else if (c->fading == -1) {
        step = (unsigned int)((double)0xFFFFFFFF /
               ((double)fade_out_ms / FADE_STEP_MS));
        if (step == 0) step = 1;
        if (c->opacity <= step) {
            c->opacity = 0;
            c->fading = 0;
            set_opacity(XtWindow(c->frame_shell), 0);
            compositor_repaint();
            void (*cb)(Client *) = c->fade_done_cb;
            c->fade_done_cb = NULL;
            if (cb) cb(c);
            return;
        }
        c->opacity -= step;
    } else {
        return;
    }

    set_opacity(XtWindow(c->frame_shell), c->opacity);
    compositor_repaint();
    c->fade_timer = XtAppAddTimeOut(app, FADE_STEP_MS, fade_step, (XtPointer)c);
}

/* ── toggle compositing action ───────────────────────────────────────── */

void togglecompositing(const WmArg *arg)
{
    (void)arg;
    fade_enabled = !fade_enabled;
    if (!fade_enabled) {
        for (Client *c = clients; c; c = c->next)
            if (c->fading) fade_cancel(c);
        compositor_stop();
    } else {
        compositor_start();
    }
    drawbar();
}