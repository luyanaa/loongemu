#ifndef LATX_BARE_QAPI_ERROR_H
#define LATX_BARE_QAPI_ERROR_H
/* Error reporting from the copied QEMU helpers is unused in the bare build:
 * provide the type and the macro surface, all no-ops. */
#include "qemu/osdep.h"
#include "qapi/qapi-types-error.h"
typedef struct Error { QapiErrorClass cls; } Error;
static inline void error_setg(Error **errp, const char *fmt, ...) { (void)errp; (void)fmt; }
static inline void error_setg_errno(Error **errp, int e, const char *fmt, ...) { (void)errp; (void)e; (void)fmt; }
static inline void error_free(Error *err) { (void)err; }
static inline const char *error_get_pretty(const Error *err) { (void)err; return ""; }
#endif
