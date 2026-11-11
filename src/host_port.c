/*
 * host_port.c - host side serial endpoint for the MC68VZ328 emulator
 */
#define _GNU_SOURCE
#include "host_port.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

static struct termios saved_stdin_termios;
static int stdin_termios_saved;

static void restore_stdin_termios(void)
{
    if (stdin_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_stdin_termios);
        stdin_termios_saved = 0;
    }
}

void host_port_init(host_port_t *p)
{
    memset(p, 0, sizeof(*p));
    p->in_fd = -1;
    p->out_fd = -1;
}

void host_port_close(host_port_t *p)
{
    if (p->close_in && p->in_fd >= 0)
        close(p->in_fd);
    if (p->close_out && p->out_fd >= 0 && p->out_fd != p->in_fd)
        close(p->out_fd);
    if (stdin_termios_saved) {
        restore_stdin_termios();
    }
    host_port_init(p);
}

static void set_raw(int fd, int keep_isig)
{
    struct termios t;

    if (tcgetattr(fd, &t) < 0)
        return;
    cfmakeraw(&t);
    if (keep_isig)
        t.c_lflag |= ISIG;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &t);
}

int host_port_open_pty(host_port_t *p, const char *link_path,
                       char *err, size_t errlen)
{
    int master;
    int slave;
    char *name;

    host_port_init(p);

    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        snprintf(err, errlen, "posix_openpt: %s", strerror(errno));
        return -1;
    }
    if (grantpt(master) < 0 || unlockpt(master) < 0) {
        snprintf(err, errlen, "grantpt/unlockpt: %s", strerror(errno));
        close(master);
        return -1;
    }
    name = ptsname(master);
    if (!name) {
        snprintf(err, errlen, "ptsname: %s", strerror(errno));
        close(master);
        return -1;
    }
    snprintf(p->slave_path, sizeof(p->slave_path), "%s", name);

    /* Open the slave once to put it in raw mode.  This avoids echo and
     * newline translation between the emulator and a client such as kermit. */
    slave = open(name, O_RDWR | O_NOCTTY);
    if (slave >= 0) {
        set_raw(slave, 1);
        close(slave);
    }

    if (fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK) < 0) {
        snprintf(err, errlen, "fcntl: %s", strerror(errno));
        close(master);
        return -1;
    }

    p->in_fd = master;
    p->out_fd = master;
    p->close_in = 1;
    p->close_out = 0;
    p->is_pty = 1;

    if (link_path && *link_path) {
        unlink(link_path);
        if (symlink(name, link_path) < 0) {
            snprintf(err, errlen, "symlink %s: %s", link_path,
                     strerror(errno));
            /* Not fatal; the pty itself is usable. */
        }
    }
    return 0;
}

int host_port_open_serial(host_port_t *p, const char *device,
                          char *err, size_t errlen)
{
    int fd;

    host_port_init(p);
    fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        snprintf(err, errlen, "open %s: %s", device, strerror(errno));
        return -1;
    }
    if (isatty(fd))
        set_raw(fd, 1);
    p->in_fd = fd;
    p->out_fd = fd;
    p->close_in = 1;
    p->close_out = 0;
    return 0;
}

void host_port_open_stdio(host_port_t *p)
{
    host_port_init(p);
    p->in_fd = STDIN_FILENO;
    p->out_fd = STDOUT_FILENO;
    p->close_in = 0;
    p->close_out = 0;

    if (isatty(STDIN_FILENO) && !stdin_termios_saved) {
        if (tcgetattr(STDIN_FILENO, &saved_stdin_termios) == 0) {
            stdin_termios_saved = 1;
            atexit(restore_stdin_termios);
        }
        /* Keep ISIG so Ctrl-C still terminates the emulator. */
        set_raw(STDIN_FILENO, 1);
    }
    if (fcntl(STDIN_FILENO, F_SETFL,
              fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK) < 0) {
        /* stdin may not support this; reads will simply block. */
    }
}

static int fill_input(host_port_t *p)
{
    ssize_t n;

    if (p->in_fd < 0)
        return -1;
    if (p->in_pos < p->in_len)
        return 0;

    p->in_pos = 0;
    p->in_len = 0;
    n = read(p->in_fd, p->in_buf, sizeof(p->in_buf));
    if (n > 0) {
        p->in_len = (size_t)n;
        return 0;
    }
    if (n == 0) {
        p->input_eof = 1;
        return -1;
    }
    if (errno == EINTR)
        return fill_input(p);
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EIO) {
        /* EIO happens on a pty master while no slave is open. */
        return -1;
    }
    p->input_eof = 1;
    return -1;
}

int host_port_getc(host_port_t *p)
{
    if (fill_input(p) < 0)
        return -1;
    return p->in_buf[p->in_pos++];
}

int host_port_wait_input(host_port_t *p, int timeout_ms)
{
    struct pollfd fds;

    if (p->in_fd < 0)
        return -1;
    if (p->in_pos < p->in_len)
        return 0;

    fds.fd = p->in_fd;
    fds.events = POLLIN;
    fds.revents = 0;
    if (poll(&fds, 1, timeout_ms) > 0)
        return 0;
    return -1;
}

void host_port_write_byte(host_port_t *p, uint8_t b)
{
    if (p->out_len >= sizeof(p->out_buf)) {
        host_port_flush(p);
        if (p->out_len >= sizeof(p->out_buf)) {
            p->dropped_output++;
            return;
        }
    }
    p->out_buf[p->out_len++] = b;
}

void host_port_write(host_port_t *p, const void *data, size_t len)
{
    const uint8_t *q = (const uint8_t *)data;
    size_t i;

    for (i = 0; i < len; i++)
        host_port_write_byte(p, q[i]);
}

void host_port_write_str(host_port_t *p, const char *s)
{
    if (s)
        host_port_write(p, s, strlen(s));
}

void host_port_flush(host_port_t *p)
{
    size_t off = 0;

    if (p->out_fd < 0 || p->out_len == 0)
        return;

    while (off < p->out_len) {
        ssize_t n = write(p->out_fd, p->out_buf + off, p->out_len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                      errno == EIO))
            break;
        break;
    }

    if (off > 0) {
        if (off < p->out_len)
            memmove(p->out_buf, p->out_buf + off, p->out_len - off);
        p->out_len -= off;
    }
}
