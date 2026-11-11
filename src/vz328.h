/*
 * vz328.h - MC68VZ328 board/machine emulation
 */
#ifndef VZ328_MACHINE_H
#define VZ328_MACHINE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "host_port.h"
#include "srec.h"
#include "timer.h"
#include "uart.h"

#define VZ_SYSCLK_HZ       33161216.0
#define VZ_ADDR_MASK       0x00ffffffu
#define VZ_RAM_BASE        0x000000u
#define VZ_RAM_SIZE        0x040000u
#define VZ_ROM_BASE        0x00c00000u
#define VZ_ROM_SIZE        0x00080000u
#define VZ_IO_BASE         0x00d00000u
#define VZ_IO_SIZE         0x00020000u
#define VZ_REG_BASE        0x00fff000u
#define VZ_REG_SIZE        0x00001000u
#define VZ_STUB_ADDRESS    (VZ_ROM_BASE)
#define VZ_DEFAULT_SSP     0x0003fff0u
#define VZ_DEFAULT_PC      0x00000400u
#define VZ_MAGIC_ADDRESS   0x00fff7feu
#define VZ_MAGIC_EXCEPTION 0xdead
#define VZ_MAGIC_EXIT      0x600d

typedef enum {
    VZ_IPL_OFF = 0,
    VZ_IPL_WAIT_RECORD,
    VZ_IPL_WAIT_KEY,
    VZ_IPL_RUN
} vz_ipl_state_t;

typedef struct vz328 {
    uint8_t *mem;                /* 16 MiB full 24-bit address space */

    host_port_t *host[2];        /* UART1, UART2 */
    uart_t uart[2];
    vz_timer_t timer[2];

    uint32_t ivr;
    uint32_t imr;
    uint32_t isr;
    uint32_t ipr;
    uint32_t ilcr;
    int irq_level;

    uint64_t cycles;
    bool running;
    bool cpu_initialized;
    bool halted;
    uint16_t halt_code;

    bool trace_io;
    bool trace_irq;
    bool show_led;

    uint8_t led[8];              /* LED0..LED7, ASCII / PD4435 code */

    uint8_t push_buttons;        /* 0xFFF419, active low, default 0x0F */
    uint8_t toggle_switches;     /* 0xD00041, default 0xFF */

    vz_ipl_state_t ipl_state;
    char ipl_line[1024];
    size_t ipl_len;
    srec_info_t ipl_info;

    uint64_t uart_rx_bytes;
    uint64_t uart_tx_bytes;
} vz328_t;

int vz328_init(vz328_t *m);
void vz328_destroy(vz328_t *m);

/* Power-on reset: clears RAM, installs the default vector table and resets
 * all on-chip peripherals. */
void vz328_power_on(vz328_t *m);

/* Reset the interrupt controller, UARTs and timers, leaving RAM untouched. */
void vz328_reset_devices(vz328_t *m);

/* Start a loaded program at pc with an initial supervisor stack pointer. */
void vz328_start_program(vz328_t *m, uint32_t pc, uint32_t ssp);

/* Advance all peripherals to m->cycles. */
void vz328_tick(vz328_t *m);

/* Recompute IPR/ISR and drive the CPU IPL lines. */
void vz328_update_irqs(vz328_t *m);

/* Earliest cycle at which a peripheral needs servicing. */
uint64_t vz328_next_event(vz328_t *m);

/* CPU bus access. */
uint32_t vz328_read8(vz328_t *m, uint32_t address);
uint32_t vz328_read16(vz328_t *m, uint32_t address);
uint32_t vz328_read32(vz328_t *m, uint32_t address);
void vz328_write8(vz328_t *m, uint32_t address, uint32_t value);
void vz328_write16(vz328_t *m, uint32_t address, uint32_t value);
void vz328_write32(vz328_t *m, uint32_t address, uint32_t value);

/* Loader access (used by the S-record loader and the IPL). */
int vz328_poke8(void *ctx, uint32_t address, uint8_t value);
uint32_t vz328_peek8(vz328_t *m, uint32_t address);

/* Feed a received byte to the UART or to the IPL state machine. */
void vz328_receive_host_byte(vz328_t *m, int channel, uint8_t byte);

/* IPL (boot loader) emulation. */
void vz328_ipl_begin(vz328_t *m);
void vz328_ipl_feed(vz328_t *m, uint8_t byte);
void vz328_ipl_status(vz328_t *m, FILE *out);

/* Load a .abs file directly. */
int vz328_load_abs(vz328_t *m, const char *path, srec_info_t *info,
                   char *err, size_t errlen);

#endif
