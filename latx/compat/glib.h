#ifndef LATX_BARE_GLIB_H
#define LATX_BARE_GLIB_H
/* Minimal glib vocabulary for the retained LATX sources.  The real GLib is a
 * host dependency the bare build must not have; these declarations exist so the
 * stripped headers still compile.  Helpers that are actually reached get
 * implementations in baremetal/shim/glib_stub.c. */
#include <stddef.h>
#include <stdint.h>

typedef struct _GTree GTree;
typedef struct _GHashTable GHashTable;
typedef struct _GPrivate { void *p; } GPrivate;
typedef void *gpointer;
typedef const void *gconstpointer;
typedef unsigned int guint;
typedef int gint;
typedef char gchar;
typedef int gboolean;
typedef unsigned long gulong;
typedef void (*GDestroyNotify)(gpointer);
typedef guint (*GHashFunc)(gconstpointer);
typedef gboolean (*GEqualFunc)(gconstpointer, gconstpointer);
typedef gint (*GCompareFunc)(gconstpointer, gconstpointer);

#define G_PRIVATE_INIT(f) { 0 }
#define g_assert(x) do { if (!(x)) { g_assert_not_reached(); } } while (0)
#define TRUE 1
#define FALSE 0

void  g_free(gpointer mem);
gpointer g_malloc(size_t n);
gpointer g_malloc0(size_t n);
gpointer g_realloc(gpointer mem, size_t n);
#define g_new(type, n)   ((type *)g_malloc(sizeof(type) * (size_t)(n)))
#define g_new0(type, n)  ((type *)g_malloc0(sizeof(type) * (size_t)(n)))

void  g_assertion_failed(const char *expr, const char *file, int line, const char *func);
void  g_assert_not_reached(void) __attribute__((noreturn));

void  g_private_set(GPrivate *key, gpointer value);
gpointer g_private_get(GPrivate *key);

GTree *g_tree_new(GCompareFunc cmp);
void  g_tree_ref(GTree *tree);
void  g_tree_destroy(GTree *tree);
gpointer g_tree_lookup(GTree *tree, gconstpointer key);
void  g_tree_insert(GTree *tree, gpointer key, gpointer value);

GHashTable *g_hash_table_new(GHashFunc hash, GEqualFunc eq);
gpointer    g_hash_table_lookup(GHashTable *h, gconstpointer key);
void        g_hash_table_insert(GHashTable *h, gpointer key, gpointer value);
void        g_hash_table_destroy(GHashTable *h);
guint       g_str_hash(gconstpointer v);
gboolean    g_str_equal(gconstpointer a, gconstpointer b);
#endif
