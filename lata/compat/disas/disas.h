#ifndef LATA_DISAS_H
#define LATA_DISAS_H
#include "qemu/osdep.h"
#include <stdio.h>
void disas(FILE *out, void *code, unsigned long size);
#endif
