/*
 * cpu.h - Musashi 68000 glue for the MC68VZ328 emulator
 */
#ifndef VZ328_CPU_H
#define VZ328_CPU_H

#include <stdint.h>
#include <stdio.h>

#include "vz328.h"

int vz_cpu_init(vz328_t *m);
void vz_cpu_reset(vz328_t *m, uint32_t pc, uint32_t ssp);
int vz_cpu_execute(int cycles);
void vz_cpu_dump(FILE *out);
uint32_t vz_cpu_get_pc(void);

#endif
