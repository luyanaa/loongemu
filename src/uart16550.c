/* NS16550A-compatible UART model. */
#include "loongemu/uart16550.h"

#define UART_RBR_THR_DLL 0
#define UART_IER_DLM     1
#define UART_IIR_FCR     2
#define UART_LCR         3
#define UART_MCR         4
#define UART_LSR         5
#define UART_MSR         6
#define UART_SCR         7

#define LCR_DLAB  0x80
#define LSR_DR    0x01
#define LSR_THRE  0x20
#define LSR_TEMT  0x40
#define IIR_NO_INT 0x01
#define IIR_THRE   0x02
#define IIR_RDA    0x04
#define IIR_FIFO_BITS 0xC0

static void update_irq(LoongemuUart16550 *uart)
{
    int level = ((uart->ier & 0x01) && uart->rx_available) ||
                ((uart->ier & 0x02) && uart->thre_pending);

    if (level != uart->irq_level) {
        uart->irq_level = level;
        if (uart->irq != 0) {
            uart->irq(uart->opaque, level);
        }
    }
}

void loongemu_uart16550_init(LoongemuUart16550 *uart,
                             void (*tx)(void *opaque, uint8_t byte),
                             void (*irq)(void *opaque, int level),
                             void *opaque)
{
    if (uart == 0) {
        return;
    }
    uart->dll = uart->dlm = uart->ier = uart->fcr = 0;
    uart->lcr = uart->mcr = uart->scr = 0;
    uart->rbr = 0;
    uart->rx_available = 0;
    uart->thre_pending = 1; /* transmitter starts empty */
    uart->fifo_enabled = 0;
    uart->tx = tx;
    uart->irq = irq;
    uart->opaque = opaque;
    uart->irq_level = 0;
    update_irq(uart);
}

void loongemu_uart16550_receive(LoongemuUart16550 *uart, uint8_t byte)
{
    if (uart == 0) {
        return;
    }
    uart->rbr = byte;
    uart->rx_available = 1;
    update_irq(uart);
}

static uint8_t read_reg(LoongemuUart16550 *uart, uint64_t offset)
{
    int dlab = (uart->lcr & LCR_DLAB) != 0;

    switch (offset & 7) {
    case UART_RBR_THR_DLL:
        if (dlab) {
            return uart->dll;
        }
        {
            uint8_t value = uart->rbr;
            uart->rx_available = 0;
            update_irq(uart);
            return value;
        }
    case UART_IER_DLM:
        return dlab ? uart->dlm : uart->ier;
    case UART_IIR_FCR: {
        uint8_t iir = IIR_NO_INT;
        if ((uart->ier & 0x01) && uart->rx_available) {
            iir = IIR_RDA;
        } else if ((uart->ier & 0x02) && uart->thre_pending) {
            iir = IIR_THRE;
            uart->thre_pending = 0; /* acknowledged by the IIR read */
            update_irq(uart);
        }
        if (uart->fifo_enabled) {
            iir |= IIR_FIFO_BITS;
        }
        return iir;
    }
    case UART_LCR:
        return uart->lcr;
    case UART_MCR:
        return uart->mcr;
    case UART_LSR:
        return (uint8_t)(LSR_THRE | LSR_TEMT |
                         (uart->rx_available ? LSR_DR : 0));
    case UART_MSR:
        return 0;
    default:
        return uart->scr;
    }
}

uint64_t loongemu_uart16550_read(LoongemuUart16550 *uart, uint64_t offset,
                                 unsigned size)
{
    uint64_t value = 0;

    if (uart == 0 || size == 0 || size > 4) {
        return 0;
    }
    for (unsigned i = 0; i < size; i++) {
        value |= (uint64_t)read_reg(uart, offset + i) << (8 * i);
    }
    return value;
}

static void write_reg(LoongemuUart16550 *uart, uint64_t offset, uint8_t value)
{
    int dlab = (uart->lcr & LCR_DLAB) != 0;

    switch (offset & 7) {
    case UART_RBR_THR_DLL:
        if (dlab) {
            uart->dll = value;
            return;
        }
        if (uart->tx != 0) {
            uart->tx(uart->opaque, value);
        }
        uart->thre_pending = 1;
        break;
    case UART_IER_DLM:
        if (dlab) {
            uart->dlm = value;
        } else {
            uart->ier = (uint8_t)(value & 0x0f);
        }
        break;
    case UART_IIR_FCR:
        uart->fcr = value;
        uart->fifo_enabled = value & 0x01;
        break;
    case UART_LCR:
        uart->lcr = value;
        break;
    case UART_MCR:
        uart->mcr = value;
        break;
    case UART_LSR:
        break; /* read-only */
    case UART_MSR:
        break; /* read-only */
    default:
        uart->scr = value;
        break;
    }
    update_irq(uart);
}

void loongemu_uart16550_write(LoongemuUart16550 *uart, uint64_t offset,
                              uint64_t value, unsigned size)
{
    if (uart == 0 || size == 0 || size > 4) {
        return;
    }
    for (unsigned i = 0; i < size; i++) {
        write_reg(uart, offset + i, (uint8_t)(value >> (8 * i)));
    }
}

int loongemu_uart16550_irq_level(const LoongemuUart16550 *uart)
{
    return uart == 0 ? 0 : uart->irq_level;
}

/* --- MMIO bus attachment --- */

static uint64_t uart_bus_read(void *opaque, uint64_t offset, unsigned size)
{
    return loongemu_uart16550_read((LoongemuUart16550 *)opaque, offset, size);
}

static void uart_bus_write(void *opaque, uint64_t offset, uint64_t value,
                           unsigned size)
{
    loongemu_uart16550_write((LoongemuUart16550 *)opaque, offset, value, size);
}

int loongemu_uart16550_attach(LoongemuMmioBus *bus, LoongemuUart16550 *uart,
                              uint64_t base, uint64_t size)
{
    if (bus == 0 || uart == 0 || size < 8) {
        return 0;
    }
    return loongemu_mmio_bus_add(bus, base, size, uart_bus_read, uart_bus_write,
                                 uart, "uart16550");
}

void loongemu_uart_plic_irq(void *opaque, int level)
{
    LoongemuUartPlicLine *line = (LoongemuUartPlicLine *)opaque;

    if (line == 0 || line->plic == 0) {
        return;
    }
    loongemu_plic_set_irq(line->plic, line->source, level);
}
