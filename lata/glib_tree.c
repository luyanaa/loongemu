/* Minimal, real GTree (unbalanced BST) for the LATA TU optimiser.
 * Refcount semantics follow GLib: g_tree_destroy() drops one reference. */
#include "glib.h"
#include <stdlib.h>

struct node {
    struct node *left, *right;
    gpointer key, value;
};

struct _GTree {
    GCompareFunc cmp;
    int refs;
    struct node *root;
};

static struct node *node_new(gpointer key, gpointer value)
{
    struct node *n = malloc(sizeof(*n));
    if (n) { n->left = n->right = NULL; n->key = key; n->value = value; }
    return n;
}

static void node_free(struct node *n)
{
    if (!n) return;
    node_free(n->left);
    node_free(n->right);
    free(n);
}

GTree *g_tree_new(GCompareFunc cmp)
{
    GTree *t = malloc(sizeof(*t));
    if (t) { t->cmp = cmp; t->refs = 1; t->root = NULL; }
    return t;
}

void g_tree_ref(GTree *tree)
{
    if (tree) tree->refs++;
}

void g_tree_destroy(GTree *tree)
{
    if (!tree) return;
    if (--tree->refs > 0) return;
    node_free(tree->root);
    free(tree);
}

gpointer g_tree_lookup(GTree *tree, gconstpointer key)
{
    struct node *n = tree ? tree->root : NULL;
    while (n) {
        gint c = tree->cmp(key, n->key);
        if (c == 0) return n->value;
        n = (c < 0) ? n->left : n->right;
    }
    return NULL;
}

void g_tree_insert(GTree *tree, gpointer key, gpointer value)
{
    struct node **link;
    if (!tree) return;
    link = &tree->root;
    while (*link) {
        gint c = tree->cmp(key, (*link)->key);
        if (c == 0) { (*link)->value = value; return; }
        link = (c < 0) ? &(*link)->left : &(*link)->right;
    }
    *link = node_new(key, value);
}
