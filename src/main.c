/*
 * main.c - MC68VZ328 UART/TIMER emulator command line frontend
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cpu.h"
#include "host_port.h"
#include "srec.h"
#include "vz328.h"

typedef enum {
    EP_DEFAULT,
    EP_PTY,
    EP_SERIAL,
    EP_STDIO,
    EP_NONE
} endpoint_mode_t;

typedef struct options {
    const char *load_file;
    bool force_ipl;
    bool wait_key;
    bool fast;
    bool dump;
    bool show_led;
    bool trace_io;
    bool trace_irq;

    endpoint_mode_t uart1_mode;
    const char *uart1_serial;
    const char *uart1_link;

    endpoint_mode_t uart2_mode;
    const char *uart2_serial;
    const char *uart2_link;

    uint64_t max_cycles;
    double run_time;
    uint8_t buttons;
    uint8_t switches;
} options_t;

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static void usage(FILE *out)
{
    fprintf(out,
"Usage: m68k-vz328 [options]\n"
"\n"
"Program loading:\n"
"  -l, --load FILE        Load an m68k-as .abs S-record file and run it\n"
"  -i, --ipl              Start in IPL mode and wait for S-records on UART1\n"
"      --wait-key         Wait for a key before starting a loaded program\n"
"\n"
"UART1 connection (default: PTY in IPL mode, stdio with --load):\n"
"  -p, --pty              Create a pseudo terminal for UART1\n"
"      --pty-link PATH    Create a PTY and a symlink at PATH\n"
"  -s, --serial DEVICE    Use an existing serial device for UART1\n"
"      --stdio            Use stdin/stdout for UART1\n"
"\n"
"UART2 connection (default: not connected):\n"
"      --uart2 DEVICE     Use an existing serial device for UART2\n"
"      --uart2-pty        Create a pseudo terminal for UART2\n"
"      --uart2-link PATH  Create a UART2 PTY and a symlink at PATH\n"
"\n"
"Board inputs and display:\n"
"      --buttons MASK     Push button state, active low (default 0x0F)\n"
"      --switches MASK    Toggle switch state (default 0xFF)\n"
"      --led              Show LED contents on stderr\n"
"\n"
"Execution:\n"
"      --fast             Run as fast as possible (no real-time throttle)\n"
"      --max-cycles N     Stop after N CPU cycles (K/M/G suffixes accepted)\n"
"      --run-time SEC     Stop after SEC emulated seconds\n"
"      --dump             Dump CPU registers on exit\n"
"      --trace-io         Trace internal register and board I/O accesses\n"
"      --trace-irq        Trace interrupt controller activity\n"
"  -h, --help             Show this help\n"
"  -V, --version          Show version\n"
"\n"
"Exit magic: writing 0x600D to 0xFFF7FE exits normally; 0xDEAD is used by\n"
"the built-in exception stub and exits with an error.\n");
}

static void version(void)
{
    printf("m68k-vz328 0.1 - MC68VZ328 UART/TIMER emulator\n");
}

static bool parse_u64(const char *s, uint64_t *out)
{
    char *end;
    unsigned long long v;

    errno = 0;
    v = strtoull(s, &end, 0);
    if (errno || end == s)
        return false;
    if (*end == 'k' || *end == 'K') {
        v *= 1000ull;
        end++;
    } else if (*end == 'm' || *end == 'M') {
        v *= 1000000ull;
        end++;
    } else if (*end == 'g' || *end == 'G') {
        v *= 1000000000ull;
        end++;
    }
    if (*end != '\0')
        return false;
    *out = (uint64_t)v;
    return true;
}

static bool parse_u32(const char *s, uint32_t *out)
{
    uint64_t v;

    if (!parse_u64(s, &v) || v > UINT32_MAX)
        return false;
    *out = (uint32_t)v;
    return true;
}

static double monotonic_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1.0e9;
}

static void sleep_seconds(double seconds)
{
    struct timespec ts;

    if (seconds <= 0.0)
        return;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1.0e9);
    nanosleep(&ts, NULL);
}

static int open_endpoint(host_port_t *p, endpoint_mode_t mode,
                         const char *device, const char *link,
                         const char *name, bool *is_open,
                         char *err, size_t errlen)
{
    *is_open = false;

    switch (mode) {
    case EP_NONE:
        return 0;

    case EP_PTY:
        if (host_port_open_pty(p, link, err, errlen) < 0)
            return -1;
        fprintf(stderr, "%s PTY: %s\n", name, p->slave_path);
        if (link && *link)
            fprintf(stderr, "%s link: %s\n", name, link);
        *is_open = true;
        return 0;

    case EP_SERIAL:
        if (!device || !*device) {
            snprintf(err, errlen, "%s: no serial device specified", name);
            return -1;
        }
        if (host_port_open_serial(p, device, err, errlen) < 0)
            return -1;
        fprintf(stderr, "%s serial: %s\n", name, device);
        *is_open = true;
        return 0;

    case EP_STDIO:
        host_port_open_stdio(p);
        *is_open = true;
        return 0;

    case EP_DEFAULT:
        break;
    }

    snprintf(err, errlen, "%s: invalid endpoint", name);
    return -1;
}

static bool uart_rx_enabled(const uart_t *u)
{
    return (u->ustcnt & 0xc000) == 0xc000;
}

static uint64_t rx_interval_cycles(const uart_t *u)
{
    double c = uart_frame_cycles(u);

    if (c < 1.0)
        c = 1.0;
    return (uint64_t)ceil(c);
}

static bool wait_for_key(host_port_t *port, bool port_open)
{
    if (port_open) {
        host_port_write_str(port,
                            "\r\nif you want to start your program, hit any key\r\n");
        host_port_flush(port);
        while (!stop_requested) {
            int c = host_port_getc(port);
            if (c >= 0)
                return true;
            host_port_wait_input(port, 100);
        }
    } else {
        int c;
        fprintf(stderr, "Press Enter to start the program...\n");
        while (!stop_requested) {
            c = getchar();
            if (c != EOF)
                return true;
        }
    }
    return false;
}

static void poll_ipl_input(vz328_t *m, host_port_t *port, bool port_open)
{
    if (port_open) {
        for (;;) {
            int c = host_port_getc(port);
            if (c < 0)
                break;
            vz328_receive_host_byte(m, 0, (uint8_t)c);
            if (m->ipl_state == VZ_IPL_RUN)
                break;
        }
        host_port_flush(port);
    }
}

static void poll_uart_input(vz328_t *m, int channel, host_port_t *port,
                            bool port_open, uint64_t *next_rx)
{
    uart_t *u = &m->uart[channel];
    int budget = 256;

    if (!port_open || !uart_rx_enabled(u))
        return;

    while (m->cycles >= *next_rx && budget-- > 0) {
        int c = host_port_getc(port);
        if (c < 0) {
            if (*next_rx < m->cycles)
                *next_rx = m->cycles;
            break;
        }
        vz328_receive_host_byte(m, channel, (uint8_t)c);
        *next_rx += rx_interval_cycles(u);
        if (m->cycles > 1000000ull && *next_rx < m->cycles - 1000000ull)
            *next_rx = m->cycles;
    }
}

static void drain_output(vz328_t *m, host_port_t *p1, bool p1_open,
                         host_port_t *p2, bool p2_open)
{
    uint64_t limit = m->cycles + (uint64_t)(VZ_SYSCLK_HZ * 1.0);

    while (m->cycles < limit) {
        if (!m->uart[0].tx_busy && m->uart[0].tx_count == 0 &&
            !m->uart[1].tx_busy && m->uart[1].tx_count == 0)
            break;

        {
            uint64_t event = vz328_next_event(m);
            uint64_t step = 10000;

            if (event > m->cycles && event - m->cycles < step)
                step = event - m->cycles;
            if (step == 0)
                step = 1;
            m->cycles += step;
            vz328_tick(m);
        }
        if (p1_open)
            host_port_flush(p1);
        if (p2_open)
            host_port_flush(p2);
    }
}

int main(int argc, char **argv)
{
    options_t opt;
    vz328_t m;
    host_port_t uart1_port;
    host_port_t uart2_port;
    bool uart1_open = false;
    bool uart2_open = false;
    char err[512];
    int rc = 0;

    static const struct option long_options[] = {
        {"load",        required_argument, NULL, 'l'},
        {"ipl",         no_argument,       NULL, 'i'},
        {"pty",         no_argument,       NULL, 'p'},
        {"pty-link",    required_argument, NULL, 1000},
        {"serial",      required_argument, NULL, 's'},
        {"stdio",       no_argument,       NULL, 1001},
        {"uart2",       required_argument, NULL, 1002},
        {"uart2-pty",   no_argument,       NULL, 1003},
        {"uart2-link",  required_argument, NULL, 1004},
        {"fast",        no_argument,       NULL, 1005},
        {"max-cycles",  required_argument, NULL, 1006},
        {"run-time",    required_argument, NULL, 1007},
        {"wait-key",    no_argument,       NULL, 1008},
        {"buttons",     required_argument, NULL, 1009},
        {"switches",    required_argument, NULL, 1010},
        {"led",         no_argument,       NULL, 1011},
        {"trace-io",    no_argument,       NULL, 1012},
        {"trace-irq",   no_argument,       NULL, 1013},
        {"dump",        no_argument,       NULL, 1014},
        {"help",        no_argument,       NULL, 'h'},
        {"version",     no_argument,       NULL, 'V'},
        {NULL, 0, NULL, 0}
    };

    memset(&opt, 0, sizeof(opt));
    opt.buttons = 0x0f;
    opt.switches = 0xff;

    for (;;) {
        int c = getopt_long(argc, argv, "l:ips:hV", long_options, NULL);
        uint32_t v;

        if (c == -1)
            break;
        switch (c) {
        case 'l':
            opt.load_file = optarg;
            break;
        case 'i':
            opt.force_ipl = true;
            break;
        case 'p':
            opt.uart1_mode = EP_PTY;
            break;
        case 's':
            opt.uart1_mode = EP_SERIAL;
            opt.uart1_serial = optarg;
            break;
        case 1000:
            opt.uart1_mode = EP_PTY;
            opt.uart1_link = optarg;
            break;
        case 1001:
            opt.uart1_mode = EP_STDIO;
            break;
        case 1002:
            opt.uart2_mode = EP_SERIAL;
            opt.uart2_serial = optarg;
            break;
        case 1003:
            opt.uart2_mode = EP_PTY;
            break;
        case 1004:
            opt.uart2_mode = EP_PTY;
            opt.uart2_link = optarg;
            break;
        case 1005:
            opt.fast = true;
            break;
        case 1006:
            if (!parse_u64(optarg, &opt.max_cycles)) {
                fprintf(stderr, "invalid --max-cycles value: %s\n", optarg);
                return 1;
            }
            break;
        case 1007: {
            char *end;
            double d = strtod(optarg, &end);
            if (end == optarg || *end != '\0' || d < 0.0) {
                fprintf(stderr, "invalid --run-time value: %s\n", optarg);
                return 1;
            }
            opt.run_time = d;
            break;
        }
        case 1008:
            opt.wait_key = true;
            break;
        case 1009:
            if (!parse_u32(optarg, &v) || (v & ~0x0fu) != 0) {
                fprintf(stderr, "invalid --buttons mask: %s\n", optarg);
                return 1;
            }
            opt.buttons = (uint8_t)v;
            break;
        case 1010:
            if (!parse_u32(optarg, &v) || (v & ~0xffu) != 0) {
                fprintf(stderr, "invalid --switches mask: %s\n", optarg);
                return 1;
            }
            opt.switches = (uint8_t)v;
            break;
        case 1011:
            opt.show_led = true;
            break;
        case 1012:
            opt.trace_io = true;
            break;
        case 1013:
            opt.trace_irq = true;
            break;
        case 1014:
            opt.dump = true;
            break;
        case 'h':
            usage(stdout);
            return 0;
        case 'V':
            version();
            return 0;
        default:
            usage(stderr);
            return 1;
        }
    }

    if (optind < argc) {
        fprintf(stderr, "unexpected argument: %s\n", argv[optind]);
        usage(stderr);
        return 1;
    }
    if (opt.force_ipl && opt.load_file) {
        fprintf(stderr, "--ipl and --load cannot be used together\n");
        return 1;
    }

    if (opt.uart1_mode == EP_DEFAULT)
        opt.uart1_mode = opt.load_file ? EP_STDIO : EP_PTY;
    if (opt.uart2_mode == EP_DEFAULT)
        opt.uart2_mode = EP_NONE;
    if (opt.uart1_mode == EP_STDIO && opt.uart2_mode == EP_STDIO) {
        fprintf(stderr, "UART1 and UART2 cannot both use --stdio\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    host_port_init(&uart1_port);
    host_port_init(&uart2_port);

    if (vz328_init(&m) < 0) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    vz_cpu_init(&m);
    m.trace_io = opt.trace_io;
    m.trace_irq = opt.trace_irq;
    m.show_led = opt.show_led;
    m.push_buttons = opt.buttons;
    m.toggle_switches = opt.switches;
    vz328_power_on(&m);

    if (open_endpoint(&uart1_port, opt.uart1_mode, opt.uart1_serial,
                      opt.uart1_link, "UART1", &uart1_open,
                      err, sizeof(err)) < 0) {
        fprintf(stderr, "UART1: %s\n", err);
        rc = 1;
        goto done;
    }
    if (uart1_open) {
        uart_set_port(&m.uart[0], &uart1_port);
        m.host[0] = &uart1_port;
    }

    if (open_endpoint(&uart2_port, opt.uart2_mode, opt.uart2_serial,
                      opt.uart2_link, "UART2", &uart2_open,
                      err, sizeof(err)) < 0) {
        fprintf(stderr, "UART2: %s\n", err);
        rc = 1;
        goto done;
    }
    if (uart2_open) {
        uart_set_port(&m.uart[1], &uart2_port);
        m.host[1] = &uart2_port;
    }

    if (opt.load_file) {
        srec_info_t info;

        if (vz328_load_abs(&m, opt.load_file, &info, err, sizeof(err)) < 0) {
            fprintf(stderr, "load failed: %s\n", err);
            rc = 1;
            goto done;
        }
        fprintf(stderr, "Loaded %s: %" PRIu32 " records, %08" PRIX32
                "-%08" PRIX32 ", start %08" PRIX32 "\n",
                opt.load_file, info.data_records, info.min_address,
                info.max_address,
                info.have_start ? info.start_address : VZ_DEFAULT_PC);

        if (opt.wait_key) {
            if (!wait_for_key(uart1_open ? &uart1_port : NULL, uart1_open)) {
                rc = 1;
                goto done;
            }
        }
        vz328_start_program(&m, info.have_start ? info.start_address :
                            VZ_DEFAULT_PC, VZ_DEFAULT_SSP);
    } else {
        vz328_ipl_begin(&m);
    }

    if (opt.run_time > 0.0 && opt.max_cycles == 0)
        opt.max_cycles = (uint64_t)(opt.run_time * VZ_SYSCLK_HZ);

    {
        double wall_start = monotonic_seconds();
        uint64_t next_rx[2] = {0, 0};
        bool started = (m.ipl_state == VZ_IPL_RUN);

        while (!stop_requested && !m.halted) {
            if (!started) {
                poll_ipl_input(&m, &uart1_port, uart1_open);
                if (m.ipl_state == VZ_IPL_RUN) {
                    started = true;
                    wall_start = monotonic_seconds();
                    next_rx[0] = m.cycles;
                    next_rx[1] = m.cycles;
                } else {
                    if (uart1_open)
                        host_port_wait_input(&uart1_port, 20);
                    else
                        sleep_seconds(0.02);
                    if (uart1_open && uart1_port.input_eof &&
                        uart1_port.in_pos >= uart1_port.in_len) {
                        fprintf(stderr, "input closed before a program was loaded\n");
                        break;
                    }
                    continue;
                }
            }

            poll_uart_input(&m, 0, &uart1_port, uart1_open, &next_rx[0]);
            poll_uart_input(&m, 1, &uart2_port, uart2_open, &next_rx[1]);
            if (m.halted)
                break;

            {
                uint64_t now = m.cycles;
                uint64_t event = vz328_next_event(&m);
                uint64_t slice = 20000;
                int used;

                if (event != UINT64_MAX && event > now &&
                    event - now < slice)
                    slice = event - now;
                if (opt.max_cycles != 0) {
                    if (now >= opt.max_cycles)
                        break;
                    if (opt.max_cycles - now < slice)
                        slice = opt.max_cycles - now;
                }
                if (slice < 1)
                    slice = 1;
                if (slice > 0x7fffffff)
                    slice = 0x7fffffff;

                used = vz_cpu_execute((int)slice);
                if (used <= 0)
                    used = (int)slice;
                m.cycles += (uint64_t)used;
                vz328_tick(&m);
            }

            if (uart1_open)
                host_port_flush(&uart1_port);
            if (uart2_open)
                host_port_flush(&uart2_port);

            if (opt.max_cycles != 0 && m.cycles >= opt.max_cycles)
                break;

            if (!opt.fast) {
                double target = wall_start +
                                (double)m.cycles / VZ_SYSCLK_HZ;
                double now = monotonic_seconds();
                double delay = target - now;

                /* Keep the loop responsive to host input. */
                if (delay > 0.002)
                    delay = 0.002;
                if (delay > 0.0002)
                    sleep_seconds(delay);
            }
        }
    }

done:
    drain_output(&m, &uart1_port, uart1_open, &uart2_port, uart2_open);
    if (uart1_open)
        host_port_flush(&uart1_port);
    if (uart2_open)
        host_port_flush(&uart2_port);

    /* Closing the master side of a PTY discards bytes that the client has not
     * read yet.  Give kermit or another terminal program a moment to consume
     * the final output before the PTYs go away. */
    if ((uart1_open && uart1_port.is_pty) ||
        (uart2_open && uart2_port.is_pty)) {
        sleep_seconds(0.05);
        if (uart1_open)
            host_port_flush(&uart1_port);
        if (uart2_open)
            host_port_flush(&uart2_port);
        sleep_seconds(0.05);
    }

    if (opt.dump) {
        fprintf(stderr, "---- CPU registers ----\n");
        vz_cpu_dump(stderr);
    }

    if (m.halted) {
        if (m.halt_code == VZ_MAGIC_EXIT) {
            fprintf(stderr, "Program requested exit (0x600D) at PC=%06" PRIX32
                    "\n", vz_cpu_get_pc());
        } else {
            fprintf(stderr, "Unhandled exception or abort (0xDEAD) at PC=%06"
                    PRIX32 "\n", vz_cpu_get_pc());
            rc = 2;
        }
    } else if (stop_requested) {
        fprintf(stderr, "Interrupted\n");
        rc = 0;
    } else if (opt.max_cycles != 0 && m.cycles >= opt.max_cycles) {
        fprintf(stderr, "Stopped after %" PRIu64 " cycles\n", m.cycles);
    }

    if (uart1_open)
        host_port_close(&uart1_port);
    if (uart2_open)
        host_port_close(&uart2_port);
    vz328_destroy(&m);
    return rc;
}
