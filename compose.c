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
#include "xmplat_seam.h"

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
    XRenderPictFormat *fmt;  /* cached pict format for visual (None-per-frame lookup avoided) */
    int     depth;      /* cached window depth */
    int     x, y;           /* cached position (root-relative for root children) */
    int     width, height;  /* last known size, to detect stale pictures */
    int     map_state;      /* cached IsUnmapped/IsViewable/IsUnviewable */
    int     has_argb;       /* cached: pict format has alpha channel */
    Window  above;          /* last observed above-sibling (restack detection) */
    unsigned int opacity;   /* cached _NET_WM_WINDOW_OPACITY */
    int     opacity_valid;  /* opacity cache populated */
    Window  client_win;    /* client window inside the frame (None if not a frame) */
    Damage  client_damage; /* damage for client window */
    Window  form_win;      /* frame_form window inside the frame (None if not a frame) */
    Damage  form_damage;   /* damage for frame_form */
} TrkWin;

static TrkWin *trk = NULL;
static int ntrk = 0;
static int trk_cap = 0;

/* forward decls for dirty-region helpers */
static void dirty_mark_full(void);
static void dirty_add_rect(int x, int y, int w, int h);
static void dirty_add_win(const TrkWin *t);

/* cached root-children stacking order — XQueryTree only when structure changed */
static Window *child_stack = NULL;
static unsigned int nchild_stack = 0;
static int children_dirty = 1;

/* dirty-region tracking: paint only the union of damaged rects */
static int dirty_full = 1;      /* next paint covers whole screen */
static int dirty_valid = 0;     /* dirty bbox is populated */
static XRectangle dirty;        /* dirty bbox in root coordinates */

/* cached root background */
static Picture root_bg_picture = None;      /* picture for root_bg_pixmap */
static Pixmap root_bg_picture_src = None;   /* pixmap root_bg_picture was created from */
static int root_bg_solid_valid = 0;
static XRenderColor root_bg_color = { 0x2222, 0x2222, 0x2222, 0xFFFF };

/* last alpha filled into the shared 1x1 mask (skip redundant fills) */
static unsigned short last_mask_alpha = 0;

/* 1x1 alpha pixmap for per-window opacity masking */
static Pixmap alpha_pixmap = None;
static Picture alpha_picture = None;

/* ── opacity property helper ─────────────────────────────────────────── */

void set_opacity(Window win, unsigned int opacity)
{
    _XmPlatChangeProperty(dpy, (unsigned long)win, net_wm_window_opacity, XA_CARDINAL, 32,
                    PropModeReplace, (const unsigned char *)&opacity, 1);
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
    t->fmt = XRenderFindVisualFormat(dpy, wa.visual);
    t->has_argb = t->fmt && t->fmt->type == PictTypeDirect &&
                  t->fmt->direct.alphaMask != 0;
    t->x = wa.x;
    t->y = wa.y;
    t->width = wa.width;
    t->height = wa.height;
    t->map_state = wa.map_state;
    t->above = None;
    t->opacity = 0xFFFFFFFF;
    t->opacity_valid = 0;
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
            /* the window is going away: mark its rect dirty so the next
             * repaint clears it from the root buffer.  Without this, an
             * abruptly-destroyed window (client killed / ctrl+d unmap
             * path) leaves its last composited image stuck on screen. */
            dirty_add_win(&trk[i]);
            trk[i] = trk[--ntrk];
            children_dirty = 1;
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
    free(child_stack);
    child_stack = NULL; nchild_stack = 0;
    children_dirty = 1;
    dirty_full = 1; dirty_valid = 0;
}

/* update cached map state from MapNotify/UnmapNotify in the event loop,
 * avoiding a per-frame XGetWindowAttributes round trip */
static void trk_mark_mapped(Window w, int mapped)
{
    TrkWin *t = trk_find(w);
    if (!t) {
        /* never-seen window (tooltip, override-redirect popup) —
         * track it now so its rect is known and dirtied */
        if (!mapped) return;
        t = trk_add(w);
        if (!t) return;
    }
    t->map_state = mapped ? IsViewable : IsUnmapped;
    dirty_add_win(t);
    children_dirty = 1;  /* map order may have changed stacking */
}

/* ── dirty-region helpers ───────────────────────────────────────────── */

static void dirty_mark_full(void)
{
    dirty_full = 1;
}

static void dirty_add_rect(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (dirty_full) return;  /* already everything */
    XRectangle r = { (short)x, (short)y, (unsigned short)w, (unsigned short)h };
    if (!dirty_valid) {
        dirty = r;
        dirty_valid = 1;
        return;
    }
    /* bounding-box union */
    int x2 = MAX(dirty.x + (int)dirty.width,  x + w);
    int y2 = MAX(dirty.y + (int)dirty.height, y + h);
    dirty.x      = (short)MIN(dirty.x, x);
    dirty.y      = (short)MIN(dirty.y, y);
    dirty.width  = (unsigned short)(x2 - dirty.x);
    dirty.height = (unsigned short)(y2 - dirty.y);
}

static void dirty_add_win(const TrkWin *t)
{
    /* include a small margin so translucent edges/older content is covered */
    dirty_add_rect(t->x - 1, t->y - 1, t->width + 2, t->height + 2);
}

/* mark a client's frame rect dirty (fade steps etc.) */
static void dirty_client(Client *c)
{
    if (!compositor_running) return;
    TrkWin *t = trk_find(XtWindow(c->frame_shell));
    if (t) dirty_add_win(t);
}

/* ── untrack a window (call from DestroyNotify) ─────────────────────── */

void compositor_untrack_window(Window w)
{
    trk_remove(w);
    /* also check if w is a client/form window tracked inside a frame's TrkWin */
    for (int i = 0; i < ntrk; i++) {
        int frame_changed = 0;
        if (trk[i].client_win == w) {
            if (trk[i].client_damage)
                XDamageDestroy(dpy, trk[i].client_damage);
            trk[i].client_win = None;
            trk[i].client_damage = None;
            frame_changed = 1;
        }
        if (trk[i].form_win == w) {
            if (trk[i].form_damage)
                XDamageDestroy(dpy, trk[i].form_damage);
            trk[i].form_win = None;
            trk[i].form_damage = None;
            frame_changed = 1;
        }
        /* the frame's content changed underneath it (grandchild destroyed)
         * — dirty the frame rect so the next repaint picks it up */
        if (frame_changed)
            dirty_add_win(&trk[i]);
    }
}

/* ── invalidate cached picture on resize (call from ConfigureNotify) ── */

void compositor_configure_window(Window w)
{
    TrkWin *t = trk_find(w);
    /* untracked windows are grandchildren (client/form) — their parent
     * frame is tracked and its damage covers content changes */
    if (!t) return;
    /* remember old rect so the vacated region gets repainted */
    int ox = t->x, oy = t->y, ow = t->width, oh = t->height;
    /* read the new geometry once and refresh all caches — avoids the
     * per-frame XGetWindowAttributes round trip entirely */
    XWindowAttributes wa;
    if (XGetWindowAttributes(dpy, w, &wa)) {
        if (t->picture && (wa.width != t->width || wa.height != t->height))
            trk_invalidate(t);
        t->x = wa.x;
        t->y = wa.y;
        t->width = wa.width;
        t->height = wa.height;
        t->map_state = wa.map_state;
    }
    /* ConfigureNotify also fires on restack (XRaiseWindow) — refresh order */
    children_dirty = 1;
    /* old + new rects: covers move, resize, and restack overlaps */
    dirty_add_rect(ox - 1, oy - 1, ow + 2, oh + 2);
    dirty_add_win(t);
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
    dirty_add_win(t);
}

/* ── map/unmap notification from the event loop ────────────────────── */

void compositor_map_window(Window w)   { if (compositor_running) trk_mark_mapped(w, 1); }
void compositor_unmap_window(Window w) { if (compositor_running) trk_mark_mapped(w, 0); }

/* ── get opacity for a root child window ────────────────────────────── */

static unsigned int trk_opacity(TrkWin *t)
{
    /* client frames keep opacity in the Client struct */
    for (Client *c = clients; c; c = c->next) {
        if (XtWindow(c->frame_shell) == t->win)
            return c->opacity;
    }
    if (!t->opacity_valid) {
        /* read _NET_WM_WINDOW_OPACITY property once, cache until changed */
        Atom actual;
        int fmt;
        unsigned long n, after;
        unsigned char *data = NULL;
        t->opacity = 0xFFFFFFFF;
        if (_XmPlatGetWindowProperty(dpy, (unsigned long)t->win, net_wm_window_opacity, 0, 1, False,
                               XA_CARDINAL, (unsigned long *)&actual, &fmt,
                               &n, &after, &data) == Success && data && n > 0) {
            t->opacity = *(unsigned int *)data;
            XFree(data);
        }
        t->opacity_valid = 1;
    }
    return t->opacity;
}

/* called from PropertyNotify for _NET_WM_WINDOW_OPACITY */
void compositor_opacity_changed(Window w)
{
    if (!compositor_running) return;
    TrkWin *t = trk_find(w);
    if (!t) return;
    t->opacity_valid = 0;
    dirty_add_win(t);
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

/* repaint the damaged region (or the full screen when dirty_full).
 * Root children stacking order and per-window attributes are cached;
 * XQueryTree/XGetWindowAttributes only run on structural changes. */
static void compositor_paint_all(void)
{
    if (!root_fmt || !root_picture) return;
    ensure_alpha_picture();

    /* nothing dirty — nothing to do */
    if (!dirty_full && !dirty_valid) return;

    /* determine the repaint region */
    int rx, ry, rw, rh;
    if (dirty_full) {
        rx = 0; ry = 0; rw = sw; rh = sh;
    } else {
        rx = dirty.x; ry = dirty.y;
        rw = dirty.width; rh = dirty.height;
        /* clamp to screen */
        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }
        if (rx + rw > sw) rw = sw - rx;
        if (ry + rh > sh) rh = sh - ry;
        if (rw <= 0 || rh <= 0) { dirty_full = 0; dirty_valid = 0; return; }
    }
    int dirty_only = !dirty_full;

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
        /* new buffer is blank — must repaint everything */
        dirty_full = 1;
        dirty_valid = 0;
        dirty_only = 0;
        rx = 0; ry = 0; rw = sw; rh = sh;
    }

    /* paint root background into the dirty region */
    if (root_bg_pixmap != None) {
        /* cache the background picture; invalidate when the pixmap changes */
        if (root_bg_picture == None || root_bg_picture_src != root_bg_pixmap) {
            if (root_bg_picture) XRenderFreePicture(dpy, root_bg_picture);
            root_bg_picture = XRenderCreatePicture(dpy, root_bg_pixmap, root_fmt, 0, NULL);
            root_bg_picture_src = root_bg_pixmap;
        }
        /* sample the bg picture at the region's own coordinates — the
         * bg is screen-aligned; sampling at (0,0) would paste a shifted
         * piece of the image into the buffer (the "moving background" bug) */
        XRenderComposite(dpy, PictOpSrc, root_bg_picture, None, root_buffer,
                         rx, ry, 0, 0, rx, ry, (unsigned)rw, (unsigned)rh);
    } else {
        /* cache the parsed solid color (no XAllocNamedColor per frame) */
        if (!root_bg_solid_valid) {
            XColor exact, screen_c;
            if (XAllocNamedColor(dpy, xcolormap, color_root_bg, &exact, &screen_c)) {
                root_bg_color = (XRenderColor){ screen_c.red, screen_c.green,
                                                screen_c.blue, 0xFFFF };
                XFreeColors(dpy, xcolormap, &screen_c.pixel, 1, 0);
            } else {
                root_bg_color = (XRenderColor){ 0x2222, 0x2222, 0x2222, 0xFFFF };
            }
            root_bg_solid_valid = 1;
        }
        XRenderFillRectangle(dpy, PictOpSrc, root_buffer, &root_bg_color,
                             (unsigned short)rx, (unsigned short)ry,
                             (unsigned short)rw, (unsigned short)rh);
    }

    /* refresh cached children stacking order only when structure changed */
    if (children_dirty) {
        Window dum_root, dum_parent;
        Window *children = NULL;
        unsigned int nchildren = 0;
        if (XQueryTree(dpy, root, &dum_root, &dum_parent, &children, &nchildren)) {
            free(child_stack);
            child_stack = children;
            nchild_stack = nchildren;
        } else if (children) {
            XFree(children);
        }
        children_dirty = 0;
    }

    /* composite only windows intersecting the dirty region */
    for (unsigned int i = 0; i < nchild_stack; i++) {
        Window w = child_stack[i];
        if (!w) continue;
        if (w == XtWindow(toplevel_shell)) continue;

        TrkWin *t = trk_find(w);
        if (!t) { t = trk_add(w); children_dirty = 1; }
        if (!t) continue;

        if (t->map_state != IsViewable) continue;
        if (t->width == 0 || t->height == 0) continue;

        /* clip the composite to the dirty region: source offset is in
         * window-local coords, dst is the intersection in root coords.
         * A small damage on a large window then costs O(damage), not
         * O(window). */
        int src_x = 0, src_y = 0;
        int dst_x = t->x, dst_y = t->y;
        unsigned dst_w = (unsigned)t->width, dst_h = (unsigned)t->height;
        if (dirty_only) {
            int x1 = MAX(t->x, rx);
            int y1 = MAX(t->y, ry);
            int x2 = MIN(t->x + t->width, rx + rw);
            int y2 = MIN(t->y + t->height, ry + rh);
            if (x1 >= x2 || y1 >= y2)
                continue;  /* entirely outside the dirty region */
            src_x = x1 - t->x;
            src_y = y1 - t->y;
            dst_x = x1;
            dst_y = y1;
            dst_w = (unsigned)(x2 - x1);
            dst_h = (unsigned)(y2 - y1);
        }

        unsigned int op = trk_opacity(t);
        unsigned short alpha = (unsigned short)(op >> 16);
        if (alpha == 0) continue;

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

        /* cached pict format */
        XRenderPictFormat *fmt = t->fmt;
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
        if (t->has_argb || alpha != 0xFFFF) {
            /* refill the shared 1x1 alpha mask only when alpha changed */
            if (alpha != last_mask_alpha) {
                XRenderColor ac = { 0, 0, 0, alpha };
                XRenderFillRectangle(dpy, PictOpSrc, alpha_picture, &ac, 0, 0, 1, 1);
                last_mask_alpha = alpha;
            }
            XRenderComposite(dpy, PictOpOver, t->picture, alpha_picture,
                             root_buffer,
                             src_x, src_y, 0, 0, dst_x, dst_y, dst_w, dst_h);
        } else {
            XRenderComposite(dpy, PictOpSrc, t->picture, None, root_buffer,
                             src_x, src_y, 0, 0, dst_x, dst_y, dst_w, dst_h);
        }
    }

    /* blit only the dirty region to root */
    XRenderComposite(dpy, PictOpSrc, root_buffer, None, root_picture,
                     rx, ry, 0, 0, rx, ry, (unsigned)rw, (unsigned)rh);
    if (dirty_full) {
        /* when the X server handles an exposure event on root (e.g. after a
         * window is unmapped), it paints the buffer as background, which
         * matches our composited output and prevents flashing (xcompmgr trick) */
        XSetWindowBackgroundPixmap(dpy, root, root_buffer_pixmap);
    }
    dirty_full = 0;
    dirty_valid = 0;

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
    dirty_mark_full();
    compositor_paint_all();

    compositor_running = 1;
}

/* compositor-only repaint batching: damage events schedule a repaint timer
 * that coalesces bursts — WITHOUT running the full defer_flush pass
 * (arrange/drawbar/sysinfo reads), which would make every cursor blink
 * and window animation pay for WM housekeeping */
static XtIntervalId repaint_timer = 0;

static void repaint_timer_cb(XtPointer data, XtIntervalId *id)
{
    (void)data; (void)id;
    repaint_timer = 0;
    compositor_paint_all();
}

void compositor_schedule_repaint(void)
{
    if (repaint_timer) return;
    repaint_timer = XtAppAddTimeOut(app, 0, repaint_timer_cb, NULL);
}

void compositor_stop(void)
{
    if (!compositor_running) return;
    if (repaint_timer) {
        XtRemoveTimeOut(repaint_timer);
        repaint_timer = 0;
    }

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
    if (root_bg_picture)   { XRenderFreePicture(dpy, root_bg_picture);   root_bg_picture = None; }
    root_bg_picture_src = None;
    root_bg_solid_valid = 0;
    last_mask_alpha = 0;

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

/* force a full-screen repaint on the next paint (public, for paths that
 * add new windows and cannot wait for damage events — menu/dialog open) */
void compositor_repaint_full(void)
{
    dirty_mark_full();
    if (compositor_running) compositor_paint_all();
}

/* mark a window's rect dirty (public, for nested event loops that redraw
 * a window without generating damage events we can process) */
void compositor_dirty_window(Window w)
{
    if (!compositor_running) return;
    TrkWin *t = trk_find(w);
    if (t) dirty_add_win(t);
}

/* called from bg_load() — background pixmap/color changed */
void compositor_bg_reloaded(void)
{
    root_bg_solid_valid = 0;
    if (compositor_running) {
        dirty_mark_full();
        compositor_schedule_repaint();
    }
}

/* ── damage event handler ───────────────────────────────────────────── */

int compositor_handle_damage(XDamageNotifyEvent *ev)
{
    if (!compositor_running) return 0;
    Damage d = ev->damage;
    /* find the tracked window owning this damage handle and subtract
     * only it — O(N) scan instead of subtracting every window's damage */
    for (int i = 0; i < ntrk; i++) {
        if (trk[i].damage == d || trk[i].client_damage == d ||
            trk[i].form_damage == d) {
            XDamageSubtract(dpy, d, None, None);
            /* the frame picture uses IncludeInferiors, so client/form
             * damage shows within the frame rect — mark just that rect */
            dirty_add_win(&trk[i]);
            break;
        }
    }
    compositor_schedule_repaint();
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
            dirty_client(c);
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
            dirty_client(c);
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
    dirty_client(c);
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