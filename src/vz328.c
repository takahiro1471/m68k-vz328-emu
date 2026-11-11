/*
 * vz328.c - MC68VZ328 board/machine emulation
 *
 * Memory map used by the software experiment CPU board:
 *
 *   0x000000 - 0x03FFFF   RAM (256 KiB)
 *   0xC00000 - 0xC7FFFF   ROM (the emulator installs a small exception stub)
 *   0xD00000 - 0xD1FFFF   board I/O (LEDs and toggle switches)
 *   0xFFF000 - 0xFFFFFF   MC68VZ328 internal registers
 *
 * Only the peripheral registers needed by the UART/TIMER experiment are
 * implemented.  Unimplemented registers read as zero and ignore writes.
 */
#include "vz328.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "m68k.h"

/* Interrupt controller registers */
#define REG_IVR    0x300
#define REG_IMR    0x304
#define REG_ISR    0x30c
#define REG_IPR    0x310
#define REG_ILCR   0x314

/* General purpose timers */
#define REG_TIMER1 0x600
#define REG_TIMER2 0x610

/* UARTs */
#define REG_UART1  0x900
#define REG_UART2  0x910

/* Board I/O */
#define IO_TOGGLE_SWITCH 0x00d00041u

/* Interrupt source bits in IMR/ISR/IPR */
#define IRQ_TIMER1 (1u << 1)
#define IRQ_UART1  (1u << 2)
#define IRQ_TIMER2 (1u << 5)
#define IRQ_UART2  (1u << 12)

static int timer_irq_level(const vz328_t *m, int channel)
{
    unsigned level;

    if (channel == 0)
        return 6;                /* Timer 1 is fixed at level 6 */
    level = m->ilcr & 0x7;       /* Timer 2 is configurable */
    return (level >= 1 && level <= 6) ? (int)level : 3;
}

static int uart_irq_level(const vz328_t *m, int channel)
{
    unsigned level;

    if (channel == 0)
        return 4;                /* UART 1 is fixed at level 4 */
    level = (m->ilcr >> 8) & 0x7; /* UART 2 is configurable */
    return (level >= 1 && level <= 6) ? (int)level : 5;
}

static bool in_regs(uint32_t a)
{
    return a >= VZ_REG_BASE && a < VZ_REG_BASE + VZ_REG_SIZE;
}

static bool in_io(uint32_t a)
{
    return a >= VZ_IO_BASE && a < VZ_IO_BASE + VZ_IO_SIZE;
}

static bool in_rom(uint32_t a)
{
    return a >= VZ_ROM_BASE && a < VZ_ROM_BASE + VZ_ROM_SIZE;
}

static void trace_io(const vz328_t *m, const char *op, int size,
                     uint32_t address, uint32_t value)
{
    if (m->trace_io)
        fprintf(stderr, "[io] %s%d %06" PRIX32 " = %0*" PRIX32 "\n",
                op, size, address & VZ_ADDR_MASK, size * 2, value);
}

static void request_halt(vz328_t *m, uint16_t code)
{
    m->halted = true;
    m->halt_code = code;
    m->running = false;
}

/* ------------------------------------------------------------------ */
/* Board I/O                                                          */
/* ------------------------------------------------------------------ */

static int led_index(uint32_t address)
{
    switch (address) {
    case 0x00d00039u: return 0;  /* LED0 */
    case 0x00d0003bu: return 1;  /* LED1 */
    case 0x00d0003du: return 2;  /* LED2 */
    case 0x00d0003fu: return 3;  /* LED3 */
    case 0x00d00029u: return 4;  /* LED4 */
    case 0x00d0002bu: return 5;  /* LED5 */
    case 0x00d0002du: return 6;  /* LED6 */
    case 0x00d0002fu: return 7;  /* LED7 */
    default: return -1;
    }
}

static uint32_t io_read8(vz328_t *m, uint32_t address)
{
    uint32_t v = 0;

    if (address == IO_TOGGLE_SWITCH)
        v = m->toggle_switches;
    trace_io(m, "R", 1, address, v);
    return v;
}

static void io_write8(vz328_t *m, uint32_t address, uint32_t value)
{
    int led = led_index(address);

    trace_io(m, "W", 1, address, value);
    if (led >= 0) {
        m->led[led] = (uint8_t)value;
        if (m->show_led) {
            int i;
            fprintf(stderr, "[LED] ");
            for (i = 7; i >= 0; i--) {
                uint8_t c = m->led[i];
                fputc((c >= 0x20 && c < 0x7f) ? c : '.', stderr);
            }
            fputc('\n', stderr);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Internal peripheral registers                                      */
/* ------------------------------------------------------------------ */

static uint32_t irq_reg_byte(uint32_t reg, uint8_t byte)
{
    int shift = (int)(3 - (byte & 3)) * 8;
    return (reg >> shift) & 0xff;
}

static uint32_t timer_read_byte(vz328_t *m, uint32_t r)
{
    if (r >= REG_TIMER1 && r < REG_TIMER1 + 0x0c)
        return timer_read(&m->timer[0], (uint16_t)(r - REG_TIMER1), 1);
    if (r >= REG_TIMER2 && r < REG_TIMER2 + 0x0c)
        return timer_read(&m->timer[1], (uint16_t)(r - REG_TIMER2), 1);
    return 0;
}

static uint32_t uart_read_byte(vz328_t *m, uint32_t r, double now)
{
    if (r >= REG_UART1 && r < REG_UART1 + 0x0c)
        return uart_read(&m->uart[0], (uint16_t)(r - REG_UART1), 1, now);
    if (r >= REG_UART2 && r < REG_UART2 + 0x0c)
        return uart_read(&m->uart[1], (uint16_t)(r - REG_UART2), 1, now);
    return 0;
}

static uint32_t reg_read8(vz328_t *m, uint32_t r, double now)
{
    uint32_t v;

    if (r >= REG_IMR && r < REG_IMR + 4)
        v = irq_reg_byte(m->imr, (uint8_t)(r - REG_IMR));
    else if (r >= REG_ISR && r < REG_ISR + 4)
        v = irq_reg_byte(m->isr, (uint8_t)(r - REG_ISR));
    else if (r >= REG_IPR && r < REG_IPR + 4)
        v = irq_reg_byte(m->ipr, (uint8_t)(r - REG_IPR));
    else if (r == REG_IVR)
        v = m->ivr & 0xff;
    else if (r == REG_ILCR)
        v = (m->ilcr >> 8) & 0xff;
    else if (r == REG_ILCR + 1)
        v = m->ilcr & 0xff;
    else if (r >= REG_TIMER1 && r < REG_TIMER2 + 0x0c)
        v = timer_read_byte(m, r);
    else if (r >= REG_UART1 && r < REG_UART2 + 0x0c)
        v = uart_read_byte(m, r, now);
    else if (r == 0x418)
        v = 0;                   /* PDDATA high byte */
    else if (r == 0x419)
        v = (0xf0u | (m->push_buttons & 0x0fu));
    else if (r == 0x41a || r == 0x41b)
        v = 0;
    else
        v = 0;

    trace_io(m, "R", 1, VZ_REG_BASE + r, v);
    return v;
}

static void timer_write_word(vz328_t *m, uint32_t r, uint32_t value,
                             int size, double now)
{
    if (r >= REG_TIMER1 && r < REG_TIMER1 + 0x0c)
        timer_write(&m->timer[0], (uint16_t)(r - REG_TIMER1), value, size, now);
    else if (r >= REG_TIMER2 && r < REG_TIMER2 + 0x0c)
        timer_write(&m->timer[1], (uint16_t)(r - REG_TIMER2), value, size, now);
}

static void uart_write_word(vz328_t *m, uint32_t r, uint32_t value,
                            int size, double now)
{
    if (r >= REG_UART1 && r < REG_UART1 + 0x0c)
        uart_write(&m->uart[0], (uint16_t)(r - REG_UART1), value, size, now);
    else if (r >= REG_UART2 && r < REG_UART2 + 0x0c)
        uart_write(&m->uart[1], (uint16_t)(r - REG_UART2), value, size, now);
}

static void reg_write8(vz328_t *m, uint32_t r, uint32_t value, double now)
{
    uint32_t v = value & 0xff;

    if (r >= REG_IMR && r < REG_IMR + 4) {
        int shift = (int)(3 - (r - REG_IMR)) * 8;
        m->imr = (m->imr & ~(0xffu << shift)) | (v << shift);
    } else if (r >= REG_ISR && r < REG_ISR + 4) {
        /* External edge-triggered bits are W1C; internal bits are level
         * triggered and cleared at their source.  The emulator has no
         * external IRQ pins, so writing ISR has no effect. */
    } else if (r >= REG_IPR && r < REG_IPR + 4) {
        /* read-only */
    } else if (r == REG_IVR) {
        m->ivr = v;
    } else if (r == REG_ILCR) {
        m->ilcr = (m->ilcr & 0x00ff) | (v << 8);
    } else if (r == REG_ILCR + 1) {
        m->ilcr = (m->ilcr & 0xff00) | v;
    } else if (r >= REG_TIMER1 && r < REG_TIMER2 + 0x0c) {
        timer_write_word(m, r, v, 1, now);
    } else if (r >= REG_UART1 && r < REG_UART2 + 0x0c) {
        uart_write_word(m, r, v, 1, now);
    } else if (r == 0x419) {
        /* PDDATA is an input port on this board. */
    }

    trace_io(m, "W", 1, VZ_REG_BASE + r, v);
}

static void reg_write16(vz328_t *m, uint32_t r, uint32_t value, double now)
{
    uint16_t v = (uint16_t)value;

    if (r == REG_IVR) {
        m->ivr = (value >> 8) & 0xff;
    } else if (r == REG_IMR) {
        m->imr = (m->imr & 0x0000ffffu) | ((uint32_t)v << 16);
    } else if (r == REG_IMR + 2) {
        m->imr = (m->imr & 0xffff0000u) | v;
    } else if (r == REG_ISR || r == REG_ISR + 2 || r == REG_IPR ||
               r == REG_IPR + 2) {
        /* ISR/IPR: ISR external bits are W1C; IPR is read-only. */
    } else if (r == REG_ILCR) {
        m->ilcr = v;
    } else if (r >= REG_TIMER1 && r < REG_TIMER2 + 0x0c) {
        timer_write_word(m, r, v, 2, now);
    } else if (r >= REG_UART1 && r < REG_UART2 + 0x0c) {
        uart_write_word(m, r, v, 2, now);
    } else if (r == VZ_MAGIC_ADDRESS - VZ_REG_BASE) {
        if (v == VZ_MAGIC_EXCEPTION || v == VZ_MAGIC_EXIT)
            request_halt(m, v);
    }

    trace_io(m, "W", 2, VZ_REG_BASE + r, v);
}

/* ------------------------------------------------------------------ */
/* CPU bus access                                                     */
/* ------------------------------------------------------------------ */

uint32_t vz328_read8(vz328_t *m, uint32_t address)
{
    double now = (double)m->cycles;

    address &= VZ_ADDR_MASK;
    if (in_regs(address)) {
        uint32_t v = reg_read8(m, address - VZ_REG_BASE, now);
        /* Reading URX pops data and may clear a receive interrupt. */
        vz328_update_irqs(m);
        return v;
    }
    if (in_io(address))
        return io_read8(m, address);
    return m->mem[address];
}

uint32_t vz328_read16(vz328_t *m, uint32_t address)
{
    double now = (double)m->cycles;
    uint32_t a = address & VZ_ADDR_MASK;
    uint32_t v;
    uint32_t r;

    if (!in_regs(a) && !in_io(a))
        return ((uint32_t)m->mem[a] << 8) |
               m->mem[(a + 1) & VZ_ADDR_MASK];

    if (in_io(a)) {
        v = (io_read8(m, a) << 8) |
            io_read8(m, (a + 1) & VZ_ADDR_MASK);
        trace_io(m, "R", 2, a, v);
        return v;
    }

    r = a - VZ_REG_BASE;
    if (r >= REG_IMR && r < REG_IMR + 4) {
        if ((r & 3) == 0)
            v = m->imr >> 16;
        else
            v = m->imr & 0xffff;
    } else if (r >= REG_ISR && r < REG_ISR + 4) {
        if ((r & 3) == 0)
            v = m->isr >> 16;
        else
            v = m->isr & 0xffff;
    } else if (r >= REG_IPR && r < REG_IPR + 4) {
        if ((r & 3) == 0)
            v = m->ipr >> 16;
        else
            v = m->ipr & 0xffff;
    } else if (r == REG_IVR) {
        v = (m->ivr & 0xff) << 8;
    } else if (r == REG_ILCR) {
        v = m->ilcr & 0xffff;
    } else if (r >= REG_TIMER1 && r < REG_TIMER1 + 0x0c) {
        v = timer_read(&m->timer[0], (uint16_t)(r - REG_TIMER1), 2);
    } else if (r >= REG_TIMER2 && r < REG_TIMER2 + 0x0c) {
        v = timer_read(&m->timer[1], (uint16_t)(r - REG_TIMER2), 2);
    } else if (r >= REG_UART1 && r < REG_UART1 + 0x0c) {
        v = uart_read(&m->uart[0], (uint16_t)(r - REG_UART1), 2, now);
    } else if (r >= REG_UART2 && r < REG_UART2 + 0x0c) {
        v = uart_read(&m->uart[1], (uint16_t)(r - REG_UART2), 2, now);
    } else if (r == 0x418) {
        v = 0xf0u | (m->push_buttons & 0x0fu);
    } else {
        v = 0;
    }

    trace_io(m, "R", 2, a, v);
    vz328_update_irqs(m);
    return v;
}

uint32_t vz328_read32(vz328_t *m, uint32_t address)
{
    uint32_t a = address & VZ_ADDR_MASK;
    uint32_t r;
    uint32_t v;

    if (!in_regs(a) && !in_io(a)) {
        return ((uint32_t)m->mem[a] << 24) |
               ((uint32_t)m->mem[(a + 1) & VZ_ADDR_MASK] << 16) |
               ((uint32_t)m->mem[(a + 2) & VZ_ADDR_MASK] << 8) |
               m->mem[(a + 3) & VZ_ADDR_MASK];
    }

    if (in_io(a)) {
        v = (vz328_read16(m, a) << 16) | vz328_read16(m, a + 2);
        return v;
    }

    r = a - VZ_REG_BASE;
    if (r == REG_IMR)
        v = m->imr;
    else if (r == REG_ISR)
        v = m->isr;
    else if (r == REG_IPR)
        v = m->ipr;
    else if (r == REG_IVR)
        v = (m->ivr & 0xff) << 24;
    else
        v = (vz328_read16(m, a) << 16) | vz328_read16(m, a + 2);

    trace_io(m, "R", 4, a, v);
    vz328_update_irqs(m);
    return v;
}

void vz328_write8(vz328_t *m, uint32_t address, uint32_t value)
{
    double now = (double)m->cycles;

    address &= VZ_ADDR_MASK;
    if (in_regs(address)) {
        reg_write8(m, address - VZ_REG_BASE, value, now);
        vz328_update_irqs(m);
        return;
    }
    if (in_io(address)) {
        io_write8(m, address, value);
        return;
    }
    if (in_rom(address))
        return;                  /* ROM is read-only */
    m->mem[address] = (uint8_t)value;
}

void vz328_write16(vz328_t *m, uint32_t address, uint32_t value)
{
    double now = (double)m->cycles;
    uint32_t a = address & VZ_ADDR_MASK;

    if (in_regs(a)) {
        reg_write16(m, a - VZ_REG_BASE, value, now);
        vz328_update_irqs(m);
        return;
    }
    if (in_io(a)) {
        io_write8(m, a, value >> 8);
        io_write8(m, a + 1, value & 0xff);
        return;
    }
    if (in_rom(a))
        return;
    m->mem[a] = (uint8_t)(value >> 8);
    m->mem[(a + 1) & VZ_ADDR_MASK] = (uint8_t)value;
}

void vz328_write32(vz328_t *m, uint32_t address, uint32_t value)
{
    double now = (double)m->cycles;
    uint32_t a = address & VZ_ADDR_MASK;
    uint32_t r;

    if (in_regs(a)) {
        r = a - VZ_REG_BASE;
        if (r == REG_IMR) {
            m->imr = value;
        } else if (r == REG_ISR || r == REG_IPR) {
            /* See reg_write16(). */
        } else if (r == REG_IVR) {
            m->ivr = (value >> 24) & 0xff;
        } else {
            reg_write16(m, r, value >> 16, now);
            reg_write16(m, r + 2, value & 0xffff, now);
        }
        trace_io(m, "W", 4, a, value);
        vz328_update_irqs(m);
        return;
    }
    if (in_io(a)) {
        io_write8(m, a, value >> 24);
        io_write8(m, a + 1, (value >> 16) & 0xff);
        io_write8(m, a + 2, (value >> 8) & 0xff);
        io_write8(m, a + 3, value & 0xff);
        return;
    }
    if (in_rom(a))
        return;
    m->mem[a] = (uint8_t)(value >> 24);
    m->mem[(a + 1) & VZ_ADDR_MASK] = (uint8_t)(value >> 16);
    m->mem[(a + 2) & VZ_ADDR_MASK] = (uint8_t)(value >> 8);
    m->mem[(a + 3) & VZ_ADDR_MASK] = (uint8_t)value;
}

/* ------------------------------------------------------------------ */
/* Interrupt controller                                               */
/* ------------------------------------------------------------------ */

void vz328_update_irqs(vz328_t *m)
{
    double now = (double)m->cycles;
    uint32_t raw = 0;
    uint32_t unmasked;
    int level = 0;

    if (uart_irq(&m->uart[0], now))
        raw |= IRQ_UART1;
    if (timer_irq(&m->timer[0]))
        raw |= IRQ_TIMER1;
    if (uart_irq(&m->uart[1], now))
        raw |= IRQ_UART2;
    if (timer_irq(&m->timer[1]))
        raw |= IRQ_TIMER2;

    m->ipr = raw;
    unmasked = raw & ~m->imr;
    m->isr = unmasked;

    if (unmasked & IRQ_TIMER1)
        level = timer_irq_level(m, 0);
    if ((unmasked & IRQ_UART1) && uart_irq_level(m, 0) > level)
        level = uart_irq_level(m, 0);
    if ((unmasked & IRQ_TIMER2) && timer_irq_level(m, 1) > level)
        level = timer_irq_level(m, 1);
    if ((unmasked & IRQ_UART2) && uart_irq_level(m, 1) > level)
        level = uart_irq_level(m, 1);

    if (m->trace_irq && level != m->irq_level)
        fprintf(stderr, "[irq] level %d (ISR=%08" PRIX32 " IPR=%08" PRIX32
                " IMR=%08" PRIX32 ")\n", level, m->isr, m->ipr, m->imr);

    m->irq_level = level;
    m68k_set_irq((unsigned int)level);
}

/* ------------------------------------------------------------------ */
/* Time / peripherals                                                 */
/* ------------------------------------------------------------------ */

void vz328_tick(vz328_t *m)
{
    double now = (double)m->cycles;

    uart_tick(&m->uart[0], now);
    uart_tick(&m->uart[1], now);
    timer_tick(&m->timer[0], now);
    timer_tick(&m->timer[1], now);
    vz328_update_irqs(m);
}

uint64_t vz328_next_event(vz328_t *m)
{
    double now = (double)m->cycles;
    double next = uart_next_event(&m->uart[0], now);
    double t;

    t = uart_next_event(&m->uart[1], now);
    if (t < next)
        next = t;
    t = timer_next_event(&m->timer[0], now);
    if (t < next)
        next = t;
    t = timer_next_event(&m->timer[1], now);
    if (t < next)
        next = t;

    if (next > 9.0e18)
        return UINT64_MAX;
    if (next <= now)
        return m->cycles + 1;
    return (uint64_t)ceil(next);
}

/* ------------------------------------------------------------------ */
/* Host input / IPL                                                   */
/* ------------------------------------------------------------------ */

void vz328_receive_host_byte(vz328_t *m, int channel, uint8_t byte)
{
    if (channel == 0 && m->ipl_state != VZ_IPL_OFF &&
        m->ipl_state != VZ_IPL_RUN) {
        vz328_ipl_feed(m, byte);
        return;
    }
    if (channel < 0 || channel > 1)
        return;
    uart_receive(&m->uart[channel], byte, (double)m->cycles);
    vz328_update_irqs(m);
}

static host_port_t *ipl_port(vz328_t *m)
{
    return m->host[0];
}

static void ipl_write(vz328_t *m, const char *s)
{
    host_port_t *p = ipl_port(m);

    if (!p)
        return;
    host_port_write_str(p, s);
    host_port_flush(p);
}

void vz328_ipl_begin(vz328_t *m)
{
    m->ipl_state = VZ_IPL_WAIT_RECORD;
    m->ipl_len = 0;
    m->ipl_line[0] = '\0';
    srec_info_init(&m->ipl_info);

    ipl_write(m,
              "\r\n"
              "MC68VZ328 Board IPL Program. by Humblesoft.\r\n"
              "Ver. 1.01-01 2004/07/05(Mon) (230400bps, 33MHz)\r\n"
              "waiting for S-file\r\n");
}

void vz328_ipl_feed(vz328_t *m, uint8_t byte)
{
    char err[256];
    bool terminated = false;

    if (m->ipl_state == VZ_IPL_WAIT_KEY) {
        char buf[160];
        uint32_t start = m->ipl_info.have_start ?
                         m->ipl_info.start_address : VZ_DEFAULT_PC;

        snprintf(buf, sizeof(buf), "\r\n*** your program start. ***\r\n");
        ipl_write(m, buf);
        vz328_start_program(m, start, VZ_DEFAULT_SSP);
        return;
    }

    if (m->ipl_state != VZ_IPL_WAIT_RECORD)
        return;

    /* Kermit may translate LF to CR when transmit linefeed is disabled.
     * The hardware IPL accepts either conventional serial line ending. */
    if (byte == '\r' || byte == '\n') {
        if (m->ipl_len == 0)
            return;
        m->ipl_line[m->ipl_len] = '\0';
        m->ipl_len = 0;

        if (srec_parse_line(m->ipl_line, vz328_poke8, m, &m->ipl_info,
                            &terminated, err, sizeof(err)) < 0) {
            char buf[350];
            snprintf(buf, sizeof(buf), "\r\n? %s\r\n", err);
            ipl_write(m, buf);
            return;
        }

        ipl_write(m, ".");
        if (terminated) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "\r\n\r\n*** Load Success ***\r\n"
                     "Load Address:%08" PRIX32 "-%08" PRIX32 "\r\n"
                     "Start Address:%08" PRIX32 "\r\n"
                     "if you want to start your program, hit any key\r\n"
                     "\r\n",
                     m->ipl_info.min_address, m->ipl_info.max_address,
                     m->ipl_info.have_start ?
                     m->ipl_info.start_address : VZ_DEFAULT_PC);
            ipl_write(m, buf);
            m->ipl_state = VZ_IPL_WAIT_KEY;
        }
        return;
    }

    if (m->ipl_len >= sizeof(m->ipl_line) - 1)
        m->ipl_len = 0;          /* discard overlong line */
    m->ipl_line[m->ipl_len++] = (char)byte;
}

void vz328_ipl_status(vz328_t *m, FILE *out)
{
    fprintf(out, "ipl state=%d records=%" PRIu32 "\n",
            (int)m->ipl_state, m->ipl_info.data_records);
    if (m->ipl_info.have_data)
        fprintf(out, "  load %08" PRIX32 "-%08" PRIX32 " start %08" PRIX32
                "\n", m->ipl_info.min_address, m->ipl_info.max_address,
                m->ipl_info.start_address);
}

/* ------------------------------------------------------------------ */
/* Initialization / reset                                             */
/* ------------------------------------------------------------------ */

int vz328_init(vz328_t *m)
{
    memset(m, 0, sizeof(*m));
    m->mem = calloc(1, (size_t)VZ_ADDR_MASK + 1);
    if (!m->mem)
        return -1;

    m->ivr = 0;
    m->imr = 0x00ffffffu;
    m->isr = 0;
    m->ipr = 0;
    m->ilcr = 0x6533;
    m->push_buttons = 0x0f;
    m->toggle_switches = 0xff;
    m->ipl_state = VZ_IPL_OFF;
    m->running = false;

    uart_init(&m->uart[0], 0);
    uart_init(&m->uart[1], 1);
    timer_init(&m->timer[0], 0);
    timer_init(&m->timer[1], 1);
    return 0;
}

void vz328_destroy(vz328_t *m)
{
    free(m->mem);
    m->mem = NULL;
}

static void install_stub(vz328_t *m)
{
    /* move.w #0xdead, 0x00fff7fe ; stop #0x2700 */
    static const uint8_t stub[] = {
        0x33, 0xfc, 0xde, 0xad, 0x00, 0xff, 0xf7, 0xfe,
        0x4e, 0x72, 0x27, 0x00
    };

    memcpy(m->mem + VZ_STUB_ADDRESS, stub, sizeof(stub));
}

static void install_vectors(vz328_t *m)
{
    int i;

    for (i = 0; i < 256; i++) {
        uint32_t a = (uint32_t)i * 4;
        m->mem[a] = (uint8_t)(VZ_STUB_ADDRESS >> 24);
        m->mem[a + 1] = (uint8_t)(VZ_STUB_ADDRESS >> 16);
        m->mem[a + 2] = (uint8_t)(VZ_STUB_ADDRESS >> 8);
        m->mem[a + 3] = (uint8_t)VZ_STUB_ADDRESS;
    }

    m->mem[4 * 0 + 0] = 0;
    m->mem[4 * 0 + 1] = 0;
    m->mem[4 * 0 + 2] = (uint8_t)(VZ_DEFAULT_SSP >> 8);
    m->mem[4 * 0 + 3] = (uint8_t)VZ_DEFAULT_SSP;

    m->mem[4 * 1 + 0] = 0;
    m->mem[4 * 1 + 1] = 0;
    m->mem[4 * 1 + 2] = (uint8_t)(VZ_DEFAULT_PC >> 8);
    m->mem[4 * 1 + 3] = (uint8_t)VZ_DEFAULT_PC;
}

void vz328_reset_devices(vz328_t *m)
{
    double now = (double)m->cycles;

    m->ivr = 0;
    m->imr = 0x00ffffffu;
    m->isr = 0;
    m->ipr = 0;
    m->ilcr = 0x6533;
    m->irq_level = 0;
    m->halted = false;
    m->halt_code = 0;

    uart_reset(&m->uart[0], now);
    uart_reset(&m->uart[1], now);
    timer_reset(&m->timer[0], now);
    timer_reset(&m->timer[1], now);
    if (m->cpu_initialized)
        m68k_set_irq(0);
}

void vz328_power_on(vz328_t *m)
{
    memset(m->mem, 0, (size_t)VZ_ADDR_MASK + 1);
    memset(m->led, 0, sizeof(m->led));
    install_stub(m);
    install_vectors(m);

    m->ipl_state = VZ_IPL_OFF;
    m->ipl_len = 0;
    m->push_buttons = 0x0f;
    m->toggle_switches = 0xff;
    vz328_reset_devices(m);
}

void vz328_start_program(vz328_t *m, uint32_t pc, uint32_t ssp)
{
    /* Install the reset exception vectors. */
    m->mem[0] = (uint8_t)(ssp >> 24);
    m->mem[1] = (uint8_t)(ssp >> 16);
    m->mem[2] = (uint8_t)(ssp >> 8);
    m->mem[3] = (uint8_t)ssp;
    m->mem[4] = (uint8_t)(pc >> 24);
    m->mem[5] = (uint8_t)(pc >> 16);
    m->mem[6] = (uint8_t)(pc >> 8);
    m->mem[7] = (uint8_t)pc;

    m->ipl_state = VZ_IPL_RUN;
    m->running = true;
    m->halted = false;
    m->halt_code = 0;
    vz_cpu_reset(m, pc, ssp);
    vz328_update_irqs(m);
}

/* ------------------------------------------------------------------ */
/* Loader helpers                                                     */
/* ------------------------------------------------------------------ */

int vz328_poke8(void *ctx, uint32_t address, uint8_t value)
{
    vz328_t *m = ctx;

    m->mem[address & VZ_ADDR_MASK] = value;
    return 0;
}

uint32_t vz328_peek8(vz328_t *m, uint32_t address)
{
    return m->mem[address & VZ_ADDR_MASK];
}

int vz328_load_abs(vz328_t *m, const char *path, srec_info_t *info,
                   char *err, size_t errlen)
{
    return srec_load_file(path, vz328_poke8, m, info, err, errlen);
}
