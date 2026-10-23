#pragma once

#include <stddef.h>

/*
 * GBK <-> UTF-8 conversion.
 *
 * FatFS is configured with CONFIG_FATFS_CODEPAGE_936 so that Chinese names on
 * the card come back as GBK bytes, but LVGL and the HTTP layer speak UTF-8.
 * These helpers convert in place into caller supplied buffers and always
 * NUL-terminate.  The return value is the number of bytes written (excluding
 * the terminator) or, when the output does not fit, the required size.
 */

size_t gbk_to_utf8(const char *in, size_t in_len, char *out, size_t out_size);
size_t utf8_to_gbk(const char *in, char *out, size_t out_size);
