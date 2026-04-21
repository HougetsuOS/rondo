/* rondo configuration */
#ifndef CONFIG_H
#define CONFIG_H

#include <X11/Xlib.h>

#define LENGTH(X) (sizeof(X) / sizeof((X)[0]))

/* ── runtime config variables (defined in config.c) ──────────────────── */

/* dimensions */
extern int    cfg_frame_width;
extern int    cfg_title_height;
extern int    cfg_bar_height;
extern int    cfg_btn_width;
extern int    cfg_btn_height;
extern int    cfg_bar_border_width;
extern int    cfg_bar_corner_size;
extern int    cfg_bar_btn_width;
extern int    cfg_icon_width;
extern int    cfg_icon_height;
extern int    cfg_icon_padding;
extern int    cfg_num_workspaces;
extern float  cfg_master_ratio;
extern unsigned int cfg_modkey;

/* flags */
extern int    cfg_show_bar;
extern int    cfg_fade_enabled;
extern int    cfg_fade_in_ms;
extern int    cfg_fade_out_ms;
extern int    cfg_tooltip_delay;

/* strings */
extern const char *cfg_font_name;
extern const char *cfg_term_cmd;
extern const char *cfg_menu_cmd;
extern const char *cfg_clock_format;

/* colors */
extern const char *cfg_color_title_focus;
extern const char *cfg_color_title_unfocus;
extern const char *cfg_color_title_fg;
extern const char *cfg_color_frame_light;
extern const char *cfg_color_frame_shadow;
extern const char *cfg_color_frame_bg;
extern const char *cfg_color_active_light;
extern const char *cfg_color_active_shadow;
extern const char *cfg_color_btn_fg;
extern const char *cfg_color_bar_bg;
extern const char *cfg_color_bar_fg;
extern const char *cfg_color_bar_ws_active;
extern const char *cfg_color_bar_ws_occupied;
extern const char *cfg_color_bar_ws_idle;
extern const char *cfg_color_bar_ws_bg;
extern const char *cfg_color_bar_border_light;
extern const char *cfg_color_bar_border_shadow;
extern const char *cfg_color_bar_fill;
extern const char *cfg_color_menu_bg;
extern const char *cfg_color_iconbar_bg;
extern const char *cfg_color_fb_bg;
extern const char *cfg_color_fb_light;
extern const char *cfg_color_fb_shadow;
extern const char *cfg_color_fb_fg;
extern const char *cfg_color_tooltip_bg;
extern const char *cfg_color_tooltip_fg;
extern const char *cfg_color_tooltip_border;
extern const char *cfg_tooltip_font;
extern const char *cfg_color_root_bg;
extern const char *cfg_color_root_bg2;
extern const char *cfg_color_dialog_bg;

/* ── compatibility macros ─────────────────────────────────────────────── */
/* Existing code uses FRAME_WIDTH, BAR_HEIGHT, etc. These macros redirect
 * to the runtime config variables so no other files need to change. */

#define FRAME_WIDTH    cfg_frame_width
#define TITLE_HEIGHT   cfg_title_height
#define BAR_HEIGHT     cfg_bar_height
#define BTN_WIDTH      cfg_btn_width
#define BTN_HEIGHT     cfg_btn_height
#define BAR_BORDER_WIDTH cfg_bar_border_width
#define BAR_CORNER_SIZE  cfg_bar_corner_size
#define BAR_BTN_WIDTH    cfg_bar_btn_width
#define ICON_W         cfg_icon_width
#define ICON_H         cfg_icon_height
#define ICON_PAD       cfg_icon_padding
#define NUM_WORKSPACES cfg_num_workspaces
#define MASTER_RATIO   cfg_master_ratio
#define MODKEY         cfg_modkey

#define show_bar       cfg_show_bar
#define fade_enabled   cfg_fade_enabled
#define fade_in_ms     cfg_fade_in_ms
#define fade_out_ms    cfg_fade_out_ms
#define tooltip_delay  cfg_tooltip_delay

/* effective bar height for layout: 0 when bar is hidden */
#define BAR_H (show_bar ? BAR_HEIGHT : 0)

#define CASCADE_STEP   28   /* pixel offset for cascading floating windows */
#define CASCADE_BASE   50   /* initial offset from monitor edge for cascade */

#define GAP_SIZE       8   /* layout gap — not yet configurable */

/* Color string compat macros */
#define color_title_focus    cfg_color_title_focus
#define color_title_unfocus  cfg_color_title_unfocus
#define color_title_fg       cfg_color_title_fg
#define color_frame_light    cfg_color_frame_light
#define color_frame_shadow   cfg_color_frame_shadow
#define color_frame_bg       cfg_color_frame_bg
#define color_active_light   cfg_color_active_light
#define color_active_shadow  cfg_color_active_shadow
#define color_btn_fg         cfg_color_btn_fg
#define color_bar_bg         cfg_color_bar_bg
#define color_bar_fg         cfg_color_bar_fg
#define color_bar_ws_active   cfg_color_bar_ws_active
#define color_bar_ws_occupied cfg_color_bar_ws_occupied
#define color_bar_ws_idle     cfg_color_bar_ws_idle
#define color_bar_ws_bg       cfg_color_bar_ws_bg
#define color_bar_border_light  cfg_color_bar_border_light
#define color_bar_border_shadow cfg_color_bar_border_shadow
#define color_bar_fill          cfg_color_bar_fill
#define color_menu_bg         cfg_color_menu_bg
#define color_iconbar_bg      cfg_color_iconbar_bg
#define color_fb_bg           cfg_color_fb_bg
#define color_fb_light        cfg_color_fb_light
#define color_fb_shadow       cfg_color_fb_shadow
#define color_fb_fg           cfg_color_fb_fg
#define color_tooltip_bg      cfg_color_tooltip_bg
#define color_tooltip_fg      cfg_color_tooltip_fg
#define color_tooltip_border  cfg_color_tooltip_border
#define color_root_bg         cfg_color_root_bg
#define color_root_bg2        cfg_color_root_bg2
#define color_dialog_bg       cfg_color_dialog_bg

/* font and commands */
#define font_name  cfg_font_name
#define term_cmd   cfg_term_cmd
#define menu_cmd   cfg_menu_cmd

/* ── types ───────────────────────────────────────────────────────────── */

/* argument union — must be defined before Key */
typedef union {
    int i;
    unsigned int ui;
    float f;
    const void *v;
} WmArg;

/* bar widget types and alignment */
typedef enum { BAR_ALIGN_LEFT, BAR_ALIGN_RIGHT } BarAlign;
typedef enum { BAR_WIDGET_WS, BAR_WIDGET_TITLE, BAR_WIDGET_CLOCK, BAR_WIDGET_LOAD, BAR_WIDGET_MEM, BAR_WIDGET_DISK, BAR_WIDGET_BAT, BAR_WIDGET_VOL, BAR_WIDGET_CPU, BAR_WIDGET_NET, BAR_WIDGET_TEMP } BarWidgetType;

/* icon bar display mode */
typedef enum { ICON_MODE_ICON, ICON_MODE_TEXT, ICON_MODE_ICON_TEXT } IconMode;

/* bar position: which screen edge the bar is attached to */
typedef enum { BAR_POS_TOP, BAR_POS_BOTTOM, BAR_POS_LEFT, BAR_POS_RIGHT } BarPosition;

extern IconMode cfg_icon_mode;
#define icon_mode cfg_icon_mode

extern BarPosition cfg_bar_position;
extern BarPosition cfg_iconbar_position;
#define bar_position     cfg_bar_position
#define iconbar_position  cfg_iconbar_position

typedef struct {
    BarWidgetType type;
    BarAlign align;
} BarWidget;

/* keybindings */
typedef struct {
    unsigned int mod;
    KeySym keysym;
    void (*func)(const WmArg *arg);
    WmArg arg;
} Key;

/* root menu actions */
enum {
    ROOT_NEW, ROOT_SHUFFLE_UP, ROOT_SHUFFLE_DOWN, ROOT_REFRESH,
    ROOT_RESTART, ROOT_QUIT
};

/* root menu item (for dynamic config) */
#define MENU_SEP -1

typedef struct {
    const char *label;
    int action;
} CfgMenuItem;

/* background mode */
typedef enum {
    BG_SOLID, BG_PATTERN, BG_IMAGE
} BgMode;

typedef enum {
    BG_CENTERED, BG_SCALED, BG_TILED, BG_STRETCHED, BG_SCALE_FILLED
} BgImageMode;

typedef enum {
    PAT_CHECKERBOARD, PAT_DIAGONAL_STRIPES, PAT_HORIZONTAL_STRIPES,
    PAT_VERTICAL_STRIPES, PAT_DOTS, PAT_CROSSHATCH, PAT_WEAVE
} BgPattern;

extern BgMode cfg_bg_mode;
extern BgImageMode cfg_bg_mode_image;
extern BgPattern cfg_bg_pattern;
extern const char *cfg_bg_image_path;
extern int cfg_bg_pattern_size;
extern Pixmap root_bg_pixmap;

/* ── dynamic config arrays (defined in config.c) ────────────────────── */

extern Key *cfg_keys;
extern int  cfg_num_keys;
#define keys     cfg_keys
#define num_keys cfg_num_keys

extern BarWidget *cfg_bar_widgets;
extern int        cfg_num_bar_widgets;
#define bar_widgets     cfg_bar_widgets
#define num_bar_widgets cfg_num_bar_widgets

extern CfgMenuItem *cfg_root_menu_items;
extern int          cfg_num_root_menu_items;

/* ── config function prototypes ─────────────────────────────────────── */

void cfg_init(void);
void cfg_reload(void);
void cfg_cleanup(void);
void reloadconfig(const WmArg *arg);

/* ── background function prototypes ──────────────────────────────────── */

void bg_load(void);
void bg_free(void);

#endif /* CONFIG_H */