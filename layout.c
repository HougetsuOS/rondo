/*
 * rondo — layout: master-stack and binary tree tiling
 */
#include "wm.h"

/* Apply only min/max size constraints for tiled windows.
 * PResizeInc and PAspect are for interactive resizing (character grid
 * snapping) and create gaps when applied to tiled layouts. */
static void apply_size_hints_tiled(Client *c, int *w, int *h) {
    if (c->size_hints_flags & PMinSize) {
        if (*w < c->min_width)  *w = c->min_width;
        if (*h < c->min_height) *h = c->min_height;
    }
    if (c->size_hints_flags & PMaxSize) {
        if (*w > c->max_width)  *w = c->max_width;
        if (*h > c->max_height) *h = c->max_height;
    }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

/* Place a single tiled client at (x,y) with size (w,h),
 * applying size hints and repositioning the X windows. */
static void place_client(Client *c, int x, int y, int w, int h) {
    c->x = x; c->y = y; c->w = w; c->h = h;
    int cx, cy, cw, ch;
    frame_to_client(c->w, c->h, &cx, &cy, &cw, &ch, c->no_decor);
    apply_size_hints_tiled(c, &cw, &ch);
    client_to_frame(cw, ch, &c->w, &c->h, c->no_decor);
    moveresizeframe(c);
    frame_to_client(c->w, c->h, &cx, &cy, &cw, &ch, c->no_decor);
    XMoveResizeWindow(dpy, c->win, cx, cy, cw, ch);
    updateframe(c);
    send_configure_notify(c);
}

/* ── master-stack layout ─────────────────────────────────────────────── */

static void arrange_master_stack(void) {
    int n = tiledcount();
    if (n == 0) return;

    BarGeometry g = calc_bar_geometry();
    int mx = g.x, my = g.y, mw = g.w, mh = g.h;

    Client *c = nexttiled(clients);
    if (!c) return;

    if (n == 1) {
        place_client(c, mx, my, mw, mh);
        return;
    }

    /* master area */
    int master_w = (int)(mw * mfact);
    int stack_w  = mw - master_w;

    place_client(c, mx, my, master_w, mh);

    /* stack area */
    c = nexttiled(c->next);
    int sy = my;

    for (; c; c = nexttiled(c->next)) {
        int remaining = mh - (sy - my);
        int windows_left = 0;
        for (Client *t = c; t; t = nexttiled(t->next)) windows_left++;
        int h = remaining / windows_left;

        place_client(c, mx + master_w, sy, stack_w, h);
        sy += c->h;
    }
}

/* ── binary tree layout ──────────────────────────────────────────────── */

typedef struct BTreeNode {
    Client *client;         /* non-NULL for leaf nodes */
    struct BTreeNode *left;
    struct BTreeNode *right;
    struct BTreeNode *parent;
    int split_vert;         /* 1 = vertical split, 0 = horizontal */
} BTreeNode;

static BTreeNode **btree_roots;  /* per-workspace roots */
static int btree_num_ws;

static BTreeNode *btree_new_leaf(Client *c) {
    BTreeNode *n = calloc(1, sizeof(BTreeNode));
    if (!n) return NULL;
    n->client = c;
    c->btree_node = n;
    return n;
}

static void btree_free_subtree(BTreeNode *node) {
    if (!node) return;
    if (node->client)
        node->client->btree_node = NULL;
    btree_free_subtree(node->left);
    btree_free_subtree(node->right);
    free(node);
}

static int btree_depth(BTreeNode *node) {
    int d = 0;
    while (node && node->parent) {
        d++;
        node = node->parent;
    }
    return d;
}

/* Ensure the per-workspace array is large enough for workspace index ws. */
static void btree_ensure_ws(int ws) {
    if (ws < btree_num_ws) return;
    int new_size = ws + 1;
    btree_roots = realloc(btree_roots, sizeof(BTreeNode *) * (size_t)new_size);
    for (int i = btree_num_ws; i < new_size; i++)
        btree_roots[i] = NULL;
    btree_num_ws = new_size;
}

void btree_add(Client *c) {
    btree_ensure_ws(c->ws);

    BTreeNode *leaf = btree_new_leaf(c);
    if (!leaf) return;

    BTreeNode *root = btree_roots[c->ws];

    if (!root) {
        /* first window on this workspace */
        btree_roots[c->ws] = leaf;
        return;
    }

    /* Find the focused tiled client's node on this workspace. */
    BTreeNode *split_target = NULL;
    if (focused && focused->ws == c->ws &&
        !focused->is_floating && !focused->is_minimized &&
        !focused->is_hidden && focused->btree_node) {
        split_target = focused->btree_node;
    } else {
        /* Fallback: use the deepest left-most leaf */
        split_target = root;
        while (split_target->left)
            split_target = split_target->left;
    }

    /* split_target is a leaf node (it has a client).
     * Turn it into an inner node whose children are
     * the original client and the new client. */
    Client *orig_client = split_target->client;
    BTreeNode *old_left = btree_new_leaf(orig_client);
    if (!old_left) { free(leaf); return; }

    /* Reuse split_target as the inner node */
    split_target->client = NULL;
    split_target->left = old_left;
    split_target->right = leaf;
    split_target->split_vert = (btree_depth(split_target) % 2 == 0);
    old_left->parent = split_target;
    leaf->parent = split_target;
}

void btree_remove(Client *c) {
    BTreeNode *leaf = c->btree_node;
    if (!leaf) return;
    c->btree_node = NULL;

    if (c->ws >= btree_num_ws) {
        /* workspace index out of range — just free the subtree */
        btree_free_subtree(leaf);
        return;
    }

    BTreeNode *root = btree_roots[c->ws];

    if (leaf == root) {
        /* removing the only window on this workspace */
        btree_roots[c->ws] = NULL;
        free(leaf);
        return;
    }

    /* leaf has a parent (inner node). Replace the parent with the sibling. */
    BTreeNode *parent = leaf->parent;
    BTreeNode *sibling = (parent->left == leaf) ? parent->right : parent->left;

    if (parent->parent) {
        if (parent->parent->left == parent)
            parent->parent->left = sibling;
        else
            parent->parent->right = sibling;
        sibling->parent = parent->parent;
    } else {
        /* parent is root */
        btree_roots[c->ws] = sibling;
        sibling->parent = NULL;
    }

    free(leaf);
    free(parent);
}

void btree_cleanup(void) {
    for (int i = 0; i < btree_num_ws; i++)
        btree_free_subtree(btree_roots[i]);
    free(btree_roots);
    btree_roots = NULL;
    btree_num_ws = 0;
}

static void btree_arrange_node(BTreeNode *node, int x, int y, int w, int h) {
    if (!node) return;
    if (node->client) {
        /* leaf node — place the client */
        place_client(node->client, x, y, w, h);
        return;
    }
    /* inner node — subdivide */
    if (node->split_vert) {
        int lw = w / 2;
        btree_arrange_node(node->left, x, y, lw, h);
        btree_arrange_node(node->right, x + lw, y, w - lw, h);
    } else {
        int th = h / 2;
        btree_arrange_node(node->left, x, y, w, th);
        btree_arrange_node(node->right, x, y + th, w, h - th);
    }
}

/* Ensure all tiled clients on the current workspace are in the btree.
 * This handles the case where we switch to btree layout after clients
 * were managed in master-stack mode (they were never added to the tree). */
static void btree_ensure_built(void) {
    btree_ensure_ws(curws);

    /* Add any visible tiled client on curws that isn't in the tree yet. */
    for (Client *c = clients; c; c = c->next) {
        if (c->ws == curws && !c->is_floating && !c->is_minimized &&
            !c->is_hidden && !c->btree_node)
            btree_add(c);
    }
}

static void arrange_btree(void) {
    btree_ensure_built();
    if (curws >= btree_num_ws || !btree_roots[curws]) return;
    BarGeometry g = calc_bar_geometry();
    btree_arrange_node(btree_roots[curws], g.x, g.y, g.w, g.h);
}

/* Swap two clients in the binary tree by exchanging their leaf node pointers. */
void btree_swap(Client *a, Client *b) {
    BTreeNode *na = a->btree_node;
    BTreeNode *nb = b->btree_node;
    if (!na || !nb) return;
    na->client = b;
    nb->client = a;
    a->btree_node = nb;
    b->btree_node = na;
}

/* ── dispatcher ─────────────────────────────────────────────────────── */

void arrange(void) {
    if (cur_layout == LAYOUT_BINARY_TREE)
        arrange_btree();
    else
        arrange_master_stack();
}