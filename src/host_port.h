/*
 * host_port.h - host side serial endpoint for the MC68VZ328 emulator
 *
 * The emulator's UARTs can be connected to a pseudo terminal (for
 * m68k-term/kermit), an existing serial device, or stdin/stdout.
 */
#ifndef VZ328_HOST_PORT_H
#define VZ328_HOST_PORT_H

#include <stddef.h>
#include <stdint.h>

#define HOST_PORT_IN_CAP  8192
#define HOST_PORT_OUT_CAP 65536

typedef struct host_port {
    int in_fd;
    int out_fd;
    int close_in;
    int close_out;

    int is_pty;
    char slave_path[256];

    int input_eof;
    unsigned long dropped_output;

    uint8_t in_buf[HOST_PORT_IN_CAP];
    size_t in_len;
    size_t in_pos;

    uint8_t out_buf[HOST_PORT_OUT_CAP];
    size_t out_len;
} host_port_t;

void host_port_init(host_port_t *p);
void host_port_close(host_port_t *p);

/* Open a pseudo terminal.  The slave device name is stored in p->slave_path.
 * If link_path is not NULL, a symlink pointing at the slave is created. */
int host_port_open_pty(host_port_t *p, const char *link_path,
                       char *err, size_t errlen);

/* Open an existing character device/pty. */
int host_port_open_serial(host_port_t *p, const char *device,
                          char *err, size_t errlen);

/* Use stdin/stdout.  If stdin is a tty it is put into a raw-ish mode. */
void host_port_open_stdio(host_port_t *p);

/* Returns the next input byte, or -1 if none is currently available. */
int host_port_getc(host_port_t *p);

/* Queue output.  Data is written to the fd by host_port_flush(). */
void host_port_write_byte(host_port_t *p, uint8_t b);
void host_port_write(host_port_t *p, const void *data, size_t len);
void host_port_write_str(host_port_t *p, const char *s);

/* Try to write queued output.  Safe to call often. */
void host_port_flush(host_port_t *p);

/* Wait up to timeout_ms for input (used when the CPU is stopped). */
int host_port_wait_input(host_port_t *p, int timeout_ms);

#endif
