/**
 * FatFS is configured with CONFIG_FATFS_API_ENCODING_UTF_8 (FF_LFN_UNICODE=1,
 * FF_STRF_ENCODE=UTF-8), so every name that FatFS hands us is already UTF-8 and
 * every path we hand back must be UTF-8 as well. These helpers used to convert
 * between GBK and UTF-8; they are kept as pass-throughs so call sites stay
 * readable and a future codepage switch only has to touch this file.
 */
#include "gbk.h"

#include <string.h>

size_t gbk_to_utf8(const char *in, size_t in_len, char *out, size_t out_size)
{
    if (in == NULL || out == NULL || out_size == 0) {
        return 0;
    }
    if (in_len > out_size - 1) {
        in_len = out_size - 1;
    }
    memcpy(out, in, in_len);
    out[in_len] = '\0';
    return in_len;
}

size_t utf8_to_gbk(const char *in, char *out, size_t out_size)
{
    size_t len = in ? strlen(in) : 0;
    if (out == NULL || out_size == 0) {
        return 0;
    }
    if (len > out_size - 1) {
        len = out_size - 1;
    }
    if (len) {
        memcpy(out, in, len);
    }
    out[len] = '\0';
    return len;
}
