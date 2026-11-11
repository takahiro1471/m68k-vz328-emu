/*
 * uart.h - MC68VZ328 UART (UART1/UART2) emulation
 */
#ifndef VZ328_UART_H
#define VZ328_UART_H

#include <stdbool.h>
#include <stdint.h>

#include "host_port.h"

#define UART_FIFO_SIZE 8

typedef struct uart {
    int channel;                 /* 0 = UART1, 1 = UART2 */
    host_port_t *port;

    uint16_t ustcnt;
    uint16_t ubaud;
    uint16_t umisc;
    uint16_t nipr;
    uint16_t utx_ctrl;           /* writable UTX control bits */

    uint8_t tx_fifo[UART_FIFO_SIZE];
    int tx_head;
    int tx_tail;
    int tx_count;

    int tx_busy;
    uint8_t tx_shift;
    double tx_done_cycle;

    uint8_t rx_fifo[UART_FIFO_SIZE];
    int rx_head;
    int rx_tail;
    int rx_count;
    uint8_t rx_error;            /* bit3 OVRUN, bit2 FRAME, bit1 BREAK, bit0 PARITY */
    double rx_last_cycle;

    uint32_t tx_chars;
    uint32_t rx_chars;
} uart_t;

void uart_init(uart_t *u, int channel);
void uart_reset(uart_t *u, double now);
void uart_set_port(uart_t *u, host_port_t *port);

uint32_t uart_read(uart_t *u, uint16_t offset, int size, double now);
void uart_write(uart_t *u, uint16_t offset, uint32_t value, int size,
                double now);

void uart_tick(uart_t *u, double now);
double uart_next_event(const uart_t *u, double now);
bool uart_irq(const uart_t *u, double now);

/* Push one received byte into the 8-byte RxFIFO. */
void uart_receive(uart_t *u, uint8_t byte, double now);

/* Detected baud rate from UBAUD (for information/debug). */
double uart_baud(const uart_t *u);
double uart_frame_cycles(const uart_t *u);

#endif
