#ifndef LATX_BARE_QEMU_GUEST_RANDOM_H
#define LATX_BARE_QEMU_GUEST_RANDOM_H
#include "qemu/osdep.h"
#include "qapi/error.h"
int qemu_guest_getrandom(void *buf, size_t num, Error **errp);
void qemu_guest_getrandom_nofail(void *buf, size_t num);
#endif
