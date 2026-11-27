#include "qemu/osdep.h"
#include "latm-uart.h"

void latm_uart_init(LatmUart *u)
{
    if (u) memset(u, 0, sizeof(*u));
}

static int rx_available(const LatmUart *u)
{
    return u->rx_head != u->rx_tail;
}

void latm_uart_rx_push(LatmUart *u, uint8_t value)
{
    if (!u) return;
    unsigned next = (u->rx_head + 1) % sizeof(u->rx_fifo);
    if (next != u->rx_tail) {
        u->rx_fifo[u->rx_head] = value;
        u->rx_head = next;
    }
}

uint64_t latm_uart_read(void *opaque, uint64_t off, unsigned size)
{
    LatmUart *u = opaque;
    if (!u) return 0;
    u->rd_count++;
    u->last_rd_off = off & 7;
    int dlab = (u->lcr & 0x80) != 0;
    switch (off & 7) {
    case 0:
        if (dlab) return u->dll;
        if (rx_available(u)) {
            uint8_t v = u->rx_fifo[u->rx_tail];
            u->rx_tail = (u->rx_tail + 1) % sizeof(u->rx_fifo);
            return v;
        }
        return 0;
    case 1: return dlab ? u->dlm : u->ier;
    case 2: {
        uint8_t v = ((u->ier & 1) && rx_available(u)) ? 0x04 :
                    ((u->ier & 2) && u->tx_pending) ? 0x02 : 0x01;
        u->tx_pending = 0;
        return v;
    }
    case 3: return u->lcr;
    case 4: return u->mcr;
    case 5: return 0x60 | (rx_available(u) ? 1 : 0);
    case 6: return 0xb0;
    default: return u->scr;
    }
}

void latm_uart_write(void *opaque, uint64_t off, uint64_t value, unsigned size)
{
    LatmUart *u = opaque;
    if (!u) return;
    u->wr_count++;
    int dlab = (u->lcr & 0x80) != 0;
    switch (off & 7) {
    case 0:
        if (dlab) u->dll = (uint8_t)value;
        else {
            u->tx_count++;
            if (u->ier & 2) u->tx_pending = 1;
            if (u->tx_emit) u->tx_emit(u->tx_emit_ctx, (uint8_t)value);
        }
        break;
    case 1:
        if (dlab) u->dlm = (uint8_t)value;
        else {
            u->ier = (uint8_t)value & 0x0f;
            if (u->ier & 2) u->tx_pending = 1;
        }
        break;
    case 2:
        u->fcr = (uint8_t)value;
        if (value & 0x04) u->tx_pending = 0;
        if (value & 0x02) u->rx_head = u->rx_tail = 0;
        break;
    case 3: u->lcr = (uint8_t)value; break;
    case 4: u->mcr = (uint8_t)value; break;
    case 5: case 6: break;
    default: u->scr = (uint8_t)value; break;
    }
}

int latm_uart_irq_level(const LatmUart *u)
{
    return u && (((u->ier & 1) && rx_available(u)) ||
                 ((u->ier & 2) && u->tx_pending));
}
