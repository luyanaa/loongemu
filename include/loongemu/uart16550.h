#ifndef LOONGEMU_UART16550_H
#define LOONGEMU_UART16550_H

#include <stdint.h>
#include "loongemu/mmio_bus.h"
#include "loongemu/riscv_plic.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NS16550A-compatible UART model (8-bit registers, 16550A FIFO capability).
 * THR writes go to the host console through `tx`; received bytes are injected
 * with loongemu_uart16550_receive(). The interrupt output is a level that the
 * platform wires to a PLIC/APLIC source.
 */

typedef struct LoongemuUart16550 {
    uint8_t dll;
    uint8_t dlm;
    uint8_t ier; /* bits: 0 RDA, 1 THRE, 2 RLOS, 3 MSI */
    uint8_t fcr;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t scr;
    uint8_t rbr;
    int rx_available;
    int thre_pending;
    int fifo_enabled;
    void (*tx)(void *opaque, uint8_t byte);
    void (*irq)(void *opaque, int level);
    void *opaque;
    int irq_level;
} LoongemuUart16550;

void loongemu_uart16550_init(LoongemuUart16550 *uart,
                             void (*tx)(void *opaque, uint8_t byte),
                             void (*irq)(void *opaque, int level),
                             void *opaque);

/* Injects one received byte (sets DR and may raise an interrupt). */
void loongemu_uart16550_receive(LoongemuUart16550 *uart, uint8_t byte);

/* Register access; `size` is 1..4 and reads assemble consecutive registers. */
uint64_t loongemu_uart16550_read(LoongemuUart16550 *uart, uint64_t offset,
                                 unsigned size);
void loongemu_uart16550_write(LoongemuUart16550 *uart, uint64_t offset,
                              uint64_t value, unsigned size);

int loongemu_uart16550_irq_level(const LoongemuUart16550 *uart);

/* Registers the UART at `base` on the MMIO bus (byte register window). */
int loongemu_uart16550_attach(LoongemuMmioBus *bus, LoongemuUart16550 *uart,
                              uint64_t base, uint64_t size);

/* Bridges the UART interrupt line to a PLIC source. */
typedef struct LoongemuUartPlicLine {
    LoongemuPlic *plic;
    uint32_t source;
} LoongemuUartPlicLine;

void loongemu_uart_plic_irq(void *opaque, int level);

#ifdef __cplusplus
}
#endif

#endif /* LOONGEMU_UART16550_H */
