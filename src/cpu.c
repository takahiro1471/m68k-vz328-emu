/*
 * cpu.c - Musashi 68000 glue for the MC68VZ328 emulator
 */
#include "cpu.h"

#include <inttypes.h>
#include <stdio.h>

#include "m68k.h"

static vz328_t *g_machine;

static int int_ack_callback(int level)
{
    vz328_t *m = g_machine;
    int vector;

    if (!m)
        return M68K_INT_ACK_AUTOVECTOR;

    if (m->ivr == 0)
        vector = 0x0f;           /* uninitialized interrupt vector */
    else
        vector = (int)(m->ivr & 0xf8) | (level & 7);

    if (m->trace_irq)
        fprintf(stderr, "[irq] ack level %d -> vector 0x%02X (PC=%06" PRIX32
                ")\n", level, vector, vz_cpu_get_pc());

    return vector;
}

static void reset_instr_callback(void)
{
    if (g_machine && g_machine->trace_io)
        fprintf(stderr, "[cpu] RESET instruction\n");
}

unsigned int m68k_read_memory_8(unsigned int address)
{
    return vz328_read8(g_machine, address);
}

unsigned int m68k_read_memory_16(unsigned int address)
{
    return vz328_read16(g_machine, address);
}

unsigned int m68k_read_memory_32(unsigned int address)
{
    return vz328_read32(g_machine, address);
}

void m68k_write_memory_8(unsigned int address, unsigned int value)
{
    vz328_write8(g_machine, address, value);
}

void m68k_write_memory_16(unsigned int address, unsigned int value)
{
    vz328_write16(g_machine, address, value);
}

void m68k_write_memory_32(unsigned int address, unsigned int value)
{
    vz328_write32(g_machine, address, value);
}

void m68k_write_memory_32_pd(unsigned int address, unsigned int value)
{
    /* Predecrement order: high word first, then low word. */
    vz328_write16(g_machine, address + 2, value >> 16);
    vz328_write16(g_machine, address, value & 0xffff);
}

int vz_cpu_init(vz328_t *m)
{
    g_machine = m;
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_set_int_ack_callback(int_ack_callback);
    m68k_set_reset_instr_callback(reset_instr_callback);
    m->cpu_initialized = true;
    return 0;
}

void vz_cpu_reset(vz328_t *m, uint32_t pc, uint32_t ssp)
{
    if (!m->cpu_initialized)
        return;

    /* m68k_pulse_reset() reads the initial SSP and PC from vectors 0 and 1.
     * vz328_start_program() has already installed them. */
    m68k_pulse_reset();
    m68k_set_reg(M68K_REG_SR, 0x2700);
    m68k_set_reg(M68K_REG_ISP, ssp);
    m68k_set_reg(M68K_REG_PC, pc & VZ_ADDR_MASK);
}

int vz_cpu_execute(int cycles)
{
    if (!g_machine || !g_machine->cpu_initialized)
        return 0;
    return m68k_execute(cycles);
}

uint32_t vz_cpu_get_pc(void)
{
    if (!g_machine)
        return 0;
    return m68k_get_reg(NULL, M68K_REG_PC) & VZ_ADDR_MASK;
}

void vz_cpu_dump(FILE *out)
{
    int i;

    if (!g_machine) {
        fprintf(out, "(CPU not initialized)\n");
        return;
    }

    for (i = 0; i < 8; i++) {
        fprintf(out, "D%d=%08" PRIX32 "%s", i,
                m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + i)),
                (i == 3 || i == 7) ? "\n" : " ");
    }
    for (i = 0; i < 8; i++) {
        fprintf(out, "A%d=%08" PRIX32 "%s", i,
                m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + i)),
                (i == 3 || i == 7) ? "\n" : " ");
    }
    fprintf(out, "PC=%06" PRIX32 " SR=%04" PRIX32 " USP=%08" PRIX32
            " ISP=%08" PRIX32 "\n",
            m68k_get_reg(NULL, M68K_REG_PC) & VZ_ADDR_MASK,
            m68k_get_reg(NULL, M68K_REG_SR),
            m68k_get_reg(NULL, M68K_REG_USP),
            m68k_get_reg(NULL, M68K_REG_ISP));
}
