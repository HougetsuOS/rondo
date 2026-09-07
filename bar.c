/*
 * rondo — status bar and icon bar
 */
#include "wm.h"
#include "xmplat_seam.h"
#include <time.h>
#include <Imlib2.h>
#include <sys/sysinfo.h>
#include <sys/statvfs.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>

/* ── bar refresh timer ─────────────────────────────────────────────────── */

XtIntervalId bar_refresh_timer = 0;

static void bar_refresh_cb(XtPointer data, XtIntervalId *id) {
    (void)data; (void)id;
    bar_refresh_timer = 0;
    drawbar();
    start_bar_timer();
}

void start_bar_timer(void) {
    if (bar_refresh_timer)
        XtRemoveTimeOut(bar_refresh_timer);
    bar_refresh_timer = XtAppAddTimeOut(app, 5000, bar_refresh_cb, NULL);
}

/* ── widget text helpers ──────────────────────────────────────────────── */

/* Fill buf with the display text for a text widget type.
 * Returns the text length, or 0 on failure.
 * If max_width is true, returns the widest possible text for stable sizing.
 *
 * Event-driven draws (defer_flush fires on every focus change) must not
 * re-read /proc, sysinfo, statvfs, battery sysfs each time — widget data
 * is refreshed through a short-interval cache gate below. */
static time_t stats_gate[16];  /* last refresh per widget type */

static int stats_fresh(int type, int interval)
{
    time_t now = time(NULL);
    if (now - stats_gate[type] >= interval) {
        stats_gate[type] = now;
        return 0;  /* stale — caller refreshes */
    }
    return 1;      /* fresh — caller reuses cached text */
}

static int widget_text(BarWidgetType type, char *buf, int buflen, int max_width) {
    switch (type) {
    case BAR_WIDGET_LOAD: {
        if (max_width) { snprintf(buf, buflen, " %.2f ", 99.99); return (int)strlen(buf); }
        static char cache[32];
        if (!stats_fresh(type, 1) || !cache[0]) {
            double load[1];
            int n = getloadavg(load, 1);
            if (n < 1) return 0;
            snprintf(cache, sizeof(cache), " %.2f ", load[0]);
        }
        snprintf(buf, buflen, "%s", cache);
        return (int)strlen(buf);
    }
    case BAR_WIDGET_MEM: {
        if (max_width) { snprintf(buf, buflen, " 999.9G/999G "); return (int)strlen(buf); }
        static char cache[32];
        if (!stats_fresh(type, 1) || !cache[0]) {
            struct sysinfo si;
            if (sysinfo(&si) < 0) return 0;
            unsigned long total_mb = si.totalram * si.mem_unit / (1024 * 1024);
            unsigned long used_mb = (si.totalram - si.freeram) * si.mem_unit / (1024 * 1024);
            if (total_mb >= 1024)
                snprintf(cache, sizeof(cache), " %.1fG/%.0fG ", used_mb / 1024.0, total_mb / 1024.0);
            else
                snprintf(cache, sizeof(cache), " %luM/%luM ", used_mb, total_mb);
        }
        snprintf(buf, buflen, "%s", cache);
        return (int)strlen(buf);
    }
    case BAR_WIDGET_DISK: {
        if (max_width) { snprintf(buf, buflen, " 999.9T/999.9T "); return (int)strlen(buf); }
        static char cache[32];
        if (!stats_fresh(type, 10) || !cache[0]) {
            struct statvfs vfs;
            if (statvfs("/", &vfs) < 0) return 0;
            unsigned long long total_bytes = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
            unsigned long long used_bytes = (unsigned long long)(vfs.f_blocks - vfs.f_bfree) * vfs.f_frsize;
            double used_gb = used_bytes / (1024.0 * 1024.0 * 1024.0);
            double total_gb = total_bytes / (1024.0 * 1024.0 * 1024.0);
            if (total_gb >= 1000.0)
                snprintf(cache, sizeof(cache), " %.1fT/%.1fT ", used_gb / 1000.0, total_gb / 1000.0);
            else
                snprintf(cache, sizeof(cache), " %.0fG/%.0fG ", used_gb, total_gb);
        }
        snprintf(buf, buflen, "%s", cache);
        return (int)strlen(buf);
    }
    case BAR_WIDGET_CLOCK: {
        if (max_width) {
            /* Use a fixed-width reference: all digits are narrower than letters,
             * so use the widest possible strftime output. We simulate with
             * the longest day/month names: "Wednesday 99 September 99:99:99" */
            struct tm max_tm = { .tm_wday = 3, .tm_mday = 99, .tm_mon = 8,
                                 .tm_hour = 99, .tm_min = 99, .tm_sec = 99 };
            strftime(buf, buflen, cfg_clock_format, &max_tm);
            return (int)strlen(buf);
        }
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        strftime(buf, buflen, cfg_clock_format, tm);
        return (int)strlen(buf);
    }
    case BAR_WIDGET_BAT: {
        if (max_width) { snprintf(buf, buflen, " 100%% F "); return (int)strlen(buf); }
        static char cache[32];
        if (!stats_fresh(type, 30) || !cache[0]) {
            const char *bats[] = { "BAT0", "BAT1", NULL };
            int have = 0;
            for (int i = 0; bats[i]; i++) {
                char path[128];
                snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", bats[i]);
                FILE *f = fopen(path, "r");
                if (!f) continue;
                int pct = 0;
                if (fscanf(f, "%d", &pct) != 1) { fclose(f); continue; }
                fclose(f);
                snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", bats[i]);
                f = fopen(path, "r");
                char status[32] = "Discharging";
                if (f) {
                    if (fgets(status, sizeof(status), f)) {
                        status[strcspn(status, "\n")] = '\0';
                    }
                    fclose(f);
                }
                char icon = 'V'; /* discharging ▼ */
                if (strcmp(status, "Charging") == 0)   icon = '^'; /* ▲ */
                else if (strcmp(status, "Full") == 0)   icon = 'F'; /* ⏻ */
                snprintf(cache, sizeof(cache), " %d%% %c ", pct, icon);
                have = 1;
                break;
            }
            if (!have) return 0;
        }
        snprintf(buf, buflen, "%s", cache);
        return (int)strlen(buf);
    }
    case BAR_WIDGET_VOL: {
        if (max_width) { snprintf(buf, buflen, " 100%% "); return (int)strlen(buf); }
        /* TTL cache: popen fork+exec+pipe is far too expensive per redraw */
        static int cache_vol = -1, cache_muted = 0;
        static time_t cache_at = 0;
        time_t now = time(NULL);
        if (cache_vol < 0 || now - cache_at >= 2) {
            FILE *p = popen("amixer get Master 2>/dev/null", "r");
            if (p) {
                char line[256];
                int vol = -1, muted = 0;
                while (fgets(line, sizeof(line), p)) {
                    char *pct = strstr(line, "%]");
                    if (pct) {
                        char *br = strchr(line, '[');
                        if (br) vol = atoi(br + 1);
                    }
                    if (strstr(line, "[off]")) muted = 1;
                }
                pclose(p);
                if (vol >= 0) {
                    cache_vol = vol;
                    cache_muted = muted;
                    cache_at = now;
                }
            }
        }
        if (cache_vol < 0) return 0;
        if (cache_muted)
            snprintf(buf, buflen, " MUTE ");
        else
            snprintf(buf, buflen, " %d%% ", cache_vol);
        return (int)strlen(buf);
    }
    case BAR_WIDGET_CPU: {
        if (max_width) { snprintf(buf, buflen, " CPU 100%% "); return (int)strlen(buf); }
        static char cache[32];
        static long long prev_idle = 0, prev_total = 0;
        if (!stats_fresh(type, 1) || !cache[0]) {
            FILE *f = fopen("/proc/stat", "r");
            if (!f) return 0;
            long long user, nice, system, idle, iowait, irq, softirq, steal;
            if (fscanf(f, "cpu %lld %lld %lld %lld %lld %lld %lld %lld",
                       &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) < 8) {
                fclose(f);
                return 0;
            }
            fclose(f);
            long long cur_idle = idle + iowait;
            long long cur_total = user + nice + system + idle + iowait + irq + softirq + steal;
            long long d_idle = cur_idle - prev_idle;
            long long d_total = cur_total - prev_total;
            prev_idle = cur_idle;
            prev_total = cur_total;
            if (d_total == 0) return 0;
            int pct = (int)((d_total - d_idle) * 100 / d_total);
            if (pct > 100) pct = 100;
            snprintf(cache, sizeof(cache), " CPU %d%% ", pct);
        }
        snprintf(buf, buflen, "%s", cache);
        return (int)strlen(buf);
    }
    case BAR_WIDGET_NET: {
        if (max_width) { snprintf(buf, buflen, " wwlpxxxxxxxx ^ "); return (int)strlen(buf); }
        static char cache[64];
        if (!stats_fresh(type, 5) || !cache[0]) {
            FILE *f = fopen("/proc/net/route", "r");
            if (!f) return 0;
            char line[256], iface[32] = "";
            while (fgets(line, sizeof(line), f)) {
                char ifname[32];
                unsigned int dest;
                if (sscanf(line, "%31s %x", ifname, &dest) == 2 && dest == 0) {
                    strncpy(iface, ifname, sizeof(iface) - 1);
                    iface[sizeof(iface) - 1] = '\0';
                    break;
                }
            }
            fclose(f);
            if (iface[0] == '\0') return 0;
            char path[128];
            snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", iface);
            f = fopen(path, "r");
            int up = 0;
            if (f) {
                char state[32];
                if (fgets(state, sizeof(state), f) && strncmp(state, "up", 2) == 0)
                    up = 1;
                fclose(f);
            }
            snprintf(cache, sizeof(cache), " %s %c ", iface, up ? '^' : 'V');
        }
        snprintf(buf, buflen, "%s", cache);
        return (int)strlen(buf);
    }
    case BAR_WIDGET_LAYOUT: {
        snprintf(buf, buflen, " %c ", cur_layout == LAYOUT_BINARY_TREE ? 'T' : 'M');
        return (int)strlen(buf);
    }
    case BAR_WIDGET_TEMP: {
        if (max_width) { snprintf(buf, buflen, " 999C "); return (int)strlen(buf); }
        static char cache[16];
        if (!stats_fresh(type, 5) || !cache[0]) {
            FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
            if (!f) return 0;
            int millideg;
            if (fscanf(f, "%d", &millideg) != 1) { fclose(f); return 0; }
            fclose(f);
            snprintf(cache, sizeof(cache), " %dC ", millideg / 1000);
        }
        snprintf(buf, buflen, "%s", cache);
        return (int)strlen(buf);
    }
    default:
        return 0;
    }
}

/* ── geometry helpers ─────────────────────────────────────────────────── */

int is_horizontal(BarPosition p) { return p == BAR_POS_TOP || p == BAR_POS_BOTTOM; }
int is_vertical(BarPosition p)   { return p == BAR_POS_LEFT || p == BAR_POS_RIGHT; }

/* Effective bar thickness: must be wide enough for buttons when vertical.
 * For horizontal bars, this is just BAR_HEIGHT.
 * For vertical bars, this is max(BAR_HEIGHT, 2*BAR_BORDER_WIDTH + BAR_BTN_WIDTH)
 * so the bar is always wide enough for workspace buttons. */
int bar_effective_thickness(void) {
    if (is_vertical(bar_position)) {
        int min_thick = 2 * BAR_BORDER_WIDTH + BAR_BTN_WIDTH;
        return BAR_HEIGHT > min_thick ? BAR_HEIGHT : min_thick;
    }
    return BAR_HEIGHT;
}

BarGeometry calc_bar_geometry(void) {
    BarGeometry g;
    int bar_thick = show_bar ? bar_effective_thickness() : 0;
    int bar_h = show_bar ? bar_thick : 0;
    int ibar_vis = iconbar_visible();
    /* thickness of icon bar perpendicular to its length:
     * vertical bar: width = ICON_W; horizontal bar: height = icon_entry_h() */
    int ibar_thick_v = ICON_W;
    int ibar_thick_h = ibar_vis ? icon_entry_h() : 0;

    /* --- status bar rect --- */
    g.bar_x = mon.x; g.bar_y = mon.y; g.bar_w = 0; g.bar_h = 0;
    if (bar_h) {
        switch (bar_position) {
        case BAR_POS_TOP:
            g.bar_x = mon.x; g.bar_y = mon.y;
            g.bar_w = mon.w; g.bar_h = bar_thick;
            break;
        case BAR_POS_BOTTOM:
            g.bar_x = mon.x; g.bar_y = mon.y + mon.h - bar_thick;
            g.bar_w = mon.w; g.bar_h = bar_thick;
            break;
        case BAR_POS_LEFT:
            g.bar_x = mon.x; g.bar_y = mon.y;
            g.bar_w = bar_thick; g.bar_h = mon.h;
            break;
        case BAR_POS_RIGHT:
            g.bar_x = mon.x + mon.w - bar_thick; g.bar_y = mon.y;
            g.bar_w = bar_thick; g.bar_h = mon.h;
            break;
        }
    }

    /* --- icon bar rect (before same-side adjustment) --- */
    g.ibar_x = mon.x; g.ibar_y = mon.y; g.ibar_w = 0; g.ibar_h = 0;
    if (ibar_vis) {
        switch (iconbar_position) {
        case BAR_POS_LEFT:
            g.ibar_x = mon.x; g.ibar_y = mon.y;
            g.ibar_w = ibar_thick_v; g.ibar_h = mon.h;
            break;
        case BAR_POS_RIGHT:
            g.ibar_x = mon.x + mon.w - ibar_thick_v; g.ibar_y = mon.y;
            g.ibar_w = ibar_thick_v; g.ibar_h = mon.h;
            break;
        case BAR_POS_TOP:
            g.ibar_x = mon.x; g.ibar_y = mon.y;
            g.ibar_w = mon.w; g.ibar_h = ibar_thick_h;
            break;
        case BAR_POS_BOTTOM:
            g.ibar_x = mon.x; g.ibar_y = mon.y + mon.h - ibar_thick_h;
            g.ibar_w = mon.w; g.ibar_h = ibar_thick_h;
            break;
        }
        /* same-side adjustment: status bar closer to edge */
        if (bar_h && bar_position == iconbar_position) {
            if (iconbar_position == BAR_POS_LEFT)
                g.ibar_x += bar_thick;
            else if (iconbar_position == BAR_POS_RIGHT)
                g.ibar_x -= bar_thick;
            else if (iconbar_position == BAR_POS_TOP)
                g.ibar_y += bar_thick;
            else /* BAR_POS_BOTTOM */
                g.ibar_y -= bar_thick;
        }
    }

    /* --- tiling area --- */
    g.x = mon.x; g.y = mon.y; g.w = mon.w; g.h = mon.h;
    if (bar_h) {
        switch (bar_position) {
        case BAR_POS_TOP:    g.y += bar_thick; g.h -= bar_thick; break;
        case BAR_POS_BOTTOM: g.h -= bar_thick; break;
        case BAR_POS_LEFT:   g.x += bar_thick; g.w -= bar_thick; break;
        case BAR_POS_RIGHT:  g.w -= bar_thick; break;
        }
    }
    if (ibar_vis) {
        switch (iconbar_position) {
        case BAR_POS_LEFT:   g.x += ibar_thick_v; g.w -= ibar_thick_v; break;
        case BAR_POS_RIGHT:  g.w -= ibar_thick_v; break;
        case BAR_POS_TOP:    g.y += ibar_thick_h; g.h -= ibar_thick_h; break;
        case BAR_POS_BOTTOM: g.h -= ibar_thick_h; break;
        }
    }

    /* safety: clamp tiling area to positive dimensions */
    if (g.w < 0) g.w = 0;
    if (g.h < 0) g.h = 0;

    return g;
}

/* ── status bar ─────────────────────────────────────────────────────── */

/* forward declarations */
static void drawbar_horizontal(void);
static void drawbar_vertical(void);
static void drawiconbar_vertical(void);
static void drawiconbar_horizontal(void);

/* Allocated width for the expanding title widget (set by drawbar layout pass). */
static int title_alloc_w = 0;

/* Volume widget hit-test region (set during draw, used for scroll events). */
static int vol_widget_x = -1, vol_widget_y = -1;
static int vol_widget_w = 0, vol_widget_h = 0;

/* Tray widget allocated region (set during draw, used by tray_reposition). */
/* Defined in tray.c — extern declarations in wm.h */

/* Helper: measure and draw a single bar widget.
 * Returns the width the widget occupies.
 * If draw is false, only measures (no drawing).
 * x is the left edge of the widget's allocated space.
 * btn_y, btn_sz, border_light, border_shadow are bar metrics.
 * For BAR_WIDGET_TITLE: when measuring returns 0 (expanding);
 *   when drawing, uses title_alloc_w as the allocated width.
 */
static int draw_bar_widget(BarWidgetType type, int x, int btn_y, int btn_sz,
                           XftColor *border_light,
                           XftColor *border_shadow, int draw) {
    switch (type) {
    case BAR_WIDGET_WS: {
        /* workspace indicator buttons — packed together */
        int wx = x;
        for (int i = 0; i < NUM_WORKSPACES; i++) {
            ws_x[i] = wx;
            XftColor *col;
            if (i == curws) {
                col = &col_bar_ws_active;
            } else {
                int occupied = 0;
                for (Client *c = clients; c; c = c->next)
                    if (c->ws == i) { occupied = 1; break; }
                col = occupied ? &col_bar_ws_occupied : &col_bar_ws_idle;
            }
            if (draw) {
                char label[12];
                snprintf(label, sizeof(label), "%d", i + 1);
                /* workspace labels "1".."N" are constant — cache extents */
                static XGlyphInfo ws_ext[16];
                static XftFont *ws_ext_font = NULL;
                if (ws_ext_font != xftfont) {
                    for (int j = 0; j < NUM_WORKSPACES && j < 16; j++) {
                        char l[12];
                        snprintf(l, sizeof(l), "%d", j + 1);
                        XftTextExtents8(dpy, xftfont, (XftChar8 *)l,
                                        (int)strlen(l), &ws_ext[j]);
                    }
                    ws_ext_font = xftfont;
                }
                XGlyphInfo ext = ws_ext[i < 16 ? i : 0];

                XftColor *btn_fill = &col_bar_ws_bg;
                XftDrawRect(xftdraw, btn_fill, wx, btn_y, btn_sz, btn_sz);

                if (i == curws)
                    bevel_rect_inv(xftdraw, wx, btn_y, btn_sz, btn_sz,
                                   1, 1, 1, 1, border_light, border_shadow);
                else
                    bevel_rect(xftdraw, wx, btn_y, btn_sz, btn_sz,
                               1, 1, 1, 1, border_light, border_shadow);

                int text_x = wx + (btn_sz - ext.xOff) / 2;
                XftDrawStringUtf8(xftdraw, col, xftfont,
                                   text_x, btn_y + (btn_sz + xftfont->ascent - xftfont->descent) / 2,
                                   (XftChar8 *)label, (int)strlen(label));
            }
            wx += btn_sz;
        }
        return NUM_WORKSPACES * btn_sz;
    }

    case BAR_WIDGET_TITLE: {
        /* expanding widget — reports 0 width when measuring;
         * when drawing, uses title_alloc_w set by drawbar() */
        if (!draw) return 0;
        int w = title_alloc_w;
        if (w <= 0) return 0;
        /* draw title button background */
        XftDrawRect(xftdraw, &col_bar_ws_bg, x, btn_y, w, btn_sz);
        bevel_rect(xftdraw, x, btn_y, w, btn_sz,
                   1, 1, 1, 1, border_light, border_shadow);
        /* draw window name, clipped to allocated width */
        if (focused && focused->ws == curws && focused->name[0]) {
            int namelen = (int)strlen(focused->name);
            int text_y = btn_y + (btn_sz + xftfont->ascent - xftfont->descent) / 2;
            XRectangle clip = { .x = x + 2, .y = btn_y, .width = (unsigned short)(w - 4), .height = (unsigned short)btn_sz };
            XftDrawSetClipRectangles(xftdraw, 0, 0, &clip, 1);
            XftDrawStringUtf8(xftdraw, &col_bar_fg, xftfont,
                               x + 2, text_y,
                               (XftChar8 *)focused->name, namelen);
            XftDrawSetClipRectangles(xftdraw, 0, 0,
                &(XRectangle){ .x = 0, .y = 0, .width = (unsigned short)mon.w, .height = (unsigned short)mon.h }, 1);
        }
        return w;
    }

    case BAR_WIDGET_TRAY: {
        int sz = tray_icon_size();
        int pad = 2;
        int n = num_tray_icons > 0 ? num_tray_icons : 0;
        int total = n * (sz + pad) + pad;
        if (draw && total > 0) {
            /* record allocated region for tray_reposition() */
            tray_widget_x = x;
            tray_widget_y = btn_y;
            tray_widget_w = total;
            tray_widget_h = btn_sz;
        }
        return total;
    }

    case BAR_WIDGET_LAYOUT: /* fallthrough */
    case BAR_WIDGET_CLOCK: /* fallthrough */
    case BAR_WIDGET_LOAD: /* fallthrough */
    case BAR_WIDGET_MEM: /* fallthrough */
    case BAR_WIDGET_DISK: /* fallthrough */
    case BAR_WIDGET_BAT: /* fallthrough */
    case BAR_WIDGET_VOL: /* fallthrough */
    case BAR_WIDGET_CPU: /* fallthrough */
    case BAR_WIDGET_NET: /* fallthrough */
    case BAR_WIDGET_TEMP: {
        char text[64];
        int len = widget_text(type, text, sizeof(text), !draw);
        if (len <= 0) return 0;
        /* measure: use max-width; draw: use max-width for box, center real text.
         * max-width extents are invariant per widget type + font — cache them
         * (invalidated when the font reloads) instead of re-measuring per call. */
        static XGlyphInfo max_ext_cache[BAR_WIDGET_TEMP + 1];
        static int max_ext_valid[BAR_WIDGET_TEMP + 1] = {0};
        static XftFont *max_ext_font[BAR_WIDGET_TEMP + 1] = {0};
        XGlyphInfo ext;
        XGlyphInfo max_ext;
        XftTextExtents8(dpy, xftfont, (XftChar8 *)text, len, &ext);
        if (!max_ext_valid[type] || max_ext_font[type] != xftfont) {
            char max_text[64];
            int max_len = widget_text(type, max_text, sizeof(max_text), 1);
            if (max_len > 0) {
                XftTextExtents8(dpy, xftfont, (XftChar8 *)max_text, max_len,
                                &max_ext_cache[type]);
                max_ext_font[type] = xftfont;
                max_ext_valid[type] = 1;
            } else {
                max_ext_cache[type] = ext;
                max_ext_font[type] = xftfont;
                max_ext_valid[type] = 1;
            }
        }
        max_ext = max_ext_cache[type];
        int w = max_ext.xOff + 2;  /* always use max width for stable sizing */
        if (draw) {
            XftDrawRect(xftdraw, &col_bar_ws_bg, x, btn_y, w, btn_sz);
            bevel_rect(xftdraw, x, btn_y, w, btn_sz,
                       1, 1, 1, 1, border_light, border_shadow);
            int text_x = x + 1 + (max_ext.xOff - ext.xOff) / 2;
            int text_y = btn_y + (btn_sz + xftfont->ascent - xftfont->descent) / 2;
            XftDrawStringUtf8(xftdraw, &col_bar_fg, xftfont,
                               text_x, text_y,
                               (XftChar8 *)text, len);
        }
        /* record volume widget hit-test region */
        if (type == BAR_WIDGET_VOL) {
            vol_widget_x = x;
            vol_widget_y = btn_y;
            vol_widget_w = w;
            vol_widget_h = btn_sz;
        }
        return w;
    }

    }
    return 0;
}

void drawbar(void) {
    if (!show_bar) return;
    if (!xftdraw || !xftfont) return;
    tray_widget_x = 0; tray_widget_y = 0;
    tray_widget_w = 0; tray_widget_h = 0;
    if (is_horizontal(bar_position))
        drawbar_horizontal();
    else
        drawbar_vertical();
    if (tray_widget_w > 0 && num_tray_icons > 0)
        tray_reposition();
}

/* Draw the bar border for a bar at a given position.
 * The edge flush against the screen gets no border. */
static void draw_bar_border(XftDraw *draw, int bw, int cs,
                            XftColor *light, XftColor *shadow,
                            int bar_w, int bar_h) {
    switch (bar_position) {
    case BAR_POS_TOP:
        /* no top border — flush against screen top */
        stretcher_corner(draw, bar_w - cs, bar_h - cs, STRETCH_SE, bw, cs, cs, light, shadow);
        stretcher_corner(draw, 0, bar_h - cs, STRETCH_SW, bw, cs, cs, light, shadow);
        if (bar_w > 2 * cs)
            bevel_rect(draw, cs, bar_h - bw, bar_w - 2*cs, bw, 1, 1, 2, 1, light, shadow);
        if (bar_h > cs)
            bevel_rect(draw, 0, 0, bw, bar_h - cs, 1, 1, 1, 2, light, shadow);
        if (bar_h > cs)
            bevel_rect(draw, bar_w - bw, 0, bw, bar_h - cs, 1, 2, 1, 1, light, shadow);
        break;
    case BAR_POS_BOTTOM:
        /* no bottom border — flush against screen bottom */
        stretcher_corner(draw, 0, 0, STRETCH_NW, bw, cs, cs, light, shadow);
        stretcher_corner(draw, bar_w - cs, 0, STRETCH_NE, bw, cs, cs, light, shadow);
        if (bar_w > 2 * cs)
            bevel_rect(draw, cs, 0, bar_w - 2*cs, bw, 2, 1, 1, 1, light, shadow);
        if (bar_h > cs)
            bevel_rect(draw, 0, cs, bw, bar_h - cs, 1, 1, 1, 2, light, shadow);
        if (bar_h > cs)
            bevel_rect(draw, bar_w - bw, cs, bw, bar_h - cs, 1, 2, 1, 1, light, shadow);
        break;
    case BAR_POS_LEFT:
        /* no left border — flush against screen left */
        stretcher_corner(draw, bar_w - cs, 0, STRETCH_NE, bw, cs, cs, light, shadow);
        stretcher_corner(draw, bar_w - cs, bar_h - cs, STRETCH_SE, bw, cs, cs, light, shadow);
        if (bar_h > 2 * cs)
            bevel_rect(draw, bar_w - bw, cs, bw, bar_h - 2*cs, 1, 2, 1, 1, light, shadow);
        if (bar_w > cs)
            bevel_rect(draw, 0, 0, bar_w - cs, bw, 1, 1, 1, 2, light, shadow);
        if (bar_w > cs)
            bevel_rect(draw, 0, bar_h - bw, bar_w - cs, bw, 1, 1, 2, 1, light, shadow);
        break;
    case BAR_POS_RIGHT:
        /* no right border — flush against screen right */
        stretcher_corner(draw, 0, 0, STRETCH_NW, bw, cs, cs, light, shadow);
        stretcher_corner(draw, 0, bar_h - cs, STRETCH_SW, bw, cs, cs, light, shadow);
        if (bar_h > 2 * cs)
            bevel_rect(draw, 0, cs, bw, bar_h - 2*cs, 1, 1, 1, 2, light, shadow);
        if (bar_w > cs)
            bevel_rect(draw, 0, 0, bar_w - cs, bw, 1, 1, 1, 2, light, shadow);
        if (bar_w > cs)
            bevel_rect(draw, 0, bar_h - bw, bar_w - cs, bw, 1, 1, 2, 1, light, shadow);
        break;
    }
}

static void drawbar_horizontal(void) {
    int bw = BAR_BORDER_WIDTH;
    int cs = BAR_CORNER_SIZE;
    XftColor *border_light = &col_bar_border_light;
    XftColor *border_shadow = &col_bar_border_shadow;
    int bar_w = mon.w;
    int bar_h = BAR_HEIGHT;

    /* bar background fill */
    {
        XmPlatDrawCtx c = _XmPlatCtx(dpy, barwin, argb_gc);
        _XmPlatSetForeground(c, col_bar_fill.pixel);
        _XmPlatFillRect(c, 0, 0, (unsigned)bar_w, (unsigned)bar_h);
        _XmPlatCtxFree(c);
    }

    draw_bar_border(xftdraw, bw, cs, border_light, border_shadow, bar_w, bar_h);

    /* interior area */
    int btn_sz = BAR_BTN_WIDTH;
    int btn_y = (bar_h - bw - btn_sz) / 2;

    /* available width between left and right corners */
    int left_x = bw;
    int right_edge = bar_w - bw;

    /* Phase 1: measure non-title widgets (title is expanding — reports 0) */
    int left_fixed = 0, right_fixed = 0;
    for (int i = 0; i < num_bar_widgets; i++) {
        if (bar_widgets[i].type == BAR_WIDGET_TITLE) continue;
        int w = draw_bar_widget(bar_widgets[i].type, 0, btn_y, btn_sz,
                                border_light, border_shadow, 0);
        if (bar_widgets[i].align == BAR_ALIGN_LEFT)
            left_fixed += w;
        else
            right_fixed += w;
    }

    /* title fills the gap between left-fixed and right-fixed widgets */
    int avail = right_edge - left_x;
    title_alloc_w = avail - left_fixed - right_fixed;
    if (title_alloc_w < 0) title_alloc_w = 0;

    /* Phase 2: draw left-aligned non-title widgets */
    int lx = left_x;
    for (int i = 0; i < num_bar_widgets; i++) {
        if (bar_widgets[i].type == BAR_WIDGET_TITLE) continue;
        if (bar_widgets[i].align != BAR_ALIGN_LEFT) continue;
        int w = draw_bar_widget(bar_widgets[i].type, lx, btn_y, btn_sz,
                                border_light, border_shadow, 0);
        draw_bar_widget(bar_widgets[i].type, lx, btn_y, btn_sz,
                        border_light, border_shadow, 1);
        lx += w;
    }

    /* Phase 3: draw title (expanding — fills the gap) */
    for (int i = 0; i < num_bar_widgets; i++) {
        if (bar_widgets[i].type != BAR_WIDGET_TITLE) continue;
        draw_bar_widget(BAR_WIDGET_TITLE, lx, btn_y, btn_sz,
                        border_light, border_shadow, 1);
        lx += title_alloc_w;
        break;
    }

    /* Phase 4: draw right-aligned non-title widgets (right-to-left) */
    int rx = right_edge;
    for (int i = num_bar_widgets - 1; i >= 0; i--) {
        if (bar_widgets[i].type == BAR_WIDGET_TITLE) continue;
        if (bar_widgets[i].align != BAR_ALIGN_RIGHT) continue;
        int w = draw_bar_widget(bar_widgets[i].type, 0, btn_y, btn_sz,
                                border_light, border_shadow, 0);
        rx -= w;
        draw_bar_widget(bar_widgets[i].type, rx, btn_y, btn_sz,
                        border_light, border_shadow, 1);
    }

    XFlush(dpy);
}

/* Vertical status bar (left or right edge).
 * Workspace buttons stack vertically, title text drawn
 * character-by-character, clock at the bottom. */
static void drawbar_vertical(void) {
    int bw = BAR_BORDER_WIDTH;
    int bar_w = bar_effective_thickness();  /* thickness when vertical */
    /* vertical bar corners should not exceed bar width */
    int cs = BAR_CORNER_SIZE;
    if (cs > bar_w - bw) cs = bar_w - bw;
    XftColor *border_light = &col_bar_border_light;
    XftColor *border_shadow = &col_bar_border_shadow;
    int bar_h = mon.h;

    /* interior x-range: LEFT bar has right border but no left border,
     * RIGHT bar has left border but no right border */
    int interior_left  = (bar_position == BAR_POS_RIGHT) ? bw : 0;
    int interior_right = (bar_position == BAR_POS_LEFT)  ? bar_w - bw : bar_w;
    int interior_w = interior_right - interior_left;

    /* bar background fill */
    {
        XmPlatDrawCtx c = _XmPlatCtx(dpy, barwin, argb_gc);
        _XmPlatSetForeground(c, col_bar_fill.pixel);
        _XmPlatFillRect(c, 0, 0, (unsigned)bar_w, (unsigned)bar_h);
        _XmPlatCtxFree(c);
    }

    draw_bar_border(xftdraw, bw, cs, border_light, border_shadow, bar_w, bar_h);

    /* workspace buttons stacked vertically at the top */
    int btn_sz = BAR_BTN_WIDTH;
    if (btn_sz > interior_w) btn_sz = interior_w;
    int btn_x = interior_left + (interior_w - btn_sz) / 2;
    int wy = bw;
    for (int i = 0; i < NUM_WORKSPACES; i++) {
        ws_y[i] = wy;
        XftColor *col;
        if (i == curws) {
            col = &col_bar_ws_active;
        } else {
            int occupied = 0;
            for (Client *c = clients; c; c = c->next)
                if (c->ws == i) { occupied = 1; break; }
            col = occupied ? &col_bar_ws_occupied : &col_bar_ws_idle;
        }
        char label[12];
        snprintf(label, sizeof(label), "%d", i + 1);
        XftColor *btn_fill = &col_bar_ws_bg;
        XftDrawRect(xftdraw, btn_fill, btn_x, wy, btn_sz, btn_sz);
        if (i == curws)
            bevel_rect_inv(xftdraw, btn_x, wy, btn_sz, btn_sz,
                           1, 1, 1, 1, border_light, border_shadow);
        else
            bevel_rect(xftdraw, btn_x, wy, btn_sz, btn_sz,
                       1, 1, 1, 1, border_light, border_shadow);
        /* cache workspace label extents (same cache as horizontal bar) */
        static XGlyphInfo wsv_ext[16];
        static XftFont *wsv_ext_font = NULL;
        if (wsv_ext_font != xftfont) {
            for (int j = 0; j < NUM_WORKSPACES && j < 16; j++) {
                char l[12];
                snprintf(l, sizeof(l), "%d", j + 1);
                XftTextExtents8(dpy, xftfont, (XftChar8 *)l,
                                (int)strlen(l), &wsv_ext[j]);
            }
            wsv_ext_font = xftfont;
        }
        XGlyphInfo ext = wsv_ext[i < 16 ? i : 0];
        int text_x = btn_x + (btn_sz - ext.xOff) / 2;
        XftDrawStringUtf8(xftdraw, col, xftfont,
                           text_x, wy + (btn_sz + xftfont->ascent - xftfont->descent) / 2,
                           (XftChar8 *)label, (int)strlen(label));
        wy += btn_sz;
    }

    /* text widgets (clock, load, mem, disk) stacked at the bottom, drawn vertically */
    {
        /* collect bottom-aligned widgets in reverse order so we stack up from bottom */
        BarWidgetType bottom_widgets[16];
        int n_bottom = 0;
        for (int i = num_bar_widgets - 1; i >= 0; i--) {
            if (bar_widgets[i].align == BAR_ALIGN_RIGHT &&
                bar_widgets[i].type != BAR_WIDGET_WS && bar_widgets[i].type != BAR_WIDGET_TITLE) {
                if (n_bottom < 16)
                    bottom_widgets[n_bottom++] = bar_widgets[i].type;
            }
        }
        /* measure max heights first for stable positioning */
        int max_h[16];
        for (int bi = 0; bi < n_bottom && bi < 16; bi++) {
            if (bottom_widgets[bi] == BAR_WIDGET_TRAY) {
                int sz = tray_icon_size();
                int pad = 2;
                max_h[bi] = num_tray_icons * (sz + pad) + pad;
                if (max_h[bi] < sz + pad) max_h[bi] = sz + pad;
            } else {
                char mtext[64];
                int mlen = widget_text(bottom_widgets[bi], mtext, sizeof(mtext), 1);
                if (mlen <= 0) { max_h[bi] = 0; continue; }
                XGlyphInfo ext;
                XftTextExtents8(dpy, xftfont, (XftChar8 *)mtext, mlen, &ext);
                max_h[bi] = ext.xOff + 4;  /* +4 for gap */
            }
        }
        /* compute y positions from max heights */
        int text_y_start = bar_h - bw - 4;
        for (int bi = n_bottom - 1; bi >= 0; bi--)
            text_y_start -= max_h[bi];
        if (text_y_start < wy + 4) text_y_start = wy + 4;

        int widget_y_cursor = text_y_start;
        for (int bi = 0; bi < n_bottom; bi++) {
            if (max_h[bi] == 0) continue;
            if (bottom_widgets[bi] == BAR_WIDGET_TRAY) {
                /* tray: no text to draw, just record region for wrapper positioning */
                tray_widget_x = interior_left;
                tray_widget_y = widget_y_cursor;
                tray_widget_w = interior_w;
                tray_widget_h = max_h[bi];
                widget_y_cursor += max_h[bi];
                continue;
            }
            char text[64];
            int len = widget_text(bottom_widgets[bi], text, sizeof(text), 0);
            if (len <= 0) { widget_y_cursor += max_h[bi]; continue; }
            /* center real text within the max-height slot */
            XGlyphInfo ext;
            XftTextExtents8(dpy, xftfont, (XftChar8 *)text, len, &ext);
            int text_h = ext.xOff;
            int slot_h = max_h[bi];
            int cy = widget_y_cursor + (slot_h - text_h) / 2;
            /* draw character-by-character vertically */
            for (int ci = 0; ci < len; ci++) {
                XGlyphInfo ch_ext;
                XftTextExtents8(dpy, xftfont, (XftChar8 *)&text[ci], 1, &ch_ext);
                int ch_x = interior_left + (interior_w - ch_ext.xOff) / 2;
                XftDrawStringUtf8(xftdraw, &col_bar_fg, xftfont,
                                  ch_x, cy + xftfont->ascent,
                                  (XftChar8 *)&text[ci], 1);
                cy += ch_ext.xOff;
            }
            widget_y_cursor += max_h[bi];
        }
        /* record volume widget hit-test region for vertical bar */
        vol_widget_x = -1; vol_widget_w = 0;
        vol_widget_y = text_y_start;
        vol_widget_h = (bar_h - bw - 4) - text_y_start;
        for (int bi = 0; bi < n_bottom; bi++) {
            if (bottom_widgets[bi] == BAR_WIDGET_VOL) {
                /* find the y range for just the volume widget */
                int vy = text_y_start;
                for (int bj = 0; bj < bi; bj++)
                    vy += max_h[bj];
                vol_widget_y = vy;
                vol_widget_h = max_h[bi];
                vol_widget_x = interior_left;
                vol_widget_w = interior_w;
                break;
            }
        }
    }

    /* title: vertical text in the middle area */
    if (focused && focused->ws == curws && focused->name[0]) {
        int name_y = wy + 4;
        int name_end = bar_h - bw - 40; /* leave room for bottom text widgets */
        int namelen = (int)strlen(focused->name);
        int cx = interior_left + interior_w / 2;
        XRectangle clip = { .x = (short)interior_left, .y = (short)name_y,
                            .width = (unsigned short)interior_w, .height = (unsigned short)(name_end - name_y) };
        XftDrawSetClipRectangles(xftdraw, 0, 0, &clip, 1);
        int ty = name_y + xftfont->ascent;
        for (int ci = 0; ci < namelen && ty < name_end; ci++) {
            XGlyphInfo ch_ext;
            XftTextExtents8(dpy, xftfont, (XftChar8 *)&focused->name[ci], 1, &ch_ext);
            int ch_x = cx - ch_ext.xOff / 2;
            XftDrawStringUtf8(xftdraw, &col_bar_fg, xftfont,
                              ch_x, ty, (XftChar8 *)&focused->name[ci], 1);
            ty += xftfont->ascent + xftfont->descent;
        }
        XftDrawSetClipRectangles(xftdraw, 0, 0,
            &(XRectangle){ .x = 0, .y = 0, .width = (unsigned short)mon.w, .height = (unsigned short)mon.h }, 1);
    }

    XFlush(dpy);
}

/* handle click on status bar — workspace labels */
void handle_bar_click(int x, int y) {
    if (is_horizontal(bar_position)) {
        /* horizontal bar: test X positions */
        for (int i = 0; i < NUM_WORKSPACES; i++) {
            int end_x = (i < NUM_WORKSPACES - 1) ? ws_x[i + 1] : ws_x[i] + BAR_BTN_WIDTH;
            if (x >= ws_x[i] && x < end_x) {
                if (i != curws) {
                    WmArg arg = { .ui = (unsigned int)(i + 1) };
                    viewworkspace(&arg);
                }
                return;
            }
        }
    } else {
        /* vertical bar: test Y positions */
        for (int i = 0; i < NUM_WORKSPACES; i++) {
            int end_y = (i < NUM_WORKSPACES - 1) ? ws_y[i + 1] : ws_y[i] + BAR_BTN_WIDTH;
            if (y >= ws_y[i] && y < end_y) {
                if (i != curws) {
                    WmArg arg = { .ui = (unsigned int)(i + 1) };
                    viewworkspace(&arg);
                }
                return;
            }
        }
    }
}

/* One-shot timer callback: redraw bar after volume change. */
static void vol_redraw_cb(XtPointer data, XtIntervalId *id) {
    (void)data; (void)id;
    drawbar();
}

/* Adjust volume by delta percent using pactl. Positive = up, negative = down. */
static void volume_change(int delta) {
    const char *cmd = delta > 0
        ? "pactl set-sink-volume @DEFAULT_SINK@ +5% 2>/dev/null"
        : "pactl set-sink-volume @DEFAULT_SINK@ -5% 2>/dev/null";
    if (fork() == 0) {
        if (dpy) close(ConnectionNumber(dpy));
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    /* redraw bar after a short delay to reflect new volume */
    XtAppAddTimeOut(app, 30, vol_redraw_cb, NULL);
}

/* Handle scroll wheel on the status bar.
 * Button4 = scroll up, Button5 = scroll down.
 * Currently only the volume widget responds to scroll. */
void handle_bar_scroll(int x, int y, int button) {
    if (vol_widget_w <= 0) return;  /* no volume widget */
    if (x >= vol_widget_x && x < vol_widget_x + vol_widget_w &&
        y >= vol_widget_y && y < vol_widget_y + vol_widget_h) {
        volume_change(button == Button4 ? 1 : -1);
    }
}


/* ── icon bar (minimized window icons on left side) ─────────────────── */

/* Free the cached scaled icon resources for a client. */
static void icon_scaled_cache_clear(Client *c) {
    if (c->icon_scaled_pm)   XFreePixmap(dpy, c->icon_scaled_pm);
    if (c->icon_scaled_mask) XFreePixmap(dpy, c->icon_scaled_mask);
    c->icon_scaled_pm = None;
    c->icon_scaled_mask = None;
    c->icon_scaled_w = 0;
    c->icon_scaled_h = 0;
}

/* Draw an icon pixmap scaled to fit within (dw × dh) at position (dx, dy) on target,
 * preserving aspect ratio and centering. Uses Imlib2 for scaling.
 * The scaled result (and scaled mask) is cached per client — rebuilt only
 * when the icon or target size changes, instead of per icon-bar redraw. */
static void draw_icon_scaled(Client *c, Drawable target, int dx, int dy, int dw, int dh) {
    if (!c || c->icon_pixmap == None || c->icon_w <= 0 || c->icon_h <= 0)
        return;

    /* Scale to fit within (dw, dh), preserving aspect ratio */
    int sw, sh;
    if (c->icon_w * dh > c->icon_h * dw) {
        /* icon is wider than target ratio — width-limited */
        sw = dw;
        sh = (c->icon_h * dw) / c->icon_w;
    } else {
        /* icon is taller than target ratio — height-limited */
        sh = dh;
        sw = (c->icon_w * dh) / c->icon_h;
    }
    if (sw <= 0) sw = 1;
    if (sh <= 0) sh = 1;

    /* Center the scaled icon within the target area */
    int ox = dx + (dw - sw) / 2;
    int oy = dy + (dh - sh) / 2;

    /* rebuild cache if missing, resized, or the source icon changed */
    if (c->icon_scaled_w != sw || c->icon_scaled_h != sh) {
        icon_scaled_cache_clear(c);
    }

    if (c->icon_scaled_pm == None) {
        imlib_context_set_drawable(c->icon_pixmap);
        Imlib_Image img = imlib_create_image_from_drawable(0, 0, 0,
                                                           c->icon_w, c->icon_h, 0);
        if (!img)
            return;

        if (c->icon_mask != None) {
            /* With mask: render onto a temp pixmap filled with the bar
             * background, then cache it with a scaled clip mask. */
            Pixmap tmp = XCreatePixmap(dpy, target, (unsigned)sw, (unsigned)sh, 32);
            {
                XmPlatDrawCtx c = _XmPlatCtx(dpy, tmp, argb_gc);
                _XmPlatSetForeground(c, col_iconbar_bg.pixel);
                _XmPlatFillRect(c, 0, 0, (unsigned)sw, (unsigned)sh);
                _XmPlatCtxFree(c);
            }
            imlib_context_set_image(img);
            {
                Visual *prev_vis = imlib_context_get_visual();
                Colormap prev_cmap = imlib_context_get_colormap();
                imlib_context_set_visual(argb_visual);
                imlib_context_set_colormap(argb_colormap);
                imlib_context_set_drawable(tmp);
                imlib_render_image_on_drawable_at_size(0, 0, sw, sh);
                imlib_context_set_visual(prev_vis);
                imlib_context_set_colormap(prev_cmap);
            }
            imlib_free_image();

            /* Build a scaled 1-bit clip mask: read the original mask once,
             * scale client-side, and write rows with XPutImage (single
             * request instead of per-pixel XDrawPoint). */
            Pixmap scaled_mask = XCreatePixmap(dpy, target, (unsigned)sw, (unsigned)sh, 1);
            XmPlatImage mask_img = _XmPlatImageFromSurface2(dpy, c->icon_mask, 0, 0,
                                                             (unsigned)c->icon_w,
                                                             (unsigned)c->icon_h);
            char *scaled_bits = malloc((size_t)((sw + 7) / 8) * (size_t)sh);
            int have_mask_data = 0;
            if (mask_img && scaled_bits) {
                memset(scaled_bits, 0, (size_t)((sw + 7) / 8) * (size_t)sh);
                /* Sample original mask pixels with nearest-neighbor scaling.
                 * Bitmap bit order for clip masks is LSBFirst per byte. */
                for (int sy = 0; sy < sh; sy++) {
                    for (int sx = 0; sx < sw; sx++) {
                        int src_x = sx * c->icon_w / sw;
                        int src_y = sy * c->icon_h / sh;
                        if (_XmPlatImageGetPixel(mask_img, src_x, src_y)) {
                            scaled_bits[sy * ((sw + 7) / 8) + (sx >> 3)]
                                |= (char)(1 << (sx & 7));
                        }
                    }
                }
                have_mask_data = 1;
                _XmPlatImageFree(mask_img);
            }
            GC mono_gc = XCreateGC(dpy, scaled_mask, 0, NULL);
            /* XYBitmap upload maps 1-bits to the GC foreground — the clip
             * mask needs pixel 1 where the icon is visible */
            XSetForeground(dpy, mono_gc, 1);
            XSetBackground(dpy, mono_gc, 0);
            if (have_mask_data) {
                /* 1-bit LSBFirst XYBitmap token over caller-owned bits;
                 * _XmPlatPutImage uploads it in one request */
                XmPlatImage out = _XmPlatImageCreateBitmap(dpy, scaled_bits,
                                                            (unsigned)sw,
                                                            (unsigned)sh);
                if (out) {
                    XmPlatDrawCtx mc = _XmPlatCtx(dpy, scaled_mask, mono_gc);
                    _XmPlatPutImage(mc, out, 0, 0, 0, 0,
                                    (unsigned)sw, (unsigned)sh);
                    _XmPlatCtxFree(mc);
                    _XmPlatImageFree(out);  /* frees scaled_bits */
                    scaled_bits = NULL;
                }
            } else {
                /* Fallback: no mask data, make everything opaque */
                XmPlatDrawCtx mc = _XmPlatCtx(dpy, scaled_mask, mono_gc);
                _XmPlatSetForeground(mc, 1);
                _XmPlatFillRect(mc, 0, 0, (unsigned)sw, (unsigned)sh);
                _XmPlatCtxFree(mc);
            }
            XFreeGC(dpy, mono_gc);
            free(scaled_bits);  /* no-op when ownership moved to the image token */

            /* cache the temp pixmap (with its clip mask) for future draws */
            c->icon_scaled_pm = tmp;
            c->icon_scaled_mask = scaled_mask;
            c->icon_scaled_w = sw;
            c->icon_scaled_h = sh;
        } else {
            /* No mask: cache the scaled render itself */
            Pixmap cached = XCreatePixmap(dpy, target, (unsigned)sw, (unsigned)sh, 32);
            imlib_context_set_image(img);
            {
                Visual *prev_vis = imlib_context_get_visual();
                Colormap prev_cmap = imlib_context_get_colormap();
                imlib_context_set_visual(argb_visual);
                imlib_context_set_colormap(argb_colormap);
                imlib_context_set_drawable(cached);
                imlib_render_image_on_drawable_at_size(0, 0, sw, sh);
                imlib_context_set_visual(prev_vis);
                imlib_context_set_colormap(prev_cmap);
            }
            imlib_free_image();
            c->icon_scaled_pm = cached;
            c->icon_scaled_mask = None;
            c->icon_scaled_w = sw;
            c->icon_scaled_h = sh;
        }
    }

    /* blit the cached scaled icon to the target.
     * _XmPlatBlitMask is an XCopyPlane of the mask itself (it REPLACES the
     * destination with the mask) — NOT a masked copy of src.  The icon must
     * be blitted with the mask installed as the GC clip, exactly like the
     * original XSetClipMask + XCopyArea sequence. */
    {
        XmPlatDrawCtx cctx = _XmPlatCtx(dpy, target, argb_gc);
        XmPlatSurface src = _XmPlatSurface(dpy, c->icon_scaled_pm);
        if (c->icon_mask != None) {
            _XmPlatSetClipMaskSurf(cctx, _XmPlatSurface(dpy, c->icon_scaled_mask), ox, oy);
            _XmPlatBlit(cctx, src, 0, 0, ox, oy, (unsigned)sw, (unsigned)sh);
            _XmPlatClrClip(dpy, argb_gc);
        } else {
            _XmPlatBlit(cctx, src, 0, 0, ox, oy, (unsigned)sw, (unsigned)sh);
        }
        _XmPlatCtxFree(cctx);
    }
}


/* cached per-client name extents (measured when name/font changes) */
static XGlyphInfo client_name_ext(Client *c) {
    int max_chars = ICON_W / 6;
    int namelen = (int)strlen(c->name);
    if (namelen > max_chars) namelen = max_chars;
    if (c->name_ext_font != xftfont || c->name_ext_len != namelen) {
        XftTextExtents8(dpy, xftfont, (XftChar8 *)c->name,
                        namelen, &c->name_ext);
        c->name_ext_font = xftfont;
        c->name_ext_len = namelen;
    }
    return c->name_ext;
}

/* height of one icon bar entry, varies by display mode */
int icon_entry_h(void) {
    if (!xftfont) return ICON_H;  /* safety: return default before font is loaded */
    int text_h = xftfont->ascent + xftfont->descent;
    switch (icon_mode) {
    case ICON_MODE_ICON:      return ICON_H;
    case ICON_MODE_TEXT:      return text_h + 8;
    case ICON_MODE_ICON_TEXT: return ICON_H + text_h + 4;
    }
    return ICON_H;
}

int iconbar_visible(void) {
    for (Client *c = clients; c; c = c->next)
        if (c->ws == curws && c->is_minimized) return 1;
    return 0;
}

/* count minimized windows on current workspace */
int minimized_count(void) {
    int n = 0;
    for (Client *c = clients; c; c = c->next)
        if (c->ws == curws && c->is_minimized) n++;
    return n;
}

void drawiconbar(void) {
    if (!iconbar_visible()) return;
    if (is_vertical(iconbar_position))
        drawiconbar_vertical();
    else
        drawiconbar_horizontal();
}

/* Ensure the icon bar back-buffer pixmap matches the current size.
 * Returns the XftDraw for the back-buffer (creating/recreating as needed).
 * Sizes are tracked locally — no XGetGeometry round trip per draw. */
static int iconbar_buf_w = 0, iconbar_buf_h = 0;
static XftDraw *iconbar_ensure_buf(int w, int h) {
    if (iconbar_buf != None && iconbar_buf_draw) {
        if (iconbar_buf_w == w && iconbar_buf_h == h)
            return iconbar_buf_draw;
        /* size changed — recreate */
        XftDrawDestroy(iconbar_buf_draw);
        XFreePixmap(dpy, iconbar_buf);
        iconbar_buf_draw = NULL;
        iconbar_buf = None;
    }
    iconbar_buf = XCreatePixmap(dpy, iconbar, (unsigned)w, (unsigned)h, 32);
    iconbar_buf_draw = XftDrawCreate(dpy, iconbar_buf, argb_visual, argb_colormap);
    iconbar_buf_w = w;
    iconbar_buf_h = h;
    return iconbar_buf_draw;
}

/* Vertical icon bar (left or right edge) — icons stacked top-to-bottom */
static void drawiconbar_vertical(void) {
    int entry_h = icon_entry_h();
    BarGeometry g = calc_bar_geometry();

    /* use back-buffer to avoid flicker */
    XftDraw *xd = iconbar_ensure_buf(g.ibar_w, g.ibar_h);
    Drawable buf = iconbar_buf;

    /* clear the entire icon bar before redrawing */
    XftDrawRect(xd, &col_iconbar_bg, 0, 0, g.ibar_w, g.ibar_h);

    /* calculate total icon content height */
    int n = minimized_count();
    int total_h = n * entry_h;

    /* view area: skip region overlapped by the status bar.
     * Bars on different orientations overlap each other (e.g., horizontal status bar
     * at top overlaps the top of a vertical icon bar on the left).
     * Bars on the same side don't overlap — calc_bar_geometry() already positions
     * them adjacent. Bars on the same orientation but different sides don't overlap. */
    int bar_thick = show_bar ? bar_effective_thickness() : 0;
    int arrow_top = 0;
    int arrow_bot = g.ibar_h;
    if (show_bar && is_horizontal(bar_position)) {
        /* status bar is horizontal — it overlaps the top or bottom of the icon bar */
        if (bar_position == BAR_POS_TOP)
            arrow_top = bar_thick;
        else
            arrow_bot -= bar_thick;
    }
    /* If both bars are vertical on the same side, calc_bar_geometry() already
     * adjusted the icon bar position — no additional offset needed. */

    /* scroll arrows — compact in scroll direction, full width in the other */
    const int arrow_h = ICON_W * 3 / 5;
    int view_top = arrow_top + ICON_PAD;
    int view_bot = arrow_bot - ICON_PAD;
    int view_h = view_bot - view_top;

    /* clamp scroll offset */
    int max_scroll = total_h - view_h;
    if (max_scroll < 0) max_scroll = 0;
    if (iconbar_scroll > max_scroll) iconbar_scroll = max_scroll;
    if (iconbar_scroll < 0) iconbar_scroll = 0;

    /* only reserve space for arrows that are actually needed */
    int need_up = (iconbar_scroll > 0);
    int need_down = (iconbar_scroll < max_scroll);
    int content_top = need_up   ? arrow_top + arrow_h : arrow_top;
    int content_bot = need_down ? arrow_bot - arrow_h : arrow_bot;

    /* draw icons first (before arrows, so arrow buttons paint over any
     * Imlib2 icon overflow that bypasses the Xft clip region) */
    const int x = 0;
    int icon_y = content_top - iconbar_scroll;
    XRectangle clip_rect = { .x = 0, .y = content_top, .width = ICON_W, .height = content_bot - content_top };
    int text_h = xftfont->ascent + xftfont->descent;

    XftDrawSetClipRectangles(xd, 0, 0, &clip_rect, 1);
    for (Client *c = clients; c; c = c->next) {
        if (c->ws != curws || !c->is_minimized) continue;
        int entry_bottom = icon_y + entry_h;
        if (entry_bottom < content_top) { icon_y += entry_h; continue; }
        if (icon_y > content_bot) break;

        XftColor *fill = (c == focused) ? &col_title_focus : &col_frame_bg;
        XftDrawRect(xd, fill, x + 2, icon_y + 2, ICON_W - 4, entry_h - 4);
        bevel_rect(xd, x, icon_y, ICON_W, entry_h,
                   2, 2, 2, 2, &col_frame_light, &col_frame_shadow);

        int max_chars = ICON_W / 6;
        int namelen = (int)strlen(c->name);
        if (namelen > max_chars) namelen = max_chars;

        switch (icon_mode) {
        case ICON_MODE_ICON:
            draw_icon_scaled(c, buf, x + 4, icon_y + 4, ICON_W - 8, ICON_H - 8);
            break;
        case ICON_MODE_TEXT:
            if (namelen > 0) {
                XGlyphInfo ext = client_name_ext(c);
                int text_x = x + (ICON_W - ext.xOff) / 2;
                int text_y = icon_y + (entry_h + text_h) / 2 - xftfont->descent;
                XftDrawStringUtf8(xd, &col_title_fg, xftfont,
                                  text_x, text_y,
                                  (XftChar8 *)c->name, namelen);
            }
            break;
        case ICON_MODE_ICON_TEXT:
            draw_icon_scaled(c, buf, x + 4, icon_y + 4, ICON_W - 8, ICON_H - 8);
            if (namelen > 0) {
                XGlyphInfo ext = client_name_ext(c);
                int text_x = x + (ICON_W - ext.xOff) / 2;
                int text_y = icon_y + ICON_H + 4 + xftfont->ascent;
                XftDrawStringUtf8(xd, &col_title_fg, xftfont,
                                  text_x, text_y,
                                  (XftChar8 *)c->name, namelen);
            }
            break;
        }

        icon_y += entry_h;
    }
    XftDrawSetClipRectangles(xd, 0, 0,
        &(XRectangle){ .x = 0, .y = 0, .width = (unsigned short)g.ibar_w, .height = (unsigned short)g.ibar_h }, 1);

    /* draw arrow buttons on top of icons so they cover any Imlib2 overflow */
    if (need_up) {
        int ay = arrow_top;
        XftDrawRect(xd, &col_iconbar_bg, 0, ay, ICON_W, arrow_h);
        XftDrawRect(xd, &col_frame_bg, 2, ay + 2, ICON_W - 4, arrow_h - 4);
        bevel_rect(xd, 0, ay, ICON_W, arrow_h,
                   2, 2, 2, 2, &col_frame_light, &col_frame_shadow);
        int cx = ICON_W / 2;
        int my = ay + arrow_h / 2;
        int sz = 6;
        XftDrawRect(xd, &col_btn_fg, cx - sz, my + sz/2, sz * 2 + 1, 1);
        for (int i = 1; i <= sz; i++)
            XftDrawRect(xd, &col_btn_fg, cx - sz + i, my + sz/2 - i, (sz - i) * 2 + 1, 1);
    }
    if (need_down) {
        int ay = arrow_bot - arrow_h;
        XftDrawRect(xd, &col_iconbar_bg, 0, ay, ICON_W, arrow_h);
        XftDrawRect(xd, &col_frame_bg, 2, ay + 2, ICON_W - 4, arrow_h - 4);
        bevel_rect(xd, 0, ay, ICON_W, arrow_h,
                   2, 2, 2, 2, &col_frame_light, &col_frame_shadow);
        int cx = ICON_W / 2;
        int my = ay + arrow_h / 2;
        int sz = 6;
        XftDrawRect(xd, &col_btn_fg, cx - sz, my - sz/2, sz * 2 + 1, 1);
        for (int i = 1; i <= sz; i++)
            XftDrawRect(xd, &col_btn_fg, cx - sz + i, my - sz/2 + i, (sz - i) * 2 + 1, 1);
    }

    /* copy back-buffer to window in one shot */
    {
        XmPlatDrawCtx cctx = _XmPlatCtx(dpy, iconbar, argb_gc);
        _XmPlatBlit(cctx, _XmPlatSurface(dpy, buf), 0, 0, 0, 0,
                    (unsigned)g.ibar_w, (unsigned)g.ibar_h);
        _XmPlatCtxFree(cctx);
    }
    XFlush(dpy);
}

/* Horizontal icon bar (top or bottom edge) — icons laid out left-to-right */
static void drawiconbar_horizontal(void) {
    int entry_h = icon_entry_h();
    BarGeometry g = calc_bar_geometry();

    /* use back-buffer to avoid flicker */
    XftDraw *xd = iconbar_ensure_buf(g.ibar_w, g.ibar_h);
    Drawable buf = iconbar_buf;

    /* clear the entire icon bar */
    XftDrawRect(xd, &col_iconbar_bg, 0, 0, g.ibar_w, g.ibar_h);

    int n = minimized_count();
    int total_w = n * ICON_W;

    /* view area: skip region overlapped by status bar if on same side */
    int bar_thick = show_bar ? bar_effective_thickness() : 0;
    int arrow_left = 0;
    int arrow_right = g.ibar_w;
    if (show_bar && is_vertical(bar_position)) {
        /* status bar is vertical — it overlaps the left or right of the icon bar */
        if (bar_position == BAR_POS_LEFT)
            arrow_left = bar_thick;
        else
            arrow_right -= bar_thick;
    }
    /* If both bars are horizontal on the same side, calc_bar_geometry() already
     * adjusted the icon bar position — no additional offset needed. */

    /* scroll arrows — compact in scroll direction, full height in the other */
    const int arrow_w = entry_h * 3 / 5;
    int view_left = arrow_left + ICON_PAD;
    int view_right = arrow_right - ICON_PAD;
    int view_w = view_right - view_left;

    /* clamp scroll offset */
    int max_scroll = total_w - view_w;
    if (max_scroll < 0) max_scroll = 0;
    if (iconbar_scroll > max_scroll) iconbar_scroll = max_scroll;
    if (iconbar_scroll < 0) iconbar_scroll = 0;

    /* only reserve space for arrows that are actually needed */
    int need_left = (iconbar_scroll > 0);
    int need_right = (iconbar_scroll < max_scroll);
    int content_left = need_left  ? arrow_left + arrow_w : arrow_left;
    int content_right = need_right ? arrow_right - arrow_w : arrow_right;

    /* draw icons first (before arrows, so arrow buttons paint over any
     * Imlib2 icon overflow that bypasses the Xft clip region) */
    int icon_x = content_left - iconbar_scroll;
    XRectangle clip_rect = { .x = content_left, .y = 0, .width = content_right - content_left, .height = entry_h };
    int text_h = xftfont->ascent + xftfont->descent;

    XftDrawSetClipRectangles(xd, 0, 0, &clip_rect, 1);
    for (Client *c = clients; c; c = c->next) {
        if (c->ws != curws || !c->is_minimized) continue;
        int entry_right = icon_x + ICON_W;
        if (entry_right < content_left) { icon_x += ICON_W; continue; }
        if (icon_x > content_right) break;

        XftColor *fill = (c == focused) ? &col_title_focus : &col_frame_bg;
        XftDrawRect(xd, fill, icon_x + 2, 2, ICON_W - 4, entry_h - 4);
        bevel_rect(xd, icon_x, 0, ICON_W, entry_h,
                   2, 2, 2, 2, &col_frame_light, &col_frame_shadow);

        int max_chars = ICON_W / 6;
        int namelen = (int)strlen(c->name);
        if (namelen > max_chars) namelen = max_chars;

        switch (icon_mode) {
        case ICON_MODE_ICON:
            draw_icon_scaled(c, buf, icon_x + 4, 4, ICON_W - 8, ICON_H - 8);
            break;
        case ICON_MODE_TEXT:
            if (namelen > 0) {
                XGlyphInfo ext = client_name_ext(c);
                int text_x = icon_x + (ICON_W - ext.xOff) / 2;
                int text_y = (entry_h + text_h) / 2 - xftfont->descent;
                XftDrawStringUtf8(xd, &col_title_fg, xftfont,
                                  text_x, text_y,
                                  (XftChar8 *)c->name, namelen);
            }
            break;
        case ICON_MODE_ICON_TEXT:
            draw_icon_scaled(c, buf, icon_x + 4, 4, ICON_W - 8, ICON_H - 8);
            if (namelen > 0) {
                XGlyphInfo ext = client_name_ext(c);
                int text_x = icon_x + (ICON_W - ext.xOff) / 2;
                int text_y = entry_h - xftfont->descent - 2;
                XftDrawStringUtf8(xd, &col_title_fg, xftfont,
                                  text_x, text_y,
                                  (XftChar8 *)c->name, namelen);
            }
            break;
        }

        icon_x += ICON_W;
    }
    XftDrawSetClipRectangles(xd, 0, 0,
        &(XRectangle){ .x = 0, .y = 0, .width = (unsigned short)g.ibar_w, .height = (unsigned short)g.ibar_h }, 1);

    /* draw arrow buttons on top of icons so they cover any Imlib2 overflow */
    if (need_left) {
        int ax = arrow_left;
        XftDrawRect(xd, &col_iconbar_bg, ax, 0, arrow_w, entry_h);
        XftDrawRect(xd, &col_frame_bg, ax + 2, 2, arrow_w - 4, entry_h - 4);
        bevel_rect(xd, ax, 0, arrow_w, entry_h,
                   2, 2, 2, 2, &col_frame_light, &col_frame_shadow);
        int cy = entry_h / 2;
        int mx = ax + arrow_w / 2;
        int sz = 6;
        XftDrawRect(xd, &col_btn_fg, mx + sz/2, cy - sz, 1, sz * 2 + 1);
        for (int i = 1; i <= sz; i++)
            XftDrawRect(xd, &col_btn_fg, mx + sz/2 - i, cy - sz + i, 1, (sz - i) * 2 + 1);
    }
    if (need_right) {
        int ax = arrow_right - arrow_w;
        XftDrawRect(xd, &col_iconbar_bg, ax, 0, arrow_w, entry_h);
        XftDrawRect(xd, &col_frame_bg, ax + 2, 2, arrow_w - 4, entry_h - 4);
        bevel_rect(xd, ax, 0, arrow_w, entry_h,
                   2, 2, 2, 2, &col_frame_light, &col_frame_shadow);
        int cy = entry_h / 2;
        int mx = ax + arrow_w / 2;
        int sz = 6;
        XftDrawRect(xd, &col_btn_fg, mx - sz/2, cy - sz, 1, sz * 2 + 1);
        for (int i = 1; i <= sz; i++)
            XftDrawRect(xd, &col_btn_fg, mx - sz/2 + i, cy - sz + i, 1, (sz - i) * 2 + 1);
    }

    /* copy back-buffer to window in one shot */
    {
        XmPlatDrawCtx cctx = _XmPlatCtx(dpy, iconbar, argb_gc);
        _XmPlatBlit(cctx, _XmPlatSurface(dpy, buf), 0, 0, 0, 0,
                    (unsigned)g.ibar_w, (unsigned)g.ibar_h);
        _XmPlatCtxFree(cctx);
    }
    XFlush(dpy);
}

/* Keep the icon bar (and status bar) above all client windows.
 * Called after every XRaiseWindow on a client frame so minimized-window
 * icons stay visible even when the focused window overlaps them.
 * barwin goes above iconbar so it covers the overlapping corner. */
void wm_restack_bars(void) {
    if (iconbar_visible())
        XRaiseWindow(dpy, iconbar);
    if (show_bar)
        XRaiseWindow(dpy, barwin);
}

void updateiconbar(void) {
    if (iconbar_visible()) {
        BarGeometry g = calc_bar_geometry();
        XMoveResizeWindow(dpy, iconbar, g.ibar_x, g.ibar_y,
                          g.ibar_w > 0 ? g.ibar_w : 1,
                          g.ibar_h > 0 ? g.ibar_h : 1);
        XMapWindow(dpy, iconbar);
        /* raise status bar above icon bar so it covers the overlapping corner */
        if (show_bar)
            XRaiseWindow(dpy, barwin);
        XFlush(dpy);
        drawiconbar();
    } else {
        XUnmapWindow(dpy, iconbar);
    }
    /* NOTE: arrange()/drawbar() are not called here — callers
     * (defer_flush, workspace switches) own that pass. */
}

void handle_iconbar_click(int x, int y) {
    int bar_thick = show_bar ? bar_effective_thickness() : 0;
    if (is_vertical(iconbar_position)) {
        /* vertical icon bar: Y-based click testing */
        int entry_h = icon_entry_h();
        int n = minimized_count();
        int total_h = n * entry_h;
        BarGeometry g = calc_bar_geometry();
        int arrow_top = 0, arrow_bot = g.ibar_h;
        if (show_bar && is_horizontal(bar_position)) {
            if (bar_position == BAR_POS_TOP) arrow_top = bar_thick;
            else arrow_bot -= bar_thick;
        }
        int view_top = arrow_top + ICON_PAD;
        int view_bot = arrow_bot - ICON_PAD;
        int view_h = view_bot - view_top;
        const int arrow_h = ICON_W * 3 / 5;

        /* clamp scroll offset */
        int max_scroll = total_h - view_h;
        if (max_scroll < 0) max_scroll = 0;
        if (iconbar_scroll > max_scroll) iconbar_scroll = max_scroll;

        int need_up = (iconbar_scroll > 0);
        int need_down = (iconbar_scroll < max_scroll);
        int content_top = need_up ? arrow_top + arrow_h : arrow_top;

        /* up arrow */
        if (need_up && y >= arrow_top && y < arrow_top + arrow_h) {
            iconbar_scroll_by(-(entry_h));
            return;
        }
        /* down arrow */
        if (need_down && y >= arrow_bot - arrow_h && y < arrow_bot) {
            iconbar_scroll_by(entry_h);
            return;
        }
        int icon_y = content_top - iconbar_scroll;
        for (Client *c = clients; c; c = c->next) {
            if (c->ws != curws || !c->is_minimized) continue;
            if (y >= icon_y && y < icon_y + entry_h) {
                restore_client(c);
                return;
            }
            icon_y += entry_h;
        }
    } else {
        /* horizontal icon bar: X-based click testing */
        int n = minimized_count();
        int total_w = n * ICON_W;
        BarGeometry g = calc_bar_geometry();
        int arrow_left = 0, arrow_right = g.ibar_w;
        if (show_bar && is_vertical(bar_position)) {
            if (bar_position == BAR_POS_LEFT) arrow_left = bar_thick;
            else arrow_right -= bar_thick;
        }
        int view_left = arrow_left + ICON_PAD;
        int view_right = arrow_right - ICON_PAD;
        int view_w = view_right - view_left;
        int entry_h = icon_entry_h();
        const int arrow_w = entry_h * 3 / 5;

        /* clamp scroll offset */
        int max_scroll = total_w - view_w;
        if (max_scroll < 0) max_scroll = 0;
        if (iconbar_scroll > max_scroll) iconbar_scroll = max_scroll;

        int need_left = (iconbar_scroll > 0);
        int need_right = (iconbar_scroll < max_scroll);
        int content_left = need_left ? arrow_left + arrow_w : arrow_left;

        /* left arrow */
        if (need_left && x >= arrow_left && x < arrow_left + arrow_w) {
            iconbar_scroll_by(-(ICON_W));
            return;
        }
        /* right arrow */
        if (need_right && x >= arrow_right - arrow_w && x < arrow_right) {
            iconbar_scroll_by(ICON_W);
            return;
        }
        int icon_x = content_left - iconbar_scroll;
        for (Client *c = clients; c; c = c->next) {
            if (c->ws != curws || !c->is_minimized) continue;
            if (x >= icon_x && x < icon_x + ICON_W) {
                restore_client(c);
                return;
            }
            icon_x += ICON_W;
        }
    }
}

void iconbar_scroll_by(int delta) {
    if (is_vertical(iconbar_position)) {
        /* vertical: scroll by entry height */
        int entry_h = icon_entry_h();
        int n = minimized_count();
        int total_h = n * entry_h;
        BarGeometry g = calc_bar_geometry();
        int bar_thick = show_bar ? bar_effective_thickness() : 0;
        int arrow_top = 0, arrow_bot = g.ibar_h;
        if (show_bar && is_horizontal(bar_position)) {
            if (bar_position == BAR_POS_TOP) arrow_top = bar_thick;
            else arrow_bot -= bar_thick;
        }
        int arrow_h = ICON_W * 3 / 5;
        /* use full content area (both arrows) for clamping —
         * drawiconbar will adapt arrow visibility dynamically */
        int content_top = arrow_top + arrow_h;
        int content_bot = arrow_bot - arrow_h;
        int content_h = content_bot - content_top;
        int max_scroll = total_h - content_h;
        if (max_scroll < 0) max_scroll = 0;

        iconbar_scroll += delta;
        if (iconbar_scroll > max_scroll) iconbar_scroll = max_scroll;
        if (iconbar_scroll < 0) iconbar_scroll = 0;
        drawiconbar();
    } else {
        /* horizontal: scroll by ICON_W */
        int n = minimized_count();
        int total_w = n * ICON_W;
        BarGeometry g = calc_bar_geometry();
        int bar_thick = show_bar ? bar_effective_thickness() : 0;
        int arrow_left = 0, arrow_right = g.ibar_w;
        if (show_bar && is_vertical(bar_position)) {
            if (bar_position == BAR_POS_LEFT) arrow_left = bar_thick;
            else arrow_right -= bar_thick;
        }
        int arrow_w = icon_entry_h() * 3 / 5;
        int content_left = arrow_left + arrow_w;
        int content_right = arrow_right - arrow_w;
        int content_w = content_right - content_left;
        int max_scroll = total_w - content_w;
        if (max_scroll < 0) max_scroll = 0;

        iconbar_scroll += delta;
        if (iconbar_scroll > max_scroll) iconbar_scroll = max_scroll;
        if (iconbar_scroll < 0) iconbar_scroll = 0;
        drawiconbar();
    }
}

/* ── icon tooltip ──────────────────────────────────────────────────────── */

/* Find the minimized client at iconbar position (x, y).
 * Returns NULL if no icon is at that position. */
static Client *iconbar_client_at(int x, int y) {
    int bar_thick = show_bar ? bar_effective_thickness() : 0;
    BarGeometry g = calc_bar_geometry();

    if (is_vertical(iconbar_position)) {
        int entry_h = icon_entry_h();
        int arrow_top = 0, arrow_bot = g.ibar_h;
        if (show_bar && is_horizontal(bar_position)) {
            if (bar_position == BAR_POS_TOP) arrow_top = bar_thick;
            else arrow_bot -= bar_thick;
        }
        int need_up = (iconbar_scroll > 0);
        int arrow_h = ICON_W * 3 / 5;
        int content_top = need_up ? arrow_top + arrow_h : arrow_top;
        int icon_y = content_top - iconbar_scroll;
        for (Client *c = clients; c; c = c->next) {
            if (c->ws != curws || !c->is_minimized) continue;
            if (y >= icon_y && y < icon_y + entry_h)
                return c;
            icon_y += entry_h;
        }
    } else {
        int arrow_left = 0, arrow_right = g.ibar_w;
        if (show_bar && is_vertical(bar_position)) {
            if (bar_position == BAR_POS_LEFT) arrow_left = bar_thick;
            else arrow_right -= bar_thick;
        }
        int need_left = (iconbar_scroll > 0);
        int entry_h = icon_entry_h();
        int arrow_w = entry_h * 3 / 5;
        int content_left = need_left ? arrow_left + arrow_w : arrow_left;
        int icon_x = content_left - iconbar_scroll;
        for (Client *c = clients; c; c = c->next) {
            if (c->ws != curws || !c->is_minimized) continue;
            if (x >= icon_x && x < icon_x + ICON_W)
                return c;
            icon_x += ICON_W;
        }
    }
    return NULL;
}

static XtIntervalId tooltip_timer = 0;
char tooltip_text[256];
int tooltip_text_len = 0;

/* tooltip visibility flag — shared between render/hide to skip redundant
 * unmap requests on every motion event */
int tooltip_shown = 0;

static void tooltip_render(void) {
    if (tooltip_text_len <= 0) return;
    XGlyphInfo ext;
    XftTextExtents8(dpy, tooltip_font, (XftChar8 *)tooltip_text, tooltip_text_len, &ext);
    int pad = 6;
    int tw = ext.xOff + pad * 2;
    int th = tooltip_font->ascent + tooltip_font->descent + pad * 2;

    /* position at cursor, offset slightly down-right */
    int rx, ry, ix, iy;
    Window child;
    unsigned int mask;
    XQueryPointer(dpy, iconbar, &child, &child, &rx, &ry, &ix, &iy, &mask);
    int tx = rx + 8;
    int ty = ry + 8;
    if (tx + tw > mon.x + mon.w) tx = rx - tw - 4;
    if (ty + th > mon.y + mon.h) ty = ry - th - 4;
    if (tx < 0) tx = 0;
    if (ty < 0) ty = 0;

    XMoveResizeWindow(dpy, tooltip_win, tx, ty,
                      (unsigned)(tw > 1 ? tw : 1),
                      (unsigned)(th > 1 ? th : 1));
    XMapRaised(dpy, tooltip_win);
    tooltip_shown = 1;
}

static void tooltip_timer_cb(XtPointer data, XtIntervalId *id) {
    (void)data; (void)id;
    tooltip_timer = 0;
    Window child;
    int ix, iy;
    unsigned int mask;
    XQueryPointer(dpy, iconbar, &child, &child, &(int){0}, &(int){0}, &ix, &iy, &mask);
    Client *c = iconbar_client_at(ix, iy);
    if (!c || !c->name[0]) return;

    tooltip_text_len = snprintf(tooltip_text, sizeof(tooltip_text), "%s", c->name);
    tooltip_render();
}

void show_icon_tooltip(int x, int y) {
    (void)x; (void)y;
    hide_icon_tooltip();
    if (tooltip_delay <= 0) {
        tooltip_timer_cb(NULL, NULL);
    } else {
        tooltip_timer = XtAppAddTimeOut(app, (unsigned long)tooltip_delay,
                                        tooltip_timer_cb, NULL);
    }
}

void hide_icon_tooltip(void) {
    if (tooltip_timer) {
        XtRemoveTimeOut(tooltip_timer);
        tooltip_timer = 0;
    }
    /* skip the unmap request when already hidden — this runs on every
     * motion event over the iconbar */
    if (tooltip_shown) {
        XUnmapWindow(dpy, tooltip_win);
        tooltip_shown = 0;
    }
}