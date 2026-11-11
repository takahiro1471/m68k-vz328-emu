# Makefile for the MC68VZ328 UART/TIMER emulator

CC      ?= cc
CFLAGS  ?= -O2 -g
BUILD    = build

CPPFLAGS += -Isrc -I$(BUILD) -Ithird_party/musashi \
            -Ithird_party/musashi/softfloat
CFLAGS   += -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
LDLIBS   += -lm -lutil

# Musashi configuration: 68000 only, with interrupt acknowledge and address
# error emulation enabled.
M68K_DEFS = \
	-DM68K_EMULATE_INT_ACK=1 \
	-DM68K_EMULATE_ADDRESS_ERROR=1 \
	-DM68K_EMULATE_TRACE=1 \
	-DM68K_EMULATE_RESET=1 \
	-DM68K_EMULATE_010=0 \
	-DM68K_EMULATE_EC020=0 \
	-DM68K_EMULATE_020=0 \
	-DM68K_EMULATE_030=0 \
	-DM68K_EMULATE_040=0 \
	-DM68K_EMULATE_PMMU=0

APP_OBJS = \
	$(BUILD)/src/main.o \
	$(BUILD)/src/vz328.o \
	$(BUILD)/src/cpu.o \
	$(BUILD)/src/uart.o \
	$(BUILD)/src/timer.o \
	$(BUILD)/src/host_port.o \
	$(BUILD)/src/srec.o

M68K_OBJS = \
	$(BUILD)/musashi/m68kcpu.o \
	$(BUILD)/musashi/m68kops.o \
	$(BUILD)/musashi/softfloat.o

TARGET = m68k-vz328

all: $(TARGET)

$(TARGET): $(APP_OBJS) $(M68K_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/src:
	mkdir -p $@

$(BUILD)/musashi:
	mkdir -p $@

$(BUILD)/src/%.o: src/%.c | $(BUILD)/src
	$(CC) $(CPPFLAGS) $(CFLAGS) $(M68K_DEFS) -c -o $@ $<

$(BUILD)/m68kmake: third_party/musashi/m68kmake.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD):
	mkdir -p $@

$(BUILD)/m68kops.c $(BUILD)/m68kops.h: $(BUILD)/m68kmake \
                                     third_party/musashi/m68k_in.c
	cd $(BUILD) && ./m68kmake ./ ../third_party/musashi/m68k_in.c

$(BUILD)/musashi/m68kcpu.o: third_party/musashi/m68kcpu.c \
                            third_party/musashi/m68kcpu.h \
                            $(BUILD)/m68kops.h | $(BUILD)/musashi
	$(CC) $(CPPFLAGS) $(CFLAGS) $(M68K_DEFS) -c -o $@ $<

$(BUILD)/musashi/m68kops.o: $(BUILD)/m68kops.c \
                            $(BUILD)/m68kops.h | $(BUILD)/musashi
	$(CC) $(CPPFLAGS) $(CFLAGS) $(M68K_DEFS) -c -o $@ $<

$(BUILD)/musashi/softfloat.o: third_party/musashi/softfloat/softfloat.c \
                              | $(BUILD)/musashi
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

tests/%.abs: tests/%.s
	cd tests && m68k-as -t 400 $(notdir $<)

TEST_ABS = \
	tests/uart_poll.abs \
	tests/uart_irq.abs \
	tests/uart_irq_min.abs \
	tests/uart2_poll.abs \
	tests/timer_irq.abs \
	tests/timer2_irq.abs \
	tests/exception.abs \
	tests/ipl_hello.abs

test: all $(TEST_ABS)
	tests/run_tests.sh

clean:
	rm -rf $(BUILD)
	rm -f $(TARGET)
	rm -f tests/*.abs tests/*.LIS tests/*.map tests/*.o

.PHONY: all test clean
