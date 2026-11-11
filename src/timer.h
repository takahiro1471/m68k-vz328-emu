/*
 * timer.h - MC68VZ328 general purpose timer emulation
 */
#ifndef VZ328_TIMER_H
#define VZ328_TIMER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct vz_timer {
    int channel;                 /* 0 = timer 1, 1 = timer 2 */
    uint16_t tctl;
    uint16_t tprer;
    uint16_t tcmp;
    uint16_t tcr;
    uint16_t tcn;
    uint16_t tstat;

    double next_tick_cycle;
    double period_cycles;
} vz_timer_t;

void timer_init(vz_timer_t *t, int channel);
void timer_reset(vz_timer_t *t, double now);

uint32_t timer_read(vz_timer_t *t, uint16_t offset, int size);
void timer_write(vz_timer_t *t, uint16_t offset, uint32_t value, int size,
                 double now);

void timer_tick(vz_timer_t *t, double now);
double timer_next_event(const vz_timer_t *t, double now);
bool timer_irq(const vz_timer_t *t);

#endif
