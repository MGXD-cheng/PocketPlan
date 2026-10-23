/*
 * WebDAV server for the SD card, plus the provisioning pages.
 *
 * Everything lives in one root URI space so that Windows can map
 * "http://<device>/" as a network drive, while a plain browser hitting a
 * directory gets a small HTML file browser with the Wi-Fi form.
 */
#include "webdav.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "board.h"
#include "config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "gbk.h"
#include "net.h"
#include "webapi.h"

static const char *TAG = "webdav";

#define WWW_ROOT "/sdcard"
#define PATHBUF 512

static esp_err_t page_handler(httpd_req_t *req, const char *uri_path);

/* ------------------------------ tiny buffer ----------------------------- */
typedef struct {
    char *p;
    size_t len;
    size_t cap;
    bool oom;
} buf_t;

static void buf_init(buf_t *b, size_t cap)
{
    b->p = malloc(cap);
    b->len = 0;
    b->cap = b->p ? cap : 0;
    b->oom = b->p == NULL;
    if (b->p) {
        b->p[0] = '\0';
    }
}

static void buf_addf(buf_t *b, const char *fmt, ...)
{
    if (b->oom) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (need < 0) {
        b->oom = true;
        return;
    }
    if ((size_t)need >= b->cap - b->len) {
        size_t want = b->len + (size_t)need + 1;
        char *np = realloc(b->p, want);
        if (np == NULL) {
            b->oom = true;
            return;
        }
        b->p = np;
        b->cap = want;
        va_start(ap, fmt);
        vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
        va_end(ap);
    }
    b->len += (size_t)need;
}

static void buf_reset(buf_t *b)
{
    b->len = 0;
    if (b->p) {
        b->p[0] = '\0';
    }
}

static void buf_free(buf_t *b)
{
    free(b->p);
    b->p = NULL;
}

/* ------------------------------- helpers -------------------------------- */
static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    char *dst = s;
    for (char *src = s; *src; src++) {
        if (*src == '%' && hex_val(src[1]) >= 0 && src[1] && hex_val(src[2]) >= 0 && src[2]) {
            *dst++ = (char)((hex_val(src[1]) << 4) | hex_val(src[2]));
            src += 2;
        } else {
            *dst++ = *src;
        }
    }
    *dst = '\0';
}

/* Extract just the path element of a URL, dropping scheme://host and query. */
static void url_path_only(const char *in, char *out, size_t out_size)
{
    const char *p = in;
    const char *scheme = strstr(in, "://");
    if (scheme != NULL) {
        const char *slash = strchr(scheme + 3, '/');
        p = slash ? slash : "/";
    }
    size_t n = 0;
    while (*p && *p != '?' && n + 1 < out_size) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    if (out[0] != '/') {
        if (n + 2 <= out_size) {
            memmove(out + 1, out, n + 1);
            out[0] = '/';
        }
    }
}

static void xml_escape(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 8 < out_size; p++) {
        switch (*p) {
        case '&': o += (size_t)snprintf(out + o, out_size - o, "&amp;"); break;
        case '<': o += (size_t)snprintf(out + o, out_size - o, "&lt;"); break;
        case '>': o += (size_t)snprintf(out + o, out_size - o, "&gt;"); break;
        case '"': o += (size_t)snprintf(out + o, out_size - o, "&quot;"); break;
        case '\'': o += (size_t)snprintf(out + o, out_size - o, "&apos;"); break;
        default: out[o++] = *p; break;
        }
    }
    out[o] = '\0';
}

static void href_encode(const char *in, char *out, size_t out_size)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < out_size; p++) {
        unsigned char c = *p;
        bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
        if (safe) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = '\0';
}

static void decode_uri(httpd_req_t *req, char *out, size_t out_size)
{
    char tmp[PATHBUF];
    url_path_only(req->uri, tmp, sizeof(tmp));
    url_decode(tmp);
    snprintf(out, out_size, "%s", tmp);
}

/* UTF-8 request path -> GBK FlexSPI path on the card. */
static void uri_to_fs(const char *uri_path, char *fs_path, size_t fs_size)
{
    char gbk[PATHBUF];
    utf8_to_gbk(uri_path, gbk, sizeof(gbk));
    if (strcmp(gbk, "/") == 0) {
        snprintf(fs_path, fs_size, "%s", WWW_ROOT);
    } else {
        snprintf(fs_path, fs_size, "%s%s", WWW_ROOT, gbk);
    }
}

static const char *content_type_for(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL) {
        return "application/octet-stream";
    }
    if (strcasecmp(dot, ".mp3") == 0) return "audio/mpeg";
    if (strcasecmp(dot, ".flac") == 0 || strcasecmp(dot, ".fla") == 0) return "audio/flac";
    if (strcasecmp(dot, ".wav") == 0) return "audio/wav";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    if (strcasecmp(dot, ".txt") == 0) return "text/plain; charset=gbk";
    if (strcasecmp(dot, ".md") == 0) return "text/markdown; charset=utf-8";
    if (strcasecmp(dot, ".json") == 0) return "application/json";
    if (strcasecmp(dot, ".pdf") == 0) return "application/pdf";
    if (strcasecmp(dot, ".epub") == 0) return "application/epub+zip";
    if (strcasecmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    return "application/octet-stream";
}

static void http_date(time_t t, char *out, size_t out_size)
{
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    strftime(out, out_size, "%a, %d %b %Y %H:%M:%S GMT", &tm_utc);
}

static void iso_date(time_t t, char *out, size_t out_size)
{
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

/* ------------------------------ DAV plumbing ---------------------------- */
static void dav_prop_entry(buf_t *b, const char *href, const char *name, const struct stat *st, bool is_dir)
{
    char esc_name[512];
    char esc_href[1024];
    xml_escape(name, esc_name, sizeof(esc_name));
    href_encode(href, esc_href, sizeof(esc_href));

    char modified[64];
    char created[64];
    http_date(st->st_mtime, modified, sizeof(modified));
    iso_date(st->st_mtime, created, sizeof(created));

    buf_addf(b,
             "<D:response><D:href>%s</D:href><D:propstat><D:prop>"
             "<D:displayname>%s</D:displayname><D:resourcetype>%s</D:resourcetype>",
             esc_href, esc_name, is_dir ? "<D:collection/>" : "");
    if (!is_dir) {
        buf_addf(b, "<D:getcontentlength>%u</D:getcontentlength>", (unsigned)st->st_size);
        buf_addf(b, "<D:getcontenttype>%s</D:getcontenttype>", content_type_for(name));
    }
    buf_addf(b,
             "<D:getlastmodified>%s</D:getlastmodified><D:creationdate>%s</D:creationdate>"
             "<D:supportedlock><D:lockentry><D:lockscope><D:exclusive/></D:lockscope>"
             "<D:locktype><D:write/></D:locktype></D:lockentry></D:supportedlock>"
             "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>",
             modified, created);
}

static esp_err_t send_xml(httpd_req_t *req, buf_t *b, const char *status)
{
    if (b->oom) {
        buf_free(b);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/xml; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, b->p, (ssize_t)b->len);
    buf_free(b);
    return err;
}

/* --------------------------------- handlers ------------------------------ */
/* WebDAV is opt-in: mounting the card as a network drive hands out read/write
 * access to every file on it to anyone on the LAN, while a plain browser only
 * needs the REST API. So config->webdav defaults to false, the handlers stay
 * registered (the URI table is fixed at httpd_start) and every WebDAV request
 * is refused while the switch is off - flipping the switch therefore takes
 * effect immediately, with no reboot and no re-registration. */
static bool dav_enabled(void)
{
    return pp_config()->webdav;
}

static esp_err_t dav_deny(httpd_req_t *req)
{
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "WebDAV is disabled", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_options(httpd_req_t *req)
{
    if (!dav_enabled()) {
        return dav_deny(req);
    }
    httpd_resp_set_hdr(req, "DAV", "1, 2");
    httpd_resp_set_hdr(req, "MS-Author-Via", "DAV");
    httpd_resp_set_hdr(req, "Allow",
                       "OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, PROPPATCH, MKCOL, MOVE, COPY, LOCK, UNLOCK");
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t h_propfind(httpd_req_t *req)
{
    if (!dav_enabled()) {
        return dav_deny(req);
    }
    char uri_path[PATHBUF];
    decode_uri(req, uri_path, sizeof(uri_path));
    char fs_path[PATHBUF];
    uri_to_fs(uri_path, fs_path, sizeof(fs_path));

    struct stat st;
    if (stat(fs_path, &st) != 0) {
        ESP_LOGW(TAG, "PROPFIND %s -> 404", fs_path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }

    char depth_hdr[8] = "1";
    httpd_req_get_hdr_value_str(req, "Depth", depth_hdr, sizeof(depth_hdr));
    bool want_children = (strcmp(depth_hdr, "0") != 0) && S_ISDIR(st.st_mode);

    buf_t b;
    buf_init(&b, 2048);
    buf_addf(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">");

    const char *base = strrchr(uri_path, '/');
    char self_name[256];
    if (base == NULL || base[1] == '\0') {
        snprintf(self_name, sizeof(self_name), "sdcard");
    } else {
        snprintf(self_name, sizeof(self_name), "%s", base + 1);
    }
    dav_prop_entry(&b, uri_path, self_name, &st, S_ISDIR(st.st_mode));

    if (want_children) {
        DIR *dir = opendir(fs_path);
        if (dir != NULL) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                    continue;
                }
                char child_fs[PATHBUF];
                snprintf(child_fs, sizeof(child_fs), "%s/%s", fs_path, ent->d_name);
                struct stat child_st;
                if (stat(child_fs, &child_st) != 0) {
                    continue;
                }
                char child_utf8[256];
                gbk_to_utf8(ent->d_name, strlen(ent->d_name), child_utf8, sizeof(child_utf8));
                char child_href[PATHBUF];
                if (strcmp(uri_path, "/") == 0) {
                    snprintf(child_href, sizeof(child_href), "/%s", child_utf8);
                } else {
                    snprintf(child_href, sizeof(child_href), "%s/%s", uri_path, child_utf8);
                }
                bool child_dir = S_ISDIR(child_st.st_mode);
                if (child_dir) {
                    size_t n = strlen(child_href);
                    if (n + 1 < sizeof(child_href)) {
                        child_href[n] = '/';
                        child_href[n + 1] = '\0';
                    }
                }
                dav_prop_entry(&b, child_href, child_utf8, &child_st, child_dir);
            }
            closedir(dir);
        }
    }

    buf_addf(&b, "</D:multistatus>");
    return send_xml(req, &b, "207 Multi-Status");
}

static esp_err_t send_file(httpd_req_t *req, const char *uri_path, const char *fs_path, bool head_only)
{
    struct stat st;
    if (stat(fs_path, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    if (S_ISDIR(st.st_mode)) {
        return page_handler(req, uri_path);
    }
    FILE *f = fopen(fs_path, "rb");
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot open");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, content_type_for(uri_path));
    if (head_only) {
        char len_hdr[24];
        snprintf(len_hdr, sizeof(len_hdr), "%u", (unsigned)st.st_size);
        httpd_resp_set_hdr(req, "Content-Length", len_hdr);
        fclose(f);
        return httpd_resp_send(req, NULL, 0);
    }
    static char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_get(httpd_req_t *req)
{
    if (!dav_enabled()) {
        return dav_deny(req);
    }
    char uri_path[PATHBUF];
    decode_uri(req, uri_path, sizeof(uri_path));
    char fs_path[PATHBUF];
    uri_to_fs(uri_path, fs_path, sizeof(fs_path));
    return send_file(req, uri_path, fs_path, false);
}

static esp_err_t h_head(httpd_req_t *req)
{
    if (!dav_enabled()) {
        return dav_deny(req);
    }
    char uri_path[PATHBUF];
    decode_uri(req, uri_path, sizeof(uri_path));
    char fs_path[PATHBUF];
    uri_to_fs(uri_path, fs_path, sizeof(fs_path));
    return send_file(req, uri_path, fs_path, true);
}

static esp_err_t h_put(httpd_req_t *req)
{
    if (!dav_enabled()) {
        return dav_deny(req);
    }
    char uri_path[PATHBUF];
    decode_uri(req, uri_path, sizeof(uri_path));
    char fs_path[PATHBUF];
    uri_to_fs(uri_path, fs_path, sizeof(fs_path));

    bool existed = access(fs_path, F_OK) == 0;
    FILE *f = fopen(fs_path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "PUT cannot create %s (errno %d)", fs_path, errno);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot create");
        return ESP_FAIL;
    }
    static char chunk[4096];
    int remaining = req->content_len;
    while (remaining > 0) {
        int want = remaining < (int)sizeof(chunk) ? remaining : (int)sizeof(chunk);
        int got = httpd_req_recv(req, chunk, want);
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            fclose(f);
            unlink(fs_path);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        if (fwrite(chunk, 1, (size_t)got, f) != (size_t)got) {
            fclose(f);
            unlink(fs_path);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
            return ESP_FAIL;
        }
        remaining -= got;
    }
    fclose(f);
    ESP_LOGI(TAG, "PUT %s (%d bytes)", fs_path, req->content_len);
    httpd_resp_set_status(req, existed ? "204 No Content" : "201 Created");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t remove_recursive(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_FAIL;
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path) == 0 ? ESP_OK : ESP_FAIL;
    }
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return ESP_FAIL;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char child[PATHBUF];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        remove_recursive(child);
    }
    closedir(dir);
    return rmdir(path) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t h_delete(httpd_req_t *req)
{
    if (!dav_enabled()) {
        return dav_deny(req);
    }
    char uri_path[PATHBUF];
    decode_uri(req, uri_path, sizeof(uri_path));
    if (strcmp(uri_path, "/") == 0) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "cannot delete root");
        return ESP_FAIL;
    }
    char fs_path[PATHBUF];
    uri_to_fs(uri_path, fs_path, sizeof(fs_path));
    if (remove_recursive(fs_path) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "delete failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "DELETE %s", fs_path);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t h_mkcol(httpd_req_t *req)
{
    if (!dav_enabled()) {
        return dav_deny(req);
    }
    char uri_path[PATHBUF];
    decode_uri(req, uri_path, sizeof(uri_path));
    char fs_path[PATHBUF];
    uri_to_fs(uri_path, fs_path, sizeof(fs_path));

    struct stat st;
    if (stat(fs_path, &st) == 0) {
        httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "already exists");
        return ESP_FAIL;
    }
    if (mkdir(fs_path, 0775) != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mkdir failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "MKCOL %s", fs_path);
    httpd_resp_set_status(req, "201 Created");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t h_move(httpd_req_t *req)
{
    if (!dav_enabled()) {
        return dav_deny(req);
    }
    char dest_hdr[PATHBUF];
    if (httpd_req_get_hdr_value_str(req, "Destination", dest_hdr, sizeof(dest_hdr)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no Destination header");
        return ESP_FAIL;
    }
    char dest_path[PATHBUF];
    url_path_only(dest_hdr, dest_path, sizeof(dest_path));
    url_decode(dest_path);

    char from_uri[PATHBUF];
    decode_uri(req, from_uri, sizeof(from_uri));

    char from_fs[PATHBUF];
    char to_fs[PATHBUF];
    uri_to_fs(from_uri, from_fs, sizeof(from_fs));
    uri_to_fs(dest_path, to_fs, sizeof(to_fs));

    struct stat st;
    bool existed = stat(to_fs, &st) == 0;
    if (existed) {
        remove_recursive(to_fs);
    }
    if (rename(from_fs, to_fs) != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "MOVE %s -> %s", from_fs, to_fs);
    httpd_resp_set_status(req, existed ? "204 No Content" : "201 Created");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t h_proppatch(httpd_req_t *req)
{
    if (!dav_enabled()) {
        return dav_deny(req);
    }
    char uri_path[PATHBUF];
    decode_uri(req, uri_path, sizeof(uri_path));
    buf_t b;
    buf_init(&b, 512);
    char esc_href[1024];
    href_encode(uri_path, esc_href, sizeof(esc_href));
    buf_addf(&b,
             "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">"
             "<D:response><D:href>%s</D:href><D:propstat><D:prop/>"
             "<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response></D:multistatus>",
             esc_href);
    return send_xml(req, &b, "207 Multi-Status");
}

static esp_err_t h_lock(httpd_req_t *req)
{
    if (!dav_enabled()) {
        return dav_deny(req);
    }
    buf_t b;
    buf_init(&b, 512);
    buf_addf(&b,
             "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:prop xmlns:D=\"DAV:\">"
             "<D:lockdiscovery><D:activelock><D:locktype><D:write/></D:locktype>"
             "<D:lockscope><D:exclusive/></D:lockscope><D:depth>infinity</D:depth>"
             "<D:timeout>Second-3600</D:timeout>"
             "<D:locktoken><D:href>opaquelocktoken:pocketplan</D:href></D:locktoken>"
             "</D:activelock></D:lockdiscovery></D:prop>");
    httpd_resp_set_hdr(req, "Lock-Token", "<opaquelocktoken:pocketplan>");
    return send_xml(req, &b, "200 OK");
}

/* --------------------------------- HTML page ----------------------------- */
static const char *PAGE_HEAD =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>PocketPlan</title><style>"
    "body{font-family:-apple-system,'Segoe UI',Roboto,sans-serif;margin:0;background:#f1f3f4;color:#202124}"
    "header{background:#1a73e8;color:#fff;padding:16px 20px}"
    "h1{margin:0;font-size:20px}header p{margin:4px 0 0;opacity:.85;font-size:13px}"
    "main{padding:16px 20px}.card{background:#fff;border-radius:12px;padding:14px 16px;margin-bottom:16px;"
    "box-shadow:0 1px 3px rgba(0,0,0,.08)}table{width:100%;border-collapse:collapse}"
    "td{padding:9px 4px;border-bottom:1px solid #eee;font-size:14px}td.s{text-align:right;color:#80868b}"
    "a{color:#1a73e8;text-decoration:none}input{width:100%;padding:9px;margin:5px 0;border:1px solid #dadce0;"
    "border-radius:8px;box-sizing:border-box}button{background:#1a73e8;color:#fff;border:0;padding:10px 18px;"
    "border-radius:8px;font-size:14px}small{color:#80868b}"
    "</style></head><body><header><h1>PocketPlan</h1><p>";

static esp_err_t page_handler(httpd_req_t *req, const char *uri_path)
{
    char fs_path[PATHBUF];
    uri_to_fs(uri_path, fs_path, sizeof(fs_path));

    char sta_ip[16];
    pp_net_sta_ip(sta_ip, sizeof(sta_ip));
    char ap_ip[16];
    pp_net_ap_ip(ap_ip, sizeof(ap_ip));
    bool connected = pp_net_sta_connected();

    buf_t b;
    buf_init(&b, 4096);
    buf_addf(&b, "%s", PAGE_HEAD);
    if (connected) {
        buf_addf(&b, "Wi-Fi connected: %s &middot; WebDAV: http://%s/</p></header><main>", sta_ip, sta_ip);
    } else {
        buf_addf(&b, "Hotspot: %s &middot; http://%s/</p></header><main>", pp_net_ap_ssid(), ap_ip);
    }

    const char *saved = pp_net_saved_ssid();
    buf_addf(&b,
             "<div class=\"card\"><b>Wi-Fi</b><br><small>%s%s</small>"
             "<form method=\"POST\" action=\"/wifi\">"
             "<input name=\"ssid\" placeholder=\"Wi-Fi name\" value=\"%s\">"
             "<input name=\"pass\" placeholder=\"Password\" type=\"password\">"
             "<button type=\"submit\">Save and connect</button></form></div>",
             connected ? "Connected to: " : "Currently saved: ",
             saved[0] ? saved : "(none)", saved);

    buf_addf(&b, "<div class=\"card\"><b>%s</b><table>", uri_path);

    DIR *dir = opendir(fs_path);
    if (dir != NULL) {
        for (int pass = 0; pass < 2; pass++) {
            rewinddir(dir);
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                    continue;
                }
                char child[PATHBUF];
                snprintf(child, sizeof(child), "%s/%s", fs_path, ent->d_name);
                struct stat st;
                if (stat(child, &st) != 0) {
                    continue;
                }
                bool is_dir = S_ISDIR(st.st_mode);
                if ((pass == 0) != is_dir) {
                    continue;
                }
                char utf8[256];
                gbk_to_utf8(ent->d_name, strlen(ent->d_name), utf8, sizeof(utf8));
                char esc[512];
                xml_escape(utf8, esc, sizeof(esc));
                char href[768];
                href_encode(utf8, href, sizeof(href));
                if (is_dir) {
                    buf_addf(&b, "<tr><td><a href=\"%s/\">%s/</a></td><td class=\"s\">dir</td></tr>",
                             href, esc);
                } else {
                    buf_addf(&b, "<tr><td><a href=\"%s\">%s</a></td><td class=\"s\">%.1f MB</td></tr>",
                             href, esc, st.st_size / 1024.0 / 1024.0);
                }
            }
        }
        closedir(dir);
    }
    buf_addf(&b,
             "</table></div><p><small>Map <code>http://%s/</code> as a network drive to upload and "
             "download files.</small></p></main></body></html>",
             connected ? sta_ip : ap_ip);

    if (b.oom) {
        buf_free(&b);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, b.p, (ssize_t)b.len);
    buf_free(&b);
    return err;
}

static esp_err_t h_wifi_post(httpd_req_t *req)
{
    char body[512];
    int total = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) {
            break;
        }
        got += r;
    }
    body[got] = '\0';

    char ssid[64] = {0};
    char pass[96] = {0};
    char *saveptr = NULL;
    for (char *kv = strtok_r(body, "&", &saveptr); kv != NULL; kv = strtok_r(NULL, "&", &saveptr)) {
        char *eq = strchr(kv, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        char *value = eq + 1;
        for (char *p = value; *p; p++) {
            if (*p == '+') {
                *p = ' ';
            }
        }
        url_decode(value);
        if (strcmp(kv, "ssid") == 0) {
            snprintf(ssid, sizeof(ssid), "%s", value);
        } else if (strcmp(kv, "pass") == 0) {
            snprintf(pass, sizeof(pass), "%s", value);
        }
    }

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    pp_net_set_credentials(ssid, pass);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req,
                           "<!DOCTYPE html><meta charset=\"utf-8\"><body style=\"font-family:sans-serif;"
                           "padding:24px\"><h2>Saved</h2><p>Connecting to the network... "
                           "<a href=\"/\">back</a></p></body>",
                           HTTPD_RESP_USE_STRLEN);
}

/* ---------------------------------- start -------------------------------- */
esp_err_t pp_webdav_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    /* 23 REST handlers + 12 WebDAV methods; the default 32 is too tight and
     * silently drops the WebDAV HEAD/GET wildcards. */
    cfg.max_uri_handlers = 48;
    cfg.lru_purge_enable = true;
    /* The TLS handshake for /api/ai runs on this task, 8 KB is not enough. */
    cfg.stack_size = 16384;
    cfg.recv_wait_timeout = 20;
    cfg.send_wait_timeout = 20;
    cfg.keep_alive_enable = true;
    cfg.max_open_sockets = 4;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* The web app and the REST API must be registered first: the WebDAV
     * wildcard GET handler below would otherwise answer "/" with a directory
     * listing. */
    pp_webapi_register(server);

    static const httpd_uri_t handlers[] = {
        {.uri = "/wifi", .method = HTTP_POST, .handler = h_wifi_post},
        {.uri = "/*", .method = HTTP_OPTIONS, .handler = h_options},
        {.uri = "/*", .method = HTTP_PROPFIND, .handler = h_propfind},
        {.uri = "/*", .method = HTTP_PROPPATCH, .handler = h_proppatch},
        {.uri = "/*", .method = HTTP_MKCOL, .handler = h_mkcol},
        {.uri = "/*", .method = HTTP_MOVE, .handler = h_move},
        {.uri = "/*", .method = HTTP_DELETE, .handler = h_delete},
        {.uri = "/*", .method = HTTP_PUT, .handler = h_put},
        {.uri = "/*", .method = HTTP_LOCK, .handler = h_lock},
        {.uri = "/*", .method = HTTP_UNLOCK, .handler = h_options},
        {.uri = "/*", .method = HTTP_HEAD, .handler = h_head},
        {.uri = "/*", .method = HTTP_GET, .handler = h_get},
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t e = httpd_register_uri_handler(server, &handlers[i]);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "register %s failed: %s", handlers[i].uri, esp_err_to_name(e));
        }
    }
    ESP_LOGI(TAG, "WebDAV listening on port 80 (%s)", pp_config()->webdav ? "enabled" : "disabled");
    return ESP_OK;
}
