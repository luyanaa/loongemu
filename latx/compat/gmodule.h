#ifndef LATX_BARE_GMODULE_H
#define LATX_BARE_GMODULE_H
/* Minimal stand-in for the glib surface pulled in by tu.h.  The TU optimiser is
 * disabled in this build, so only the type/allocator vocabulary is needed. */
#include "qemu/osdep.h"
typedef struct _GModule { int unused; } GModule;
#endif
