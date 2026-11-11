/*
 * timer.c - MC68VZ328 general purpose timer emulation
 *
 * TCTL:   bit 8 FRR, bits 7-6 CAP, bit 5 OM, bit 4 IRQEN,
 *         bits 3-1 CLKSOURCE, bit 0 TEN
 * TPRER:  bits 7-0 prescaler (divide by value + 1)
 * TCMP:   compare value
 * TCN:    current counter (read-only)
 * TSTAT:  bit 1 CAPT, bit 0 COMP; write 0 to clear
 */
#include "timer.h"

#include <math.h>
#include <string.h>

#define VZ_SYSCLK 33161216.0
#define VZ_CLK32  32768.0
#define TIMER_INF 1.0e30

#define TCTL_FRR       0x0100
#define TCTL_CAP_MASK  0x00c0
#define TCTL_OM        0x0020
#define TCTL_IRQEN     0x0010
#define TCTL_CLK_MASK  0x000e
#define TCTL_TEN       0x0001

#define TSTAT_CAPT     0x0002
#define TSTAT_COMP     0x0001

void timer_init(vz_timer_t *t, int channel)
{
    memset(t, 0, sizeof(*t));
    t->channel = channel;
    timer_reset(t, 0.0);
}

void timer_reset(vz_timer_t *t, double now)
{
    (void)now;
    t->tctl = 0;
    t->tprer = 0;
    t->tcmp = 0xffff;
    t->tcr = 0;
    t->tcn = 0;
    t->tstat = 0;
    t->next_tick_cycle = TIMER_INF;
    t->period_cycles = 0;
}

static double timer_period_cycles(const vz_timer_t *t)
{
    unsigned clk = (t->tctl & TCTL_CLK_MASK) >> 1;
    double source;
    double divide = (double)(t->tprer + 1);

    switch (clk) {
    case 0:
        return 0.0;             /* stopped */
    case 1:
        source = VZ_SYSCLK;
        break;
    case 2:
        source = VZ_SYSCLK / 16.0;
        break;
    case 3:
        /* External TIN clock is not connected in this emulator. */
        return 0.0;
    default:
        source = VZ_CLK32;
        break;
    }
    return VZ_SYSCLK * divide / source;
}

static void timer_schedule(vz_timer_t *t, double now)
{
    double period = timer_period_cycles(t);

    if (!(t->tctl & TCTL_TEN) || period <= 0.0) {
        t->next_tick_cycle = TIMER_INF;
        t->period_cycles = 0;
        return;
    }
    t->period_cycles = period;
    t->next_tick_cycle = now + period;
}

static void timer_compare_event(vz_timer_t *t)
{
    t->tstat |= TSTAT_COMP;
    if (!(t->tctl & TCTL_FRR))
        t->tcn = 0;
}

void timer_tick(vz_timer_t *t, double now)
{
    int guard = 0;

    if (!(t->tctl & TCTL_TEN) || t->period_cycles <= 0.0)
        return;

    while (now >= t->next_tick_cycle && guard++ < 1000000) {
        t->tcn = (uint16_t)(t->tcn + 1);
        if (t->tcn == t->tcmp)
            timer_compare_event(t);
        t->next_tick_cycle += t->period_cycles;
    }
}

double timer_next_event(const vz_timer_t *t, double now)
{
    (void)now;
    if (!(t->tctl & TCTL_TEN) || t->period_cycles <= 0.0)
        return TIMER_INF;
    return t->next_tick_cycle;
}

bool timer_irq(const vz_timer_t *t)
{
    if (t->tstat & TSTAT_COMP)
        return (t->tctl & TCTL_IRQEN) != 0;
    if (t->tstat & TSTAT_CAPT)
        return (t->tctl & TCTL_IRQEN) != 0;
    return false;
}

uint32_t timer_read(vz_timer_t *t, uint16_t offset, int size)
{
    uint16_t v;

    switch (offset & ~1) {
    case 0x0:
        v = t->tctl;
        break;
    case 0x2:
        v = t->tprer;
        break;
    case 0x4:
        v = t->tcmp;
        break;
    case 0x6:
        v = t->tcr;
        break;
    case 0x8:
        v = t->tcn;
        break;
    case 0xa:
        v = t->tstat;
        break;
    default:
        v = 0;
        break;
    }

    if (size == 1)
        return (offset & 1) ? (v & 0xff) : (v >> 8);
    if (size == 2)
        return v;
    return (uint32_t)v << 16;
}

void timer_write(vz_timer_t *t, uint16_t offset, uint32_t value, int size,
                 double now)
{
    uint16_t old;
    uint16_t v;

    switch (offset & ~1) {
    case 0x0:
        old = t->tctl;
        break;
    case 0x2:
        old = t->tprer;
        break;
    case 0x4:
        old = t->tcmp;
        break;
    case 0x6:
        old = t->tcr;
        break;
    case 0x8:
        return;                 /* TCN is read-only */
    case 0xa:
        old = t->tstat;
        break;
    default:
        return;
    }

    if (size == 2 || (size == 4 && (offset & 2) == 0))
        v = (uint16_t)value;
    else if (size == 1 && (offset & 1))
        v = (uint16_t)((old & 0xff00) | (value & 0xff));
    else
        v = (uint16_t)((old & 0x00ff) | ((value & 0xff) << 8));

    switch (offset & ~1) {
    case 0x0: {
        unsigned old_clk = t->tctl & (TCTL_TEN | TCTL_CLK_MASK);
        unsigned new_clk;
        t->tctl = v & 0x01ff;
        new_clk = t->tctl & (TCTL_TEN | TCTL_CLK_MASK);
        if (old_clk != new_clk)
            timer_schedule(t, now);
        /* If the counter already matches the compare value when the timer
         * is enabled, the comparator fires immediately. */
        if ((t->tctl & TCTL_TEN) && t->tcn == t->tcmp)
            timer_compare_event(t);
        break;
    }
    case 0x2:
        t->tprer = v & 0x00ff;
        timer_schedule(t, now);
        break;
    case 0x4:
        t->tcmp = v;
        break;
    case 0x6:
        /* TCR is the read-only capture register.  TIN capture is not
         * implemented, so ignore writes. */
        break;
    case 0xa:
        /* Writing 0 clears the status bits.  Writing 1 leaves them set. */
        t->tstat &= v;
        break;
    }
}
