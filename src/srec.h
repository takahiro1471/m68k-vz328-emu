/*
 * srec.h - Motorola S-record loader
 */
#ifndef VZ328_SREC_H
#define VZ328_SREC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct srec_info {
    uint32_t start_address;
    uint32_t min_address;
    uint32_t max_address;
    uint32_t data_records;
    bool have_start;
    bool have_data;
} srec_info_t;

typedef int (*srec_write_fn)(void *ctx, uint32_t address, uint8_t value);

void srec_info_init(srec_info_t *info);

/*
 * Parse one S-record line.  The line may contain a trailing CR/LF, but must
 * not contain more than one record.  Returns 0 on success, -1 on a malformed
 * record or checksum error.  *terminated is set when an S7/S8/S9 record was
 * seen.
 */
int srec_parse_line(const char *line, srec_write_fn write_fn, void *ctx,
                    srec_info_t *info, bool *terminated,
                    char *err, size_t errlen);

/* Load an m68k-as style .abs file. */
int srec_load_file(const char *path, srec_write_fn write_fn, void *ctx,
                   srec_info_t *info, char *err, size_t errlen);

#endif
