/*
 * rondo — icon acquisition for the icon bar
 *
 * Priority order:
 *   1. _NET_WM_ICON   (EWMH ARGB icons — set by most modern apps)
 *   2. WM_HINTS icon  (legacy X apps; handled in client.c)
 *   3. WM_CLASS → .desktop file → Icon= → icon-theme lookup
 *   4. built-in default "X program" icon
 *
 * Results are rendered to a 32-bit ARGB pixmap (alpha flattened onto the
 * entry background) stored in c->icon_pixmap so draw_icon_scaled() needs
 * no knowledge of where the icon came from.
 */
#include "wm.h"
#include "xmplat_seam.h"
#include <Imlib2.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ctype.h>

#define ICON_MAX_DIM 128   /* largest _NET_WM_ICON variant we consider */
#define ICON_PICK_DIM 64   /* preferred _NET_WM_ICON size */

/* release pixels we own (call before overwriting and from unmanage) */
void icon_pixels_free(Client *c) {
    if (c->icon_ours && c->icon_pixmap)
        XFreePixmap(dpy, c->icon_pixmap);
    free(c->icon_argb);
    c->icon_pixmap = None;
    c->icon_argb = NULL;
    c->icon_ours = 0;
}

/* ── take an Imlib image's ARGB pixels as the client icon ────────────── */

static void icon_set_from_imlib(Client *c, Imlib_Image img) {
    if (!img) return;
    imlib_context_set_image(img);
    int w = imlib_image_get_width();
    int h = imlib_image_get_height();
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
        imlib_free_image_and_decache();
        return;
    }

    /* copy the raw ARGB pixels out; draw_icon_scaled renders them via
     * imlib_create_image_using_data (no X drawable round-trip, real alpha) */
    unsigned int *src = (unsigned int *)imlib_image_get_data_for_reading_only();
    unsigned int *copy = malloc((size_t)w * h * sizeof(unsigned int));
    if (!copy) { imlib_free_image_and_decache(); return; }
    memcpy(copy, src, (size_t)w * h * sizeof(unsigned int));
    imlib_free_image_and_decache();

    icon_pixels_free(c);
    c->icon_argb = copy;
    c->icon_w = w;
    c->icon_h = h;
    c->icon_pixmap = None;
    c->icon_mask = None;
    c->icon_ours = 1;
}

/* ── 1. _NET_WM_ICON ─────────────────────────────────────────────────── */

void icon_load_netwm(Client *c) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;
    if (_XmPlatGetWindowProperty(dpy, (unsigned long)c->win, net_wm_icon,
                                 0, 1 << 20, False, XA_CARDINAL,
                                 (unsigned long *)&actual_type, &actual_format,
                                 &nitems, &bytes_after, &data) != Success ||
        !data || actual_format != 32 || nitems < 2) {
        if (data) XFree(data);
        return;
    }

    /* Walk [w, h, pixels...] entries; keep the largest, preferring sizes
     * close to ICON_PICK_DIM.  format==32 property data are longs
     * (8 bytes per entry on LP64). */
    unsigned long *vals = (unsigned long *)data;
    unsigned long best_off = 0, off = 0;
    long best_score = -1;
    while (off + 2 <= nitems) {
        unsigned long w = vals[off], h = vals[off + 1];
        if (w == 0 || h == 0 || w > 4096 || h > 4096 ||
            off + 2 + w * h > nitems)
            break;
        long score = (w > (unsigned long)ICON_MAX_DIM ||
                      h > (unsigned long)ICON_MAX_DIM)
                         ? -1
                         : (w <= (unsigned long)ICON_PICK_DIM &&
                            h <= (unsigned long)ICON_PICK_DIM)
                                ? 1000 - (long)(ICON_PICK_DIM - w)
                                : (long)w;
        if (score > best_score) {
            best_score = score;
            best_off = off;
        }
        off += 2 + w * h;
    }
    if (best_score < 0) {
        XFree(data);
        return;
    }
    unsigned long w = vals[best_off], h = vals[best_off + 1];

    /* _NET_WM_ICON pixels are plain ARGB32 (host byte order); imlib uses
     * the same packing for DATA32 */
    Imlib_Image img = imlib_create_image_using_copied_data(
        (int)w, (int)h, (uint32_t *)(vals + best_off + 2));
    XFree(data);
    if (img)
        icon_set_from_imlib(c, img);
}

/* ── 3. WM_CLASS → .desktop → icon theme ────────────────────────────── */

/* find an icon file by name across the usual locations; returns malloc'd path */
static char *icon_find_file(const char *name) {
    static const char *size_dirs[] = {
        "/usr/share/icons/hicolor/48x48/apps",
        "/usr/share/icons/hicolor/64x64/apps",
        "/usr/share/icons/hicolor/32x32/apps",
        "/usr/share/icons/hicolor/128x128/apps",
        "/usr/share/icons/hicolor/256x256/apps",
        "/usr/share/icons/breeze/48x48/apps",
        "/usr/share/icons/breeze-dark/48x48/apps",
        "/usr/share/pixmaps",
        NULL
    };
    static const char *exts[] = { ".png", ".xpm", ".svg", NULL };

    char path[PATH_MAX];
    /* unqualified names may live directly in pixmaps/theme dirs */
    for (int d = 0; size_dirs[d]; d++)
        for (int e = 0; exts[e]; e++) {
            snprintf(path, sizeof(path), "%s/%s%s", size_dirs[d], name, exts[e]);
            struct stat st;
            if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
                return strdup(path);
            /* theme dirs also use subdirs like actions/... — skip; icons for
             * apps are covered above */
        }
    return NULL;
}

/* Does this .desktop file correspond to the app class?
 * Matches StartupWMClass=, or the basename of Exec= (case-insensitive). */
static int desktop_matches_class(FILE *f, const char *appclass) {
    char buf[512];
    char exe[512] = "";
    char wmc[512] = "";
    rewind(f);
    while (fgets(buf, sizeof(buf), f)) {
        if (strncmp(buf, "Exec=", 5) == 0) {
            char *nl = strchr(buf + 5, '\n');
            if (nl) *nl = '\0';
            /* first token; strip path; strip args */
            char *tok = buf + 5;
            while (*tok == ' ') tok++;
            char *sp = strchr(tok, ' ');
            if (sp) *sp = '\0';
            char *base = strrchr(tok, '/');
            snprintf(exe, sizeof(exe), "%s", base ? base + 1 : tok);
        } else if (strncmp(buf, "StartupWMClass=", 15) == 0) {
            char *nl = strchr(buf, '\n');
            if (nl) *nl = '\0';
            snprintf(wmc, sizeof(wmc), "%s", buf + 15);
        }
        if (exe[0] && wmc[0]) break;
    }
    if (wmc[0] && strcmp(wmc, appclass) == 0) return 1;
    /* Exec basename vs class basename, case-insensitive */
    {
        char lb[256];
        snprintf(lb, sizeof(lb), "%s", appclass);
        for (char *p = lb; *p; p++) *p = (char)tolower((unsigned char)*p);
        char *lbase = strrchr(lb, '/');
        if (lbase) lbase++;
        else lbase = lb;
        if (exe[0]) {
            char le[512];
            snprintf(le, sizeof(le), "%s", exe);
            for (char *p = le; *p; p++) *p = (char)tolower((unsigned char)*p);
            if (strcmp(le, lbase) == 0) return 1;
        }
    }
    return 0;
}

/* scan .desktop files for one matching the app class; returns malloc'd Icon */
static char *desktop_icon_name(const char *appclass) {
    static const char *dirs[] = {
        "/usr/share/applications",
        "/usr/local/share/applications",
        NULL
    };
    char path[PATH_MAX];
    char buf[512];

    for (int d = 0; dirs[d]; d++) {
        size_t dlen = strlen(dirs[d]);
        snprintf(path, sizeof(path), "%s", dirs[d]);
        DIR *dir = opendir(path);
        if (!dir) continue;
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            size_t nlen = strlen(de->d_name);
            if (nlen < 8 || strcmp(de->d_name + nlen - 8, ".desktop") != 0)
                continue;
            snprintf(path + dlen, sizeof(path) - dlen, "/%s", de->d_name);
            FILE *f = fopen(path, "r");
            if (!f) continue;
            if (desktop_matches_class(f, appclass)) {
                rewind(f);
                while (fgets(buf, sizeof(buf), f)) {
                    if (strncmp(buf, "Icon=", 5) == 0) {
                        char *nl = strchr(buf, '\n');
                        if (nl) *nl = '\0';
                        char *val = strdup(buf + 5);
                        fclose(f);
                        closedir(dir);
                        return val;
                    }
                }
            }
            fclose(f);
        }
        closedir(dir);
    }
    return NULL;
}

void icon_load_desktop(Client *c) {
    XClassHint ch;
    if (!XGetClassHint(dpy, c->win, &ch))
        return;
    char *cls = ch.res_class;
    if (!cls || !*cls) {
        if (cls) XFree(cls);
        if (ch.res_name) XFree(ch.res_name);
        return;
    }
    /* strip a leading path if res_class looks like one */
    {
        char *slash = strrchr(cls, '/');
        if (slash) cls = slash + 1;
    }

    char *iname = desktop_icon_name(cls);
    if (!iname && ch.res_name) {
        char *rslash = strrchr(ch.res_name, '/');
        iname = desktop_icon_name(rslash ? rslash + 1 : ch.res_name);
    }
    if (ch.res_name) XFree(ch.res_name);
    XFree(ch.res_class);
    if (!iname) return;

    char *file = icon_find_file(iname);
    free(iname);
    if (!file) return;

    Imlib_Image img = imlib_load_image(file);
    free(file);
    if (img)
        icon_set_from_imlib(c, img);
}

/* ── 4. default icon ─────────────────────────────────────────────────── */

/* Procedural "X program" icon: a gray Motif-ish button with the X11 logo
 * silhouette.  48x48 ARGB. */
void icon_load_default(Client *c) {
    enum { W = 48, H = 48 };
    Imlib_Image img = imlib_create_image(W, H);
    if (!img) return;
    imlib_context_set_image(img);
    unsigned int *pix = (unsigned int *)imlib_image_get_data();

    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int edge = (x < 2 || y < 2 || x >= W - 2 || y >= H - 2);
            int v = edge ? 90 : 208;
            /* X logo: two crossing bars in X11 blue */
            int on_diag = (abs(x * 2 - y - W / 2) < 7) ||
                          (abs(x * 2 + y - (W + H) / 2 + 8) < 7);
            unsigned int px = on_diag
                ? 0xFF0000C8u                              /* blue X */
                : (0xFFu << 24) | (v << 16) | (v << 8) | v; /* gray plate */
            pix[y * W + x] = px;
        }
    icon_set_from_imlib(c, img);
}

/* ── entry point: fill c->icon_* if WM_HINTS didn't provide one ──────── */

static int icon_have(Client *c) {
    return (c->icon_pixmap != None || c->icon_argb != NULL) &&
           c->icon_w > 0 && c->icon_h > 0;
}

void icon_acquire(Client *c) {
    /* WM_HINTS icon already present? (app-owned pixmap, w/h known) */
    if (c->icon_pixmap != None && c->icon_w > 0 && c->icon_h > 0 &&
        !c->icon_ours)
        return;
    icon_load_netwm(c);
    if (c->icon_pixmap != None || c->icon_argb) return;
    icon_load_desktop(c);
    if (c->icon_pixmap != None || c->icon_argb) return;
    icon_load_default(c);
}