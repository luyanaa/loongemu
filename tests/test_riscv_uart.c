/* Host tests for the 16550 UART model and its MMIO bus attachment. */
#include <stdio.h>
#include <string.h>
#include "loongemu/uart16550.h"
#include "loongemu/mmio_bus.h"

static int checks;
static int failures;

#define CHECK(cond, name)                                                   \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "UART: FAIL %s\n", name);                       \
        }                                                                   \
    } while (0)

static char tx_buf[64];
static int tx_len;
static int irq_levels[8];
static int irq_count;

static void tx_cb(void *opaque, uint8_t byte)
{
    (void)opaque;
    if (tx_len < (int)sizeof(tx_buf) - 1) {
        tx_buf[tx_len++] = (char)byte;
        tx_buf[tx_len] = 0;
    }
}

static void irq_cb(void *opaque, int level)
{
    (void)opaque;
    if (irq_count < 8) {
        irq_levels[irq_count++] = level;
    }
}

int main(void)
{
    LoongemuUart16550 uart;
    LoongemuMmioBus bus;
    uint64_t value = 0;

    loongemu_uart16550_init(&uart, tx_cb, irq_cb, NULL);
    tx_len = 0;
    irq_count = 0;

    CHECK(loongemu_uart16550_read(&uart, 5, 1) == 0x60,
          "LSR reports THRE and TEMT after init");
    CHECK(loongemu_uart16550_read(&uart, 2, 1) == 0x01,
          "IIR has no pending interrupt after init");

    /* Divisor latch access through LCR.DLAB. */
    loongemu_uart16550_write(&uart, 3, 0x83, 1); /* 8N1 + DLAB */
    loongemu_uart16550_write(&uart, 0, 1, 1);    /* DLL */
    loongemu_uart16550_write(&uart, 1, 2, 1);    /* DLM */
    CHECK(loongemu_uart16550_read(&uart, 0, 1) == 1, "DLL round-trips");
    CHECK(loongemu_uart16550_read(&uart, 1, 1) == 2, "DLM round-trips");
    loongemu_uart16550_write(&uart, 3, 0x03, 1); /* clear DLAB */

    /* THR writes reach the host console and keep THRE asserted. */
    loongemu_uart16550_write(&uart, 0, 'A', 1);
    CHECK(tx_len == 1 && tx_buf[0] == 'A', "THR write reaches the console");
    CHECK(loongemu_uart16550_read(&uart, 5, 1) & 0x20,
          "LSR stays THRE after a write");

    /* THRE interrupt: level high until IIR acknowledges it. */
    irq_count = 0;
    loongemu_uart16550_write(&uart, 1, 0x02, 1); /* IER.THRE */
    CHECK(irq_count == 1 && irq_levels[0] == 1,
          "IER.THRE raises the interrupt line");
    CHECK(loongemu_uart16550_irq_level(&uart) == 1, "IRQ level is high");
    CHECK(loongemu_uart16550_read(&uart, 2, 1) == 0x02,
          "IIR reports THRE");
    CHECK(loongemu_uart16550_irq_level(&uart) == 0,
          "IIR read acknowledges THRE");
    CHECK(loongemu_uart16550_read(&uart, 2, 1) == 0x01,
          "IIR is idle after the acknowledgement");
    loongemu_uart16550_write(&uart, 1, 0x00, 1); /* IER off */

    /* Receive path: DR, RBR and the RDA interrupt. */
    irq_count = 0;
    loongemu_uart16550_write(&uart, 1, 0x01, 1); /* IER.RDA */
    loongemu_uart16550_receive(&uart, 'B');
    CHECK(loongemu_uart16550_read(&uart, 5, 1) & 0x01,
          "LSR.DR is set after a received byte");
    CHECK(irq_count == 1 && irq_levels[0] == 1, "RDA raises the line");
    CHECK(loongemu_uart16550_read(&uart, 2, 1) == 0x04, "IIR reports RDA");
    CHECK(loongemu_uart16550_read(&uart, 0, 1) == 'B', "RBR returns the byte");
    CHECK(loongemu_uart16550_read(&uart, 5, 1) == 0x60,
          "reading RBR clears DR");
    CHECK(loongemu_uart16550_irq_level(&uart) == 0, "RDA line drops");
    loongemu_uart16550_write(&uart, 1, 0x00, 1);

    /* FIFO enable changes the IIR identification bits. */
    loongemu_uart16550_write(&uart, 2, 0x01, 1); /* FCR.FIFO enable */
    loongemu_uart16550_receive(&uart, 'C');
    CHECK((loongemu_uart16550_read(&uart, 2, 1) & 0xC0) == 0xC0,
          "FIFO-enabled IIR sets bits 7:6");
    (void)loongemu_uart16550_read(&uart, 0, 1);

    /* Bus attachment: guest-style byte accesses. */
    loongemu_mmio_bus_init(&bus);
    CHECK(loongemu_uart16550_attach(&bus, &uart, 0x10000000, 0x100) == 1,
          "UART attaches to the bus");
    tx_len = 0;
    CHECK(loongemu_mmio_bus_write(&bus, 0x10000000, 1, 'D') == 0,
          "bus write reaches the UART");
    CHECK(tx_len == 1 && tx_buf[0] == 'D', "bus THR write reaches the console");
    CHECK(loongemu_mmio_bus_read(&bus, 0x10000005, 1, &value) == 0 &&
          value == 0x60, "bus LSR read");
    CHECK(loongemu_mmio_bus_read(&bus, 0x10000100, 1, &value) != 0,
          "out-of-window UART access fails");

    /*
     * Route cache: a repeated access reuses the remembered device, but the
     * width is part of the key and a registry change invalidates it.
     */
    CHECK(loongemu_mmio_bus_read(&bus, 0x10000005, 1, &value) == 0 &&
          value == 0x60, "repeat access hits the route cache");
    CHECK(loongemu_mmio_bus_read(&bus, 0x10000005, 4, &value) == 0,
          "same address with another width still resolves");
    CHECK(loongemu_mmio_bus_remove(&bus, 0x10000000) == 1,
          "UART detaches from the bus");
    CHECK(loongemu_mmio_bus_read(&bus, 0x10000005, 1, &value) != 0,
          "a cached route cannot outlive its device");
    CHECK(loongemu_uart16550_attach(&bus, &uart, 0x10000000, 0x100) == 1,
          "UART re-attaches after removal");
    CHECK(loongemu_mmio_bus_read(&bus, 0x10000005, 1, &value) == 0 &&
          value == 0x60, "re-attached UART answers the same address");

    printf("UART: %d checks passed\n", checks);
    return failures == 0 ? 0 : 1;
}
