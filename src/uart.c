/*
 * uart.c - MC68VZ328 UART (UART1/UART2) emulation
 *
 * Register layout (from the MC68VZ328 User's Manual chapter 14):
 *
 *   USTCNT  +0x00  status/control
 *   UBAUD   +0x02  baud control
 *   URX     +0x04  receiver (reading the data byte pops the RxFIFO)
 *   UTX     +0x06  transmitter (writing the low byte enqueues data)
 *   UMISC   +0x08  miscellaneous
 *   NIPR    +0x0A  non-integer prescaler
 *
 * The emulated UART has the same 8-byte FIFOs as UART1 on the real chip.
 * A byte is handed to the shift register as soon as possible, which keeps
 * the FIFO EMPTY/TX AVAIL semantics seen by the lab software.
 */
#include "uart.h"

#include <math.h>
#include <string.h>

#define VZ_SYSCLK 33161216.0
#define UART_INF 1.0e30

/* USTCNT bits */
#define USTCNT_UEN   0x8000
#define USTCNT_RXEN  0x4000
#define USTCNT_TXEN  0x2000
#define USTCNT_CLKM  0x1000
#define USTCNT_PEN   0x0800
#define USTCNT_ODD   0x0400
#define USTCNT_STOP  0x0200
#define USTCNT_8BIT  0x0100
#define USTCNT_ODEN  0x0080
#define USTCNT_CTSD  0x0040
#define USTCNT_RXFE  0x0020
#define USTCNT_RXHE  0x0010
#define USTCNT_RXRE  0x0008
#define USTCNT_TXEE  0x0004
#define USTCNT_TXHE  0x0002
#define USTCNT_TXAE  0x0001

static int uart_fifo_push(uart_t *u, uint8_t b)
{
    if (u->tx_count >= UART_FIFO_SIZE)
        return -1;
    u->tx_fifo[u->tx_tail] = b;
    u->tx_tail = (u->tx_tail + 1) % UART_FIFO_SIZE;
    u->tx_count++;
    return 0;
}

static int uart_fifo_pop(uart_t *u, uint8_t *b)
{
    if (u->tx_count <= 0)
        return -1;
    *b = u->tx_fifo[u->tx_head];
    u->tx_head = (u->tx_head + 1) % UART_FIFO_SIZE;
    u->tx_count--;
    return 0;
}

static int rx_fifo_push(uart_t *u, uint8_t b)
{
    if (u->rx_count >= UART_FIFO_SIZE)
        return -1;
    u->rx_fifo[u->rx_tail] = b;
    u->rx_tail = (u->rx_tail + 1) % UART_FIFO_SIZE;
    u->rx_count++;
    return 0;
}

static int rx_fifo_pop(uart_t *u, uint8_t *b)
{
    if (u->rx_count <= 0)
        return -1;
    *b = u->rx_fifo[u->rx_head];
    u->rx_head = (u->rx_head + 1) % UART_FIFO_SIZE;
    u->rx_count--;
    return 0;
}

void uart_init(uart_t *u, int channel)
{
    memset(u, 0, sizeof(*u));
    u->channel = channel;
    uart_reset(u, 0.0);
}

void uart_reset(uart_t *u, double now)
{
    (void)now;
    u->ustcnt = 0;
    u->ubaud = 0x003f;       /* reset value from the user's manual */
    u->umisc = 0;
    u->nipr = 0;
    u->utx_ctrl = 0;
    u->tx_head = u->tx_tail = u->tx_count = 0;
    u->tx_busy = 0;
    u->tx_shift = 0;
    u->tx_done_cycle = 0;
    u->rx_head = u->rx_tail = u->rx_count = 0;
    u->rx_error = 0;
    u->rx_last_cycle = now;
    u->tx_chars = 0;
    u->rx_chars = 0;
}

void uart_set_port(uart_t *u, host_port_t *port)
{
    u->port = port;
}

double uart_baud(const uart_t *u)
{
    int divide = (u->ubaud >> 8) & 7;
    int prescaler = 65 - (u->ubaud & 0x3f);
    double div;

    if (prescaler <= 0)
        prescaler = 1;
    div = 16.0 * (double)(1u << divide) * (double)prescaler;
    return VZ_SYSCLK / div;
}

double uart_frame_cycles(const uart_t *u)
{
    int bits = 1;            /* start */
    bits += (u->ustcnt & USTCNT_8BIT) ? 8 : 7;
    if (u->ustcnt & USTCNT_PEN)
        bits++;
    bits += (u->ustcnt & USTCNT_STOP) ? 2 : 1;

    /* The baud rate is derived from SYSCLK, so the frame time in CPU
     * cycles is exact and independent of rounding in uart_baud(). */
    {
        int divide = (u->ubaud >> 8) & 7;
        int prescaler = 65 - (u->ubaud & 0x3f);
        if (prescaler <= 0)
            prescaler = 1;
        return 16.0 * (double)(1u << divide) * (double)prescaler *
               (double)bits;
    }
}

static void tx_start_if_ready(uart_t *u, double now)
{
    uint8_t b;

    if (u->tx_busy)
        return;
    if (!(u->ustcnt & USTCNT_TXEN))
        return;
    if (uart_fifo_pop(u, &b) < 0)
        return;
    u->tx_busy = 1;
    u->tx_shift = b;
    u->tx_done_cycle = now + uart_frame_cycles(u);
}

void uart_tick(uart_t *u, double now)
{
    int guard = 0;

    /* Complete any characters whose transmission time has elapsed. */
    while (u->tx_busy && now >= u->tx_done_cycle && guard++ < 1024) {
        if (u->port)
            host_port_write_byte(u->port, u->tx_shift);
        u->tx_chars++;
        u->tx_busy = 0;
        tx_start_if_ready(u, u->tx_done_cycle);
    }
}

double uart_next_event(const uart_t *u, double now)
{
    (void)now;
    if (u->tx_busy)
        return u->tx_done_cycle;
    return UART_INF;
}

static uint16_t uart_urx_status(const uart_t *u, double now)
{
    uint16_t s = 0;

    if (u->rx_count >= UART_FIFO_SIZE)
        s |= 0x8000;         /* FIFO FULL */
    if (u->rx_count >= 4)
        s |= 0x4000;         /* FIFO HALF (four or fewer slots left) */
    if (u->rx_count > 0)
        s |= 0x2000;         /* DATA READY */
    if (u->rx_count > 0 &&
        now - u->rx_last_cycle > 30.0 * uart_frame_cycles(u))
        s |= 0x1000;         /* OLD DATA */
    s |= (uint16_t)(u->rx_error & 0x0f) << 8;
    if (u->rx_count > 0)
        s |= u->rx_fifo[u->rx_head];
    return s;
}

static uint16_t uart_utx_status(const uart_t *u)
{
    uint16_t s = 0;

    if (u->tx_count == 0)
        s |= 0x8000;         /* FIFO EMPTY */
    if (u->tx_count < 4)
        s |= 0x4000;         /* FIFO HALF */
    if (u->tx_count < UART_FIFO_SIZE)
        s |= 0x2000;         /* TX AVAIL */
    s |= u->utx_ctrl & 0x1800;  /* SEND BREAK, NOCTS1 */
    if (u->tx_busy || u->tx_count > 0)
        s |= 0x0400;         /* BUSY */
    s |= 0x0200;             /* CTS1 STAT: no flow control, line asserted */
    return s;
}

static uint8_t uart_read_urx_byte(uart_t *u, int low, double now)
{
    uint16_t s = uart_urx_status(u, now);
    uint8_t b;

    if (!low)
        return (uint8_t)(s >> 8);
    if (u->rx_count > 0) {
        rx_fifo_pop(u, &b);
        u->rx_error = 0;
        u->rx_last_cycle = now;
        return b;
    }
    return 0;
}

uint32_t uart_read(uart_t *u, uint16_t offset, int size, double now)
{
    uint16_t v = 0;

    switch (offset & ~1) {
    case 0x00:
        v = u->ustcnt;
        break;
    case 0x02:
        v = u->ubaud;
        break;
    case 0x04:
        if (size == 1)
            return uart_read_urx_byte(u, (offset & 1) != 0, now);
        v = uart_urx_status(u, now);
        if (size >= 2 && u->rx_count > 0) {
            uint8_t b;
            rx_fifo_pop(u, &b);
            u->rx_error = 0;
            u->rx_last_cycle = now;
        }
        break;
    case 0x06:
        v = uart_utx_status(u);
        break;
    case 0x08:
        v = u->umisc;
        break;
    case 0x0a:
        v = u->nipr;
        break;
    default:
        v = 0;
        break;
    }

    if (size == 1)
        return (offset & 1) ? (v & 0xff) : (v >> 8);
    if (size == 2)
        return v;
    /* Long accesses to these 16-bit registers are unusual; return the
     * register in the high half and zero in the low half. */
    return (uint32_t)v << 16;
}

void uart_write(uart_t *u, uint16_t offset, uint32_t value, int size,
                double now)
{
    uint16_t v;
    uint16_t old;
    int low_byte = 0;
    int high_byte = 0;

    switch (size) {
    case 1:
        if (offset & 1)
            low_byte = 1;
        else
            high_byte = 1;
        break;
    case 2:
        low_byte = high_byte = 1;
        break;
    case 4:
        /* Treat a long write as writing the high word of a 16-bit
         * register.  This is not used by the lab software. */
        high_byte = 1;
        if ((offset & 2) == 0)
            value >>= 16;
        break;
    default:
        return;
    }

    switch (offset & ~1) {
    case 0x00:
        old = u->ustcnt;
        if (size == 2 || (size == 4 && (offset & 2) == 0))
            v = (uint16_t)value;
        else if (high_byte)
            v = (old & 0x00ff) | (uint16_t)((value & 0xff) << 8);
        else
            v = (old & 0xff00) | (uint16_t)(value & 0xff);
        u->ustcnt = v;

        if (!(v & USTCNT_TXEN)) {
            u->tx_head = u->tx_tail = u->tx_count = 0;
        }
        if (!(v & USTCNT_RXEN)) {
            u->rx_head = u->rx_tail = u->rx_count = 0;
            u->rx_error = 0;
        }
        if (!(v & USTCNT_UEN)) {
            u->tx_head = u->tx_tail = u->tx_count = 0;
            u->rx_head = u->rx_tail = u->rx_count = 0;
            u->rx_error = 0;
        }
        tx_start_if_ready(u, now);
        break;

    case 0x02:
        old = u->ubaud;
        if (size == 2 || (size == 4 && (offset & 2) == 0))
            v = (uint16_t)value;
        else if (high_byte)
            v = (old & 0x00ff) | (uint16_t)((value & 0xff) << 8);
        else
            v = (old & 0xff00) | (uint16_t)(value & 0xff);
        u->ubaud = v;
        break;

    case 0x04:
        /* URX is read-only. */
        break;

    case 0x06:
        old = u->utx_ctrl;
        if (size == 2 || (size == 4 && (offset & 2) == 0))
            v = (uint16_t)value;
        else if (high_byte)
            v = (old & 0x00ff) | (uint16_t)((value & 0xff) << 8);
        else
            v = (old & 0xff00) | (uint16_t)(value & 0xff);
        u->utx_ctrl = v & 0x1800;

        if (low_byte && (u->ustcnt & USTCNT_TXEN)) {
            uint8_t data = (uint8_t)(value & 0xff);
            if ((u->ustcnt & USTCNT_8BIT) == 0)
                data &= 0x7f;
            uart_fifo_push(u, data);
            tx_start_if_ready(u, now);
        }
        break;

    case 0x08:
        old = u->umisc;
        if (size == 2 || (size == 4 && (offset & 2) == 0))
            v = (uint16_t)value;
        else if (high_byte)
            v = (old & 0x00ff) | (uint16_t)((value & 0xff) << 8);
        else
            v = (old & 0xff00) | (uint16_t)(value & 0xff);
        u->umisc = v;
        break;

    case 0x0a:
        old = u->nipr;
        if (size == 2 || (size == 4 && (offset & 2) == 0))
            v = (uint16_t)value;
        else if (high_byte)
            v = (old & 0x00ff) | (uint16_t)((value & 0xff) << 8);
        else
            v = (old & 0xff00) | (uint16_t)(value & 0xff);
        u->nipr = v;
        break;

    default:
        break;
    }
}

void uart_receive(uart_t *u, uint8_t byte, double now)
{
    if (!(u->ustcnt & USTCNT_UEN) || !(u->ustcnt & USTCNT_RXEN))
        return;
    if ((u->ustcnt & USTCNT_8BIT) == 0)
        byte &= 0x7f;
    if (rx_fifo_push(u, byte) < 0) {
        u->rx_error |= 0x08; /* overrun */
        return;
    }
    u->rx_chars++;
    u->rx_last_cycle = now;
}

bool uart_irq(const uart_t *u, double now)
{
    bool tx = false;
    bool rx = false;

    if (!(u->ustcnt & USTCNT_UEN))
        return false;

    if (u->ustcnt & USTCNT_TXEN) {
        if ((u->ustcnt & USTCNT_TXEE) && u->tx_count == 0)
            tx = true;
        if ((u->ustcnt & USTCNT_TXHE) && u->tx_count < 4)
            tx = true;
        if ((u->ustcnt & USTCNT_TXAE) && u->tx_count < UART_FIFO_SIZE)
            tx = true;
    }

    if (u->ustcnt & USTCNT_RXEN) {
        if ((u->ustcnt & USTCNT_RXRE) && u->rx_count > 0)
            rx = true;
        if ((u->ustcnt & USTCNT_RXFE) && u->rx_count >= UART_FIFO_SIZE)
            rx = true;
        if ((u->ustcnt & USTCNT_RXHE) && u->rx_count >= 4)
            rx = true;
        if ((u->ustcnt & USTCNT_ODEN) && u->rx_count > 0 &&
            now - u->rx_last_cycle > 30.0 * uart_frame_cycles(u))
            rx = true;
    }

    return tx || rx;
}
