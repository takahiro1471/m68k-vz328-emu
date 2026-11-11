/*
 * srec.c - Motorola S-record loader
 *
 * Supports S0, S1, S2, S3, S5, S6, S7, S8 and S9 records.  m68k-as emits
 * S0/S2/S5/S8 for this board.
 */
#include "srec.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void srec_info_init(srec_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->min_address = UINT32_MAX;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_hex_byte(const char *p)
{
    int hi = hexval((unsigned char)p[0]);
    int lo = hexval((unsigned char)p[1]);
    if (hi < 0 || lo < 0)
        return -1;
    return (hi << 4) | lo;
}

int srec_parse_line(const char *line, srec_write_fn write_fn, void *ctx,
                    srec_info_t *info, bool *terminated,
                    char *err, size_t errlen)
{
    char buf[1024];
    size_t len;
    int type;
    int count;
    int addr_len;
    uint32_t address = 0;
    int i;
    unsigned sum;
    int checksum;

    if (terminated)
        *terminated = false;

    while (*line == ' ' || *line == '\t')
        line++;
    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' ' || line[len - 1] == '\t'))
        len--;
    if (len == 0)
        return 0;
    if (len >= sizeof(buf)) {
        snprintf(err, errlen, "S-record line too long");
        return -1;
    }
    memcpy(buf, line, len);
    buf[len] = '\0';

    if (buf[0] != 'S' || !isdigit((unsigned char)buf[1])) {
        snprintf(err, errlen, "not an S-record");
        return -1;
    }
    type = buf[1] - '0';
    if (len < 4) {
        snprintf(err, errlen, "short S-record");
        return -1;
    }
    count = parse_hex_byte(buf + 2);
    if (count < 0) {
        snprintf(err, errlen, "bad length field");
        return -1;
    }
    /* count includes address, data and checksum */
    if ((size_t)(4 + count * 2) != len) {
        snprintf(err, errlen, "length mismatch (len=%zu, count=%d)",
                 len, count);
        return -1;
    }

    switch (type) {
    case 0: addr_len = 2; break;
    case 1: addr_len = 2; break;
    case 2: addr_len = 3; break;
    case 3: addr_len = 4; break;
    case 5: addr_len = 2; break;
    case 6: addr_len = 3; break;
    case 7: addr_len = 4; break;
    case 8: addr_len = 3; break;
    case 9: addr_len = 2; break;
    default:
        snprintf(err, errlen, "unsupported S%d record", type);
        return -1;
    }
    if (count < addr_len + 1) {
        snprintf(err, errlen, "record too short for address");
        return -1;
    }

    sum = (unsigned)count;
    for (i = 0; i < count - 1; i++) {
        int b = parse_hex_byte(buf + 4 + i * 2);
        if (b < 0) {
            snprintf(err, errlen, "bad hex digit");
            return -1;
        }
        sum += (unsigned)b;
    }
    checksum = parse_hex_byte(buf + 4 + (count - 1) * 2);
    if (checksum < 0) {
        snprintf(err, errlen, "bad checksum field");
        return -1;
    }
    if (((sum + (unsigned)checksum) & 0xff) != 0xff) {
        snprintf(err, errlen, "checksum error");
        return -1;
    }

    for (i = 0; i < addr_len; i++) {
        int b = parse_hex_byte(buf + 4 + i * 2);
        address = (address << 8) | (uint32_t)b;
    }

    switch (type) {
    case 0:
    case 5:
    case 6:
        /* Header / record count: no memory image. */
        return 0;

    case 1:
    case 2:
    case 3: {
        int data_bytes = count - addr_len - 1;
        int j;
        for (j = 0; j < data_bytes; j++) {
            int b = parse_hex_byte(buf + 4 + (addr_len + j) * 2);
            uint32_t a = address + (uint32_t)j;
            if (b < 0) {
                snprintf(err, errlen, "bad data byte");
                return -1;
            }
            if (write_fn && write_fn(ctx, a, (uint8_t)b) < 0) {
                snprintf(err, errlen, "cannot write address 0x%06X", a);
                return -1;
            }
            if (a < info->min_address)
                info->min_address = a;
            if (a > info->max_address)
                info->max_address = a;
            info->have_data = true;
        }
        info->data_records++;
        return 0;
    }

    case 7:
    case 8:
    case 9:
        info->start_address = address & 0xffffffu;
        info->have_start = true;
        if (terminated)
            *terminated = true;
        return 0;
    }

    return 0;
}

int srec_load_file(const char *path, srec_write_fn write_fn, void *ctx,
                   srec_info_t *info, char *err, size_t errlen)
{
    FILE *f;
    char line[1200];
    unsigned lineno = 0;
    bool terminated = false;
    int rc = 0;

    srec_info_init(info);
    f = fopen(path, "r");
    if (!f) {
        snprintf(err, errlen, "open %s: %s", path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        bool term_here = false;
        lineno++;
        if (line[0] == '\0' || line[0] == '\n' || line[0] == '\r')
            continue;
        if (srec_parse_line(line, write_fn, ctx, info, &term_here,
                            err, errlen) < 0) {
            char detail[256];
            snprintf(detail, sizeof(detail), "%s", err);
            snprintf(err, errlen, "%s:%u: %s", path, lineno, detail);
            rc = -1;
            break;
        }
        if (term_here) {
            terminated = true;
            break;
        }
    }
    fclose(f);

    if (rc == 0 && !info->have_data) {
        snprintf(err, errlen, "%s: no data records", path);
        rc = -1;
    }
    if (rc == 0 && !terminated) {
        /* Not an error: some tools omit S7/S8/S9. */
    }
    return rc;
}
