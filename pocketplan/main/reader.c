/*
 * Text reader core. Books stay on the SD card; pages are decoded from a small
 * line buffer in PSRAM, so the book size is bounded by the card, not the heap.
 * Pagination uses the real LVGL font metrics through lv_text_get_size().
 */
#include "reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "gbk_table.h"

static const char *TAG = "reader";

#define LINE_BUF  (8 * 1024)
#define PROBE_LEN 4096

enum { ENC_UTF8 = 0, ENC_GBK = 1, ENC_UTF16LE = 2, ENC_UTF16BE = 3 };

struct pp_book {
    char path[320];
    char title[96];
    FILE *f;
    long size;     /* payload bytes, BOM excluded */
    long data_off; /* where the text starts in the file */
    int enc;
    uint8_t *raw; /* raw bytes of the current line */
    char *line;   /* UTF-8 scratch for one line */
};

/* ------------------------------ encoding -------------------------------- */
static bool utf8_valid(const char *s, size_t len)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < len) {
        unsigned char c = p[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        int extra;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + (size_t)extra >= len) {
            return false;
        }
        for (int k = 1; k <= extra; k++) {
            if ((p[i + k] & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000)) {
            return false;
        }
        i += (size_t)extra + 1;
    }
    return true;
}

static size_t utf8_put(char *out, uint32_t cp)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

/* GBK -> UTF-8; unmapped bytes become '?'. Returns bytes written. */
static size_t gbk_to_utf8_buf(const unsigned char *in, size_t in_len, char *out, size_t out_size)
{
    size_t o = 0;
    size_t i = 0;
    while (i < in_len && o + 4 < out_size) {
        unsigned char c = in[i];
        if (c < 0x80) {
            out[o++] = (char)c;
            i++;
            continue;
        }
        if (i + 1 >= in_len) {
            break;
        }
        int trail = gbk_trail_index[in[i + 1]];
        uint16_t uni = 0xFFFF;
        if (c >= GBK_LEAD_MIN && c <= GBK_LEAD_MAX && trail >= 0) {
            uni = gbk_uni[c - GBK_LEAD_MIN][trail];
        }
        if (uni == 0xFFFF) {
            out[o++] = '?';
        } else {
            o += utf8_put(out + o, uni);
        }
        i += 2;
    }
    out[o] = '\0';
    return o;
}

static size_t utf16_to_utf8(const unsigned char *in, size_t len, bool little, char *out,
                            size_t out_size)
{
    size_t o = 0;
    for (size_t i = 0; i + 1 < len && o + 4 < out_size; i += 2) {
        uint32_t c = little ? (uint32_t)(in[i] | (in[i + 1] << 8))
                            : (uint32_t)((in[i] << 8) | in[i + 1]);
        if (c >= 0xD800 && c <= 0xDBFF && i + 3 < len) {
            uint32_t lo = little ? (uint32_t)(in[i + 2] | (in[i + 3] << 8))
                                 : (uint32_t)((in[i + 2] << 8) | in[i + 3]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        o += utf8_put(out + o, c);
    }
    out[o] = '\0';
    return o;
}

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

/* -------------------------------- lines --------------------------------- */

/* Read one line starting at payload offset pos, decoded to UTF-8 in book->line.
 * Returns the UTF-8 length, zero at end of file; raw_len receives the source
 * bytes consumed (newline included) so the caller can advance. */
static size_t read_line(pp_book_t *b, long pos, long *raw_len)
{
    *raw_len = 0;
    if (pos >= b->size) {
        return 0;
    }
    if (fseek(b->f, b->data_off + pos, SEEK_SET) != 0) {
        return 0;
    }
    size_t want = (size_t)(b->size - pos) > LINE_BUF ? LINE_BUF : (size_t)(b->size - pos);
    size_t got = fread(b->raw, 1, want, b->f);
    if (got == 0) {
        return 0;
    }

    size_t content = got;
    size_t consumed = got;
    bool utf16 = b->enc == ENC_UTF16LE || b->enc == ENC_UTF16BE;
    if (utf16) {
        content &= ~(size_t)1; /* whole code units only */
        consumed &= ~(size_t)1;
        for (size_t i = 0; i + 1 < content; i += 2) {
            unsigned be_nl = b->enc == ENC_UTF16LE ? b->raw[i + 1] : b->raw[i];
            unsigned le_nl = b->enc == ENC_UTF16LE ? b->raw[i] : b->raw[i + 1];
            if (be_nl == 0 && le_nl == '\n') {
                content = i;
                consumed = i + 2;
                break;
            }
        }
    } else {
        char *nl = memchr(b->raw, '\n', content);
        if (nl != NULL) {
            content = (size_t)(nl - (char *)b->raw);
            consumed = content + 1;
        }
    }
    if (content > 0 && b->raw[content - 1] == '\r') {
        content--;
    }
    *raw_len = (long)consumed;

    if (b->enc == ENC_GBK) {
        if (content > 0 && content < got && (b->raw[content - 1] & 0x80) != 0) {
            content--; /* never split a double byte character */
            *raw_len = (long)content;
        }
        return gbk_to_utf8_buf(b->raw, content, b->line, LINE_BUF * 2 - 1);
    }
    if (utf16) {
        return utf16_to_utf8(b->raw, content, b->enc == ENC_UTF16LE, b->line, LINE_BUF * 2 - 1);
    }
    if (content > LINE_BUF * 2 - 2) {
        content = LINE_BUF * 2 - 2;
    }
    memcpy(b->line, b->raw, content);
    b->line[content] = '\0';
    return content;
}

/* How many source bytes of the current line produce the given UTF-8 prefix. */
static long raw_len_for_prefix(const pp_book_t *b, size_t utf8_len)
{
    if (b->enc == ENC_UTF8) {
        return (long)utf8_len;
    }
    size_t out = 0;
    size_t i = 0;
    while (b->raw[i] != '\0' && out < utf8_len) {
        if (b->enc == ENC_GBK) {
            size_t step = (b->raw[i] & 0x80) ? 2 : 1;
            out += (b->raw[i] & 0x80) ? 3 : 1;
            i += step;
        } else {
            uint32_t c = b->enc == ENC_UTF16LE
                             ? (uint32_t)(b->raw[i] | (b->raw[i + 1] << 8))
                             : (uint32_t)((b->raw[i] << 8) | b->raw[i + 1]);
            out += c < 0x80 ? 1 : (c < 0x800 ? 2 : 3);
            i += 2;
        }
    }
    return (long)i;
}

/* ------------------------------- opening -------------------------------- */
pp_book_t *pp_reader_open(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    pp_book_t *b = calloc(1, sizeof(*b));
    if (b == NULL) {
        fclose(f);
        return NULL;
    }
    b->f = f;
    b->size = (long)st.st_size;
    snprintf(b->path, sizeof(b->path), "%s", path);

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(b->title, sizeof(b->title), "%s", base);
    char *dot = strrchr(b->title, '.');
    if (dot) {
        *dot = '\0';
    }

    unsigned char probe[PROBE_LEN];
    size_t got = fread(probe, 1, sizeof(probe), f);
    b->enc = ENC_UTF8;
    if (got >= 3 && probe[0] == 0xEF && probe[1] == 0xBB && probe[2] == 0xBF) {
        b->data_off = 3;
    } else if (got >= 2 && probe[0] == 0xFF && probe[1] == 0xFE) {
        b->enc = ENC_UTF16LE;
        b->data_off = 2;
    } else if (got >= 2 && probe[0] == 0xFE && probe[1] == 0xFF) {
        b->enc = ENC_UTF16BE;
        b->data_off = 2;
    } else if (!utf8_valid((const char *)probe, got)) {
        b->enc = ENC_GBK;
    }
    b->size -= b->data_off;

    b->raw = heap_caps_malloc(LINE_BUF, MALLOC_CAP_SPIRAM);
    b->line = heap_caps_malloc(LINE_BUF * 2, MALLOC_CAP_SPIRAM);
    if (b->raw == NULL || b->line == NULL) {
        ESP_LOGE(TAG, "no PSRAM for the line buffer");
        pp_reader_close(b);
        return NULL;
    }
    ESP_LOGI(TAG, "opened %s (%ld bytes, %s)", b->title, b->size,
             b->enc == ENC_GBK ? "GBK"
                               : (b->enc == ENC_UTF8 ? "UTF-8"
                                                     : (b->enc == ENC_UTF16LE ? "UTF-16LE"
                                                                              : "UTF-16BE")));
    return b;
}

void pp_reader_close(pp_book_t *book)
{
    if (book == NULL) {
        return;
    }
    if (book->f) {
        fclose(book->f);
    }
    free(book->raw);
    free(book->line);
    free(book);
}

const char *pp_reader_title(const pp_book_t *book)
{
    return book ? book->title : "";
}

size_t pp_reader_length(const pp_book_t *book)
{
    return book ? (size_t)book->size : 0;
}

bool pp_reader_encoding_is_gbk(const pp_book_t *book)
{
    return book ? book->enc == ENC_GBK : false;
}

int pp_reader_percent(const pp_book_t *book, int32_t offset)
{
    if (book == NULL || book->size <= 0) {
        return 0;
    }
    if (offset <= 0) {
        return 0;
    }
    if (offset >= book->size) {
        return 100;
    }
    return (int)((int64_t)offset * 100 / (int64_t)book->size);
}

bool pp_reader_page(pp_book_t *book, int32_t start, const lv_font_t *font, int32_t width,
                    int32_t height, int32_t line_space, char *out, size_t out_size,
                    int32_t *next_off)
{
    if (book == NULL || out == NULL || out_size < 8 || book->size <= 0) {
        return false;
    }
    if (start < 0) {
        start = 0;
    }
    if (start >= book->size) {
        return false;
    }

    long pos = start;
    size_t n = 0;
    out[0] = '\0';

    while (pos < book->size) {
        long raw_len = 0;
        size_t line_len = read_line(book, pos, &raw_len);
        if (raw_len <= 0) {
            break;
        }
        if (n + line_len + 2 >= out_size) {
            break;
        }
        size_t before = n;
        memcpy(out + n, book->line, line_len);
        n += line_len;
        out[n++] = '\n';
        out[n] = '\0';

        lv_point_t size;
        lv_text_get_size(&size, out, font, 0, line_space, width, LV_TEXT_FLAG_NONE);
        if (size.y > height) {
            n = before;
            out[n] = '\0';
            break;
        }
        pos += raw_len;
    }

    if (n == 0) {
        /* A single paragraph is taller than the page: show a prefix of it and
         * continue from the matching byte offset so nothing is lost. */
        long raw_len = 0;
        size_t line_len = read_line(book, pos, &raw_len);
        if (raw_len <= 0 || line_len == 0) {
            return false;
        }
        size_t take = line_len;
        if (take + 2 > out_size) {
            take = out_size - 2;
        }
        while (take > 1 && ((unsigned char)book->line[take] & 0xC0) == 0x80) {
            take--;
        }
        memcpy(out, book->line, take);
        out[take] = '\n';
        out[take + 1] = '\0';
        long used = raw_len_for_prefix(book, take);
        if (used <= 0) {
            used = raw_len;
        }
        pos += used;
        n = take + 1;
    }

    if (next_off) {
        *next_off = (int32_t)(pos <= start ? start + 1 : pos);
    }
    return n > 0;
}

int32_t pp_reader_load_pos(const char *path)
{
    nvs_handle_t h;
    char key[12];
    snprintf(key, sizeof(key), "b%08" PRIX32, fnv1a(path));
    int32_t value = 0;
    if (nvs_open("ppread", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, key, &value);
        nvs_close(h);
    }
    return value > 0 ? value : 0;
}

void pp_reader_save_pos(const char *path, int32_t offset)
{
    nvs_handle_t h;
    char key[12];
    snprintf(key, sizeof(key), "b%08" PRIX32, fnv1a(path));
    if (nvs_open("ppread", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, key, offset);
        nvs_commit(h);
        nvs_close(h);
    }
}