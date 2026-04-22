/*
 * rondomgr — Motif-based GUI configurator for the rondo window manager
 *
 * Reads and writes ~/.rondorc (or ~/.config/rondo/config.scm),
 * then sends commands to rondo via Unix domain socket IPC.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/RowColumn.h>
#include <Xm/Label.h>
#include <Xm/Text.h>
#include <Xm/PushB.h>
#include <Xm/PushBG.h>
#include <Xm/ToggleB.h>
#include <Xm/Scale.h>
#include <Xm/Separator.h>
#include <Xm/List.h>
#include <Xm/SelectioB.h>
#include <Xm/MessageB.h>
#include <Xm/Frame.h>
#include <Xm/FileSB.h>
#include <Xm/ScrolledW.h>
#include <X11/IntrinsicP.h>   /* for CompositeWidget internal access */
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>

/* copy src into fixed-size dst, truncating silently */
#define COPY_TRUNK(dst, src) do { \
    size_t _n = sizeof(dst) - 1; \
    size_t _sl = strlen(src); \
    memcpy((dst), (src), _sl < _n ? _sl : _n); \
    (dst)[_sl < _n ? _sl : _n] = '\0'; \
} while (0)

/* ═══════════════════════════════════════════════════════════════════════
 *  Config state — mirrors rondo's config variables
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    /* dimensions */
    int frame_width, title_height, bar_height, btn_width, btn_height;
    int bar_border_width, bar_corner_size, bar_btn_width;
    int icon_width, icon_height, icon_padding;
    int num_workspaces;
    float master_ratio;
    int show_bar;
    /* compositing */
    int fade_enabled, fade_in_ms, fade_out_ms, tooltip_delay;
    /* programs */
    char font[256], terminal[256], launcher[256], modkey[64], clock_format[256];
    char tooltip_font[256];
    /* bar position / icon mode */
    int bar_position;   /* 0=top,1=bottom,2=left,3=right */
    int iconbar_position;
    int icon_mode;      /* 0=icon,1=text,2=icon-text */
    /* background */
    int bg_mode;        /* 0=solid,1=pattern,2=image */
    int bg_pattern;     /* 0=checkerboard,1=diagonal,2=horizontal,3=vertical,4=dots,5=crosshatch,6=weave */
    int bg_image_mode;  /* 0=centered,1=scaled,2=tiled,3=stretched,4=scale-filled */
    int bg_pattern_size;
    char bg_color[64], bg_color2[64], bg_image_path[512];
    /* colors */
    char col_title_focus[64], col_title_unfocus[64], col_title_fg[64];
    char col_frame_light[64], col_frame_shadow[64], col_frame_bg[64];
    char col_active_light[64], col_active_shadow[64], col_btn_fg[64];
    char col_bar_bg[64], col_bar_fg[64];
    char col_bar_ws_active[64], col_bar_ws_occupied[64], col_bar_ws_idle[64], col_bar_ws_bg[64];
    char col_bar_border_light[64], col_bar_border_shadow[64], col_bar_fill[64];
    char col_menu_bg[64];
    char col_iconbar_bg[64];
    char col_fb_bg[64], col_fb_light[64], col_fb_shadow[64], col_fb_fg[64];
    char col_tooltip_bg[64], col_tooltip_fg[64], col_tooltip_border[64];
    char col_dialog_bg[64];
} CfgState;

static CfgState cfg;
static CfgState saved_cfg;  /* snapshot after loading file */

/* ── Palette system ───────────────────────────────────────────────────── */

#define PALETTE_MAX_COLORS 30
#define MAX_PALETTES 64
#define MAX_PALETTE_NAME 64

enum {
    PK_TITLE_FOCUS, PK_TITLE_UNFOCUS, PK_TITLE_FG,
    PK_FRAME_LIGHT, PK_FRAME_SHADOW, PK_FRAME_BG,
    PK_ACTIVE_LIGHT, PK_ACTIVE_SHADOW, PK_BTN_FG,
    PK_BAR_BG, PK_BAR_FG,
    PK_BAR_WS_ACTIVE, PK_BAR_WS_OCCUPIED, PK_BAR_WS_IDLE, PK_BAR_WS_BG,
    PK_BAR_BORDER_LIGHT, PK_BAR_BORDER_SHADOW, PK_BAR_FILL,
    PK_MENU_BG,
    PK_ICONBAR_BG,
    PK_FB_BG, PK_FB_LIGHT, PK_FB_SHADOW, PK_FB_FG,
    PK_TOOLTIP_BG, PK_TOOLTIP_FG, PK_TOOLTIP_BORDER,
    PK_DIALOG_BG,
    PK_ROOT_BG, PK_ROOT_BG2,
    PK_COUNT
};

typedef struct {
    char name[MAX_PALETTE_NAME];
    int builtin;  /* 1 = built-in, cannot be deleted */
    char colors[PK_COUNT][64];
} Palette;

static Palette palettes[MAX_PALETTES];
static int num_palettes = 0;
static int current_palette_idx = 0;

static const char *palette_keys[PK_COUNT] = {
    "title-focus", "title-unfocus", "title-fg",
    "frame-light", "frame-shadow", "frame-bg",
    "active-light", "active-shadow", "btn-fg",
    "bar-bg", "bar-fg",
    "bar-ws-active", "bar-ws-occupied", "bar-ws-idle", "bar-ws-bg",
    "bar-border-light", "bar-border-shadow", "bar-fill",
    "menu-bg",
    "iconbar-bg",
    "fb-bg", "fb-light", "fb-shadow", "fb-fg",
    "tooltip-bg", "tooltip-fg", "tooltip-border",
    "dialog-bg",
    "root-bg", "root-bg2"
};

static Widget w_palette_menu = NULL;
static Widget w_palette_delete_btn = NULL;

/* bar layout */
typedef struct { int type; int align; } BarWidgetEntry;
#define BW_WS 0
#define BW_TITLE 1
#define BW_CLOCK 2
#define BW_LOAD 3
#define BW_MEM 4
#define BW_DISK 5
#define BW_BAT 6
#define BW_VOL 7
#define BW_CPU 8
#define BW_NET 9
#define BW_TEMP 10

static BarWidgetEntry bar_layout[32];
static int num_bar_widgets = 0;

/* key bindings */
typedef struct { char mod[64]; char key[64]; char action[64]; char arg[128]; } BindEntry;
static BindEntry binds[128];
static int num_binds = 0;

static void set_defaults(void) {
    cfg.frame_width = 6; cfg.title_height = 18; cfg.bar_height = 26;
    cfg.btn_width = 18; cfg.btn_height = 18;
    cfg.bar_border_width = 6; cfg.bar_corner_size = 24; cfg.bar_btn_width = 18;
    cfg.icon_width = 56; cfg.icon_height = 56; cfg.icon_padding = 4;
    cfg.num_workspaces = 9; cfg.master_ratio = 0.55f; cfg.show_bar = 1;
    cfg.fade_enabled = 1; cfg.fade_in_ms = 150; cfg.fade_out_ms = 150;
    cfg.tooltip_delay = 3000;
    strcpy(cfg.font, "monospace:size=10");
    strcpy(cfg.terminal, "xterm");
    strcpy(cfg.launcher, "dmenu_run");
    strcpy(cfg.modkey, "Alt");
    strcpy(cfg.clock_format, " %a %m/%d %H:%M ");
    cfg.tooltip_font[0] = '\0';
    cfg.bar_position = 0; cfg.iconbar_position = 2; cfg.icon_mode = 2;
    cfg.bg_mode = 1; cfg.bg_pattern = 0; cfg.bg_image_mode = 1;
    cfg.bg_pattern_size = 0;
    strcpy(cfg.bg_color, "#303030"); strcpy(cfg.bg_color2, "#404040");
    cfg.bg_image_path[0] = '\0';
    strcpy(cfg.col_title_focus, "#5F9EA0");
    strcpy(cfg.col_title_unfocus, "#A8A8A8");
    strcpy(cfg.col_title_fg, "#FFFFFF");
    strcpy(cfg.col_frame_light, "#D9D9D9");
    strcpy(cfg.col_frame_shadow, "#595959");
    strcpy(cfg.col_frame_bg, "#A8A8A8");
    strcpy(cfg.col_active_light, "#B7D4D5");
    strcpy(cfg.col_active_shadow, "#2F4F50");
    strcpy(cfg.col_btn_fg, "#000000");
    strcpy(cfg.col_bar_bg, "#A8A8A8");
    strcpy(cfg.col_bar_fg, "#FFFFFF");
    strcpy(cfg.col_bar_ws_active, "#FFFFFF");
    strcpy(cfg.col_bar_ws_occupied, "#303030");
    strcpy(cfg.col_bar_ws_idle, "#606060");
    strcpy(cfg.col_bar_ws_bg, "#5F9EA0");
    strcpy(cfg.col_bar_border_light, "#B7D4D5");
    strcpy(cfg.col_bar_border_shadow, "#2F4F50");
    strcpy(cfg.col_bar_fill, "#5F9EA0");
    strcpy(cfg.col_menu_bg, "#D4D4D4");
    strcpy(cfg.col_iconbar_bg, "#D4D4D4");
    strcpy(cfg.col_fb_bg, "#5F9EA0");
    strcpy(cfg.col_fb_light, "#B7D4D5");
    strcpy(cfg.col_fb_shadow, "#2F4F50");
    strcpy(cfg.col_fb_fg, "#FFFFFF");
    strcpy(cfg.col_tooltip_bg, "#5F9EA0");
    strcpy(cfg.col_tooltip_fg, "#FFFFFF");
    strcpy(cfg.col_tooltip_border, "#2F4F50");
    strcpy(cfg.col_dialog_bg, "#c4c4c4");
    /* default bar layout */
    BarWidgetEntry def_layout[] = {
        {BW_WS,0},{BW_TITLE,1},{BW_BAT,1},{BW_VOL,1},{BW_CPU,1},
        {BW_NET,1},{BW_TEMP,1},{BW_LOAD,1},{BW_MEM,1},{BW_DISK,1},{BW_CLOCK,1}
    };
    num_bar_widgets = 11;
    memcpy(bar_layout, def_layout, sizeof(def_layout));
    /* default key bindings */
    static const struct { const char *mod; const char *key; const char *action; const char *arg; } def_binds[] = {
        {"Alt","Return","spawn",""},
        {"Alt","p","spawn","dmenu_run"},
        {"Alt","j","focusstack","+1"},
        {"Alt","k","focusstack","-1"},
        {"Alt","h","incmaster","-0.05"},
        {"Alt","l","incmaster","+0.05"},
        {"Alt+Shift","Return","zoom",""},
        {"Alt","f","togglefullscreen",""},
        {"Alt","space","togglefloat",""},
        {"Alt+Shift","c","killclient",""},
        {"Alt+Shift","q","quit",""},
        {"Alt","1","viewworkspace","1"},
        {"Alt","2","viewworkspace","2"},
        {"Alt","3","viewworkspace","3"},
        {"Alt","4","viewworkspace","4"},
        {"Alt","5","viewworkspace","5"},
        {"Alt","6","viewworkspace","6"},
        {"Alt","7","viewworkspace","7"},
        {"Alt","8","viewworkspace","8"},
        {"Alt","9","viewworkspace","9"},
        {"Alt+Shift","1","movetoworkspace","1"},
        {"Alt+Shift","2","movetoworkspace","2"},
        {"Alt+Shift","3","movetoworkspace","3"},
        {"Alt+Shift","4","movetoworkspace","4"},
        {"Alt+Shift","5","movetoworkspace","5"},
        {"Alt+Shift","6","movetoworkspace","6"},
        {"Alt+Shift","7","movetoworkspace","7"},
        {"Alt+Shift","8","movetoworkspace","8"},
        {"Alt+Shift","9","movetoworkspace","9"},
        {"Alt","Tab","cyclewindows","+1"},
        {"Alt+Shift","Tab","lowerwindow",""},
        {"Alt+Shift","b","swapbar",""},
        {"Alt+Shift","o","togglecompositing",""},
    };
    num_binds = (int)(sizeof(def_binds)/sizeof(def_binds[0]));
    for (int i = 0; i < num_binds; i++) {
        strncpy(binds[i].mod, def_binds[i].mod, sizeof(binds[i].mod)-1);
        strncpy(binds[i].key, def_binds[i].key, sizeof(binds[i].key)-1);
        strncpy(binds[i].action, def_binds[i].action, sizeof(binds[i].action)-1);
        strncpy(binds[i].arg, def_binds[i].arg, sizeof(binds[i].arg)-1);
    }
}

/* ── Palette functions ─────────────────────────────────────────────────── */

static char *palette_cfg_ptr(int idx) {
    switch (idx) {
    case PK_TITLE_FOCUS:    return cfg.col_title_focus;
    case PK_TITLE_UNFOCUS:  return cfg.col_title_unfocus;
    case PK_TITLE_FG:       return cfg.col_title_fg;
    case PK_FRAME_LIGHT:    return cfg.col_frame_light;
    case PK_FRAME_SHADOW:   return cfg.col_frame_shadow;
    case PK_FRAME_BG:       return cfg.col_frame_bg;
    case PK_ACTIVE_LIGHT:   return cfg.col_active_light;
    case PK_ACTIVE_SHADOW:  return cfg.col_active_shadow;
    case PK_BTN_FG:         return cfg.col_btn_fg;
    case PK_BAR_BG:         return cfg.col_bar_bg;
    case PK_BAR_FG:         return cfg.col_bar_fg;
    case PK_BAR_WS_ACTIVE:  return cfg.col_bar_ws_active;
    case PK_BAR_WS_OCCUPIED:return cfg.col_bar_ws_occupied;
    case PK_BAR_WS_IDLE:    return cfg.col_bar_ws_idle;
    case PK_BAR_WS_BG:      return cfg.col_bar_ws_bg;
    case PK_BAR_BORDER_LIGHT:  return cfg.col_bar_border_light;
    case PK_BAR_BORDER_SHADOW: return cfg.col_bar_border_shadow;
    case PK_BAR_FILL:       return cfg.col_bar_fill;
    case PK_MENU_BG:        return cfg.col_menu_bg;
    case PK_ICONBAR_BG:    return cfg.col_iconbar_bg;
    case PK_FB_BG:          return cfg.col_fb_bg;
    case PK_FB_LIGHT:       return cfg.col_fb_light;
    case PK_FB_SHADOW:      return cfg.col_fb_shadow;
    case PK_FB_FG:          return cfg.col_fb_fg;
    case PK_TOOLTIP_BG:     return cfg.col_tooltip_bg;
    case PK_TOOLTIP_FG:     return cfg.col_tooltip_fg;
    case PK_TOOLTIP_BORDER: return cfg.col_tooltip_border;
    case PK_DIALOG_BG:      return cfg.col_dialog_bg;
    case PK_ROOT_BG:        return cfg.bg_color;
    case PK_ROOT_BG2:       return cfg.bg_color2;
    default:                 return NULL;
    }
}

static void init_builtin_palettes(void) {
    /* Palette 0: "CDE Classic" — the current default cadet blue */
    Palette *cde = &palettes[num_palettes++];
    strcpy(cde->name, "CDE Classic");
    cde->builtin = 1;
    strcpy(cde->colors[PK_TITLE_FOCUS],    "#5F9EA0");
    strcpy(cde->colors[PK_TITLE_UNFOCUS],  "#A8A8A8");
    strcpy(cde->colors[PK_TITLE_FG],       "#FFFFFF");
    strcpy(cde->colors[PK_FRAME_LIGHT],    "#D9D9D9");
    strcpy(cde->colors[PK_FRAME_SHADOW],   "#595959");
    strcpy(cde->colors[PK_FRAME_BG],       "#A8A8A8");
    strcpy(cde->colors[PK_ACTIVE_LIGHT],   "#B7D4D5");
    strcpy(cde->colors[PK_ACTIVE_SHADOW],  "#2F4F50");
    strcpy(cde->colors[PK_BTN_FG],         "#000000");
    strcpy(cde->colors[PK_BAR_BG],         "#A8A8A8");
    strcpy(cde->colors[PK_BAR_FG],         "#FFFFFF");
    strcpy(cde->colors[PK_BAR_WS_ACTIVE],  "#FFFFFF");
    strcpy(cde->colors[PK_BAR_WS_OCCUPIED],"#303030");
    strcpy(cde->colors[PK_BAR_WS_IDLE],    "#606060");
    strcpy(cde->colors[PK_BAR_WS_BG],      "#5F9EA0");
    strcpy(cde->colors[PK_BAR_BORDER_LIGHT],  "#B7D4D5");
    strcpy(cde->colors[PK_BAR_BORDER_SHADOW], "#2F4F50");
    strcpy(cde->colors[PK_BAR_FILL],       "#5F9EA0");
    strcpy(cde->colors[PK_MENU_BG],        "#D4D4D4");
    strcpy(cde->colors[PK_ICONBAR_BG],    "#D4D4D4");
    strcpy(cde->colors[PK_FB_BG],          "#5F9EA0");
    strcpy(cde->colors[PK_FB_LIGHT],       "#B7D4D5");
    strcpy(cde->colors[PK_FB_SHADOW],       "#2F4F50");
    strcpy(cde->colors[PK_FB_FG],          "#FFFFFF");
    strcpy(cde->colors[PK_TOOLTIP_BG],      "#5F9EA0");
    strcpy(cde->colors[PK_TOOLTIP_FG],     "#FFFFFF");
    strcpy(cde->colors[PK_TOOLTIP_BORDER], "#2F4F50");
    strcpy(cde->colors[PK_DIALOG_BG],      "#c4c4c4");
    strcpy(cde->colors[PK_ROOT_BG],        "#303030");
    strcpy(cde->colors[PK_ROOT_BG2],       "#404040");

    /* Palette 1: "Monochrome" — black & white */
    Palette *mono = &palettes[num_palettes++];
    strcpy(mono->name, "Monochrome");
    mono->builtin = 1;
    strcpy(mono->colors[PK_TITLE_FOCUS],    "#1A1A1A");
    strcpy(mono->colors[PK_TITLE_UNFOCUS],  "#505050");
    strcpy(mono->colors[PK_TITLE_FG],       "#E0E0E0");
    strcpy(mono->colors[PK_FRAME_LIGHT],    "#A0A0A0");
    strcpy(mono->colors[PK_FRAME_SHADOW],   "#303030");
    strcpy(mono->colors[PK_FRAME_BG],       "#606060");
    strcpy(mono->colors[PK_ACTIVE_LIGHT],   "#909090");
    strcpy(mono->colors[PK_ACTIVE_SHADOW],  "#1A1A1A");
    strcpy(mono->colors[PK_BTN_FG],         "#FFFFFF");
    strcpy(mono->colors[PK_BAR_BG],         "#404040");
    strcpy(mono->colors[PK_BAR_FG],         "#E0E0E0");
    strcpy(mono->colors[PK_BAR_WS_ACTIVE],  "#FFFFFF");
    strcpy(mono->colors[PK_BAR_WS_OCCUPIED],"#C0C0C0");
    strcpy(mono->colors[PK_BAR_WS_IDLE],    "#707070");
    strcpy(mono->colors[PK_BAR_WS_BG],      "#1A1A1A");
    strcpy(mono->colors[PK_BAR_BORDER_LIGHT],  "#808080");
    strcpy(mono->colors[PK_BAR_BORDER_SHADOW], "#1A1A1A");
    strcpy(mono->colors[PK_BAR_FILL],       "#1A1A1A");
    strcpy(mono->colors[PK_MENU_BG],        "#B0B0B0");
    strcpy(mono->colors[PK_ICONBAR_BG],    "#B0B0B0");
    strcpy(mono->colors[PK_FB_BG],          "#1A1A1A");
    strcpy(mono->colors[PK_FB_LIGHT],       "#808080");
    strcpy(mono->colors[PK_FB_SHADOW],       "#000000");
    strcpy(mono->colors[PK_FB_FG],          "#FFFFFF");
    strcpy(mono->colors[PK_TOOLTIP_BG],      "#1A1A1A");
    strcpy(mono->colors[PK_TOOLTIP_FG],     "#E0E0E0");
    strcpy(mono->colors[PK_TOOLTIP_BORDER], "#404040");
    strcpy(mono->colors[PK_DIALOG_BG],      "#B0B0B0");
    strcpy(mono->colors[PK_ROOT_BG],        "#1A1A1A");
    strcpy(mono->colors[PK_ROOT_BG2],       "#303030");
}

static int match_current_palette(void) {
    for (int p = 0; p < num_palettes; p++) {
        int match = 1;
        for (int i = 0; i < PK_COUNT && match; i++) {
            char *cfg_ptr = palette_cfg_ptr(i);
            if (!cfg_ptr || strcmp(cfg_ptr, palettes[p].colors[i]) != 0)
                match = 0;
        }
        if (match) return p;
    }
    return -1; /* custom — no match */
}

static void apply_palette_to_cfg(int idx) {
    if (idx < 0 || idx >= num_palettes) return;
    for (int i = 0; i < PK_COUNT; i++) {
        char *ptr = palette_cfg_ptr(i);
        if (ptr) strcpy(ptr, palettes[idx].colors[i]);
    }
}

static void capture_current_palette(Palette *pal) {
    for (int i = 0; i < PK_COUNT; i++) {
        char *ptr = palette_cfg_ptr(i);
        if (ptr) strcpy(pal->colors[i], ptr);
        else pal->colors[i][0] = '\0';
    }
}

static void save_palette_file(const Palette *pal) {
    const char *home = getenv("HOME");
    if (!home) return;
    char dirpath[PATH_MAX];
    snprintf(dirpath, sizeof(dirpath), "%s/.config/rondo/palettes", home);
    mkdir(dirpath, 0755);

    /* sanitize name for filename: replace non-alphanumeric with _ */
    char safe_name[MAX_PALETTE_NAME];
    const char *src = pal->name;
    char *dst = safe_name;
    while (*src && (dst - safe_name) < (int)sizeof(safe_name) - 1) {
        if ((*src >= 'a' && *src <= 'z') || (*src >= 'A' && *src <= 'Z') ||
            (*src >= '0' && *src <= '9') || *src == '-' || *src == '_')
            *dst++ = *src;
        else
            *dst++ = '_';
        src++;
    }
    *dst = '\0';

    char fpath[PATH_MAX];
    snprintf(fpath, sizeof(fpath), "%s/%s.palette", dirpath, safe_name);

    FILE *f = fopen(fpath, "w");
    if (!f) { perror("rondomgr: save palette"); return; }
    fprintf(f, "(palette \"%s\"", pal->name);
    for (int i = 0; i < PK_COUNT; i++)
        fprintf(f, "\n  (%s \"%s\")", palette_keys[i], pal->colors[i]);
    fprintf(f, ")\n");
    fclose(f);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Minimal S-expr config parser
 * ═══════════════════════════════════════════════════════════════════════ */

static char *cfg_buf;
static int cfg_pos, cfg_len;

static void cfg_skip(void) {
    while (cfg_pos < cfg_len) {
        while (cfg_pos < cfg_len && (cfg_buf[cfg_pos]==' '||cfg_buf[cfg_pos]=='\t'||
               cfg_buf[cfg_pos]=='\n'||cfg_buf[cfg_pos]=='\r')) cfg_pos++;
        if (cfg_pos < cfg_len && cfg_buf[cfg_pos]==';') {
            while (cfg_pos < cfg_len && cfg_buf[cfg_pos]!='\n') cfg_pos++;
        } else break;
    }
}

static int cfg_peek(void) { cfg_skip(); return cfg_pos < cfg_len ? cfg_buf[cfg_pos] : -1; }

static int cfg_read_string(char *out, int maxlen) {
    cfg_pos++; /* skip opening " */
    int i = 0;
    while (cfg_pos < cfg_len && cfg_buf[cfg_pos]!='"' && i < maxlen-1) {
        if (cfg_buf[cfg_pos]=='\\') cfg_pos++;
        out[i++] = cfg_buf[cfg_pos++];
    }
    out[i] = '\0';
    if (cfg_pos < cfg_len && cfg_buf[cfg_pos]=='"') cfg_pos++;
    return i;
}

static int cfg_read_token(char *out, int maxlen) {
    cfg_skip();
    if (cfg_pos >= cfg_len) return 0;
    if (cfg_buf[cfg_pos]=='"') return cfg_read_string(out, maxlen);
    int i = 0;
    while (cfg_pos < cfg_len && cfg_buf[cfg_pos]!=' ' && cfg_buf[cfg_pos]!='\t' &&
           cfg_buf[cfg_pos]!='\n' && cfg_buf[cfg_pos]!='\r' &&
           cfg_buf[cfg_pos]!='(' && cfg_buf[cfg_pos]!=')' && cfg_buf[cfg_pos]!=';' &&
           i < maxlen-1) {
        out[i++] = cfg_buf[cfg_pos++];
    }
    out[i] = '\0';
    return i;
}

static void cfg_skip_form(void) {
    int depth = 0;
    do {
        int c = cfg_peek();
        if (c == -1) break;
        if (c == '(') { depth++; cfg_pos++; }
        else if (c == ')') { depth--; cfg_pos++; }
        else { char tmp[256]; cfg_read_token(tmp, sizeof(tmp)); }
    } while (depth > 0);
}

static int parse_bar_position(const char *s) {
    if (strcmp(s,"top")==0) return 0;
    if (strcmp(s,"bottom")==0) return 1;
    if (strcmp(s,"left")==0) return 2;
    if (strcmp(s,"right")==0) return 3;
    return 0;
}
static int parse_icon_mode(const char *s) {
    if (strcmp(s,"icon")==0) return 0;
    if (strcmp(s,"text")==0) return 1;
    if (strcmp(s,"icon-text")==0) return 2;
    return 2;
}
static int parse_widget_type(const char *s) {
    if (strcmp(s,"ws")==0) return BW_WS;
    if (strcmp(s,"title")==0) return BW_TITLE;
    if (strcmp(s,"clock")==0) return BW_CLOCK;
    if (strcmp(s,"load")==0) return BW_LOAD;
    if (strcmp(s,"mem")==0) return BW_MEM;
    if (strcmp(s,"disk")==0) return BW_DISK;
    if (strcmp(s,"bat")==0) return BW_BAT;
    if (strcmp(s,"vol")==0) return BW_VOL;
    if (strcmp(s,"cpu")==0) return BW_CPU;
    if (strcmp(s,"net")==0) return BW_NET;
    if (strcmp(s,"temp")==0) return BW_TEMP;
    return -1;
}
static int parse_pattern(const char *s) {
    if (strcmp(s,"checkerboard")==0) return 0;
    if (strcmp(s,"diagonal-stripes")==0) return 1;
    if (strcmp(s,"horizontal-stripes")==0) return 2;
    if (strcmp(s,"vertical-stripes")==0) return 3;
    if (strcmp(s,"dots")==0) return 4;
    if (strcmp(s,"crosshatch")==0) return 5;
    if (strcmp(s,"weave")==0) return 6;
    return 0;
}
static int parse_img_mode(const char *s) {
    if (strcmp(s,"centered")==0) return 0;
    if (strcmp(s,"scaled")==0) return 1;
    if (strcmp(s,"tiled")==0) return 2;
    if (strcmp(s,"stretched")==0) return 3;
    if (strcmp(s,"scale-filled")==0) return 4;
    return 1;
}

static void load_config(void) {
    set_defaults();
    const char *home = getenv("HOME");
    if (!home) return;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.rondorc", home);
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(path, sizeof(path), "%s/.config/rondo/config.scm", home);
        f = fopen(path, "r");
    }
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    cfg_buf = malloc((size_t)sz + 1);
    cfg_len = (int)fread(cfg_buf, 1, (size_t)sz, f);
    cfg_buf[cfg_len] = '\0';
    fclose(f);
    cfg_pos = 0;

    char key[128], val[512];
    while (cfg_pos < cfg_len) {
        cfg_skip();
        if (cfg_pos >= cfg_len) break;
        if (cfg_buf[cfg_pos] != '(') { cfg_pos++; continue; }
        cfg_pos++; /* skip ( */
        if (!cfg_read_token(key, sizeof(key))) continue;

        if (strcmp(key,"frame-width")==0 && cfg_read_token(val,sizeof(val))) cfg.frame_width=atoi(val);
        else if (strcmp(key,"title-height")==0 && cfg_read_token(val,sizeof(val))) cfg.title_height=atoi(val);
        else if (strcmp(key,"bar-height")==0 && cfg_read_token(val,sizeof(val))) cfg.bar_height=atoi(val);
        else if (strcmp(key,"bar-border-width")==0 && cfg_read_token(val,sizeof(val))) cfg.bar_border_width=atoi(val);
        else if (strcmp(key,"bar-corner-size")==0 && cfg_read_token(val,sizeof(val))) cfg.bar_corner_size=atoi(val);
        else if (strcmp(key,"bar-btn-width")==0 && cfg_read_token(val,sizeof(val))) cfg.bar_btn_width=atoi(val);
        else if (strcmp(key,"btn-width")==0 && cfg_read_token(val,sizeof(val))) cfg.btn_width=atoi(val);
        else if (strcmp(key,"btn-height")==0 && cfg_read_token(val,sizeof(val))) cfg.btn_height=atoi(val);
        else if (strcmp(key,"icon-width")==0 && cfg_read_token(val,sizeof(val))) cfg.icon_width=atoi(val);
        else if (strcmp(key,"icon-height")==0 && cfg_read_token(val,sizeof(val))) cfg.icon_height=atoi(val);
        else if (strcmp(key,"icon-padding")==0 && cfg_read_token(val,sizeof(val))) cfg.icon_padding=atoi(val);
        else if (strcmp(key,"workspaces")==0 && cfg_read_token(val,sizeof(val))) cfg.num_workspaces=atoi(val);
        else if (strcmp(key,"show-bar")==0 && cfg_read_token(val,sizeof(val))) cfg.show_bar=atoi(val);
        else if (strcmp(key,"fade-enabled")==0 && cfg_read_token(val,sizeof(val))) cfg.fade_enabled=atoi(val);
        else if (strcmp(key,"fade-in-ms")==0 && cfg_read_token(val,sizeof(val))) cfg.fade_in_ms=atoi(val);
        else if (strcmp(key,"fade-out-ms")==0 && cfg_read_token(val,sizeof(val))) cfg.fade_out_ms=atoi(val);
        else if (strcmp(key,"tooltip-delay")==0 && cfg_read_token(val,sizeof(val))) cfg.tooltip_delay=atoi(val);
        else if (strcmp(key,"master-ratio")==0 && cfg_read_token(val,sizeof(val))) cfg.master_ratio=(float)atof(val);
        else if (strcmp(key,"font")==0) cfg_read_token(cfg.font,sizeof(cfg.font));
        else if (strcmp(key,"terminal")==0) cfg_read_token(cfg.terminal,sizeof(cfg.terminal));
        else if (strcmp(key,"launcher")==0) cfg_read_token(cfg.launcher,sizeof(cfg.launcher));
        else if (strcmp(key,"modkey")==0) cfg_read_token(cfg.modkey,sizeof(cfg.modkey));
        else if (strcmp(key,"clock-format")==0) cfg_read_token(cfg.clock_format,sizeof(cfg.clock_format));
        else if (strcmp(key,"tooltip-font")==0) cfg_read_token(cfg.tooltip_font,sizeof(cfg.tooltip_font));
        else if (strcmp(key,"bar-position")==0 && cfg_read_token(val,sizeof(val))) cfg.bar_position=parse_bar_position(val);
        else if (strcmp(key,"iconbar-position")==0 && cfg_read_token(val,sizeof(val))) cfg.iconbar_position=parse_bar_position(val);
        else if (strcmp(key,"icon-mode")==0 && cfg_read_token(val,sizeof(val))) cfg.icon_mode=parse_icon_mode(val);
        /* colors */
        else if (strcmp(key,"title-focus")==0) cfg_read_token(cfg.col_title_focus,sizeof(cfg.col_title_focus));
        else if (strcmp(key,"title-unfocus")==0) cfg_read_token(cfg.col_title_unfocus,sizeof(cfg.col_title_unfocus));
        else if (strcmp(key,"title-fg")==0) cfg_read_token(cfg.col_title_fg,sizeof(cfg.col_title_fg));
        else if (strcmp(key,"frame-light")==0) cfg_read_token(cfg.col_frame_light,sizeof(cfg.col_frame_light));
        else if (strcmp(key,"frame-shadow")==0) cfg_read_token(cfg.col_frame_shadow,sizeof(cfg.col_frame_shadow));
        else if (strcmp(key,"frame-bg")==0) cfg_read_token(cfg.col_frame_bg,sizeof(cfg.col_frame_bg));
        else if (strcmp(key,"active-light")==0) cfg_read_token(cfg.col_active_light,sizeof(cfg.col_active_light));
        else if (strcmp(key,"active-shadow")==0) cfg_read_token(cfg.col_active_shadow,sizeof(cfg.col_active_shadow));
        else if (strcmp(key,"btn-fg")==0) cfg_read_token(cfg.col_btn_fg,sizeof(cfg.col_btn_fg));
        else if (strcmp(key,"bar-bg")==0) cfg_read_token(cfg.col_bar_bg,sizeof(cfg.col_bar_bg));
        else if (strcmp(key,"bar-fg")==0) cfg_read_token(cfg.col_bar_fg,sizeof(cfg.col_bar_fg));
        else if (strcmp(key,"bar-ws-active")==0) cfg_read_token(cfg.col_bar_ws_active,sizeof(cfg.col_bar_ws_active));
        else if (strcmp(key,"bar-ws-occupied")==0) cfg_read_token(cfg.col_bar_ws_occupied,sizeof(cfg.col_bar_ws_occupied));
        else if (strcmp(key,"bar-ws-idle")==0) cfg_read_token(cfg.col_bar_ws_idle,sizeof(cfg.col_bar_ws_idle));
        else if (strcmp(key,"bar-ws-bg")==0) cfg_read_token(cfg.col_bar_ws_bg,sizeof(cfg.col_bar_ws_bg));
        else if (strcmp(key,"bar-border-light")==0) cfg_read_token(cfg.col_bar_border_light,sizeof(cfg.col_bar_border_light));
        else if (strcmp(key,"bar-border-shadow")==0) cfg_read_token(cfg.col_bar_border_shadow,sizeof(cfg.col_bar_border_shadow));
        else if (strcmp(key,"bar-fill")==0) cfg_read_token(cfg.col_bar_fill,sizeof(cfg.col_bar_fill));
        else if (strcmp(key,"menu-bg")==0) cfg_read_token(cfg.col_menu_bg,sizeof(cfg.col_menu_bg));
        else if (strcmp(key,"iconbar-bg")==0) cfg_read_token(cfg.col_iconbar_bg,sizeof(cfg.col_iconbar_bg));
        else if (strcmp(key,"fb-bg")==0) cfg_read_token(cfg.col_fb_bg,sizeof(cfg.col_fb_bg));
        else if (strcmp(key,"fb-light")==0) cfg_read_token(cfg.col_fb_light,sizeof(cfg.col_fb_light));
        else if (strcmp(key,"fb-shadow")==0) cfg_read_token(cfg.col_fb_shadow,sizeof(cfg.col_fb_shadow));
        else if (strcmp(key,"fb-fg")==0) cfg_read_token(cfg.col_fb_fg,sizeof(cfg.col_fb_fg));
        else if (strcmp(key,"tooltip-bg")==0) cfg_read_token(cfg.col_tooltip_bg,sizeof(cfg.col_tooltip_bg));
        else if (strcmp(key,"tooltip-fg")==0) cfg_read_token(cfg.col_tooltip_fg,sizeof(cfg.col_tooltip_fg));
        else if (strcmp(key,"tooltip-border")==0) cfg_read_token(cfg.col_tooltip_border,sizeof(cfg.col_tooltip_border));
        else if (strcmp(key,"dialog-bg")==0) cfg_read_token(cfg.col_dialog_bg,sizeof(cfg.col_dialog_bg));
        else if (strcmp(key,"root-bg2")==0) cfg_read_token(cfg.bg_color2,sizeof(cfg.bg_color2));
        /* compound: root-bg */
        else if (strcmp(key,"root-bg")==0) {
            cfg_skip();
            if (cfg_pos < cfg_len && cfg_buf[cfg_pos]=='"') {
                cfg_read_token(cfg.bg_color, sizeof(cfg.bg_color));
                cfg.bg_mode = 0;
            } else if (cfg_pos < cfg_len && cfg_buf[cfg_pos]=='(') {
                cfg_pos++;
                char sub[64];
                cfg_read_token(sub, sizeof(sub));
                if (strcmp(sub,"pattern")==0) {
                    cfg.bg_mode = 1;
                    char pname[64]; cfg_read_token(pname, sizeof(pname));
                    cfg.bg_pattern = parse_pattern(pname);
                    cfg_read_token(cfg.bg_color, sizeof(cfg.bg_color));
                    cfg_read_token(cfg.bg_color2, sizeof(cfg.bg_color2));
                    char sz[32]; if (cfg_read_token(sz,sizeof(sz)) && cfg_buf[cfg_pos-1]!=')')
                        cfg.bg_pattern_size = atoi(sz);
                } else if (strcmp(sub,"image")==0) {
                    cfg.bg_mode = 2;
                    cfg_read_token(cfg.bg_image_path, sizeof(cfg.bg_image_path));
                    char m[32]; if (cfg_read_token(m,sizeof(m)))
                        cfg.bg_image_mode = parse_img_mode(m);
                }
                cfg_skip_form();
            }
        }
        /* compound: bar-layout */
        else if (strcmp(key,"bar-layout")==0) {
            num_bar_widgets = 0;
            cfg_skip();
            while (cfg_pos < cfg_len && cfg_buf[cfg_pos]=='(') {
                cfg_pos++;
                char wt[32], al[32];
                if (cfg_read_token(wt,sizeof(wt)) && cfg_read_token(al,sizeof(al))) {
                    int t = parse_widget_type(wt);
                    if (t >= 0 && num_bar_widgets < 32) {
                        bar_layout[num_bar_widgets].type = t;
                        bar_layout[num_bar_widgets].align = (strcmp(al,"left")==0) ? 0 : 1;
                        num_bar_widgets++;
                    }
                }
                while (cfg_pos < cfg_len && cfg_buf[cfg_pos]!=')') cfg_pos++;
                if (cfg_pos < cfg_len) cfg_pos++;
                cfg_skip();
            }
        }
        /* compound: bind */
        else if (strcmp(key,"bind")==0) {
            if (num_binds < 128) {
                BindEntry *b = &binds[num_binds++];
                b->mod[0]=b->key[0]=b->action[0]=b->arg[0]='\0';
                cfg_skip();
                while (cfg_pos < cfg_len && cfg_buf[cfg_pos]=='(') {
                    cfg_pos++;
                    char sk[32], sv[128];
                    cfg_read_token(sk, sizeof(sk));
                    cfg_read_token(sv, sizeof(sv));
                    if (strcmp(sk,"mod")==0) COPY_TRUNK(b->mod,sv);
                    else if (strcmp(sk,"key")==0) COPY_TRUNK(b->key,sv);
                    else if (strcmp(sk,"action")==0) COPY_TRUNK(b->action,sv);
                    else if (strcmp(sk,"arg")==0) COPY_TRUNK(b->arg,sv);
                    while (cfg_pos < cfg_len && cfg_buf[cfg_pos]!=')') cfg_pos++;
                    if (cfg_pos < cfg_len) cfg_pos++;
                    cfg_skip();
                }
            } else cfg_skip_form();
        }
        /* compound: root-menu — skip */
        else if (strcmp(key,"root-menu")==0) cfg_skip_form();
        else cfg_skip_form();

        /* skip to closing ) */
        while (cfg_pos < cfg_len && cfg_buf[cfg_pos]!=')') cfg_pos++;
        if (cfg_pos < cfg_len) cfg_pos++;
    }
    free(cfg_buf);
    cfg_buf = NULL;
}

/* ── Palette file I/O (needs S-expr parser above) ──────────────────────── */

static void load_palette_file(const char *path, Palette *pal) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    int len = (int)fread(buf, 1, (size_t)sz, f);
    buf[len] = '\0';
    fclose(f);

    /* Save/restore global parser state */
    char *saved_buf = cfg_buf;
    int saved_pos = cfg_pos, saved_len = cfg_len;
    cfg_buf = buf;
    cfg_pos = 0;
    cfg_len = len;

    /* Parse (palette "Name" (key "val") ...) */
    cfg_skip();
    if (cfg_peek() == '(') {
        cfg_pos++;
        char tok[128];
        cfg_read_token(tok, sizeof(tok));
        if (strcmp(tok, "palette") == 0) {
            cfg_read_token(pal->name, MAX_PALETTE_NAME);
            while (cfg_pos < cfg_len) {
                cfg_skip();
                if (cfg_pos >= cfg_len || cfg_buf[cfg_pos] == ')') break;
                if (cfg_buf[cfg_pos] == '(') {
                    cfg_pos++;
                    char key[64], val[64];
                    if (cfg_read_token(key, sizeof(key)) && cfg_read_token(val, sizeof(val))) {
                        for (int i = 0; i < PK_COUNT; i++) {
                            if (strcmp(palette_keys[i], key) == 0) {
                                strcpy(pal->colors[i], val);
                                break;
                            }
                        }
                    }
                    while (cfg_pos < cfg_len && cfg_buf[cfg_pos] != ')') cfg_pos++;
                    if (cfg_pos < cfg_len) cfg_pos++;
                } else {
                    char skip[64];
                    cfg_read_token(skip, sizeof(skip));
                }
            }
        }
    }

    free(buf);
    cfg_buf = saved_buf;
    cfg_pos = saved_pos;
    cfg_len = saved_len;
}

static void load_user_palettes(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    char dirpath[PATH_MAX];
    snprintf(dirpath, sizeof(dirpath), "%s/.config/rondo/palettes", home);
    DIR *d = opendir(dirpath);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 9) continue;
        if (strcmp(ent->d_name + nlen - 9, ".palette") != 0) continue;
        if (num_palettes >= MAX_PALETTES) break;
        char fpath[PATH_MAX];
        snprintf(fpath, sizeof(fpath), "%s/%s", dirpath, ent->d_name);
        Palette *pal = &palettes[num_palettes];
        memset(pal, 0, sizeof(*pal));
        pal->builtin = 0;
        load_palette_file(fpath, pal);
        if (pal->name[0]) num_palettes++;
    }
    closedir(d);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Config file writer
 * ═══════════════════════════════════════════════════════════════════════ */

static const char *bar_pos_str(int v) { return (const char*[]){"top","bottom","left","right"}[v]; }
static const char *icon_mode_str(int v) { return (const char*[]){"icon","text","icon-text"}[v]; }
static const char *widget_type_str(int v) {
    return (const char*[]){"ws","title","clock","load","mem","disk","bat","vol","cpu","net","temp"}[v];
}
static const char *pattern_str(int v) {
    return (const char*[]){"checkerboard","diagonal-stripes","horizontal-stripes",
                           "vertical-stripes","dots","crosshatch","weave"}[v];
}
static const char *img_mode_str(int v) {
    return (const char*[]){"centered","scaled","tiled","stretched","scale-filled"}[v];
}

static int save_config(void) {
    const char *home = getenv("HOME");
    if (!home) return -1;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.rondorc", home);
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, ";; rondo configuration — generated by rondomgr\n\n");

    fprintf(f, ";; Dimensions\n");
    fprintf(f, "(frame-width %d)\n", cfg.frame_width);
    fprintf(f, "(title-height %d)\n", cfg.title_height);
    fprintf(f, "(btn-width %d)\n", cfg.btn_width);
    fprintf(f, "(btn-height %d)\n", cfg.btn_height);
    fprintf(f, "\n;; Status Bar\n");
    fprintf(f, "(bar-height %d)\n", cfg.bar_height);
    fprintf(f, "(bar-border-width %d)\n", cfg.bar_border_width);
    fprintf(f, "(bar-corner-size %d)\n", cfg.bar_corner_size);
    fprintf(f, "(bar-btn-width %d)\n", cfg.bar_btn_width);
    fprintf(f, "(show-bar %d)\n", cfg.show_bar);
    fprintf(f, "(bar-position %s)\n", bar_pos_str(cfg.bar_position));
    fprintf(f, "\n;; Bar Layout\n(bar-layout\n");
    for (int i = 0; i < num_bar_widgets; i++)
        fprintf(f, "  (%s %s)\n", widget_type_str(bar_layout[i].type),
                bar_layout[i].align==0?"left":"right");
    fprintf(f, "  )\n");
    fprintf(f, "\n(clock-format \"%s\")\n", cfg.clock_format);
    fprintf(f, "\n;; Icon Bar\n");
    fprintf(f, "(icon-width %d)\n", cfg.icon_width);
    fprintf(f, "(icon-height %d)\n", cfg.icon_height);
    fprintf(f, "(icon-padding %d)\n", cfg.icon_padding);
    fprintf(f, "(icon-mode %s)\n", icon_mode_str(cfg.icon_mode));
    fprintf(f, "(iconbar-position %s)\n", bar_pos_str(cfg.iconbar_position));
    fprintf(f, "\n;; Layout\n");
    fprintf(f, "(workspaces %d)\n", cfg.num_workspaces);
    fprintf(f, "(master-ratio %.2f)\n", cfg.master_ratio);
    fprintf(f, "\n;; Compositing\n");
    fprintf(f, "(fade-enabled %d)\n", cfg.fade_enabled);
    fprintf(f, "(fade-in-ms %d)\n", cfg.fade_in_ms);
    fprintf(f, "(fade-out-ms %d)\n", cfg.fade_out_ms);
    fprintf(f, "(tooltip-delay %d)\n", cfg.tooltip_delay);
    fprintf(f, "\n;; Programs\n");
    fprintf(f, "(font \"%s\")\n", cfg.font);
    fprintf(f, "(terminal \"%s\")\n", cfg.terminal);
    fprintf(f, "(launcher \"%s\")\n", cfg.launcher);
    fprintf(f, "(modkey \"%s\")\n", cfg.modkey);
    if (cfg.tooltip_font[0])
        fprintf(f, "(tooltip-font \"%s\")\n", cfg.tooltip_font);
    fprintf(f, "\n;; Window Frame Colors\n");
    fprintf(f, "(title-focus \"%s\")\n", cfg.col_title_focus);
    fprintf(f, "(title-unfocus \"%s\")\n", cfg.col_title_unfocus);
    fprintf(f, "(title-fg \"%s\")\n", cfg.col_title_fg);
    fprintf(f, "(frame-light \"%s\")\n", cfg.col_frame_light);
    fprintf(f, "(frame-shadow \"%s\")\n", cfg.col_frame_shadow);
    fprintf(f, "(frame-bg \"%s\")\n", cfg.col_frame_bg);
    fprintf(f, "(active-light \"%s\")\n", cfg.col_active_light);
    fprintf(f, "(active-shadow \"%s\")\n", cfg.col_active_shadow);
    fprintf(f, "(btn-fg \"%s\")\n", cfg.col_btn_fg);
    fprintf(f, "\n;; Status Bar Colors\n");
    fprintf(f, "(bar-bg \"%s\")\n", cfg.col_bar_bg);
    fprintf(f, "(bar-fg \"%s\")\n", cfg.col_bar_fg);
    fprintf(f, "(bar-ws-active \"%s\")\n", cfg.col_bar_ws_active);
    fprintf(f, "(bar-ws-occupied \"%s\")\n", cfg.col_bar_ws_occupied);
    fprintf(f, "(bar-ws-idle \"%s\")\n", cfg.col_bar_ws_idle);
    fprintf(f, "(bar-ws-bg \"%s\")\n", cfg.col_bar_ws_bg);
    fprintf(f, "(bar-border-light \"%s\")\n", cfg.col_bar_border_light);
    fprintf(f, "(bar-border-shadow \"%s\")\n", cfg.col_bar_border_shadow);
    fprintf(f, "(bar-fill \"%s\")\n", cfg.col_bar_fill);
    fprintf(f, "\n;; Menu / Feedback / Tooltip Colors\n");
    fprintf(f, "(menu-bg \"%s\")\n", cfg.col_menu_bg);
    fprintf(f, "(iconbar-bg \"%s\")\n", cfg.col_iconbar_bg);
    fprintf(f, "(fb-bg \"%s\")\n", cfg.col_fb_bg);
    fprintf(f, "(fb-light \"%s\")\n", cfg.col_fb_light);
    fprintf(f, "(fb-shadow \"%s\")\n", cfg.col_fb_shadow);
    fprintf(f, "(fb-fg \"%s\")\n", cfg.col_fb_fg);
    fprintf(f, "(tooltip-bg \"%s\")\n", cfg.col_tooltip_bg);
    fprintf(f, "(tooltip-fg \"%s\")\n", cfg.col_tooltip_fg);
    fprintf(f, "(tooltip-border \"%s\")\n", cfg.col_tooltip_border);
    fprintf(f, "(dialog-bg \"%s\")\n", cfg.col_dialog_bg);
    fprintf(f, "\n;; Background\n");
    if (cfg.bg_mode == 0)
        fprintf(f, "(root-bg \"%s\")\n", cfg.bg_color);
    else if (cfg.bg_mode == 1) {
        fprintf(f, "(root-bg (pattern \"%s\" \"%s\" \"%s\"",
                pattern_str(cfg.bg_pattern), cfg.bg_color, cfg.bg_color2);
        if (cfg.bg_pattern_size > 0)
            fprintf(f, " %d", cfg.bg_pattern_size);
        fprintf(f, "))\n");
    }
    else
        fprintf(f, "(root-bg (image \"%s\" %s))\n", cfg.bg_image_path, img_mode_str(cfg.bg_image_mode));
    if (cfg.bg_mode == 1 || cfg.bg_mode == 0)
        fprintf(f, "(root-bg2 \"%s\")\n", cfg.bg_color2);
    fprintf(f, "\n;; Key Bindings\n");
    for (int i = 0; i < num_binds; i++) {
        BindEntry *b = &binds[i];
        fprintf(f, "(bind (mod %s) (key %s) (action %s)", b->mod, b->key, b->action);
        if (b->arg[0]) fprintf(f, " (arg %s)", b->arg);
        fprintf(f, ")\n");
    }
    fprintf(f, "\n;; Root Menu\n(root-menu\n  (\"New Window\" new-window)\n  ()\n  (\"Shuffle Up\" shuffle-up)\n  (\"Shuffle Down\" shuffle-down)\n  ()\n  (\"Refresh\" refresh)\n  (\"Restart\" restart)\n  (\"Quit\" quit))\n");
    fclose(f);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Send IPC command to rondo via Unix domain socket
 * ═══════════════════════════════════════════════════════════════════════ */

static int ipc_send(const char *cmd)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    const char *display = getenv("DISPLAY");
    if (!display) display = ":0";
    /* sun_path is 108 bytes; prefix "/tmp/.rondo-ipc-" is 17 chars, leaving 91 */
    char safe[92];
    strncpy(safe, display, sizeof(safe) - 1);
    safe[sizeof(safe) - 1] = '\0';
    for (char *p = safe; *p; p++)
        if (*p == '/') *p = '_';
    snprintf(addr.sun_path, sizeof(addr.sun_path),
             "/tmp/.rondo-ipc-%s", safe);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    size_t len = strlen(cmd);
    ssize_t n = send(fd, cmd, len, MSG_NOSIGNAL);
    if (n > 0) send(fd, "\n", 1, MSG_NOSIGNAL);

    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Motif GUI
 * ═══════════════════════════════════════════════════════════════════════ */

static XtAppContext app;
static Widget toplevel, main_form, section_rc, section_sw, scroll_win;
static Widget panels[7]; /* 0=Dimensions,1=Bar,2=Appearance,3=Programs,4=Compositing,5=Background,6=Keybindings */
static int current_panel = 0;

/* widget handles for reading back values */
static Widget w_frame_width, w_title_height, w_bar_height, w_btn_width, w_btn_height;
static Widget w_bar_border_width, w_bar_corner_size, w_bar_btn_width;
static Widget w_icon_width, w_icon_height, w_icon_padding, w_num_workspaces;
static Widget w_master_ratio, w_show_bar;
static Widget w_bar_position, w_iconbar_position, w_icon_mode;
static Widget w_fade_enabled, w_fade_in_ms, w_fade_out_ms, w_tooltip_delay;
static Widget w_font, w_terminal, w_launcher, w_modkey, w_clock_format, w_tooltip_font;
static Widget w_bg_mode[3], w_bg_pattern, w_bg_color, w_bg_color2, w_bg_image_path, w_bg_image_mode;
/* color widgets */
typedef struct { Widget text; Widget preview; const char *key; char *value; } ColorWidget;
static ColorWidget color_widgets[30];
static int num_color_widgets = 0;

/* keybindings panel */
static Widget w_bind_list;       /* XmList showing current bindings */
static Widget w_bind_add_btn, w_bind_edit_btn, w_bind_remove_btn;

static void show_panel(int which) {
    for (int i = 0; i < 7; i++)
        XtUnmanageChild(panels[i]);
    if (which == 6) {
        /* keybindings panel lives directly in main_form, not in scroll_win */
        XtUnmanageChild(scroll_win);
        XtManageChild(panels[6]);
    } else {
        XtManageChild(scroll_win);
        XmScrolledWindowSetAreas(scroll_win, NULL, NULL, panels[which]);
        XtManageChild(panels[which]);
    }
    current_panel = which;
}

static void section_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    show_panel((int)(intptr_t)client_data);
}

static void make_scale(Widget parent, Widget *out, const char *label, int min, int max, int val) {
    Widget row = XtVaCreateManagedWidget("row", xmFormWidgetClass, parent,
        XmNfractionBase, 100, NULL);
    Widget lbl = XtVaCreateManagedWidget(label, xmLabelWidgetClass, row,
        XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 4, NULL);
    *out = XtVaCreateManagedWidget("scale", xmScaleWidgetClass, row,
        XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, lbl,
        XmNrightAttachment, XmATTACH_FORM,
        XmNminimum, min, XmNmaximum, max, XmNvalue, val,
        XmNshowValue, True, XmNorientation, XmHORIZONTAL,
        NULL);
}

static void make_text_row(Widget parent, Widget *out, const char *label, const char *val) {
    Widget row = XtVaCreateManagedWidget("row", xmFormWidgetClass, parent,
        XmNfractionBase, 100, NULL);
    (void)XtVaCreateManagedWidget(label, xmLabelWidgetClass, row,
        XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 4,
        XmNrightAttachment, XmATTACH_POSITION, XmNrightPosition, 35, NULL);
    *out = XtVaCreateManagedWidget("val", xmTextWidgetClass, row,
        XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_POSITION, XmNleftPosition, 36,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 4,
        XmNeditMode, XmSINGLE_LINE_EDIT,
        XmNvalue, val, NULL);
}

/* file selection — try native desktop picker, fall back to Motif */
static char *run_file_picker(void) {
    /* try zenity (GNOME/GTK desktops) */
    {
        int pipefd[2];
        if (pipe(pipefd) == 0) {
            pid_t pid = fork();
            if (pid == 0) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
                execlp("zenity", "zenity", "--file-selection",
                       "--title=Select Image", (char *)NULL);
                _exit(127);
            }
            close(pipefd[1]);
            char buf[4096];
            ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
            int status;
            waitpid(pid, &status, 0);
            close(pipefd[0]);
            if (n > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                buf[n] = '\0';
                /* strip trailing newline */
                char *nl = strchr(buf, '\n');
                if (nl) *nl = '\0';
                return strdup(buf);
            }
        }
    }
    /* try kdialog (KDE desktops) */
    {
        int pipefd[2];
        if (pipe(pipefd) == 0) {
            pid_t pid = fork();
            if (pid == 0) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
                execlp("kdialog", "kdialog", "--getopenfilename",
                       ".", "--title", "Select Image", (char *)NULL);
                _exit(127);
            }
            close(pipefd[1]);
            char buf[4096];
            ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
            int status;
            waitpid(pid, &status, 0);
            close(pipefd[0]);
            if (n > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                buf[n] = '\0';
                char *nl = strchr(buf, '\n');
                if (nl) *nl = '\0';
                return strdup(buf);
            }
        }
    }
    return NULL;
}

/* Motif file selection dialog callback */
static void file_browse_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w;
    XmFileSelectionBoxCallbackStruct *fcb = (XmFileSelectionBoxCallbackStruct *)call_data;
    char *filename = NULL;
    if (XmStringGetLtoR(fcb->value, XmFONTLIST_DEFAULT_TAG, &filename) && filename) {
        Widget text_w = (Widget)client_data;
        XmTextSetString(text_w, filename);
        XtFree(filename);
    }
}

static void destroy_widget_cb(Widget w, XtPointer client, XtPointer call) {
    (void)client; (void)call;
    XtDestroyWidget(w);
}

static void browse_btn_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    Widget text_w = (Widget)client_data;

    /* try native desktop file picker first */
    char *path = run_file_picker();
    if (path) {
        XmTextSetString(text_w, path);
        free(path);
        return;
    }

    /* fall back to Motif file selection dialog */
    Widget dlg = XmCreateFileSelectionDialog(toplevel, "file_select", NULL, 0);
    XtAddCallback(dlg, XmNokCallback, file_browse_cb, (XtPointer)text_w);
    XtAddCallback(dlg, XmNcancelCallback, destroy_widget_cb, NULL);
    XtVaSetValues(XtParent(dlg), XmNtitle, "Select Image", NULL);
    XtManageChild(dlg);
}

/* color pixel from hex string */
static Pixel color_pixel(const char *name);

static void add_color_widget(Widget parent, const char *label, const char *key, char *value) {
    Widget row = XtVaCreateManagedWidget("crow", xmFormWidgetClass, parent, NULL);
    (void)XtVaCreateManagedWidget(label, xmLabelWidgetClass, row,
        XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 4,
        XmNrightAttachment, XmATTACH_POSITION, XmNrightPosition, 35, NULL);
    Widget text = XtVaCreateManagedWidget("cval", xmTextWidgetClass, row,
        XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_POSITION, XmNleftPosition, 36,
        XmNeditMode, XmSINGLE_LINE_EDIT,
        XmNvalue, value,
        NULL);
    Widget preview = XtVaCreateManagedWidget("cprev", xmFrameWidgetClass, row,
        XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, text,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 4,
        XmNwidth, 30, XmNheight, 20,
        XmNbackground, color_pixel(value),
        NULL);
    ColorWidget *cw = &color_widgets[num_color_widgets++];
    cw->text = text; cw->preview = preview; cw->key = key; cw->value = value;
}

/* panel creation helpers */

static Pixel color_pixel(const char *name) {
    Display *d = XtDisplay(toplevel);
    Colormap cmap = DefaultColormap(d, DefaultScreen(d));
    XColor c;
    if (XParseColor(d, cmap, name, &c) && XAllocColor(d, cmap, &c))
        return c.pixel;
    return WhitePixel(d, DefaultScreen(d));
}

static Widget make_scroll_form(Widget parent) {
    Widget rc = XtVaCreateWidget("panel", xmRowColumnWidgetClass, parent,
        XmNorientation, XmVERTICAL, XmNpacking, XmPACK_COLUMN,
        XmNnumColumns, 1, XmNentryAlignment, XmALIGNMENT_BEGINNING,
        NULL);
    return rc;
}

/* option menu helper — creates a pulldown + option menu from a string array */
static Widget make_option_menu(Widget parent, const char *name,
                               char **labels, int count, int default_idx) {
    Widget pulldown = XmCreatePulldownMenu(parent, "pulldown", NULL, 0);
    Widget buttons[16];
    for (int i = 0; i < count && i < 16; i++) {
        XmString xms = XmStringCreateLocalized(labels[i]);
        Arg btn_args[1];
        XtSetArg(btn_args[0], XmNlabelString, xms);
        buttons[i] = XmCreatePushButtonGadget(pulldown, labels[i], btn_args, 1);
        XmStringFree(xms);
        XtManageChild(buttons[i]);
    }
    Arg args[4];
    int n = 0;
    XtSetArg(args[n], XmNsubMenuId, pulldown); n++;
    if (default_idx >= 0 && default_idx < count && default_idx < 16) {
        XtSetArg(args[n], XmNmenuHistory, buttons[default_idx]); n++;
    }
    /* hide the default "OptionMenu:" label */
    XmString empty = XmStringCreateLocalized("");
    XtSetArg(args[n], XmNlabelString, empty); n++;
    Widget om = XmCreateOptionMenu(parent, (String)name, args, n);
    XmStringFree(empty);
    XtManageChild(om);
    return om;
}

/* read selected index from an option menu (0-based) */
static int option_menu_index(Widget om) {
    Widget hist = NULL;
    XtVaGetValues(om, XmNmenuHistory, &hist, NULL);
    if (!hist) return 0;
    int pos = 0;
    XtVaGetValues(hist, XmNpositionIndex, &pos, NULL);
    return pos;
}

static void create_dimensions_panel(Widget parent) {
    Widget p = make_scroll_form(parent);
    panels[0] = p;
    make_scale(p, &w_frame_width, "Frame Width", 1, 20, cfg.frame_width);
    make_scale(p, &w_title_height, "Title Height", 10, 40, cfg.title_height);
    make_scale(p, &w_btn_width, "Button Width", 8, 30, cfg.btn_width);
    make_scale(p, &w_btn_height, "Button Height", 8, 30, cfg.btn_height);
    XtUnmanageChild(p);
}

static void create_bar_panel(Widget parent) {
    Widget p = make_scroll_form(parent);
    panels[1] = p;
    make_scale(p, &w_bar_height, "Bar Height", 16, 50, cfg.bar_height);
    make_scale(p, &w_bar_border_width, "Border Width", 0, 20, cfg.bar_border_width);
    make_scale(p, &w_bar_corner_size, "Corner Size", 8, 40, cfg.bar_corner_size);
    make_scale(p, &w_bar_btn_width, "Button Size", 8, 30, cfg.bar_btn_width);
    /* show-bar toggle */
    w_show_bar = XtVaCreateManagedWidget("Show Bar", xmToggleButtonWidgetClass, p,
        XmNset, cfg.show_bar, NULL);
    /* bar position option menu */
    {
        Widget row = XtVaCreateManagedWidget("row", xmFormWidgetClass, p, NULL);
        Widget lbl = XtVaCreateManagedWidget("Bar Position", xmLabelWidgetClass, row,
            XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 4, NULL);
        static char *pos_opts[] = {"top","bottom","left","right"};
        w_bar_position = make_option_menu(row, "pos", pos_opts, 4, cfg.bar_position);
        XtVaSetValues(w_bar_position,
            XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, lbl,
            XmNrightAttachment, XmATTACH_FORM, NULL);
    }
    make_scale(p, &w_icon_width, "Icon Width", 20, 100, cfg.icon_width);
    make_scale(p, &w_icon_height, "Icon Height", 20, 100, cfg.icon_height);
    make_scale(p, &w_icon_padding, "Icon Padding", 0, 20, cfg.icon_padding);
    {
        Widget row = XtVaCreateManagedWidget("row", xmFormWidgetClass, p, NULL);
        Widget lbl = XtVaCreateManagedWidget("Icon Mode", xmLabelWidgetClass, row,
            XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 4, NULL);
        static char *im_opts[] = {"icon","text","icon-text"};
        w_icon_mode = make_option_menu(row, "imode", im_opts, 3, cfg.icon_mode);
        XtVaSetValues(w_icon_mode,
            XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, lbl,
            XmNrightAttachment, XmATTACH_FORM, NULL);
    }
    {
        Widget row = XtVaCreateManagedWidget("row", xmFormWidgetClass, p, NULL);
        Widget lbl = XtVaCreateManagedWidget("Iconbar Position", xmLabelWidgetClass, row,
            XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 4, NULL);
        static char *ip_opts[] = {"top","bottom","left","right"};
        w_iconbar_position = make_option_menu(row, "ipos", ip_opts, 4, cfg.iconbar_position);
        XtVaSetValues(w_iconbar_position,
            XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, lbl,
            XmNrightAttachment, XmATTACH_FORM, NULL);
    }
    make_scale(p, &w_num_workspaces, "Workspaces", 1, 20, cfg.num_workspaces);
    make_scale(p, &w_master_ratio, "Master Ratio %", 10, 90, (int)(cfg.master_ratio*100));
    XtUnmanageChild(p);
}

/* ── Palette UI ────────────────────────────────────────────────────────── */

/* Forward declarations (defined later) */
static void push_cfg_to_widgets(void);
static void read_gui_state(void);
static void set_option_menu_idx(Widget om, int idx);
static void rebuild_palette_menu(void);

static void palette_select_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    int idx = (int)(intptr_t)client_data;
    if (idx < 0 || idx >= num_palettes) return;
    current_palette_idx = idx;
    apply_palette_to_cfg(idx);
    push_cfg_to_widgets();
    rebuild_palette_menu();
}

static void save_ok_cb(Widget dlg_w, XtPointer client, XtPointer call) {
    (void)client;
    XmSelectionBoxCallbackStruct *sel = (XmSelectionBoxCallbackStruct *)call;
    char *name = NULL;
    if (XmStringGetLtoR(sel->value, XmFONTLIST_DEFAULT_TAG, &name) && name && name[0]) {
        if (num_palettes < MAX_PALETTES) {
            Palette *pal = &palettes[num_palettes];
            memset(pal, 0, sizeof(*pal));
            pal->builtin = 0;
            COPY_TRUNK(pal->name, name);
            capture_current_palette(pal);
            save_palette_file(pal);
            num_palettes++;
            current_palette_idx = num_palettes - 1;
            rebuild_palette_menu();
        }
        XtFree(name);
    }
    XtDestroyWidget(dlg_w);
}

static void save_cancel_cb(Widget dlg_w, XtPointer client, XtPointer call) {
    (void)client; (void)call;
    XtDestroyWidget(dlg_w);
}

static void save_palette_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data; (void)call_data;
    read_gui_state();

    Widget dlg = XmCreatePromptDialog(toplevel, "save_palette", NULL, 0);
    XtVaSetValues(XtParent(dlg), XmNtitle, "Save Palette", NULL);
    XmString msg = XmStringCreateLocalized("Enter palette name:");
    XtVaSetValues(dlg, XmNselectionLabelString, msg, NULL);
    XmStringFree(msg);

    XtAddCallback(dlg, XmNokCallback, save_ok_cb, NULL);
    XtAddCallback(dlg, XmNcancelCallback, save_cancel_cb, NULL);

    XtManageChild(dlg);
}

static void delete_palette_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data; (void)call_data;
    if (current_palette_idx < 0 || current_palette_idx >= num_palettes) return;
    if (palettes[current_palette_idx].builtin) return; /* can't delete built-in */

    /* remove the file */
    const char *home = getenv("HOME");
    if (home) {
        char dirpath[PATH_MAX];
        snprintf(dirpath, sizeof(dirpath), "%s/.config/rondo/palettes", home);
        char safe_name[MAX_PALETTE_NAME];
        const char *src = palettes[current_palette_idx].name;
        char *dst = safe_name;
        while (*src && (dst - safe_name) < (int)sizeof(safe_name) - 1) {
            if ((*src >= 'a' && *src <= 'z') || (*src >= 'A' && *src <= 'Z') ||
                (*src >= '0' && *src <= '9') || *src == '-' || *src == '_')
                *dst++ = *src;
            else
                *dst++ = '_';
            src++;
        }
        *dst = '\0';
        char fpath[PATH_MAX];
        snprintf(fpath, sizeof(fpath), "%s/%s.palette", dirpath, safe_name);
        unlink(fpath);
    }

    /* remove from array */
    for (int i = current_palette_idx; i < num_palettes - 1; i++)
        palettes[i] = palettes[i + 1];
    num_palettes--;

    /* switch to CDE Classic */
    current_palette_idx = 0;
    apply_palette_to_cfg(0);
    push_cfg_to_widgets();
    rebuild_palette_menu();
}

static void rebuild_palette_menu(void) {
    if (!w_palette_menu) return;

    /* Destroy the old pulldown menu */
    Widget old_pd = NULL;
    XtVaGetValues(w_palette_menu, XmNsubMenuId, &old_pd, NULL);
    if (old_pd) XtDestroyWidget(old_pd);

    /* Create a new pulldown */
    Widget pulldown = XmCreatePulldownMenu(XtParent(w_palette_menu), "pal_pd", NULL, 0);

    for (int i = 0; i < num_palettes; i++) {
        XmString xms = XmStringCreateLocalized(palettes[i].name);
        Widget btn = XtVaCreateManagedWidget(palettes[i].name,
            xmPushButtonGadgetClass, pulldown,
            XmNlabelString, xms, NULL);
        XmStringFree(xms);
        XtAddCallback(btn, XmNactivateCallback, palette_select_cb, (XtPointer)(intptr_t)i);
    }

    /* Add "Custom" entry if current colors don't match any palette */
    if (current_palette_idx < 0) {
        XmString xms = XmStringCreateLocalized("Custom");
        (void)XtVaCreateManagedWidget("Custom",
            xmPushButtonGadgetClass, pulldown,
            XmNlabelString, xms, NULL);
        XmStringFree(xms);
    }

    XtVaSetValues(w_palette_menu, XmNsubMenuId, pulldown, NULL);

    /* Set the selected item */
    int sel = current_palette_idx >= 0 ? current_palette_idx : num_palettes;
    set_option_menu_idx(w_palette_menu, sel);

    /* Update delete button sensitivity */
    if (w_palette_delete_btn) {
        int can_delete = (current_palette_idx >= 0 &&
                          current_palette_idx < num_palettes &&
                          !palettes[current_palette_idx].builtin);
        XtVaSetValues(w_palette_delete_btn, XmNsensitive, can_delete ? True : False, NULL);
    }
}

static void create_appearance_panel(Widget parent) {
    Widget p = make_scroll_form(parent);
    panels[2] = p;
    num_color_widgets = 0;

    /* ── Palette selector row ── */
    {
        Widget pal_row = XtVaCreateManagedWidget("palrow", xmFormWidgetClass, p,
            XmNfractionBase, 100, NULL);
        (void)XtVaCreateManagedWidget("Palette:", xmLabelWidgetClass, pal_row,
            XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 4, NULL);

        /* Build the option menu items from palettes[] */
        char *pal_labels[MAX_PALETTES + 1];
        for (int i = 0; i < num_palettes; i++)
            pal_labels[i] = palettes[i].name;
        int pal_count = num_palettes;
        if (current_palette_idx < 0)
            pal_labels[pal_count++] = (char *)"Custom";

        w_palette_menu = make_option_menu(pal_row, "palette", pal_labels, pal_count,
                                           current_palette_idx >= 0 ? current_palette_idx : num_palettes);
        XtVaSetValues(w_palette_menu,
            XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_POSITION, XmNleftPosition, 20,
            XmNrightAttachment, XmATTACH_POSITION, XmNrightPosition, 55, NULL);

        /* Attach palette_select_cb to all buttons in the pulldown */
        Widget pd = NULL;
        XtVaGetValues(w_palette_menu, XmNsubMenuId, &pd, NULL);
        if (pd) {
            CompositeWidget cw = (CompositeWidget)pd;
            for (Cardinal i = 0; i < cw->composite.num_children && (int)i < pal_count; i++) {
                XtRemoveAllCallbacks(cw->composite.children[i], XmNactivateCallback);
                XtAddCallback(cw->composite.children[i], XmNactivateCallback,
                              palette_select_cb, (XtPointer)(intptr_t)i);
            }
        }

        Widget save_btn = XtVaCreateManagedWidget("Save Palette...", xmPushButtonWidgetClass, pal_row,
            XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_POSITION, XmNleftPosition, 57,
            XmNrightAttachment, XmATTACH_POSITION, XmNrightPosition, 80, NULL);
        XtAddCallback(save_btn, XmNactivateCallback, save_palette_cb, NULL);

        w_palette_delete_btn = XtVaCreateManagedWidget("Delete", xmPushButtonWidgetClass, pal_row,
            XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_POSITION, XmNleftPosition, 82,
            XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 4, NULL);
        XtAddCallback(w_palette_delete_btn, XmNactivateCallback, delete_palette_cb, NULL);
        /* grey out Delete for built-in palettes */
        XtVaSetValues(w_palette_delete_btn,
            XmNsensitive, (current_palette_idx >= 0 &&
                          current_palette_idx < num_palettes &&
                          !palettes[current_palette_idx].builtin) ? True : False, NULL);
    }

    /* separator */
    XtVaCreateManagedWidget("sep", xmSeparatorWidgetClass, p, NULL);

    add_color_widget(p, "Title Focus", "title-focus", cfg.col_title_focus);
    add_color_widget(p, "Title Unfocus", "title-unfocus", cfg.col_title_unfocus);
    add_color_widget(p, "Title FG", "title-fg", cfg.col_title_fg);
    add_color_widget(p, "Frame Light", "frame-light", cfg.col_frame_light);
    add_color_widget(p, "Frame Shadow", "frame-shadow", cfg.col_frame_shadow);
    add_color_widget(p, "Frame BG", "frame-bg", cfg.col_frame_bg);
    add_color_widget(p, "Active Light", "active-light", cfg.col_active_light);
    add_color_widget(p, "Active Shadow", "active-shadow", cfg.col_active_shadow);
    add_color_widget(p, "Button FG", "btn-fg", cfg.col_btn_fg);
    add_color_widget(p, "Bar BG", "bar-bg", cfg.col_bar_bg);
    add_color_widget(p, "Bar FG", "bar-fg", cfg.col_bar_fg);
    add_color_widget(p, "Bar WS Active", "bar-ws-active", cfg.col_bar_ws_active);
    add_color_widget(p, "Bar WS Occupied", "bar-ws-occupied", cfg.col_bar_ws_occupied);
    add_color_widget(p, "Bar WS Idle", "bar-ws-idle", cfg.col_bar_ws_idle);
    add_color_widget(p, "Bar WS BG", "bar-ws-bg", cfg.col_bar_ws_bg);
    add_color_widget(p, "Bar Border Light", "bar-border-light", cfg.col_bar_border_light);
    add_color_widget(p, "Bar Border Shadow", "bar-border-shadow", cfg.col_bar_border_shadow);
    add_color_widget(p, "Bar Fill", "bar-fill", cfg.col_bar_fill);
    add_color_widget(p, "Menu BG", "menu-bg", cfg.col_menu_bg);
    add_color_widget(p, "Iconbar BG", "iconbar-bg", cfg.col_iconbar_bg);
    add_color_widget(p, "Feedback BG", "fb-bg", cfg.col_fb_bg);
    add_color_widget(p, "Feedback Light", "fb-light", cfg.col_fb_light);
    add_color_widget(p, "Feedback Shadow", "fb-shadow", cfg.col_fb_shadow);
    add_color_widget(p, "Feedback FG", "fb-fg", cfg.col_fb_fg);
    add_color_widget(p, "Tooltip BG", "tooltip-bg", cfg.col_tooltip_bg);
    add_color_widget(p, "Tooltip FG", "tooltip-fg", cfg.col_tooltip_fg);
    add_color_widget(p, "Tooltip Border", "tooltip-border", cfg.col_tooltip_border);
    add_color_widget(p, "Dialog BG", "dialog-bg", cfg.col_dialog_bg);
    XtUnmanageChild(p);
}

static void create_programs_panel(Widget parent) {
    Widget p = make_scroll_form(parent);
    panels[3] = p;
    make_text_row(p, &w_font, "Font", cfg.font);
    make_text_row(p, &w_terminal, "Terminal", cfg.terminal);
    make_text_row(p, &w_launcher, "Launcher", cfg.launcher);
    make_text_row(p, &w_modkey, "Modkey", cfg.modkey);
    make_text_row(p, &w_clock_format, "Clock Format", cfg.clock_format);
    make_text_row(p, &w_tooltip_font, "Tooltip Font", cfg.tooltip_font);
    XtUnmanageChild(p);
}

static void create_compositing_panel(Widget parent) {
    Widget p = make_scroll_form(parent);
    panels[4] = p;
    w_fade_enabled = XtVaCreateManagedWidget("Fade Enabled", xmToggleButtonWidgetClass, p,
        XmNset, cfg.fade_enabled, NULL);
    make_scale(p, &w_fade_in_ms, "Fade In (ms)", 0, 1000, cfg.fade_in_ms);
    make_scale(p, &w_fade_out_ms, "Fade Out (ms)", 0, 1000, cfg.fade_out_ms);
    make_scale(p, &w_tooltip_delay, "Tooltip Delay (ms)", 0, 10000, cfg.tooltip_delay);
    XtUnmanageChild(p);
}

static void create_background_panel(Widget parent) {
    Widget p = make_scroll_form(parent);
    panels[5] = p;
    Widget row = XtVaCreateManagedWidget("row", xmFormWidgetClass, p, NULL);
    Widget lbl = XtVaCreateManagedWidget("Background Mode", xmLabelWidgetClass, row,
        XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 4, NULL);
    static char *bg_opts[] = {"Solid","Pattern","Image"};
    Widget bg_om = make_option_menu(row, "bgmode", bg_opts, 3, cfg.bg_mode);
    w_bg_mode[0] = bg_om;
    XtVaSetValues(bg_om,
        XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, lbl,
        XmNrightAttachment, XmATTACH_FORM, NULL);

    {
        Widget row2 = XtVaCreateManagedWidget("row2", xmFormWidgetClass, p, NULL);
        Widget lbl2 = XtVaCreateManagedWidget("Pattern", xmLabelWidgetClass, row2,
            XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 4, NULL);
        static char *pat_opts[] = {"checkerboard","diagonal","horizontal","vertical","dots","crosshatch","weave"};
        w_bg_pattern = make_option_menu(row2, "pat", pat_opts, 7, cfg.bg_pattern);
        XtVaSetValues(w_bg_pattern,
            XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, lbl2,
            XmNrightAttachment, XmATTACH_FORM, NULL);
    }
    make_text_row(p, &w_bg_color, "Color 1", cfg.bg_color);
    make_text_row(p, &w_bg_color2, "Color 2", cfg.bg_color2);
    {
        Widget row = XtVaCreateManagedWidget("row", xmFormWidgetClass, p,
            XmNfractionBase, 100, NULL);
        (void)XtVaCreateManagedWidget("Image Path", xmLabelWidgetClass, row,
            XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 4,
            XmNrightAttachment, XmATTACH_POSITION, XmNrightPosition, 35, NULL);
        w_bg_image_path = XtVaCreateManagedWidget("val", xmTextWidgetClass, row,
            XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_POSITION, XmNleftPosition, 36,
            XmNrightAttachment, XmATTACH_WIDGET, XmNrightWidget, NULL,
            XmNeditMode, XmSINGLE_LINE_EDIT,
            XmNvalue, cfg.bg_image_path, NULL);
        Widget browse_btn = XtVaCreateManagedWidget("Browse...", xmPushButtonWidgetClass, row,
            XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 4,
            NULL);
        /* attach text field's right to the browse button */
        XtVaSetValues(w_bg_image_path, XmNrightWidget, browse_btn, NULL);
        XtAddCallback(browse_btn, XmNactivateCallback, browse_btn_cb, (XtPointer)w_bg_image_path);
    }
    {
        Widget row3 = XtVaCreateManagedWidget("row3", xmFormWidgetClass, p, NULL);
        Widget lbl3 = XtVaCreateManagedWidget("Image Placement", xmLabelWidgetClass, row3,
            XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 4, NULL);
        static char *im_opts[] = {"centered","scaled","tiled","stretched","scale-filled"};
        w_bg_image_mode = make_option_menu(row3, "immode", im_opts, 5, cfg.bg_image_mode);
        XtVaSetValues(w_bg_image_mode,
            XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, lbl3,
            XmNrightAttachment, XmATTACH_FORM, NULL);
    }
    XtUnmanageChild(p);
}

/* ── Keybindings panel ─────────────────────────────────────────────────── */

/* Available action names (must match rondo's action_table in config.c) */
static const char *action_names[] = {
    "spawn","killclient","focusstack","cyclewindows","lowerwindow",
    "togglefloat","incmaster","zoom","togglefullscreen",
    "viewworkspace","movetoworkspace","quit","swapbar",
    "setlayout","cyclelayout","reloadconfig","togglecompositing"
};
#define NUM_ACTION_NAMES ((int)(sizeof(action_names)/sizeof(action_names[0])))

/* Rebuild the XmList from the binds[] array. */
static void refresh_bind_list(void) {
    if (!w_bind_list) return;
    XmListDeleteAllItems(w_bind_list);
    for (int i = 0; i < num_binds; i++) {
        char line[512];
        if (binds[i].arg[0])
            snprintf(line, sizeof(line), "%s+%s  →  %s %s",
                     binds[i].mod, binds[i].key, binds[i].action, binds[i].arg);
        else
            snprintf(line, sizeof(line), "%s+%s  →  %s",
                     binds[i].mod, binds[i].key, binds[i].action);
        XmString xms = XmStringCreateLocalized(line);
        XmListAddItem(w_bind_list, xms, i + 1);
        XmStringFree(xms);
    }
}

/* Callback data for the bind edit dialog */
typedef struct {
    Widget dlg, mod_om, key_text, act_om, arg_text;
    int edit_idx;
} DlgData;

static void bind_ok_cb(Widget w, XtPointer cd, XtPointer cbs) {
    (void)w; (void)cbs;
    DlgData *d = (DlgData *)cd;
    char *key = XmTextGetString(d->key_text);
    char *arg = XmTextGetString(d->arg_text);
    int mi = option_menu_index(d->mod_om);
    int ai = option_menu_index(d->act_om);

    if (!key || !key[0]) { XtFree(key); XtFree(arg); free(d); return; }

    static char *mod_opts[] = {"Alt","Alt+Shift","Super","Ctrl","Ctrl+Alt","Shift"};

    BindEntry b;
    strncpy(b.mod, mod_opts[mi < 6 ? mi : 0], sizeof(b.mod)-1); b.mod[sizeof(b.mod)-1]='\0';
    strncpy(b.key, key, sizeof(b.key)-1); b.key[sizeof(b.key)-1]='\0';
    strncpy(b.action, action_names[ai < NUM_ACTION_NAMES ? ai : 0], sizeof(b.action)-1); b.action[sizeof(b.action)-1]='\0';
    strncpy(b.arg, arg ? arg : "", sizeof(b.arg)-1); b.arg[sizeof(b.arg)-1]='\0';
    XtFree(key); XtFree(arg);

    if (d->edit_idx >= 0 && d->edit_idx < num_binds) {
        binds[d->edit_idx] = b;
    } else {
        if (num_binds < 128)
            binds[num_binds++] = b;
    }
    refresh_bind_list();
    free(d);
}

static void bind_cancel_cb(Widget w, XtPointer cd, XtPointer cbs) {
    (void)w; (void)cbs; free(cd);
}

static void bind_dialog(Widget parent, int edit_idx) {
    Widget dlg = XmCreateMessageDialog(
        parent, edit_idx >= 0 ? "Edit Binding" : "Add Binding", NULL, 0);
    /* remove default cancel/help */
    XtUnmanageChild(XmMessageBoxGetChild(dlg, XmDIALOG_CANCEL_BUTTON));
    XtUnmanageChild(XmMessageBoxGetChild(dlg, XmDIALOG_HELP_BUTTON));

    Widget form = XtVaCreateManagedWidget("bform", xmFormWidgetClass, dlg, NULL);

    /* Modifier */
    Widget mod_lbl = XtVaCreateManagedWidget("Modifier:", xmLabelWidgetClass, form,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 8, NULL);
    static char *mod_opts[] = {"Alt","Alt+Shift","Super","Ctrl","Ctrl+Alt","Shift"};
    Widget mod_om = make_option_menu(form, "mod", mod_opts, 6, 0);
    XtVaSetValues(mod_om,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 4,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, mod_lbl,
        XmNleftOffset, 4,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 8,
        XmNmarginWidth, 2, XmNmarginHeight, 2, NULL);

    /* Key */
    Widget key_lbl = XtVaCreateManagedWidget("Key:", xmLabelWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, mod_om, XmNtopOffset, 12,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 8, NULL);
    Widget key_text = XtVaCreateManagedWidget("key", xmTextWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, mod_om, XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, key_lbl,
        XmNleftOffset, 4,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 8,
        XmNeditMode, XmSINGLE_LINE_EDIT,
        XmNmaxLength, 63, NULL);

    /* Action */
    Widget act_lbl = XtVaCreateManagedWidget("Action:", xmLabelWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, key_text, XmNtopOffset, 12,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 8, NULL);
    Widget act_om = make_option_menu(form, "act",
        (char **)action_names, NUM_ACTION_NAMES, 0);
    XtVaSetValues(act_om,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, key_text, XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, act_lbl,
        XmNleftOffset, 4,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 8,
        XmNmarginWidth, 2, XmNmarginHeight, 2, NULL);

    /* Arg */
    Widget arg_lbl = XtVaCreateManagedWidget("Arg:", xmLabelWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, act_om, XmNtopOffset, 12,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 8, NULL);
    Widget arg_text = XtVaCreateManagedWidget("arg", xmTextWidgetClass, form,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, act_om, XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, arg_lbl,
        XmNleftOffset, 4,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 8,
        XmNeditMode, XmSINGLE_LINE_EDIT,
        XmNmaxLength, 127, NULL);

    /* pre-fill if editing */
    if (edit_idx >= 0 && edit_idx < num_binds) {
        XmTextSetString(key_text, binds[edit_idx].key);
        XmTextSetString(arg_text, binds[edit_idx].arg);
        for (int mi = 0; mi < 6; mi++) {
            if (strcmp(binds[edit_idx].mod, mod_opts[mi]) == 0) {
                set_option_menu_idx(mod_om, mi);
                break;
            }
        }
        for (int ai = 0; ai < NUM_ACTION_NAMES; ai++) {
            if (strcmp(binds[edit_idx].action, action_names[ai]) == 0) {
                set_option_menu_idx(act_om, ai);
                break;
            }
        }
    }

    DlgData *dd = malloc(sizeof(DlgData));
    dd->dlg = dlg; dd->mod_om = mod_om; dd->key_text = key_text;
    dd->act_om = act_om; dd->arg_text = arg_text; dd->edit_idx = edit_idx;

    Widget ok_btn = XmMessageBoxGetChild(dlg, XmDIALOG_OK_BUTTON);
    XtAddCallback(ok_btn, XmNactivateCallback, bind_ok_cb, (XtPointer)dd);
    XtAddCallback(dlg, XmNcancelCallback, bind_cancel_cb, (XtPointer)dd);

    XtManageChild(dlg);
}

static void bind_add_cb(Widget w, XtPointer cd, XtPointer cbs) {
    (void)w; (void)cd; (void)cbs;
    bind_dialog(w, -1);
}

static void bind_edit_cb(Widget w, XtPointer cd, XtPointer cbs) {
    (void)w; (void)cd; (void)cbs;
    int *sel = NULL, nsel = 0;
    if (!XmListGetSelectedPos(w_bind_list, &sel, &nsel) || nsel < 1) return;
    int idx = sel[0] - 1;
    XtFree((char *)sel);
    if (idx >= 0 && idx < num_binds)
        bind_dialog(w, idx);
}

static void bind_remove_cb(Widget w, XtPointer cd, XtPointer cbs) {
    (void)w; (void)cd; (void)cbs;
    int *sel = NULL, nsel = 0;
    if (!XmListGetSelectedPos(w_bind_list, &sel, &nsel) || nsel < 1) return;
    int idx = sel[0] - 1;
    XtFree((char *)sel);
    if (idx < 0 || idx >= num_binds) return;
    for (int i = idx; i < num_binds - 1; i++)
        binds[i] = binds[i + 1];
    num_binds--;
    refresh_bind_list();
}

static void create_keybindings_panel(Widget parent, Widget sep, Widget btn_row) {
    /* This panel lives directly in main_form (not inside scroll_win)
     * so the list can stretch to fill the window. */
    Widget p = XtVaCreateWidget("panel", xmFormWidgetClass, parent,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, sep,
        XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget, btn_row,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM,
        NULL);
    panels[6] = p;

    /* Button row at bottom */
    Widget kbtn_row = XtVaCreateManagedWidget("btns", xmRowColumnWidgetClass, p,
        XmNorientation, XmHORIZONTAL, XmNpacking, XmPACK_TIGHT,
        XmNbottomAttachment, XmATTACH_FORM, XmNbottomOffset, 4,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 4,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 4,
        NULL);
    w_bind_add_btn = XtVaCreateManagedWidget("Add", xmPushButtonWidgetClass, kbtn_row, NULL);
    w_bind_edit_btn = XtVaCreateManagedWidget("Edit", xmPushButtonWidgetClass, kbtn_row, NULL);
    w_bind_remove_btn = XtVaCreateManagedWidget("Remove", xmPushButtonWidgetClass, kbtn_row, NULL);
    XtAddCallback(w_bind_add_btn, XmNactivateCallback, bind_add_cb, NULL);
    XtAddCallback(w_bind_edit_btn, XmNactivateCallback, bind_edit_cb, NULL);
    XtAddCallback(w_bind_remove_btn, XmNactivateCallback, bind_remove_cb, NULL);

    /* Scrolled list fills remaining space above buttons */
    Arg list_args[2]; int list_n = 0;
    XtSetArg(list_args[list_n], XmNvisibleItemCount, 15); list_n++;
    XtSetArg(list_args[list_n], XmNselectionPolicy, XmSINGLE_SELECT); list_n++;
    w_bind_list = XmCreateScrolledList(p, "bind_list", list_args, list_n);
    XtVaSetValues(XtParent(w_bind_list),
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 4,
        XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget, kbtn_row, XmNbottomOffset, 4,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 4,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 4,
        NULL);
    XtManageChild(w_bind_list);

    refresh_bind_list();
    XtUnmanageChild(p);
}

static int scale_val(Widget w) { int v=0; XtVaGetValues(w, XmNvalue, &v, NULL); return v; }

static void set_option_menu_idx(Widget om, int idx) {
    Widget pulldown = NULL;
    XtVaGetValues(om, XmNsubMenuId, &pulldown, NULL);
    if (!pulldown) return;
    CompositeWidget cw = (CompositeWidget)pulldown;
    if (idx >= 0 && (Cardinal)idx < cw->composite.num_children) {
        XtVaSetValues(om, XmNmenuHistory, cw->composite.children[idx], NULL);
    }
}

static void push_cfg_to_widgets(void) {
    /* scales */
    XtVaSetValues(w_frame_width, XmNvalue, cfg.frame_width, NULL);
    XtVaSetValues(w_title_height, XmNvalue, cfg.title_height, NULL);
    XtVaSetValues(w_bar_height, XmNvalue, cfg.bar_height, NULL);
    XtVaSetValues(w_btn_width, XmNvalue, cfg.btn_width, NULL);
    XtVaSetValues(w_btn_height, XmNvalue, cfg.btn_height, NULL);
    XtVaSetValues(w_bar_border_width, XmNvalue, cfg.bar_border_width, NULL);
    XtVaSetValues(w_bar_corner_size, XmNvalue, cfg.bar_corner_size, NULL);
    XtVaSetValues(w_bar_btn_width, XmNvalue, cfg.bar_btn_width, NULL);
    XtVaSetValues(w_icon_width, XmNvalue, cfg.icon_width, NULL);
    XtVaSetValues(w_icon_height, XmNvalue, cfg.icon_height, NULL);
    XtVaSetValues(w_icon_padding, XmNvalue, cfg.icon_padding, NULL);
    XtVaSetValues(w_num_workspaces, XmNvalue, cfg.num_workspaces, NULL);
    XtVaSetValues(w_master_ratio, XmNvalue, (int)(cfg.master_ratio * 100), NULL);
    XtVaSetValues(w_fade_in_ms, XmNvalue, cfg.fade_in_ms, NULL);
    XtVaSetValues(w_fade_out_ms, XmNvalue, cfg.fade_out_ms, NULL);
    XtVaSetValues(w_tooltip_delay, XmNvalue, cfg.tooltip_delay, NULL);
    /* toggles */
    XmToggleButtonSetState(w_show_bar, cfg.show_bar, True);
    XmToggleButtonSetState(w_fade_enabled, cfg.fade_enabled, True);
    /* option menus */
    set_option_menu_idx(w_bar_position, cfg.bar_position);
    set_option_menu_idx(w_icon_mode, cfg.icon_mode);
    set_option_menu_idx(w_iconbar_position, cfg.iconbar_position);
    set_option_menu_idx(w_bg_mode[0], cfg.bg_mode);
    set_option_menu_idx(w_bg_pattern, cfg.bg_pattern);
    set_option_menu_idx(w_bg_image_mode, cfg.bg_image_mode);
    /* text fields */
    XmTextSetString(w_font, cfg.font);
    XmTextSetString(w_terminal, cfg.terminal);
    XmTextSetString(w_launcher, cfg.launcher);
    XmTextSetString(w_modkey, cfg.modkey);
    XmTextSetString(w_clock_format, cfg.clock_format);
    XmTextSetString(w_tooltip_font, cfg.tooltip_font);
    XmTextSetString(w_bg_color, cfg.bg_color);
    XmTextSetString(w_bg_color2, cfg.bg_color2);
    XmTextSetString(w_bg_image_path, cfg.bg_image_path);
    /* color widgets */
    for (int i = 0; i < num_color_widgets; i++) {
        XmTextSetString(color_widgets[i].text, color_widgets[i].value);
        XColor xc;
        if (XParseColor(XtDisplay(color_widgets[i].preview),
                        DefaultColormapOfScreen(XtScreen(color_widgets[i].preview)),
                        color_widgets[i].value, &xc)) {
            XAllocColor(XtDisplay(color_widgets[i].preview),
                        DefaultColormapOfScreen(XtScreen(color_widgets[i].preview)), &xc);
            XtVaSetValues(color_widgets[i].preview, XmNbackground, xc.pixel, NULL);
        }
    }
}

static void read_gui_state(void) {
    cfg.frame_width = scale_val(w_frame_width);
    cfg.title_height = scale_val(w_title_height);
    cfg.bar_height = scale_val(w_bar_height);
    cfg.btn_width = scale_val(w_btn_width);
    cfg.btn_height = scale_val(w_btn_height);
    cfg.bar_border_width = scale_val(w_bar_border_width);
    cfg.bar_corner_size = scale_val(w_bar_corner_size);
    cfg.bar_btn_width = scale_val(w_bar_btn_width);
    cfg.icon_width = scale_val(w_icon_width);
    cfg.icon_height = scale_val(w_icon_height);
    cfg.icon_padding = scale_val(w_icon_padding);
    cfg.num_workspaces = scale_val(w_num_workspaces);
    cfg.master_ratio = scale_val(w_master_ratio) / 100.0f;
    cfg.show_bar = XmToggleButtonGetState(w_show_bar);
    cfg.fade_enabled = XmToggleButtonGetState(w_fade_enabled);
    cfg.fade_in_ms = scale_val(w_fade_in_ms);
    cfg.fade_out_ms = scale_val(w_fade_out_ms);
    cfg.tooltip_delay = scale_val(w_tooltip_delay);
    /* option menus */
    cfg.bar_position = option_menu_index(w_bar_position);
    cfg.icon_mode = option_menu_index(w_icon_mode);
    cfg.iconbar_position = option_menu_index(w_iconbar_position);
    cfg.bg_mode = option_menu_index(w_bg_mode[0]);
    cfg.bg_pattern = option_menu_index(w_bg_pattern);
    cfg.bg_image_mode = option_menu_index(w_bg_image_mode);
    /* text fields */
    { char *s = XmTextGetString(w_font); strncpy(cfg.font,s,sizeof(cfg.font)-1); cfg.font[sizeof(cfg.font)-1]='\0'; XtFree(s); }
    { char *s = XmTextGetString(w_terminal); strncpy(cfg.terminal,s,sizeof(cfg.terminal)-1); cfg.terminal[sizeof(cfg.terminal)-1]='\0'; XtFree(s); }
    { char *s = XmTextGetString(w_launcher); strncpy(cfg.launcher,s,sizeof(cfg.launcher)-1); cfg.launcher[sizeof(cfg.launcher)-1]='\0'; XtFree(s); }
    { char *s = XmTextGetString(w_modkey); strncpy(cfg.modkey,s,sizeof(cfg.modkey)-1); cfg.modkey[sizeof(cfg.modkey)-1]='\0'; XtFree(s); }
    { char *s = XmTextGetString(w_clock_format); strncpy(cfg.clock_format,s,sizeof(cfg.clock_format)-1); cfg.clock_format[sizeof(cfg.clock_format)-1]='\0'; XtFree(s); }
    { char *s = XmTextGetString(w_tooltip_font); strncpy(cfg.tooltip_font,s,sizeof(cfg.tooltip_font)-1); cfg.tooltip_font[sizeof(cfg.tooltip_font)-1]='\0'; XtFree(s); }
    { char *s = XmTextGetString(w_bg_color); strncpy(cfg.bg_color,s,sizeof(cfg.bg_color)-1); cfg.bg_color[sizeof(cfg.bg_color)-1]='\0'; XtFree(s); }
    { char *s = XmTextGetString(w_bg_color2); strncpy(cfg.bg_color2,s,sizeof(cfg.bg_color2)-1); cfg.bg_color2[sizeof(cfg.bg_color2)-1]='\0'; XtFree(s); }
    { char *s = XmTextGetString(w_bg_image_path); strncpy(cfg.bg_image_path,s,sizeof(cfg.bg_image_path)-1); cfg.bg_image_path[sizeof(cfg.bg_image_path)-1]='\0'; XtFree(s); }
    /* color widgets */
    for (int i = 0; i < num_color_widgets; i++) {
        char *s = XmTextGetString(color_widgets[i].text);
        strncpy(color_widgets[i].value, s, 63); color_widgets[i].value[63]='\0';
        XtFree(s);
    }
}

/* ── apply / save callbacks ────────────────────────────────────────── */

static void apply_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)call_data;
    int save_only = client_data ? 1 : 0;
    read_gui_state();
    if (save_config() == 0) {
        saved_cfg = cfg;
        current_palette_idx = match_current_palette();
        rebuild_palette_menu();
        if (!save_only) {
            if (ipc_send("reload") < 0)
                fprintf(stderr, "rondomgr: failed to send reload command\n");
        }
    } else
        fprintf(stderr, "rondomgr: failed to save config\n");
}

static void reset_changes_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data; (void)call_data;
    cfg = saved_cfg;
    push_cfg_to_widgets();
    current_palette_idx = match_current_palette();
    rebuild_palette_menu();
}

static void reset_defaults_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data; (void)call_data;
    set_defaults();
    push_cfg_to_widgets();
    current_palette_idx = 0;
    rebuild_palette_menu();
}

static void quit_cb(Widget w, XtPointer client_data, XtPointer call_data) {
    (void)w; (void)client_data; (void)call_data;
    XtAppSetExitFlag(app);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    init_builtin_palettes();
    load_user_palettes();
    load_config();
    saved_cfg = cfg;
    current_palette_idx = match_current_palette();

    toplevel = XtVaAppInitialize(&app, "RondoMgr",
        NULL, 0, &argc, argv, NULL,
        XmNtitle, "Rondo WM Configuration",
        XmNwidth, 500, XmNheight, 600,
        NULL);

    main_form = XtVaCreateManagedWidget("main", xmFormWidgetClass, toplevel,
        XmNfractionBase, 100, NULL);

    /* section buttons — in a scrolled row so they scroll when narrow */
    {
        Arg sw_args[2];
        int sw_n = 0;
        XtSetArg(sw_args[sw_n], XmNscrollingPolicy, XmAUTOMATIC); sw_n++;
        XtSetArg(sw_args[sw_n], XmNscrollBarDisplayPolicy, XmAS_NEEDED); sw_n++;
        section_sw = XmCreateScrolledWindow(main_form, "section_sw", sw_args, sw_n);
        XtVaSetValues(section_sw,
            XmNtopAttachment, XmATTACH_FORM,
            XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM,
            NULL);
        XtManageChild(section_sw);

        section_rc = XtVaCreateManagedWidget("sections", xmRowColumnWidgetClass, section_sw,
            XmNorientation, XmHORIZONTAL, XmNpacking, XmPACK_COLUMN,
            XmNnumColumns, 1, XmNentryAlignment, XmALIGNMENT_CENTER,
            XmNradioAlwaysOne, True,
            NULL);

        const char *section_names[] = {"Dimensions","Bar","Appearance","Programs","Compositing","Background","Keybindings"};
        for (int i = 0; i < 7; i++) {
            Widget btn = XtVaCreateManagedWidget(section_names[i],
                xmPushButtonWidgetClass, section_rc, NULL);
            XtAddCallback(btn, XmNactivateCallback, section_cb, (XtPointer)(intptr_t)i);
        }

        XmScrolledWindowSetAreas(section_sw, NULL, NULL, section_rc);
    }

    /* separator */
    Widget sep = XtVaCreateManagedWidget("sep", xmSeparatorWidgetClass, main_form,
        XmNtopAttachment, XmATTACH_WIDGET,
        XmNtopWidget, section_sw,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM,
        NULL);

    /* bottom button row — create before content so content can attach to it */
    Widget btn_row = XtVaCreateManagedWidget("btns", xmRowColumnWidgetClass, main_form,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM,
        XmNorientation, XmHORIZONTAL, XmNpacking, XmPACK_TIGHT,
        NULL);

    Widget apply_btn = XtVaCreateManagedWidget("Apply && Reload", xmPushButtonWidgetClass, btn_row, NULL);
    XtAddCallback(apply_btn, XmNactivateCallback, apply_cb, NULL);

    Widget save_btn = XtVaCreateManagedWidget("Save Only", xmPushButtonWidgetClass, btn_row, NULL);
    XtAddCallback(save_btn, XmNactivateCallback, apply_cb, (XtPointer)1);

    Widget reset_changes_btn = XtVaCreateManagedWidget("Reset Changes", xmPushButtonWidgetClass, btn_row, NULL);
    XtAddCallback(reset_changes_btn, XmNactivateCallback, reset_changes_cb, NULL);

    Widget reset_defaults_btn = XtVaCreateManagedWidget("Reset Defaults", xmPushButtonWidgetClass, btn_row, NULL);
    XtAddCallback(reset_defaults_btn, XmNactivateCallback, reset_defaults_cb, NULL);

    Widget cancel_btn = XtVaCreateManagedWidget("Cancel", xmPushButtonWidgetClass, btn_row, NULL);
    XtAddCallback(cancel_btn, XmNactivateCallback, quit_cb, NULL);

    /* scrolled content area — fills space between separator and buttons */
    Arg sw_args[2];
    int sw_n = 0;
    XtSetArg(sw_args[sw_n], XmNscrollingPolicy, XmAUTOMATIC); sw_n++;
    scroll_win = XmCreateScrolledWindow(main_form, "scroll", sw_args, sw_n);
    XtVaSetValues(scroll_win,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, sep,
        XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget, btn_row,
        XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM,
        NULL);
    XtManageChild(scroll_win);

    create_dimensions_panel(scroll_win);
    create_bar_panel(scroll_win);
    create_appearance_panel(scroll_win);
    create_programs_panel(scroll_win);
    create_compositing_panel(scroll_win);
    create_background_panel(scroll_win);
    create_keybindings_panel(main_form, sep, btn_row);

    show_panel(0);
    XtRealizeWidget(toplevel);
    XtAppMainLoop(app);
    return 0;
}