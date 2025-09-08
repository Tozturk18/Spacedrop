#include "modules/auth_module/auth_module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include <civetweb.h>

/* =============== small utils =============== */

static char *expand_home(const char *path) {
    if (!path) return NULL;
    if (path[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) home = "";
        size_t n = strlen(home) + strlen(path);
        char *out = (char *)malloc(n);
        snprintf(out, n, "%s%s", home, path + 1);
        return out;
    }
    return strdup(path);
}

static int ensure_dir(const char *dir) {
    struct stat st;
    if (stat(dir, &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(dir, 0755) == 0;
}

static char *slurp_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (n > 0) fread(buf, 1, (size_t)n, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

static int spit_file(const char *path, const char *s) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t n = fwrite(s, 1, strlen(s), f);
    fclose(f);
    return n == strlen(s);
}

/* =============== tiny JSON helpers (not general-purpose; just enough) =============== */

static char *json_find_str_value(const char *json, const char *key) {
    // returns malloc'd value without quotes on success; NULL otherwise
    if (!json || !key) return NULL;
    const char *k = strstr(json, key);
    if (!k) return NULL;
    k = strchr(k, ':');
    if (!k) return NULL;
    while (*k && (*k==':' || isspace((unsigned char)*k))) k++;
    if (*k!='\"') return NULL;
    k++;
    const char *end = strchr(k, '\"');
    if (!end) return NULL;
    size_t len = (size_t)(end - k);
    char *out = (char *)malloc(len + 1);
    memcpy(out, k, len);
    out[len] = 0;
    return out;
}

static long long json_find_ll_value(const char *json, const char *key) {
    if (!json || !key) return 0;
    const char *k = strstr(json, key);
    if (!k) return 0;
    k = strchr(k, ':');
    if (!k) return 0;
    while (*k && (*k==':' || isspace((unsigned char)*k))) k++;
    char *end = NULL;
    long long v = strtoll(k, &end, 10);
    return v;
}

static int json_contains_ll_in_array(const char *json, const char *array_key, long long needle) {
    // naive: look for array_key then scan digits inside [...]
    const char *p = strstr(json, array_key);
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    const char *q = strchr(p, ']');
    if (!q) return 0;
    while (p < q) {
        if (isdigit((unsigned char)*p)) {
            long long v = strtoll(p, (char **)&p, 10);
            if (v == needle) return 1;
        } else {
            p++;
        }
    }
    return 0;
}

/* =============== tailscale helpers =============== */

static char *run_cmd_capture(const char *cmd) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    char *buf = NULL; size_t cap = 0, len = 0;
    char tmp[1024];
    while (!feof(fp)) {
        size_t n = fread(tmp, 1, sizeof(tmp), fp);
        if (!n) break;
        if (len + n + 1 > cap) {
            cap = cap ? cap * 2 : 2048;
            if (cap < len + n + 1) cap = len + n + 1;
            buf = (char *)realloc(buf, cap);
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    if (buf) buf[len] = 0;
    pclose(fp);
    return buf;
}

static char *tailscale_self_ipv4(void) {
    char *out = run_cmd_capture("tailscale ip -4");
    if (!out) return NULL;
    // trim at first newline
    out[strcspn(out, "\r\n")] = 0;
    return out;
}

static long long tailscale_user_id_for_ip(const char *ip4) {
    if (!ip4 || !*ip4) return 0;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "tailscale whois --json %s", ip4);
    char *json = run_cmd_capture(cmd);
    if (!json) return 0;

    // Prefer UserProfile.ID, fallback to Node.User
    long long id = json_find_ll_value(json, "\"UserProfile\"") ? json_find_ll_value(json, "\"ID\"") : 0;
    if (!id) {
        id = json_find_ll_value(json, "\"User\"");
    }
    free(json);
    return id;
}

/* =============== config handling =============== */

static char *g_cfg_path = NULL;
static char *g_cfg_json = NULL;       // raw JSON text
static char  g_mode[24] = "EVERYONE"; // cached
static long long g_personal_id = 0;

static const char *CONFIG_DIR  = "~/.config/spacedrop";
static const char *CONFIG_FILE = "~/.config/spacedrop/config.json";

static int config_load_or_create(void) {
    if (!g_cfg_path) g_cfg_path = expand_home(CONFIG_FILE);

    // try load existing
    g_cfg_json = slurp_file(g_cfg_path);
    if (g_cfg_json) {
        // cache important fields
        char *mode = json_find_str_value(g_cfg_json, "\"mode\"");
        if (mode) { snprintf(g_mode, sizeof(g_mode), "%s", mode); free(mode); }
        g_personal_id = json_find_ll_value(g_cfg_json, "\"personal_user_id\"");
        return 1;
    }

    // create new config
    char *dir = expand_home(CONFIG_DIR);
    if (!ensure_dir(dir)) { free(dir); return 0; }
    free(dir);

    // discover self user id
    char *ip = tailscale_self_ipv4();
    long long uid = ip ? tailscale_user_id_for_ip(ip) : 0;

    // write default config
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\n"
        "  \"mode\": \"EVERYONE\",\n"
        "  \"personal_user_id\": %lld,\n"
        "  \"contacts_user_ids\": []\n"
        "}\n", uid);
    if (!spit_file(g_cfg_path, buf)) return 0;

    g_cfg_json = slurp_file(g_cfg_path);
    snprintf(g_mode, sizeof(g_mode), "EVERYONE");
    g_personal_id = uid;
    return g_cfg_json != NULL;
}

int auth_init(void) {
    int ok = config_load_or_create();
    if (!ok) {
        // fail open: default EVERYONE if config missing
        snprintf(g_mode, sizeof(g_mode), "EVERYONE");
        g_personal_id = 0;
    }
    return 1;
}

const char *auth_mode_str(void) {
    return g_mode;
}

/* =============== connection evaluation =============== */

static int remote_ip_to_string(struct mg_connection *conn, char *buf, size_t buflen) {
    // CivetWeb 1.16: request_info->remote_addr contains IP bytes.
    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (!ri) return 0;

    // ri->remote_addr is a string (const char *) in CivetWeb
    if (strchr(ri->remote_addr, ':')) {
        // Contains ':' → likely IPv6; skip whois.
        return 0;
    }
    snprintf(buf, buflen, "%s", ri->remote_addr);
    return 1;
}

static int is_allowed_user_id(long long uid_from_request) {
    if (strcasecmp(g_mode, "EVERYONE") == 0) return 1;
    if (strcasecmp(g_mode, "OFF") == 0) return 0;
    if (strcasecmp(g_mode, "PERSONAL") == 0) {
        return (uid_from_request && g_personal_id && uid_from_request == g_personal_id);
    }
    if (strcasecmp(g_mode, "CONTACTS_ONLY") == 0) {
        if (uid_from_request == 0) return 0;
        if (g_personal_id && uid_from_request == g_personal_id) return 1;
        // check contacts array
        return json_contains_ll_in_array(g_cfg_json, "\"contacts_user_ids\"", uid_from_request);
    }
    // unknown mode → be conservative
    return 0;
}

int auth_is_allowed_conn(struct mg_connection *conn, long long *out_user_id) {
    if (out_user_id) *out_user_id = 0;

    // EVERYONE: fast-path allow even if whois fails
    if (strcasecmp(g_mode, "EVERYONE") == 0) {
        // Try to record the caller if possible (but don't block)
        char ip[64];
        if (remote_ip_to_string(conn, ip, sizeof(ip))) {
            long long uid = tailscale_user_id_for_ip(ip);
            if (out_user_id) *out_user_id = uid;
        }
        return 1;
    }

    // For other modes, resolve caller’s Tailscale User ID
    char ip[64];
    if (!remote_ip_to_string(conn, ip, sizeof(ip))) {
        return 0;
    }
    long long uid = tailscale_user_id_for_ip(ip);
    if (out_user_id) *out_user_id = uid;
    if (uid == 0) return 0;

    return is_allowed_user_id(uid);
}
