#ifndef LATM_UART_H
#define LATM_UART_H

#include <stdint.h>
#include "latm-system.h"

typedef struct LatmUart {
    uint8_t ier, fcr, lcr, mcr, scr, dll, dlm;
    uint8_t tx_pending;
    uint8_t rx_fifo[16];
    unsigned rx_head, rx_tail;
    uint64_t tx_count;
    uint64_t rd_count, wr_count;
    uint64_t last_rd_off, last_wr_off;
    /* Optional host-side sink for transmitted bytes.  NULL until the
     * harness wires the guest serial console to the host output. */
    void (*tx_emit)(void *ctx, uint8_t byte);
    void *tx_emit_ctx;
} LatmUart;

void latm_uart_init(LatmUart *u);
uint64_t latm_uart_read(void *opaque, uint64_t off, unsigned size);
void latm_uart_write(void *opaque, uint64_t off, uint64_t value, unsigned size);
void latm_uart_rx_push(LatmUart *u, uint8_t value);
int latm_uart_irq_level(const LatmUart *u);

#endif
