/*
 * rondo — Scheme-like configuration file parser and runtime config
 */
#include "wm.h"
#include <limits.h>

/* ── runtime config variables ─────────────────────────────────────────── */

int    cfg_frame_width   = 6;
int    cfg_title_height  = 18;
int    cfg_bar_height    = 26;
int    cfg_btn_width     = 18;
int    cfg_btn_height   = 18;
int    cfg_bar_border_width  = 6;
int    cfg_bar_corner_size   = 24;
int    cfg_bar_btn_width     = 18;
int    cfg_icon_width   = 56;
int    cfg_icon_height   = 56;
int    cfg_icon_padding  = 4;
IconMode cfg_icon_mode   = ICON_MODE_ICON_TEXT;
BarPosition cfg_bar_position    = BAR_POS_TOP;
BarPosition cfg_iconbar_position = BAR_POS_LEFT;
int    cfg_num_workspaces = 9;
float  cfg_master_ratio = 0.55f;
unsigned int cfg_modkey = Mod1Mask;
int    cfg_show_bar = 1;
int    cfg_fade_enabled  = 1;
int    cfg_fade_in_ms    = 150;
int    cfg_fade_out_ms   = 150;
int    cfg_tooltip_delay = 3000;

const char *cfg_font_name   = "monospace:size=10";
const char *cfg_term_cmd    = "xterm";
const char *cfg_menu_cmd    = "dmenu_run";
const char *cfg_clock_format = " %a %m/%d %H:%M ";

const char *cfg_color_title_focus    = "#5F9EA0";
const char *cfg_color_title_unfocus  = "#A8A8A8";
const char *cfg_color_title_fg       = "#FFFFFF";
const char *cfg_color_frame_light    = "#D9D9D9";
const char *cfg_color_frame_shadow   = "#595959";
const char *cfg_color_frame_bg       = "#A8A8A8";
const char *cfg_color_active_light   = "#B7D4D5";
const char *cfg_color_active_shadow  = "#2F4F50";
const char *cfg_color_btn_fg         = "#000000";
const char *cfg_color_bar_bg         = "#A8A8A8";
const char *cfg_color_bar_fg         = "#FFFFFF";
const char *cfg_color_bar_ws_active  = "#FFFFFF";
const char *cfg_color_bar_ws_occupied = "#303030";
const char *cfg_color_bar_ws_idle    = "#606060";
const char *cfg_color_bar_ws_bg      = "#5F9EA0";
const char *cfg_color_bar_border_light  = "#B7D4D5";
const char *cfg_color_bar_border_shadow = "#2F4F50";
const char *cfg_color_bar_fill          = "#5F9EA0";
const char *cfg_color_menu_bg         = "#D4D4D4";
const char *cfg_color_iconbar_bg      = "#D4D4D4";
const char *cfg_color_fb_bg           = "#5F9EA0";
const char *cfg_color_fb_light        = "#B7D4D5";
const char *cfg_color_fb_shadow       = "#2F4F50";
const char *cfg_color_fb_fg           = "#FFFFFF";
const char *cfg_color_tooltip_bg      = "#5F9EA0";
const char *cfg_color_tooltip_fg      = "#FFFFFF";
const char *cfg_color_tooltip_border  = "#2F4F50";
const char *cfg_tooltip_font          = NULL;
const char *cfg_color_root_bg         = "#303030";
const char *cfg_color_root_bg2        = "#404040";
const char *cfg_color_dialog_bg       = "#c4c4c4";

BgMode cfg_bg_mode          = BG_PATTERN;
BgImageMode cfg_bg_mode_image = BG_SCALED;
BgPattern cfg_bg_pattern    = PAT_CHECKERBOARD;
const char *cfg_bg_image_path = NULL;
int cfg_bg_pattern_size     = 0;

Key *cfg_keys       = NULL;
int  cfg_num_keys   = 0;

BarWidget *cfg_bar_widgets     = NULL;
int        cfg_num_bar_widgets = 0;

CfgMenuItem *cfg_root_menu_items     = NULL;
int          cfg_num_root_menu_items = 0;

/* ── arena allocator ──────────────────────────────────────────────────── */

#define ARENA_BLOCK 8192

typedef struct CfgArena {
    struct CfgArena *next;
    int used;
    int cap;
    char data[];
} CfgArena;

static CfgArena *cfg_arena = NULL;

static void *arena_alloc(int size) {
    if (!cfg_arena || cfg_arena->used + size > cfg_arena->cap) {
        int cap = size > ARENA_BLOCK ? size : ARENA_BLOCK;
        CfgArena *blk = malloc(sizeof(CfgArena) + (size_t)cap);
        if (!blk) return NULL;
        blk->next = cfg_arena;
        blk->used = 0;
        blk->cap  = cap;
        cfg_arena = blk;
    }
    void *p = cfg_arena->data + cfg_arena->used;
    cfg_arena->used += size;
    return p;
}

static void arena_free_all(void) {
    while (cfg_arena) {
        CfgArena *next = cfg_arena->next;
        free(cfg_arena);
        cfg_arena = next;
    }
    cfg_arena = NULL;
}

/* save/restore arena pointer — used on reload to keep old strings alive
 * until new config is fully applied */
static CfgArena *cfg_arena_saved = NULL;

static void arena_save(void) {
    cfg_arena_saved = cfg_arena;
    cfg_arena = NULL;
}

static void arena_free_saved(void) {
    while (cfg_arena_saved) {
        CfgArena *next = cfg_arena_saved->next;
        free(cfg_arena_saved);
        cfg_arena_saved = next;
    }
    cfg_arena_saved = NULL;
}

/* ── AST data structure ───────────────────────────────────────────────── */

typedef enum {
    CFG_NIL, CFG_INT, CFG_FLOAT, CFG_STRING, CFG_SYMBOL, CFG_LIST
} CfgType;

typedef struct CfgNode CfgNode;
struct CfgNode {
    CfgType type;
    union {
        long ival;
        double fval;
        char *sval;
        const char *sym;
        struct { CfgNode *head; CfgNode *tail; } list;
    } u;
};

static CfgNode *cfg_nil(void) {
    CfgNode *n = arena_alloc(sizeof(CfgNode));
    if (!n) return NULL;
    n->type = CFG_NIL;
    return n;
}

static CfgNode *cfg_int(long v) {
    CfgNode *n = arena_alloc(sizeof(CfgNode));
    if (!n) return NULL;
    n->type = CFG_INT;
    n->u.ival = v;
    return n;
}

static CfgNode *cfg_float(double v) {
    CfgNode *n = arena_alloc(sizeof(CfgNode));
    if (!n) return NULL;
    n->type = CFG_FLOAT;
    n->u.fval = v;
    return n;
}

static CfgNode *cfg_string(const char *s, int len) {
    CfgNode *n = arena_alloc(sizeof(CfgNode));
    if (!n) return NULL;
    n->type = CFG_STRING;
    char *copy = arena_alloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, s, (size_t)len);
    copy[len] = '\0';
    n->u.sval = copy;
    return n;
}

static CfgNode *cfg_symbol(const char *s, int len) {
    CfgNode *n = arena_alloc(sizeof(CfgNode));
    if (!n) return NULL;
    n->type = CFG_SYMBOL;
    /* intern: just store a copy; for this scale, linear search is fine */
    char *copy = arena_alloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, s, (size_t)len);
    copy[len] = '\0';
    n->u.sym = copy;
    return n;
}

static CfgNode *cfg_cons(CfgNode *head, CfgNode *tail) {
    CfgNode *n = arena_alloc(sizeof(CfgNode));
    if (!n) return NULL;
    n->type = CFG_LIST;
    n->u.list.head = head;
    n->u.list.tail = tail;
    return n;
}

/* ── scanner ─────────────────────────────────────────────────────────── */

typedef enum {
    TOK_LPAREN, TOK_RPAREN, TOK_STRING, TOK_INT, TOK_FLOAT,
    TOK_SYMBOL, TOK_EOF, TOK_ERROR
} CfgTokType;

typedef struct {
    CfgTokType type;
    long ival;
    double fval;
    int len;        /* token length in source */
    const char *start; /* pointer into source buffer */
} CfgToken;

typedef struct {
    const char *buf;
    const char *pos;
    int line;
} CfgParser;

/* skip whitespace and ; comments */
static void cfg_skip(CfgParser *p) {
    for (;;) {
        while (*p->pos && (*p->pos == ' ' || *p->pos == '\t' ||
               *p->pos == '\n' || *p->pos == '\r')) {
            if (*p->pos == '\n') p->line++;
            p->pos++;
        }
        if (*p->pos == ';') {
            while (*p->pos && *p->pos != '\n') p->pos++;
        } else break;
    }
}

static int cfg_next(CfgParser *p, CfgToken *tok) {
    cfg_skip(p);
    if (!*p->pos) { tok->type = TOK_EOF; return 0; }

    if (*p->pos == '(') { tok->type = TOK_LPAREN; p->pos++; return 0; }
    if (*p->pos == ')') { tok->type = TOK_RPAREN; p->pos++; return 0; }

    if (*p->pos == '"') {
        /* string literal */
        p->pos++; /* skip opening quote */
        tok->start = p->pos;
        while (*p->pos && *p->pos != '"') {
            if (*p->pos == '\\') p->pos++; /* skip escaped char */
            if (*p->pos == '\n') p->line++;
            if (*p->pos) p->pos++;
        }
        tok->len = (int)(p->pos - tok->start);
        if (*p->pos == '"') p->pos++; /* skip closing quote */
        tok->type = TOK_STRING;
        return 0;
    }

    /* number or symbol */
    tok->start = p->pos;
    int is_num = 1, has_dot = 0, sign_ok = 1;

    if ((*p->pos == '+' || *p->pos == '-') && sign_ok) {
        /* could be a signed number or a symbol like +1, -0.05 */
        p->pos++;
        if (*p->pos >= '0' && *p->pos <= '9') {
            /* it's a signed number */
        } else {
            /* it's a symbol like +1 treated as symbol by default...
             * actually +1 and -1 should be numbers. Let me handle this:
             * If + or - is followed by a digit or dot, it's a number. */
            is_num = 0; /* it's a symbol */
        }
    }

    if (is_num) {
        /* reset and re-scan from tok->start */
        p->pos = tok->start;
        if (*p->pos == '+' || *p->pos == '-') p->pos++;
        while (*p->pos) {
            if (*p->pos >= '0' && *p->pos <= '9') {
                p->pos++;
            } else if (*p->pos == '.' && !has_dot) {
                has_dot = 1;
                p->pos++;
            } else {
                break;
            }
        }
        tok->len = (int)(p->pos - tok->start);

        /* check that the character after the number is a delimiter */
        if (*p->pos && *p->pos != ' ' && *p->pos != '\t' &&
            *p->pos != '\n' && *p->pos != '\r' &&
            *p->pos != ')' && *p->pos != ';' && *p->pos != '(') {
            /* not a number — it's a symbol, rescan */
            p->pos = tok->start;
            is_num = 0;
        }
    }

    if (!is_num) {
        /* symbol: [a-zA-Z_+-][a-zA-Z0-9_+-]* */
        p->pos = tok->start;
        if ((*p->pos >= 'a' && *p->pos <= 'z') ||
            (*p->pos >= 'A' && *p->pos <= 'Z') ||
            *p->pos == '_' || *p->pos == '+' || *p->pos == '-') {
            p->pos++;
            while ((*p->pos >= 'a' && *p->pos <= 'z') ||
                   (*p->pos >= 'A' && *p->pos <= 'Z') ||
                   (*p->pos >= '0' && *p->pos <= '9') ||
                   *p->pos == '_' || *p->pos == '-' || *p->pos == '+' || *p->pos == '.') {
                p->pos++;
            }
        }
        tok->len = (int)(p->pos - tok->start);
        if (tok->len == 0) {
            tok->type = TOK_ERROR;
            return -1;
        }
        tok->type = TOK_SYMBOL;
        return 0;
    }

    /* parse the number value */
    if (has_dot) {
        tok->type = TOK_FLOAT;
        tok->fval = strtod(tok->start, NULL);
    } else {
        tok->type = TOK_INT;
        tok->ival = strtol(tok->start, NULL, 10);
    }
    return 0;
}

/* ── recursive-descent parser ────────────────────────────────────────── */

static CfgNode *cfg_parse_value(CfgParser *p) {
    CfgToken tok;
    if (cfg_next(p, &tok) < 0) return cfg_nil();

    switch (tok.type) {
    case TOK_LPAREN: {
        /* read list elements until ')' */
        CfgNode *head = NULL, *tail = NULL;
        for (;;) {
            CfgToken peek;
            const char *save = p->pos;
            int save_line = p->line;
            if (cfg_next(p, &peek) < 0) return head ? head : cfg_nil();
            if (peek.type == TOK_RPAREN) break;
            if (peek.type == TOK_EOF) break;
            /* put back the token */
            p->pos = save;
            p->line = save_line;

            CfgNode *elem = cfg_parse_value(p);
            if (!elem) break;
            /* NIL elements (empty parens) are valid — keep them as list items */
            CfgNode *cell = cfg_cons(elem, NULL);
            if (!tail) head = cell;
            else tail->u.list.tail = cell;
            tail = cell;
        }
        return head ? head : cfg_nil();
    }
    case TOK_STRING:
        return cfg_string(tok.start, tok.len);
    case TOK_INT:
        return cfg_int(tok.ival);
    case TOK_FLOAT:
        return cfg_float(tok.fval);
    case TOK_SYMBOL:
        return cfg_symbol(tok.start, tok.len);
    case TOK_RPAREN:
    case TOK_EOF:
        return cfg_nil();
    case TOK_ERROR:
    default:
        return cfg_nil();
    }
}

static CfgNode *cfg_parse_buf(const char *buf, int buflen) {
    (void)buflen;
    CfgParser p;
    p.buf = buf;
    p.pos = buf;
    p.line = 1;

    CfgNode *head = NULL, *tail = NULL;
    for (;;) {
        CfgToken peek;
        const char *save = p.pos;
        int save_line = p.line;
        if (cfg_next(&p, &peek) < 0) break;
        if (peek.type == TOK_EOF) break;
        p.pos = save;
        p.line = save_line;

        CfgNode *form = cfg_parse_value(&p);
        if (!form || form->type == CFG_NIL) break;
        CfgNode *cell = cfg_cons(form, NULL);
        if (!tail) head = cell;
        else tail->u.list.tail = cell;
        tail = cell;
    }
    return head;
}

/* ── AST helpers ──────────────────────────────────────────────────────── */

static const char *cfg_sym_name(CfgNode *n) {
    return (n && n->type == CFG_SYMBOL) ? n->u.sym : NULL;
}

static int cfg_list_len(CfgNode *n) {
    int len = 0;
    while (n && n->type == CFG_LIST) { len++; n = n->u.list.tail; }
    return len;
}

/* get nth element of a list (0-indexed) */
static CfgNode *cfg_list_nth(CfgNode *n, int idx) {
    for (int i = 0; n && n->type == CFG_LIST; n = n->u.list.tail, i++) {
        if (i == idx) return n->u.list.head;
    }
    return NULL;
}

/* find a sub-list by symbol name in a compound form: (key (sub-key val) ...)
 * Returns the LIST node whose head matches name, so you can navigate into it.
 * e.g. for ((mod "Alt") (key "Return") ...), cfg_find_sub(args, "mod")
 * returns the cons cell whose head is the list (mod "Alt"). */
static CfgNode *cfg_find_sub(CfgNode *list, const char *name) {
    for (CfgNode *n = list; n && n->type == CFG_LIST; n = n->u.list.tail) {
        CfgNode *head = n->u.list.head;
        if (head && head->type == CFG_LIST) {
            CfgNode *sub_head = head->u.list.head;
            if (sub_head && sub_head->type == CFG_SYMBOL && strcmp(sub_head->u.sym, name) == 0)
                return head;
        }
    }
    return NULL;
}

/* ── modifier mask parser ─────────────────────────────────────────────── */

static unsigned int parse_modmask(const char *s) {
    unsigned int mask = 0;
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "Alt", 3) == 0 && (p[3] == '+' || p[3] == '\0' || p[3] == ' ')) {
            mask |= Mod1Mask; p += 3;
        } else if (strncmp(p, "Super", 5) == 0 && (p[5] == '+' || p[5] == '\0' || p[5] == ' ')) {
            mask |= Mod4Mask; p += 5;
        } else if (strncmp(p, "Shift", 5) == 0 && (p[5] == '+' || p[5] == '\0' || p[5] == ' ')) {
            mask |= ShiftMask; p += 5;
        } else if (strncmp(p, "Ctrl", 4) == 0 && (p[4] == '+' || p[4] == '\0' || p[4] == ' ')) {
            mask |= ControlMask; p += 4;
        } else {
            /* unknown modifier — skip to next + or end */
            while (*p && *p != '+') p++;
        }
        while (*p == '+' || *p == ' ' || *p == '\t') p++;
    }
    return mask;
}

/* ── keysym parser ────────────────────────────────────────────────────── */

static KeySym parse_keysym(const char *s) {
    /* single character */
    if (s[0] && !s[1]) return (KeySym)s[0];
    /* common names */
    if (strcmp(s, "Return")  == 0) return XK_Return;
    if (strcmp(s, "Tab")     == 0) return XK_Tab;
    if (strcmp(s, "space")   == 0) return XK_space;
    if (strcmp(s, "Escape")  == 0) return XK_Escape;
    if (strcmp(s, "BackSpace") == 0) return XK_BackSpace;
    if (strcmp(s, "Delete")   == 0) return XK_Delete;
    if (strcmp(s, "Left")    == 0) return XK_Left;
    if (strcmp(s, "Right")   == 0) return XK_Right;
    if (strcmp(s, "Up")      == 0) return XK_Up;
    if (strcmp(s, "Down")    == 0) return XK_Down;
    if (strcmp(s, "Home")    == 0) return XK_Home;
    if (strcmp(s, "End")     == 0) return XK_End;
    if (strcmp(s, "Prior")   == 0) return XK_Prior;
    if (strcmp(s, "Next")    == 0) return XK_Next;
    if (strcmp(s, "Insert")  == 0) return XK_Insert;
    /* F1-F12 */
    if (s[0] == 'F') {
        int n = atoi(s + 1);
        if (n >= 1 && n <= 12) return XK_F1 + (n - 1);
    }
    /* 1-9, 0 */
    if (s[0] >= '0' && s[0] <= '9' && !s[1]) return (KeySym)s[0];
    /* a-z */
    if (s[0] >= 'a' && s[0] <= 'z' && !s[1]) return (KeySym)s[0];
    if (s[0] >= 'A' && s[0] <= 'Z' && !s[1]) return (KeySym)s[0];
    return NoSymbol;
}

/* ── action dispatch table ─────────────────────────────────────────────── */

typedef struct {
    const char *name;
    void (*func)(const WmArg *);
} ActionEntry;

static const ActionEntry action_table[] = {
    { "spawn",            spawn },
    { "killclient",       killclient },
    { "focusstack",       focusstack },
    { "cyclewindows",    cyclewindows },
    { "lowerwindow",     lowerwindow },
    { "togglefloat",     togglefloat },
    { "incmaster",       incmaster },
    { "zoom",            zoom },
    { "togglefullscreen",togglefullscreen },
    { "viewworkspace",   viewworkspace },
    { "movetoworkspace", movetoworkspace },
    { "quit",            quit },
    { "swapbar",         swapbar },
    { "reloadconfig",    reloadconfig },
    { "togglecompositing", togglecompositing },
    { "setlayout",       setlayout },
    { "cyclelayout",      cyclelayout },
    { "focusdir",         focusdir },
    { "swapdir",          swapdir },
};
#define NUM_ACTIONS ((int)(sizeof(action_table) / sizeof(action_table[0])))

static void (*find_action(const char *name))(const WmArg *) {
    for (int i = 0; i < NUM_ACTIONS; i++)
        if (strcmp(action_table[i].name, name) == 0)
            return action_table[i].func;
    return NULL;
}

/* ── root menu action symbols ────────────────────────────────────────── */

static int find_root_action(const char *name) {
    if (strcmp(name, "new-window")   == 0) return ROOT_NEW;
    if (strcmp(name, "shuffle-up")   == 0) return ROOT_SHUFFLE_UP;
    if (strcmp(name, "shuffle-down") == 0) return ROOT_SHUFFLE_DOWN;
    if (strcmp(name, "refresh")      == 0) return ROOT_REFRESH;
    if (strcmp(name, "restart")      == 0) return ROOT_RESTART;
    if (strcmp(name, "quit")         == 0) return ROOT_QUIT;
    return -1;
}

/* ── spawn arg storage (for bind entries with string args) ───────────── */

/* We need persistent storage for spawn argv arrays since WmArg.v points to them.
 * Allocate them from a separate list that survives arena frees. */
typedef struct CfgSpawnArg {
    struct CfgSpawnArg *next;
    const char *argv[2]; /* { string, NULL } */
} CfgSpawnArg;

static CfgSpawnArg *spawn_args = NULL;

static const char **alloc_spawn_argv(const char *cmd) {
    CfgSpawnArg *sa = malloc(sizeof(CfgSpawnArg));
    if (!sa) return NULL;
    sa->next = spawn_args;
    sa->argv[0] = strdup(cmd);
    sa->argv[1] = NULL;
    spawn_args = sa;
    return sa->argv;
}

static void free_spawn_args(void) {
    while (spawn_args) {
        CfgSpawnArg *next = spawn_args->next;
        free((void *)spawn_args->argv[0]);
        free(spawn_args);
        spawn_args = next;
    }
}

/* ── cfg_apply: walk AST and set runtime globals ──────────────────────── */

/* helpers to extract typed values from the second element of (key value) */
static int apply_int_val(CfgNode *args, long *out) {
    CfgNode *v = cfg_list_nth(args, 0);
    if (!v || (v->type != CFG_INT && v->type != CFG_FLOAT)) return -1;
    *out = (v->type == CFG_INT) ? v->u.ival : (long)v->u.fval;
    return 0;
}

static int apply_float_val(CfgNode *args, double *out) {
    CfgNode *v = cfg_list_nth(args, 0);
    if (!v) return -1;
    if (v->type == CFG_FLOAT) *out = v->u.fval;
    else if (v->type == CFG_INT) *out = (double)v->u.ival;
    else return -1;
    return 0;
}

static int apply_string_val(CfgNode *args, const char **out) {
    CfgNode *v = cfg_list_nth(args, 0);
    if (!v || v->type != CFG_STRING) return -1;
    *out = v->u.sval; /* points into arena — valid until arena free */
    return 0;
}

static void cfg_apply_form(CfgNode *form) {
    if (form->type != CFG_LIST) return;
    const char *key = cfg_sym_name(form->u.list.head);
    if (!key) return;
    CfgNode *args = form->u.list.tail; /* rest of list after key */

    long ival;
    double fval;
    const char *sval;

    /* simple integer settings */
    if (strcmp(key, "frame-width")  == 0 && apply_int_val(args, &ival) == 0)
        { cfg_frame_width = (int)ival; return; }
    if (strcmp(key, "title-height") == 0 && apply_int_val(args, &ival) == 0)
        { cfg_title_height = (int)ival; return; }
    if (strcmp(key, "bar-height")   == 0 && apply_int_val(args, &ival) == 0)
        { cfg_bar_height = (int)ival; return; }
    if (strcmp(key, "bar-border-width") == 0 && apply_int_val(args, &ival) == 0)
        { cfg_bar_border_width = (int)ival; return; }
    if (strcmp(key, "bar-corner-size")  == 0 && apply_int_val(args, &ival) == 0)
        { cfg_bar_corner_size = (int)ival; return; }
    if (strcmp(key, "bar-btn-width")   == 0 && apply_int_val(args, &ival) == 0)
        { cfg_bar_btn_width = (int)ival; return; }
    if (strcmp(key, "btn-width")    == 0 && apply_int_val(args, &ival) == 0)
        { cfg_btn_width = (int)ival; return; }
    if (strcmp(key, "btn-height")   == 0 && apply_int_val(args, &ival) == 0)
        { cfg_btn_height = (int)ival; return; }
    if (strcmp(key, "icon-width")   == 0 && apply_int_val(args, &ival) == 0)
        { cfg_icon_width = (int)ival; return; }
    if (strcmp(key, "icon-height")  == 0 && apply_int_val(args, &ival) == 0)
        { cfg_icon_height = (int)ival; return; }
    if (strcmp(key, "icon-padding") == 0 && apply_int_val(args, &ival) == 0)
        { cfg_icon_padding = (int)ival; return; }
    if (strcmp(key, "icon-mode") == 0) {
        const char *mode = cfg_sym_name(cfg_list_nth(args, 0));
        if (mode && strcmp(mode, "icon") == 0)             cfg_icon_mode = ICON_MODE_ICON;
        else if (mode && strcmp(mode, "text") == 0)         cfg_icon_mode = ICON_MODE_TEXT;
        else if (mode && strcmp(mode, "icon-text") == 0)    cfg_icon_mode = ICON_MODE_ICON_TEXT;
        return;
    }
    if (strcmp(key, "bar-position") == 0) {
        const char *pos = cfg_sym_name(cfg_list_nth(args, 0));
        if (pos && strcmp(pos, "top") == 0)          cfg_bar_position = BAR_POS_TOP;
        else if (pos && strcmp(pos, "bottom") == 0)  cfg_bar_position = BAR_POS_BOTTOM;
        else if (pos && strcmp(pos, "left") == 0)    cfg_bar_position = BAR_POS_LEFT;
        else if (pos && strcmp(pos, "right") == 0)   cfg_bar_position = BAR_POS_RIGHT;
        return;
    }
    if (strcmp(key, "iconbar-position") == 0) {
        const char *pos = cfg_sym_name(cfg_list_nth(args, 0));
        if (pos && strcmp(pos, "top") == 0)          cfg_iconbar_position = BAR_POS_TOP;
        else if (pos && strcmp(pos, "bottom") == 0)  cfg_iconbar_position = BAR_POS_BOTTOM;
        else if (pos && strcmp(pos, "left") == 0)    cfg_iconbar_position = BAR_POS_LEFT;
        else if (pos && strcmp(pos, "right") == 0)   cfg_iconbar_position = BAR_POS_RIGHT;
        return;
    }
    if (strcmp(key, "workspaces")   == 0 && apply_int_val(args, &ival) == 0)
        { cfg_num_workspaces = (int)ival; return; }
    if (strcmp(key, "show-bar")    == 0 && apply_int_val(args, &ival) == 0)
        { cfg_show_bar = (int)ival; return; }
    if (strcmp(key, "fade-enabled")  == 0 && apply_int_val(args, &ival) == 0)
        { cfg_fade_enabled = (int)ival; return; }
    if (strcmp(key, "fade-in-ms")    == 0 && apply_int_val(args, &ival) == 0)
        { cfg_fade_in_ms = (int)ival; return; }
    if (strcmp(key, "fade-out-ms")   == 0 && apply_int_val(args, &ival) == 0)
        { cfg_fade_out_ms = (int)ival; return; }
    if (strcmp(key, "tooltip-delay") == 0 && apply_int_val(args, &ival) == 0)
        { cfg_tooltip_delay = (int)ival; return; }

    /* float settings */
    if (strcmp(key, "master-ratio") == 0 && apply_float_val(args, &fval) == 0)
        { cfg_master_ratio = (float)fval; return; }

    /* string settings */
    if (strcmp(key, "font")         == 0 && apply_string_val(args, &sval) == 0)
        { cfg_font_name = sval; return; }
    if (strcmp(key, "terminal")     == 0 && apply_string_val(args, &sval) == 0)
        { cfg_term_cmd = sval; return; }
    if (strcmp(key, "launcher")     == 0 && apply_string_val(args, &sval) == 0)
        { cfg_menu_cmd = sval; return; }
    if (strcmp(key, "clock-format") == 0 && apply_string_val(args, &sval) == 0)
        { cfg_clock_format = sval; return; }
    if (strcmp(key, "modkey")       == 0 && apply_string_val(args, &sval) == 0)
        { cfg_modkey = parse_modmask(sval); return; }

    /* color settings */
    if (strcmp(key, "title-focus")    == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_title_focus = sval; return; }
    if (strcmp(key, "title-unfocus")  == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_title_unfocus = sval; return; }
    if (strcmp(key, "title-fg")      == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_title_fg = sval; return; }
    if (strcmp(key, "frame-light")   == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_frame_light = sval; return; }
    if (strcmp(key, "frame-shadow")  == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_frame_shadow = sval; return; }
    if (strcmp(key, "frame-bg")      == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_frame_bg = sval; return; }
    if (strcmp(key, "active-light")  == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_active_light = sval; return; }
    if (strcmp(key, "active-shadow") == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_active_shadow = sval; return; }
    if (strcmp(key, "btn-fg")        == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_btn_fg = sval; return; }
    if (strcmp(key, "bar-bg")         == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_bar_bg = sval; return; }
    if (strcmp(key, "bar-fg")         == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_bar_fg = sval; return; }
    if (strcmp(key, "bar-ws-active")  == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_bar_ws_active = sval; return; }
    if (strcmp(key, "bar-ws-occupied") == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_bar_ws_occupied = sval; return; }
    if (strcmp(key, "bar-ws-idle")    == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_bar_ws_idle = sval; return; }
    if (strcmp(key, "bar-ws-bg")      == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_bar_ws_bg = sval; return; }
    if (strcmp(key, "bar-border-light")  == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_bar_border_light = sval; return; }
    if (strcmp(key, "bar-border-shadow") == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_bar_border_shadow = sval; return; }
    if (strcmp(key, "bar-fill")          == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_bar_fill = sval; return; }
    if (strcmp(key, "menu-bg")        == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_menu_bg = sval; return; }
    if (strcmp(key, "iconbar-bg")      == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_iconbar_bg = sval; return; }
    if (strcmp(key, "fb-bg")           == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_fb_bg = sval; return; }
    if (strcmp(key, "fb-light")        == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_fb_light = sval; return; }
    if (strcmp(key, "fb-shadow")       == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_fb_shadow = sval; return; }
    if (strcmp(key, "fb-fg")           == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_fb_fg = sval; return; }
    if (strcmp(key, "tooltip-bg")      == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_tooltip_bg = sval; return; }
    if (strcmp(key, "tooltip-fg")      == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_tooltip_fg = sval; return; }
    if (strcmp(key, "tooltip-border")  == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_tooltip_border = sval; return; }
    if (strcmp(key, "tooltip-font")    == 0 && apply_string_val(args, &sval) == 0)
        { cfg_tooltip_font = sval; return; }
    /* compound: root-bg (solid color, pattern, or image) */
    if (strcmp(key, "root-bg") == 0) {
        CfgNode *val = cfg_list_nth(args, 0);
        if (!val) return;
        if (val->type == CFG_STRING) {
            /* simple color string → solid color */
            cfg_color_root_bg = val->u.sval;
            cfg_bg_mode = BG_SOLID;
            return;
        }
        if (val->type == CFG_LIST) {
            CfgNode *sub_head = cfg_list_nth(val, 0);
            const char *mode = cfg_sym_name(sub_head);
            if (!mode) return;

            if (strcmp(mode, "pattern") == 0) {
                /* (root-bg (pattern "name" "c1" "c2" [size])) */
                const char *name = NULL;
                CfgNode *n1 = cfg_list_nth(val, 1);
                if (n1 && n1->type == CFG_STRING) name = n1->u.sval;
                if (!name) return;

                if (strcmp(name, "checkerboard") == 0)       cfg_bg_pattern = PAT_CHECKERBOARD;
                else if (strcmp(name, "diagonal-stripes") == 0) cfg_bg_pattern = PAT_DIAGONAL_STRIPES;
                else if (strcmp(name, "horizontal-stripes") == 0) cfg_bg_pattern = PAT_HORIZONTAL_STRIPES;
                else if (strcmp(name, "vertical-stripes") == 0) cfg_bg_pattern = PAT_VERTICAL_STRIPES;
                else if (strcmp(name, "dots") == 0)           cfg_bg_pattern = PAT_DOTS;
                else if (strcmp(name, "crosshatch") == 0)     cfg_bg_pattern = PAT_CROSSHATCH;
                else if (strcmp(name, "weave") == 0)          cfg_bg_pattern = PAT_WEAVE;
                else {
                    fprintf(stderr, "rondo: config: unknown pattern '%s'\n", name);
                    return;
                }

                CfgNode *c1 = cfg_list_nth(val, 2);
                CfgNode *c2 = cfg_list_nth(val, 3);
                if (c1 && c1->type == CFG_STRING) cfg_color_root_bg = c1->u.sval;
                if (c2 && c2->type == CFG_STRING) cfg_color_root_bg2 = c2->u.sval;

                CfgNode *sz = cfg_list_nth(val, 4);
                if (sz && (sz->type == CFG_INT || sz->type == CFG_FLOAT))
                    cfg_bg_pattern_size = (int)(sz->type == CFG_INT ? sz->u.ival : sz->u.fval);
                else
                    cfg_bg_pattern_size = 0; /* auto */

                cfg_bg_mode = BG_PATTERN;
                return;
            }

            if (strcmp(mode, "image") == 0) {
                /* (root-bg (image "/path" centered|scaled|tiled|stretched)) */
                CfgNode *path_node = cfg_list_nth(val, 1);
                if (path_node && path_node->type == CFG_STRING)
                    cfg_bg_image_path = path_node->u.sval;

                const char *placement = cfg_sym_name(cfg_list_nth(val, 2));
                if (placement && strcmp(placement, "centered") == 0)
                    cfg_bg_mode_image = BG_CENTERED;
                else if (placement && strcmp(placement, "scaled") == 0)
                    cfg_bg_mode_image = BG_SCALED;
                else if (placement && strcmp(placement, "tiled") == 0)
                    cfg_bg_mode_image = BG_TILED;
                else if (placement && strcmp(placement, "stretched") == 0)
                    cfg_bg_mode_image = BG_STRETCHED;
                else if (placement && strcmp(placement, "scale-filled") == 0)
                    cfg_bg_mode_image = BG_SCALE_FILLED;
                else
                    cfg_bg_mode_image = BG_SCALED; /* default */

                cfg_bg_mode = BG_IMAGE;
                return;
            }
        }
        return;
    }
    if (strcmp(key, "root-bg2")        == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_root_bg2 = sval; return; }
    if (strcmp(key, "dialog-bg")       == 0 && apply_string_val(args, &sval) == 0)
        { cfg_color_dialog_bg = sval; return; }

    /* compound: bar-layout */
    if (strcmp(key, "bar-layout") == 0) {
        int n = cfg_list_len(args);
        if (n <= 0) return;
        free(cfg_bar_widgets);
        cfg_bar_widgets = malloc(sizeof(BarWidget) * (size_t)n);
        cfg_num_bar_widgets = 0;
        for (CfgNode *item = args; item && item->type == CFG_LIST; item = item->u.list.tail) {
            CfgNode *sub = item->u.list.head;
            if (!sub || sub->type != CFG_LIST) continue;
            const char *wtype = cfg_sym_name(cfg_list_nth(sub, 0));
            const char *align = cfg_sym_name(cfg_list_nth(sub, 1));
            if (!wtype || !align) continue;
            BarWidget *bw = &cfg_bar_widgets[cfg_num_bar_widgets];
            if (strcmp(wtype, "ws")    == 0) bw->type = BAR_WIDGET_WS;
            else if (strcmp(wtype, "title") == 0) bw->type = BAR_WIDGET_TITLE;
            else if (strcmp(wtype, "layout") == 0) bw->type = BAR_WIDGET_LAYOUT;
            else if (strcmp(wtype, "clock") == 0) bw->type = BAR_WIDGET_CLOCK;
            else if (strcmp(wtype, "load")  == 0) bw->type = BAR_WIDGET_LOAD;
            else if (strcmp(wtype, "mem")   == 0) bw->type = BAR_WIDGET_MEM;
            else if (strcmp(wtype, "disk")  == 0) bw->type = BAR_WIDGET_DISK;
            else if (strcmp(wtype, "bat")   == 0) bw->type = BAR_WIDGET_BAT;
            else if (strcmp(wtype, "vol")   == 0) bw->type = BAR_WIDGET_VOL;
            else if (strcmp(wtype, "cpu")   == 0) bw->type = BAR_WIDGET_CPU;
            else if (strcmp(wtype, "net")   == 0) bw->type = BAR_WIDGET_NET;
            else if (strcmp(wtype, "temp")  == 0) bw->type = BAR_WIDGET_TEMP;
            else continue;
            bw->align = (strcmp(align, "left") == 0) ? BAR_ALIGN_LEFT : BAR_ALIGN_RIGHT;
            cfg_num_bar_widgets++;
        }
        return;
    }

    /* compound: bind */
    if (strcmp(key, "bind") == 0) {
        CfgNode *mod_node = cfg_find_sub(args, "mod");
        CfgNode *key_node = cfg_find_sub(args, "key");
        CfgNode *act_node = cfg_find_sub(args, "action");
        if (!mod_node || !key_node || !act_node) return;

        const char *mod_str = cfg_sym_name(cfg_list_nth(mod_node->u.list.tail, 0));
        if (!mod_str) {
            CfgNode *mv = cfg_list_nth(mod_node->u.list.tail, 0);
            if (mv && mv->type == CFG_STRING) mod_str = mv->u.sval;
        }
        const char *key_str = cfg_sym_name(cfg_list_nth(key_node->u.list.tail, 0));
        if (!key_str) {
            CfgNode *kv = cfg_list_nth(key_node->u.list.tail, 0);
            if (kv && kv->type == CFG_STRING) key_str = kv->u.sval;
            else if (kv && kv->type == CFG_INT) {
                /* numeric keys like 1-9 parse as integers; convert to string */
                static char num_key[2] = "0";
                num_key[0] = (char)('0' + (kv->u.ival % 10));
                key_str = num_key;
            }
        }
        const char *act_str = cfg_sym_name(cfg_list_nth(act_node->u.list.tail, 0));
        if (!act_str) {
            CfgNode *av = cfg_list_nth(act_node->u.list.tail, 0);
            if (av && av->type == CFG_STRING) act_str = av->u.sval;
        }
        if (!mod_str || !key_str || !act_str) return;

        void (*func)(const WmArg *) = find_action(act_str);
        if (!func) {
            fprintf(stderr, "rondo: config: unknown action '%s'\n", act_str);
            return;
        }

        Key k;
        k.mod = parse_modmask(mod_str);
        k.keysym = parse_keysym(key_str);
        k.func = func;

        /* parse arg */
        CfgNode *arg_node = cfg_find_sub(args, "arg");
        if (arg_node) {
            CfgNode *av = cfg_list_nth(arg_node->u.list.tail, 0);
            if (av) {
                if (av->type == CFG_INT) {
                    k.arg.ui = (unsigned int)av->u.ival;
                } else if (av->type == CFG_FLOAT) {
                    k.arg.f = (float)av->u.fval;
                } else if (av->type == CFG_STRING) {
                    k.arg.v = alloc_spawn_argv(av->u.sval);
                } else if (av->type == CFG_SYMBOL) {
                    /* special: "terminal" or "launcher" for spawn */
                    k.arg.v = NULL;
                } else {
                    k.arg.v = NULL;
                }
            } else {
                k.arg.v = NULL;
            }
        } else {
            k.arg.v = NULL;
        }

        /* append to keys array */
        cfg_keys = realloc(cfg_keys, sizeof(Key) * (size_t)(cfg_num_keys + 1));
        cfg_keys[cfg_num_keys++] = k;
        return;
    }

    /* compound: root-menu */
    if (strcmp(key, "root-menu") == 0) {
        int n = cfg_list_len(args);
        if (n <= 0) return;
        free(cfg_root_menu_items);
        cfg_root_menu_items = malloc(sizeof(CfgMenuItem) * (size_t)n);
        cfg_num_root_menu_items = 0;
        for (CfgNode *item = args; item && item->type == CFG_LIST; item = item->u.list.tail) {
            CfgNode *entry = item->u.list.head;
            CfgMenuItem mi;
            if (!entry || entry->type == CFG_NIL) {
                /* empty list = separator */
                mi.label = NULL;
                mi.action = MENU_SEP;
            } else if (entry->type == CFG_LIST) {
                CfgNode *lbl = cfg_list_nth(entry, 0);
                CfgNode *act = cfg_list_nth(entry, 1);
                if (lbl && lbl->type == CFG_STRING && act && act->type == CFG_SYMBOL) {
                    mi.label = lbl->u.sval;
                    mi.action = find_root_action(act->u.sym);
                    if (mi.action < 0) {
                        fprintf(stderr, "rondo: config: unknown root-menu action '%s'\n", act->u.sym);
                        continue;
                    }
                } else continue;
            } else continue;
            cfg_root_menu_items[cfg_num_root_menu_items++] = mi;
        }
        return;
    }

    fprintf(stderr, "rondo: config: unknown key '%s'\n", key);
}

static void cfg_apply(CfgNode *config) {
    for (CfgNode *n = config; n && n->type == CFG_LIST; n = n->u.list.tail) {
        cfg_apply_form(n->u.list.head);
    }
}

/* ── set defaults (hardcoded values from old main.c) ──────────────────── */

static void cfg_set_defaults(void) {
    /* dimensions already initialized as static globals above */
    /* colors already initialized as static globals above */

    /* key bindings — default set */
    static Key default_keys[] = {
        { Mod1Mask,            XK_Return,     spawn,            { .v = NULL } },
        { Mod1Mask,            XK_p,          spawn,            { .v = NULL } },
        { Mod1Mask,            XK_j,          focusstack,       { .i = +1 } },
        { Mod1Mask,            XK_k,          focusstack,       { .i = -1 } },
        { Mod1Mask,            XK_h,          incmaster,        { .f = -0.05f } },
        { Mod1Mask,            XK_l,          incmaster,        { .f = +0.05f } },
        { Mod1Mask|ShiftMask,  XK_Return,     zoom,             { 0 } },
        { Mod1Mask,            XK_f,          togglefullscreen, { 0 } },
        { Mod1Mask,            XK_space,      togglefloat,      { 0 } },
        { Mod1Mask|ShiftMask,  XK_c,          killclient,       { 0 } },
        { Mod1Mask|ShiftMask,  XK_q,          quit,             { 0 } },
        { Mod1Mask,            XK_1,          viewworkspace,    { .ui = 1 } },
        { Mod1Mask,            XK_2,          viewworkspace,    { .ui = 2 } },
        { Mod1Mask,            XK_3,          viewworkspace,    { .ui = 3 } },
        { Mod1Mask,            XK_4,          viewworkspace,    { .ui = 4 } },
        { Mod1Mask,            XK_5,          viewworkspace,    { .ui = 5 } },
        { Mod1Mask,            XK_6,          viewworkspace,    { .ui = 6 } },
        { Mod1Mask,            XK_7,          viewworkspace,    { .ui = 7 } },
        { Mod1Mask,            XK_8,          viewworkspace,    { .ui = 8 } },
        { Mod1Mask,            XK_9,          viewworkspace,    { .ui = 9 } },
        { Mod1Mask|ShiftMask,  XK_1,          movetoworkspace,  { .ui = 1 } },
        { Mod1Mask|ShiftMask,  XK_2,          movetoworkspace,  { .ui = 2 } },
        { Mod1Mask|ShiftMask,  XK_3,          movetoworkspace,  { .ui = 3 } },
        { Mod1Mask|ShiftMask,  XK_4,          movetoworkspace,  { .ui = 4 } },
        { Mod1Mask|ShiftMask,  XK_5,          movetoworkspace,  { .ui = 5 } },
        { Mod1Mask|ShiftMask,  XK_6,          movetoworkspace,  { .ui = 6 } },
        { Mod1Mask|ShiftMask,  XK_7,          movetoworkspace,  { .ui = 7 } },
        { Mod1Mask|ShiftMask,  XK_8,          movetoworkspace,  { .ui = 8 } },
        { Mod1Mask|ShiftMask,  XK_9,          movetoworkspace,  { .ui = 9 } },
        { Mod1Mask,            XK_Tab,        cyclewindows,     { .i = +1 } },
        { Mod1Mask|ShiftMask,  XK_Tab,        lowerwindow,      { 0 } },
        { Mod1Mask|ShiftMask,  XK_b,          swapbar,           { 0 } },
        { Mod1Mask|ShiftMask,  XK_o,          togglecompositing, { 0 } },
        { Mod1Mask|ShiftMask,  XK_t,          cyclelayout,       { 0 } },
        { Mod1Mask,            XK_Up,         focusdir,          { .i = EDGE_N } },
        { Mod1Mask,            XK_Down,       focusdir,          { .i = EDGE_S } },
        { Mod1Mask,            XK_Left,       focusdir,          { .i = EDGE_W } },
        { Mod1Mask,            XK_Right,      focusdir,          { .i = EDGE_E } },
        { Mod1Mask|ShiftMask,  XK_Up,         swapdir,            { .i = EDGE_N } },
        { Mod1Mask|ShiftMask,  XK_Down,       swapdir,            { .i = EDGE_S } },
        { Mod1Mask|ShiftMask,  XK_Left,       swapdir,            { .i = EDGE_W } },
        { Mod1Mask|ShiftMask,  XK_Right,      swapdir,            { .i = EDGE_E } },
    };

    /* spawn args for default keys */
    static const char *term_argv[] = { "xterm", NULL };
    static const char *menu_argv[] = { "dmenu_run", NULL };
    default_keys[0].arg.v = term_argv;
    default_keys[1].arg.v = menu_argv;

    int nk = (int)LENGTH(default_keys);
    cfg_keys = realloc(cfg_keys, sizeof(Key) * (size_t)nk);
    memcpy(cfg_keys, default_keys, sizeof(Key) * (size_t)nk);
    cfg_num_keys = nk;

    /* bar widget layout */
    static BarWidget default_bar_widgets[] = {
        { BAR_WIDGET_WS,     BAR_ALIGN_LEFT },
        { BAR_WIDGET_LAYOUT, BAR_ALIGN_LEFT },
        { BAR_WIDGET_TITLE,  BAR_ALIGN_RIGHT },
        { BAR_WIDGET_BAT,    BAR_ALIGN_RIGHT },
        { BAR_WIDGET_VOL,    BAR_ALIGN_RIGHT },
        { BAR_WIDGET_CPU,   BAR_ALIGN_RIGHT },
        { BAR_WIDGET_NET,   BAR_ALIGN_RIGHT },
        { BAR_WIDGET_TEMP,  BAR_ALIGN_RIGHT },
        { BAR_WIDGET_LOAD,  BAR_ALIGN_RIGHT },
        { BAR_WIDGET_MEM,   BAR_ALIGN_RIGHT },
        { BAR_WIDGET_DISK,  BAR_ALIGN_RIGHT },
        { BAR_WIDGET_CLOCK, BAR_ALIGN_RIGHT },
    };
    int nbw = (int)LENGTH(default_bar_widgets);
    cfg_bar_widgets = realloc(cfg_bar_widgets, sizeof(BarWidget) * (size_t)nbw);
    memcpy(cfg_bar_widgets, default_bar_widgets, sizeof(BarWidget) * (size_t)nbw);
    cfg_num_bar_widgets = nbw;

    /* root menu */
    static CfgMenuItem default_root_menu[] = {
        { "New Window",    ROOT_NEW },
        { "Shuffle Up",    ROOT_SHUFFLE_UP },
        { "Shuffle Down",  ROOT_SHUFFLE_DOWN },
        { "Refresh",       ROOT_REFRESH },
        { NULL,            MENU_SEP },
        { "Restart...",    ROOT_RESTART },
        { "Quit...",       ROOT_QUIT },
    };
    int nrm = (int)LENGTH(default_root_menu);
    cfg_root_menu_items = realloc(cfg_root_menu_items, sizeof(CfgMenuItem) * (size_t)nrm);
    memcpy(cfg_root_menu_items, default_root_menu, sizeof(CfgMenuItem) * (size_t)nrm);
    cfg_num_root_menu_items = nrm;
}

/* ── file reading ────────────────────────────────────────────────────── */

static char *cfg_read_file(void) {
    const char *home = getenv("HOME");
    if (!home) return NULL;

    char path[PATH_MAX];
    FILE *f = NULL;

    /* 1. ~/.rondorc */
    snprintf(path, sizeof(path), "%s/.rondorc", home);
    f = fopen(path, "r");

    /* 2. ~/.config/rondo/config.scm */
    if (!f) {
        snprintf(path, sizeof(path), "%s/.config/rondo/config.scm", home);
        f = fopen(path, "r");
    }

    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

/* ── resolve spawn args with special "terminal"/"launcher" handling ──── */

static void resolve_spawn_args(void) {
    static const char *term_argv[2] = { "xterm", NULL };
    static const char *menu_argv[2] = { "dmenu_run", NULL };

    /* assign spawn binds with .v == NULL to terminal/launcher commands */
    int spawn_idx = 0;
    for (int i = 0; i < cfg_num_keys; i++) {
        if (cfg_keys[i].func == spawn && cfg_keys[i].arg.v == NULL) {
            if (spawn_idx == 0) {
                term_argv[0] = cfg_term_cmd;
                cfg_keys[i].arg.v = term_argv;
            } else if (spawn_idx == 1) {
                menu_argv[0] = cfg_menu_cmd;
                cfg_keys[i].arg.v = menu_argv;
            } else {
                /* more spawn binds without string arg — use term_cmd */
                cfg_keys[i].arg.v = term_argv;
            }
            spawn_idx++;
        }
    }
}

/* ── public API ───────────────────────────────────────────────────────── */

void cfg_init(void) {
    cfg_set_defaults();

    char *buf = cfg_read_file();
    if (!buf) return; /* no config file — use defaults */

    CfgNode *ast = cfg_parse_buf(buf, (int)strlen(buf));
    if (ast) {
        /* clear dynamic arrays before applying (they were set by defaults) */
        free(cfg_keys); cfg_keys = NULL; cfg_num_keys = 0;
        free(cfg_bar_widgets); cfg_bar_widgets = NULL; cfg_num_bar_widgets = 0;
        free(cfg_root_menu_items); cfg_root_menu_items = NULL; cfg_num_root_menu_items = 0;

        cfg_apply(ast);
        resolve_spawn_args();
    }

    free(buf);
    /* don't free the arena — all cfg_* string pointers point into it */
}

void cfg_reload(void) {
    cfg_set_defaults();

    /* save old arena — strings from old config are still live until
     * new config is fully applied and X resources reloaded */
    arena_save();
    free_spawn_args();

    char *buf = cfg_read_file();
    if (buf) {
        CfgNode *ast = cfg_parse_buf(buf, (int)strlen(buf));
        if (ast) {
            free(cfg_keys); cfg_keys = NULL; cfg_num_keys = 0;
            free(cfg_bar_widgets); cfg_bar_widgets = NULL; cfg_num_bar_widgets = 0;
            free(cfg_root_menu_items); cfg_root_menu_items = NULL; cfg_num_root_menu_items = 0;

            cfg_apply(ast);
            resolve_spawn_args();
        }
        free(buf);
    }

    /* now that new strings are in use, free the old arena */
    arena_free_saved();

    /* reload X resources */
    load_colors();

    /* root background (solid, pattern, or image) */
    bg_load();

    /* font */
    {
        static char prev_font[256] = "";
        if (strcmp(cfg_font_name, prev_font) != 0) {
            XftFont *newfont = XftFontOpenName(dpy, screen, cfg_font_name);
            if (newfont) {
                XftFontClose(dpy, xftfont);
                xftfont = newfont;
                strncpy(prev_font, cfg_font_name, sizeof(prev_font) - 1);
            } else {
                fprintf(stderr, "rondo: cannot load font '%s', keeping previous\n", cfg_font_name);
            }
        }
    }

    /* tooltip font */
    {
        const char *tfont = cfg_tooltip_font ? cfg_tooltip_font : cfg_font_name;
        XftFont *new_tf = XftFontOpenName(dpy, screen, tfont);
        if (new_tf) {
            if (tooltip_font != xftfont) XftFontClose(dpy, tooltip_font);
            tooltip_font = new_tf;
        }
    }

    /* key grabs */
    grabkeys();

    /* reallocate workspace coordinate arrays if workspace count changed */
    {
        int new_num = cfg_num_workspaces;
        int *new_ws_x = realloc(ws_x, (size_t)new_num * sizeof(int));
        int *new_ws_y = realloc(ws_y, (size_t)new_num * sizeof(int));
        if (new_ws_x) { ws_x = new_ws_x; memset(ws_x, 0, (size_t)new_num * sizeof(int)); }
        if (new_ws_y) { ws_y = new_ws_y; memset(ws_y, 0, (size_t)new_num * sizeof(int)); }
        /* clamp current workspace if it was removed */
        if (curws >= new_num) curws = new_num - 1;
    }

    /* bar and icon bar windows — reposition based on current config */
    BarGeometry g = calc_bar_geometry();
    XMoveResizeWindow(dpy, barwin, g.bar_x, g.bar_y,
                      g.bar_w > 0 ? (unsigned)g.bar_w : 1,
                      g.bar_h > 0 ? (unsigned)g.bar_h : 1);
    XMoveResizeWindow(dpy, iconbar, g.ibar_x, g.ibar_y,
                      g.ibar_w > 0 ? (unsigned)g.ibar_w : 1,
                      g.ibar_h > 0 ? (unsigned)g.ibar_h : 1);

    /* reposition client content windows inside frames (in case
     * title-height or frame-width changed) */
    for (Client *c = clients; c; c = c->next) {
        if (c->is_minimized || c->is_hidden) continue;
        int cx, cy, cw, ch;
        frame_to_client(c->w, c->h, &cx, &cy, &cw, &ch, c->no_decor);
        XMoveResizeWindow(dpy, c->win, cx, cy, cw, ch);
        send_configure_notify(c);
    }

    /* show/hide bar window */
    if (cfg_show_bar)
        XMapWindow(dpy, barwin);
    else
        XUnmapWindow(dpy, barwin);

    /* redraw everything */
    arrange();
    for (Client *c = clients; c; c = c->next)
        drawframe(c);
    if (cfg_show_bar)
        drawbar();
    updateiconbar();
    update_workarea();
    update_net_desktops();
}

void cfg_cleanup(void) {
    bg_free();
    free(cfg_keys);
    free(cfg_bar_widgets);
    free(cfg_root_menu_items);
    free_spawn_args();
    arena_free_all();
}

void reloadconfig(const WmArg *arg) {
    (void)arg;
    cfg_reload();
}