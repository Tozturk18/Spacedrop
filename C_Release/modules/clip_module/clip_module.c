#include "modules/clip_module/clip_module.h"
#include "modules/env_module/env_module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>

#include <civetweb.h>
#include "modules/auth_module/auth_module.h"

/* ---------------- ENV helpers ---------------- */

static const char *env_or(const char *key, const char *defv) {
    const char *v = getenv(key);
    return (v && *v) ? v : defv;
}

/* ---------------- tiny utils ---------------- */

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
    if (!needle) return NULL;
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
            if (!val) { free(needle); return NULL; }
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

/* ---------------- clipboard ops ---------------- */

static int copy_text_to_clipboard(const char *text) {
    if (!text) return 0;
    FILE *fp = popen("pbcopy", "w");
    if (!fp) return 0;
    size_t n = fwrite(text, 1, strlen(text), fp);
    int rc = pclose(fp);
    return (n == strlen(text) && rc != -1) ? 1 : 0;
}

/* AppleScript pasteboard image set:
   We write an image to a temp file and ask AppleScript to set the clipboard
   to the file’s image data (supports PNG/JPEG heuristically). */
static int set_clipboard_image_from_path(const char *img_path) {
    if (!img_path) return 0;

    // Choose a Uniform Type Identifier (UTI) hint by extension
    const char *uti = "public.png";
    const char *dot = strrchr(img_path, '.');
    if (dot) {
        if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) uti = "public.jpeg";
        else if (!strcasecmp(dot, ".png")) uti = "public.png";
        else if (!strcasecmp(dot, ".gif")) uti = "com.compuserve.gif";
        else if (!strcasecmp(dot, ".tif") || !strcasecmp(dot, ".tiff")) uti = "public.tiff";
        else if (!strcasecmp(dot, ".bmp")) uti = "com.microsoft.bmp";
        else if (!strcasecmp(dot, ".heic") || !strcasecmp(dot, ".heif")) uti = "public.heic";
    }

    // AppleScript to read file as data and set the clipboard
    char script[2048];
    // Escape quotes/backslashes in POSIX path
    char path_escaped[1024];
    size_t j = 0;
    for (const char *p = img_path; *p && j < sizeof(path_escaped)-1; ++p) {
        if (*p == '\\' || *p == '\"') path_escaped[j++] = '\\';
        path_escaped[j++] = *p;
    }
    path_escaped[j] = 0;

    snprintf(script, sizeof(script),
        "set f to POSIX file \"%s\"\n"
        "set theData to (read f as «class PNGf»)\n"
        "set the clipboard to theData\n"
        "return \"ok\"",
        path_escaped);

    // If not PNG, try generic read; for JPEG we can use class JPEG
    if (strcmp(uti, "public.png") != 0) {
        const char *cls = "PNGf";
        if (!strcmp(uti, "public.jpeg")) cls = "JPEG";
        else if (!strcmp(uti, "com.compuserve.gif")) cls = "GIFf";
        else if (!strcmp(uti, "public.tiff")) cls = "TIFF";
        else if (!strcmp(uti, "com.microsoft.bmp")) cls = "BMPf";
        // fallback generic read if unsure
        snprintf(script, sizeof(script),
            "set f to POSIX file \"%s\"\n"
            "set theData to (read f)\n"
            "set the clipboard to theData\n"
            "return \"ok\"",
            path_escaped);
        (void)cls; // (kept for future precise four-letter classes)
    }

    char cmd[64] = "osascript -e ";
    // call osascript with the script via -e (quote carefully)
    // safer path: write to a temp file, then run osascript with that file
    char tmpfile[] = "/tmp/spacedrop_setpb_XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) return 0;
    FILE *tf = fdopen(fd, "w");
    if (!tf) { close(fd); unlink(tmpfile); return 0; }
    fputs(script, tf);
    fclose(tf);

    char osacmd[256];
    snprintf(osacmd, sizeof(osacmd), "osascript %s", tmpfile);
    int rc = system(osacmd);
    unlink(tmpfile);

    return (rc == 0);
}

/* ---------------- multipart plumbing (image upload) ---------------- */

struct clip_mp_ctx {
    char tmp_path[1024];
    long long file_size;
    int saved_ok;
};

static int make_unique_tmp(char *outbuf, size_t outlen, const char *suffix_hint) {
    if (!outbuf || outlen < 32) return 0;
    if (suffix_hint && *suffix_hint) {
        // mkstemps needs XXXXXX before suffix; we’ll build manually then rename
        char tmpl[] = "/tmp/spacedrop_img_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd < 0) return 0;
        close(fd);
        snprintf(outbuf, outlen, "%s%s", tmpl, suffix_hint);
        // move the created empty file to the suffixed name
        rename(tmpl, outbuf);
        return 1;
    } else {
        snprintf(outbuf, outlen, "/tmp/spacedrop_img_XXXXXX");
        int fd = mkstemp(outbuf);
        if (fd < 0) return 0;
        close(fd);
        return 1;
    }
}

static const char *ext_from_filename(const char *name) {
    if (!name) return NULL;
    const char *dot = strrchr(name, '.');
    return dot ? dot : NULL;
}

static int mp_field_found(const char *key, const char *filename,
                          char *path, size_t pathlen, void *user_data) {
    struct clip_mp_ctx *ctx = (struct clip_mp_ctx *)user_data;
    if (key && strcmp(key, "image") == 0 && filename && *filename) {
        const char *ext = ext_from_filename(filename);
        if (!make_unique_tmp(ctx->tmp_path, sizeof(ctx->tmp_path), ext)) {
            return MG_FORM_FIELD_STORAGE_ABORT;
        }
        snprintf(path, pathlen, "%s", ctx->tmp_path);
        return MG_FORM_FIELD_STORAGE_STORE;
    }
    return MG_FORM_FIELD_STORAGE_SKIP;
}

static int mp_field_get(const char *key, const char *value,
                        size_t valuelen, void *user_data) {
    (void)key; (void)value; (void)valuelen; (void)user_data;
    return MG_FORM_FIELD_HANDLE_NEXT;
}

static int mp_field_store(const char *path, long long file_size, void *user_data) {
    struct clip_mp_ctx *ctx = (struct clip_mp_ctx *)user_data;
    if (path && *path) {
        ctx->file_size = file_size;
        ctx->saved_ok = 1;
        return MG_FORM_FIELD_HANDLE_NEXT;
    }
    return MG_FORM_FIELD_HANDLE_ABORT;
}

/* ---------------- /clip/push handler ---------------- */

static int handle_clip_push(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;

    // Authorization gate (parity with /drop auth style)
    long long caller_uid = 0;
    if (!auth_is_allowed_conn(conn, &caller_uid)) {
        const char *resp = "{\"ok\":false,\"detail\":\"Forbidden by Spacedrop auth\"}";
        mg_printf(conn,
            "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\nConnection: close\r\n"
            "Content-Length: %zu\r\n\r\n%s", strlen(resp), resp);
        return 1;
    }

    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (strcmp(ri->request_method, "POST") != 0) {
        mg_printf(conn,
            "HTTP/1.1 405 Method Not Allowed\r\nAllow: POST\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
            "{\"ok\":false,\"detail\":\"Use POST\"}");
        return 1;
    }

    const char *ct = mg_get_header(conn, "Content-Type");
    int is_urlencoded = (ct && (strncasecmp(ct, "application/x-www-form-urlencoded", 33) == 0));
    int is_multipart  = (ct && (strncasecmp(ct, "multipart/form-data", 19) == 0));

    if (is_urlencoded) {
        long long content_len = ri->content_length;
        if (content_len < 0 || content_len > (10LL * 1024 * 1024)) {
            mg_printf(conn,
                "HTTP/1.1 413 Payload Too Large\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                "{\"ok\":false,\"detail\":\"Body too large\"}");
            return 1;
        }
        char *body = (char*)malloc((size_t)content_len + 1);
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

        char *kind = form_get_value(body, "kind");
        char *text = form_get_value(body, "text");
        const char *fallback_kind = env_get("SPACEDROP_CLIP_DEFAULT", "text");
        const char *k = (kind && *kind) ? kind : fallback_kind;

        if (strcasecmp(k, "text") != 0) {
            const char *r = "{\"ok\":false,\"detail\":\"Unsupported kind. Use kind=text with x-www-form-urlencoded\"}";
            mg_printf(conn, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: close\r\n"
                            "Content-Length: %zu\r\n\r\n%s", strlen(r), r);
            free(kind); free(text); free(body);
            return 1;
        }
        if (!text) {
            const char *r = "{\"ok\":false,\"detail\":\"Missing 'text' for kind=text\"}";
            mg_printf(conn, "HTTP/1.1 422 Unprocessable Entity\r\nContent-Type: application/json\r\nConnection: close\r\n"
                            "Content-Length: %zu\r\n\r\n%s", strlen(r), r);
            free(kind); free(text); free(body);
            return 1;
        }

        int ok = copy_text_to_clipboard(text);
        char resp[128];
        int n = snprintf(resp, sizeof(resp), "{\"ok\":%s,\"kind\":\"text\"}", ok ? "true" : "false");
        mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n"
                        "Content-Length: %d\r\n\r\n%.*s", n, n, resp);

        free(kind); free(text); free(body);
        return 1;
    }

    if (is_multipart) {
        // Expect an image field named "image"
        struct clip_mp_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));

        struct mg_form_data_handler fdh;
        memset(&fdh, 0, sizeof(fdh));
        fdh.field_found = mp_field_found;
        fdh.field_get   = mp_field_get;
        fdh.field_store = mp_field_store;
        fdh.user_data   = &ctx;

        int rc = mg_handle_form_request(conn, &fdh);
        if (rc <= 0 || !ctx.saved_ok) {
            mg_printf(conn,
                "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                "{\"ok\":false,\"detail\":\"Malformed multipart or no 'image' file provided\"}");
            return 1;
        }

        int ok = set_clipboard_image_from_path(ctx.tmp_path);
        unlink(ctx.tmp_path);

        char resp[128];
        int n = snprintf(resp, sizeof(resp), "{\"ok\":%s,\"kind\":\"image\"}", ok ? "true" : "false");
        mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n"
                        "Content-Length: %d\r\n\r\n%.*s", n, n, resp);
        return 1;
    }

    // Unsupported content-type
    {
        const char *msg =
            "{\"ok\":false,\"detail\":\"Unsupported content-type. "
            "Use x-www-form-urlencoded (kind=text&text=...) or multipart/form-data with field 'image'\"}";
        mg_printf(conn, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: close\r\n"
                        "Content-Length: %zu\r\n\r\n%s", strlen(msg), msg);
        return 1;
    }
}

/* Public registration */
void clip_setup_handlers(struct mg_context *ctx) {
    mg_set_request_handler(ctx, "/clip/push", handle_clip_push, NULL);
}
