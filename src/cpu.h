/*
 * cpu.h - Musashi 68000 glue for the MC68VZ328 emulator
 */
#ifndef VZ328_CPU_H
#define VZ328_CPU_H

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#include "vz328.h"

typedef struct {
	uint16_t vector;
	uint16_t sr;
	uint16_t opcode;
	uint32_t pc;
	uint32_t access_address;
	bool has_opcode;
	bool has_access_address;
} vz_cpu_exception_info_t;

int vz_cpu_init(vz328_t *m);
void vz_cpu_reset(vz328_t *m, uint32_t pc, uint32_t ssp);
int vz_cpu_execute(int cycles);
void vz_cpu_dump(FILE *out);
uint32_t vz_cpu_get_pc(void);
bool vz_cpu_get_exception_info(vz_cpu_exception_info_t *info);

#endif
