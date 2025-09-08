#include "modules/drop_module/drop_module.h"
#include "modules/auth_module/auth_module.h"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <civetweb.h>

/* ============================ ENV HELPERS ============================ */

static const char *env_or(const char *key, const char *defv) {
    const char *v = getenv(key);
    return (v && *v) ? v : defv;
}

/* ====================== PATH + SAVE HELPERS ========================= */

// ~ expansion (simple: only leading "~/" is handled)
static char *expand_home(const char *path) {
    if (!path) return NULL;
    if (path[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) home = "";
        size_t len = strlen(home) + strlen(path);
        char *out = (char*)malloc(len);
        snprintf(out, len, "%s%s", home, path + 1);
        return out;
    }
    return strdup(path);
}

static int ensure_dir_exists(const char *dir) {
    struct stat st;
    if (stat(dir, &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(dir, 0755) == 0;
}

// --- unique enumerated filename like Finder/AirDrop: "name.ext", "name (1).ext", ...
static char *unique_enumerated_path(const char *dir, const char *basename) {
    const char *dot = strrchr(basename, '.');
    size_t stem_len = dot ? (size_t)(dot - basename) : strlen(basename);
    const char *ext = dot ? dot : "";

    size_t cap = strlen(dir) + 1 + stem_len + 1 + 10 + strlen(ext) + 1;
    char *path = (char *)malloc(cap);
    if (!path) return NULL;

    struct stat st;
    int i = 0;
    for (;;) {
        if (i == 0) {
            snprintf(path, cap, "%s/%.*s%s", dir, (int)stem_len, basename, ext);
        } else {
            snprintf(path, cap, "%s/%.*s (%d)%s", dir, (int)stem_len, basename, i, ext);
        }
        if (stat(path, &st) != 0) break;  // doesn't exist -> good
        i++;
    }
    return path;
}

static char *save_text_file(const char *dir_in, const char *basename, const char *text) {
    char *dir = expand_home(dir_in);
    if (!dir) return NULL;
    if (!ensure_dir_exists(dir)) { free(dir); return NULL; }

    char *path = unique_enumerated_path(dir, basename);
    if (!path) { free(dir); return NULL; }

    FILE *f = fopen(path, "w");
    if (!f) { free(dir); free(path); return NULL; }

    fwrite(text ? text : "", 1, text ? strlen(text) : 0, f);
    fclose(f);

    free(dir);
    return path; // caller frees
}

/* ======================= URL DETECTION HELPERS ====================== */

static int is_http_url(const char *s) {
    if (!s) return 0;
    if (strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0) {
        const char *p = strstr(s, "://");
        if (!p) return 0;
        p += 3;
        return strchr(p, '.') != NULL;
    }
    return 0;
}

static int open_url_macos(const char *url) {
    if (!url || !*url) return 0;
    pid_t pid = fork();
    if (pid == 0) {
        execl("/usr/bin/open", "open", url, (char*)NULL);
        _exit(127);
    }
    return (pid > 0) ? 1 : 0;
}

// Extract URL from common wrappers:

// 1) .txt → first non-empty line, trimmed
static char *extract_url_from_txt(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        // strip newline
        line[strcspn(line, "\r\n")] = 0;
        // trim
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0') continue;
        if (is_http_url(p)) {
            fclose(f);
            return strdup(p);
        }
    }
    fclose(f);
    return NULL;
}

// 2) .url (INI-like):
// [InternetShortcut]\nURL=https://...
static char *extract_url_from_urlini(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        if (strncasecmp(line, "URL=", 4) == 0) {
            char *p = line + 4;
            p[strcspn(p, "\r\n")] = 0;
            if (is_http_url(p)) { fclose(f); return strdup(p); }
        }
    }
    fclose(f);
    return NULL;
}

// 3) .webloc (XML plist; look for http(s) inside <string>...</string>)
static char *extract_url_from_webloc(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf)-1, f);
    buf[n] = 0;
    fclose(f);
    char *start = strcasestr(buf, "<string>");
    if (start) {
        start += 8;
        char *end = strcasestr(start, "</string>");
        if (end) {
            *end = 0;
            if (is_http_url(start)) return strdup(start);
        }
    }
    // fallback: first http(s)://
    char *p = strcasestr(buf, "http://");
    if (!p) p = strcasestr(buf, "https://");
    if (p) {
        // read until whitespace or < or "
        size_t i = 0;
        char url[1024];
        while (p[i] && !isspace((unsigned char)p[i]) && p[i] != '<' && p[i] != '"' && i < sizeof(url)-1) {
            url[i] = p[i]; i++;
        }
        url[i] = 0;
        if (is_http_url(url)) return strdup(url);
    }
    return NULL;
}

// 4) .html/.htm  → look for meta refresh or single <a href="...">
static char *extract_url_from_html(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *buf = NULL;
    size_t cap = 0, len = 0;
    char tmp[4096];
    while (!feof(f)) {
        size_t n = fread(tmp, 1, sizeof(tmp), f);
        if (!n) break;
        if (len + n + 1 > cap) {
            cap = (cap ? cap*2 : 8192);
            if (cap < len + n + 1) cap = len + n + 1;
            buf = (char*)realloc(buf, cap);
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    fclose(f);
    if (!buf) return NULL;
    buf[len] = 0;

    // meta refresh
    char *m = strcasestr(buf, "http-equiv=\"refresh\"");
    if (!m) m = strcasestr(buf, "http-equiv='refresh'");
    if (m) {
        char *u = strcasestr(m, "url=");
        if (u) {
            u += 4;
            // collect until quote or >
            char url[1024]; size_t i=0;
            while (*u && *u!='\"' && *u!='\'' && *u!='>' && !isspace((unsigned char)*u) && i<sizeof(url)-1) {
                url[i++] = *u++;
            }
            url[i]=0;
            if (is_http_url(url)) { free(buf); return strdup(url); }
        }
    }

    // anchor href
    char *a = strcasestr(buf, "<a ");
    if (a) {
        char *href = strcasestr(a, "href=");
        if (href) {
            href += 5;
            if (*href=='"' || *href=='\'') {
                char q=*href++;
                char url[1024]; size_t i=0;
                while (*href && *href!=q && i<sizeof(url)-1) url[i++]=*href++;
                url[i]=0;
                if (is_http_url(url)) { free(buf); return strdup(url); }
            }
        }
    }

    free(buf);
    return NULL;
}

/* ======================== CLIPBOARD HELPER ========================== */

static int copy_to_clipboard(const char *text) {
    if (!text) return 0;
    FILE *fp = popen("pbcopy", "w");
    if (!fp) return 0;
    size_t n = fwrite(text, 1, strlen(text), fp);
    int rc = pclose(fp);
    return (n == strlen(text) && rc != -1) ? 1 : 0;
}

/* ===================== URLENCODED (text=...) PATH =================== */

static void url_decode_inplace(char *s) {
    char *w = s;
    for (char *r = s; *r; ++r) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            int hi = (r[1] <= '9' ? r[1]-'0' : (tolower(r[1])-'a'+10));
            int lo = (r[2] <= '9' ? r[2]-'0' : (tolower(r[2])-'a'+10));
            *w++ = (char)((hi<<4) | lo);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static char *form_get_value(const char *body, const char *key) {
    size_t klen = strlen(key);
    size_t needle_len = klen + 1;
    char *needle = (char*)malloc(needle_len + 1);
    memcpy(needle, key, klen);
    needle[klen] = '=';
    needle[klen+1] = '\0';

    const char *p = body;
    while (p && *p) {
        const char *hit = strstr(p, needle);
        if (!hit) break;
        if (hit == body || *(hit - 1) == '&') {
            hit += needle_len;
            const char *end = strchr(hit, '&');
            size_t vlen = end ? (size_t)(end - hit) : strlen(hit);
            char *val = (char*)malloc(vlen + 1);
            memcpy(val, hit, vlen);
            val[vlen] = '\0';
            url_decode_inplace(val);
            free(needle);
            return val;
        }
        p = hit + needle_len;
    }
    free(needle);
    return NULL;
}

/* ======================== MULTIPART HANDLING ========================

We use CivetWeb's form API to stream-upload a file to a temp path we
choose, and then post-process it.

===================================================================== */

struct drop_mp_ctx {
    char tmp_path[1024];
    char file_name[512];
    long long file_size;
    int saved_ok;
};

/* Create a unique temp file under /tmp and return its path in outbuf */
static int make_unique_tmp(char *outbuf, size_t outlen) {
    if (!outbuf || outlen < 32) return 0;
    // Template must end with XXXXXX for mkstemp
    snprintf(outbuf, outlen, "/tmp/spacedrop_upload_XXXXXX");
    int fd = mkstemp(outbuf);
    if (fd < 0) return 0;
    close(fd);
    // mkstemp created an empty file; CivetWeb will overwrite it
    return 1;
}

/* field_found: tell CivetWeb what to do with the field */
static int mp_field_found(const char *key, const char *filename,
                          char *path, size_t pathlen, void *user_data) {
    (void)key;
    struct drop_mp_ctx *ctx = (struct drop_mp_ctx *)user_data;

    if (filename && *filename) {
        // It's a file input → store it to a unique tmp path
        snprintf(ctx->file_name, sizeof(ctx->file_name), "%s", filename);
        if (!make_unique_tmp(ctx->tmp_path, sizeof(ctx->tmp_path))) {
            return MG_FORM_FIELD_STORAGE_ABORT;
        }
        snprintf(path, pathlen, "%s", ctx->tmp_path);
        return MG_FORM_FIELD_STORAGE_STORE;
    }

    // Not a file field → we don't need it right now
    return MG_FORM_FIELD_STORAGE_SKIP;
}

/* field_get: correct signature is (key, value, valuelen, user_data) */
static int mp_field_get(const char *key, const char *value,
                        size_t valuelen, void *user_data) {
    (void)key; (void)value; (void)valuelen; (void)user_data;
    // We are not consuming non-file fields in this endpoint
    return MG_FORM_FIELD_HANDLE_NEXT;
}

/* field_store: CivetWeb has finished writing the file to tmp path */
static int mp_field_store(const char *path, long long file_size, void *user_data) {
    struct drop_mp_ctx *ctx = (struct drop_mp_ctx *)user_data;
    if (path && *path) {
        ctx->file_size = file_size;
        ctx->saved_ok = 1;
        return MG_FORM_FIELD_HANDLE_NEXT;
    }
    return MG_FORM_FIELD_HANDLE_ABORT;
}

/* ============================= /drop ================================= */

static int handle_drop(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;

    long long caller_uid = 0;
    if (!auth_is_allowed_conn(conn, &caller_uid)) {
        const char *resp = "{\"ok\":false,\"detail\":\"Forbidden by Spacedrop auth\"}";
        mg_printf(conn,
            "HTTP/1.1 403 Forbidden\r\n"
            "Content-Type: application/json\r\n"
            "Connection: close\r\n"
            "Content-Length: %zu\r\n\r\n%s",
            strlen(resp), resp);
        return 1;
    }

    const struct mg_request_info *ri = mg_get_request_info(conn);

    if (strcmp(ri->request_method, "POST") != 0) {
        mg_printf(conn,
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Allow: POST\r\n"
            "Content-Type: application/json\r\n"
            "Connection: close\r\n\r\n"
            "{\"ok\":false,\"detail\":\"Use POST\"}");
        return 1;
    }

    const char *ct = mg_get_header(conn, "Content-Type");
    int is_urlencoded = (ct && (strncasecmp(ct, "application/x-www-form-urlencoded", 33) == 0));
    int is_multipart  = (ct && (strncasecmp(ct, "multipart/form-data", 19) == 0));

    /* ---------- Path A: x-www-form-urlencoded 'text=' ---------- */
    if (is_urlencoded) {
        long long content_len = ri->content_length;
        if (content_len < 0 || content_len > (10LL * 1024 * 1024)) {
            mg_printf(conn,
                "HTTP/1.1 413 Payload Too Large\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                "{\"ok\":false,\"detail\":\"Body too large\"}");
            return 1;
        }
        char *body = NULL;
        if (content_len > 0) {
            body = (char*)malloc((size_t)content_len + 1);
            if (!body) {
                mg_printf(conn,
                    "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                    "{\"ok\":false,\"detail\":\"OOM\"}");
                return 1;
            }
            long long got = 0;
            while (got < content_len) {
                int n = mg_read(conn, body + got, (size_t)(content_len - got));
                if (n <= 0) break;
                got += n;
            }
            body[got] = '\0';
        }

        char *text = body ? form_get_value(body, "text") : NULL;

        if (text && is_http_url(text)) {
            int opened = open_url_macos(text);
            char resp[1024];
            int n = snprintf(resp, sizeof(resp),
                "{\"ok\":true,\"action\":\"opened_url\",\"url\":\"%s\",\"opened\":%s}",
                text, opened ? "true" : "false");
            mg_printf(conn,
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n"
                "Content-Length: %d\r\n\r\n%.*s", n, n, resp);
            free(text);
            free(body);
            return 1;
        }

        // Non-URL text -> clipboard/file/both
        const char *mode     = env_or("SPACEDROP_DROP_TEXT", "clipboard");   // clipboard|file|both
        const char *dl_dir   = env_or("SPACEDROP_DOWNLOADS", "~/Downloads");
        const char *basename = env_or("SPACEDROP_TEXT_BASENAME", "Spacedrop Text.txt");

        int did_clip = 0;
        char *saved_path = NULL;

        if (text && (strcasecmp(mode, "clipboard") == 0 || strcasecmp(mode, "both") == 0)) {
            did_clip = copy_to_clipboard(text);
        }
        if (text && (strcasecmp(mode, "file") == 0 || strcasecmp(mode, "both") == 0)) {
            saved_path = save_text_file(dl_dir, basename, text);
        }

        if (strcasecmp(mode, "both") == 0) {
            char resp[1024];
            if (saved_path) {
                int n = snprintf(resp, sizeof(resp),
                    "{\"ok\":true,\"action\":\"clipboard_and_saved\",\"clipboard\":%s,\"path\":\"%s\"}",
                    did_clip ? "true" : "false", saved_path);
                mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n"
                                "Content-Length: %d\r\n\r\n%.*s", n, n, resp);
            } else {
                const char *r = "{\"ok\":false,\"detail\":\"Could not save text file\"}";
                mg_printf(conn, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nConnection: close\r\n"
                                "Content-Length: %zu\r\n\r\n%s", strlen(r), r);
            }
            free(saved_path);
            free(text);
            free(body);
            return 1;
        } else if (strcasecmp(mode, "file") == 0) {
            if (saved_path) {
                char resp[1024];
                int n = snprintf(resp, sizeof(resp),
                    "{\"ok\":true,\"action\":\"saved_file\",\"path\":\"%s\"}", saved_path);
                mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n"
                                "Content-Length: %d\r\n\r\n%.*s", n, n, resp);
            } else {
                const char *r = "{\"ok\":false,\"detail\":\"Could not save text file\"}";
                mg_printf(conn, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nConnection: close\r\n"
                                "Content-Length: %zu\r\n\r\n%s", strlen(r), r);
            }
            free(saved_path);
            free(text);
            free(body);
            return 1;
        } else { // clipboard default
            char resp[128];
            int n = snprintf(resp, sizeof(resp),
                "{\"ok\":%s,\"action\":\"clipboard\"}", (text && copy_to_clipboard(text)) ? "true" : "false");
            mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n"
                            "Content-Length: %d\r\n\r\n%.*s", n, n, resp);
            free(text);
            free(body);
            return 1;
        }
    }

    /* ---------- Path B: multipart/form-data (file upload) ---------- */
    if (is_multipart) {
        struct drop_mp_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));

        struct mg_form_data_handler fdh;
        memset(&fdh, 0, sizeof(fdh));
        fdh.field_found = mp_field_found;
        fdh.field_get   = mp_field_get;    // correct signature now
        fdh.field_store = mp_field_store;
        fdh.user_data   = &ctx;

        int rc = mg_handle_form_request(conn, &fdh);
        if (rc <= 0 || !ctx.saved_ok) {
            mg_printf(conn,
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Type: application/json\r\n"
                "Connection: close\r\n\r\n"
                "{\"ok\":false,\"detail\":\"Malformed multipart or no file\"}");
            return 1;
        }

        // Decide: is this a URL wrapper?
        const char *dl_dir   = env_or("SPACEDROP_DOWNLOADS", "~/Downloads");
        const char *keep_wrap= env_or("SPACEDROP_KEEP_WRAPPER", "no"); // "yes" to keep wrapper after opening
        char *downloads = expand_home(dl_dir);

        // Lowercase filename to check extension
        char name_lc[512];
        snprintf(name_lc, sizeof(name_lc), "%s", ctx.file_name);
        for (char *p=name_lc; *p; ++p) *p = (char)tolower((unsigned char)*p);

        char *detected_url = NULL;
        if (strstr(name_lc, ".txt"))    detected_url = extract_url_from_txt(ctx.tmp_path);
        else if (strstr(name_lc, ".url"))    detected_url = extract_url_from_urlini(ctx.tmp_path);
        else if (strstr(name_lc, ".webloc")) detected_url = extract_url_from_webloc(ctx.tmp_path);
        else if (strstr(name_lc, ".html") || strstr(name_lc, ".htm")) detected_url = extract_url_from_html(ctx.tmp_path);

        if (detected_url && is_http_url(detected_url)) {
            int opened = open_url_macos(detected_url);

            if (strcasecmp(keep_wrap, "yes") != 0) {
                unlink(ctx.tmp_path);
            } else {
                // keep wrapper in Downloads using enumeration
                if (downloads && ensure_dir_exists(downloads)) {
                    char *out = unique_enumerated_path(downloads, ctx.file_name[0] ? ctx.file_name : "Link.webloc");
                    if (out) {
                        rename(ctx.tmp_path, out);
                        free(out);
                    }
                }
            }

            char resp[1024];
            int n = snprintf(resp, sizeof(resp),
                "{\"ok\":true,\"action\":\"opened_url\",\"url\":\"%s\",\"opened\":%s}",
                detected_url, opened ? "true" : "false");
            mg_printf(conn,
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n"
                "Content-Length: %d\r\n\r\n%.*s", n, n, resp);

            free(detected_url);
            free(downloads);
            return 1;
        }

        // Not a URL wrapper → save the uploaded file to Downloads (enumerate)
        if (downloads && ensure_dir_exists(downloads)) {
            char *out = unique_enumerated_path(downloads, ctx.file_name[0] ? ctx.file_name : "spacedrop.bin");
            if (out) {
                if (rename(ctx.tmp_path, out) == 0) {
                    char resp[1024];
                    int n = snprintf(resp, sizeof(resp),
                        "{\"ok\":true,\"action\":\"saved_file\",\"path\":\"%s\",\"size\":%lld}",
                        out, ctx.file_size);
                    mg_printf(conn,
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n"
                        "Content-Length: %d\r\n\r\n%.*s", n, n, resp);
                    free(out);
                    free(downloads);
                    return 1;
                } else {
                    // Fallback: copy+unlink if rename across FS ever fails (unlikely here)
                    FILE *src = fopen(ctx.tmp_path, "rb");
                    FILE *dst = fopen(out, "wb");
                    if (src && dst) {
                        char buf[8192];
                        size_t m;
                        while ((m = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, m, dst);
                        fclose(src); fclose(dst);
                        unlink(ctx.tmp_path);
                        char resp[1024];
                        int n = snprintf(resp, sizeof(resp),
                            "{\"ok\":true,\"action\":\"saved_file\",\"path\":\"%s\",\"size\":%lld}",
                            out, ctx.file_size);
                        mg_printf(conn,
                            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n"
                            "Content-Length: %d\r\n\r\n%.*s", n, n, resp);
                        free(out);
                        free(downloads);
                        return 1;
                    }
                    if (src) fclose(src);
                    if (dst) fclose(dst);
                    free(out);
                }
            }
        }

        // If we got here, saving failed
        unlink(ctx.tmp_path);
        free(downloads);
        mg_printf(conn,
            "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
            "{\"ok\":false,\"detail\":\"Failed to save uploaded file\"}");
        return 1;
    }

    /* ---------- Unsupported content-type ---------- */
    {
        const char *msg =
            "{\"ok\":false,\"detail\":\"Unsupported content-type. "
            "Use x-www-form-urlencoded (text=...) or multipart/form-data with a file.\"}";
        mg_printf(conn,
            "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: close\r\n"
            "Content-Length: %zu\r\n\r\n%s", strlen(msg), msg);
        return 1;
    }
}

void drop_setup_handlers(struct mg_context *ctx) {
    mg_set_request_handler(ctx, "/drop", handle_drop, NULL);
}
