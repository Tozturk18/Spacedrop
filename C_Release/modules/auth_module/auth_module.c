#include "modules/auth_module/auth_module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <civetweb.h>
#include <arpa/inet.h>
#include <netinet/in.h>


/* env helpers for config path overrides */
#include "modules/env_module/env_module.h"

/* =========================================================================
 * Small file + string utilities
 * ========================================================================= */

static char *expand_home(const char *path) {
    if (!path) return NULL;
    if (path[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) home = "";
        size_t n = strlen(home) + strlen(path);
        char *out = (char *)malloc(n);
        if (!out) return NULL;
        snprintf(out, n, "%s%s", home, path + 1);
        return out;
    }
    return strdup(path);
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
    /* ensure parent dir exists */
    char *dup = strdup(path);
    if (!dup) return 0;
    char *slash = strrchr(dup, '/');
    if (slash) {
        *slash = '\0';
        struct stat st;
        if (stat(dup, &st) != 0) {
            (void)mkdir(dup, 0755);
        }
    }
    free(dup);

    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t n = fwrite(s, 1, strlen(s), f);
    fclose(f);
    return n == strlen(s);
}

/* =========================================================================
 * Minimal JSON helpers (targeted to our config + tailscale JSON)
 * ========================================================================= */

static char *json_find_str_value(const char *json, const char *key_with_quotes) {
    if (!json || !key_with_quotes) return NULL;
    const char *k = strstr(json, key_with_quotes);
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
    if (!out) return NULL;
    memcpy(out, k, len);
    out[len] = 0;
    return out;
}

static long long json_find_ll_value(const char *json, const char *key_with_quotes) {
    if (!json || !key_with_quotes) return 0;
    const char *k = strstr(json, key_with_quotes);
    if (!k) return 0;
    k = strchr(k, ':');
    if (!k) return 0;
    while (*k && (*k==':' || isspace((unsigned char)*k))) k++;
    char *end = NULL;
    long long v = strtoll(k, &end, 10);
    return v;
}

static int json_contains_ll_in_array(const char *json, const char *array_key_with_quotes, long long needle) {
    const char *p = strstr(json, array_key_with_quotes);
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    const char *q = strchr(p, ']');
    if (!q) return 0;
    while (p < q) {
        if (isdigit((unsigned char)*p) || (*p=='-' && isdigit((unsigned char)p[1]))) {
            long long v = strtoll(p, (char **)&p, 10);
            if (v == needle) return 1;
        } else {
            p++;
        }
    }
    return 0;
}

/* =========================================================================
 * Shell exec helpers for tailscale
 * ========================================================================= */

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
            if (!buf) { pclose(fp); return NULL; }
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
    char *nl = strpbrk(out, "\r\n");
    if (nl) *nl = 0;
    return out;
}

/* whois JSON → UserProfile.ID (fallback Node.User) */
static long long tailscale_user_id_for_ip(const char *ip4) {
    if (!ip4 || !*ip4) return 0;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "tailscale whois --json %s", ip4);
    char *json = run_cmd_capture(cmd);
    if (!json) return 0;

    long long uid = 0;
    const char *up = strstr(json, "\"UserProfile\"");
    if (up) {
        const char *idk = strstr(up, "\"ID\"");
        if (idk) {
            uid = json_find_ll_value(idk, "\"ID\"");
        }
    }
    if (!uid) uid = json_find_ll_value(json, "\"User\"");
    free(json);
    return uid;
}

/* Fallback: status --json → map ip to node → node.UserID */
static long long tailscale_user_id_from_status_by_ip(const char *ip4) {
    if (!ip4 || !*ip4) return 0;
    char *json = run_cmd_capture("tailscale status --json");
    if (!json) return 0;

    long long uid = 0;
    const char *p = json;
    while ((p = strstr(p, "\"TailscaleIPs\"")) != NULL) {
        const char *arr = strchr(p, '[');
        const char *end = arr ? strchr(arr, ']') : NULL;
        if (!arr || !end) { p += 14; continue; }

        if (memmem(arr, (size_t)(end - arr), ip4, strlen(ip4)) != NULL) {
            const char *obj = arr;
            while (obj > json && *obj != '{') obj--;
            const char *k = strstr(obj, "\"UserID\"");
            if (k && k < end) {
                k = strchr(k, ':');
                if (k) {
                    while (*k && (*k==':' || isspace((unsigned char)*k))) k++;
                    uid = strtoll(k, NULL, 10);
                }
            }
            break;
        }
        p = end + 1;
    }
    free(json);
    return uid;
}

/* =========================================================================
 * Config handling (env-aware)
 * ========================================================================= */

static char *g_cfg_path = NULL;
static char *g_cfg_json = NULL;
static char  g_mode[24] = "EVERYONE";
static long long g_personal_id = 0;

/* Load existing config or create a fresh one.
   Path precedence:
     1) SPACEDROP_CONFIG (full path)
     2) SPACEDROP_CONF_PATH (full path)
     3) SPACEDROP_CONF_DIR + "/config.json"
     4) default "~/.config/spacedrop/config.json"
*/
static int config_load_or_create(void) {
    if (!g_cfg_path) {
        char *p_full = env_get_path_expanded("SPACEDROP_CONFIG", NULL);
        if (!p_full) p_full = env_get_path_expanded("SPACEDROP_CONF_PATH", NULL);

        if (p_full) {
            g_cfg_path = p_full;
        } else {
            char *dir = env_get_path_expanded("SPACEDROP_CONF_DIR", "~/.config/spacedrop");
            if (!dir) return 0;
            size_t cap = strlen(dir) + 1 + strlen("config.json") + 1;
            g_cfg_path = (char*)malloc(cap);
            if (!g_cfg_path) { free(dir); return 0; }
            snprintf(g_cfg_path, cap, "%s/%s", dir, "config.json");
            free(dir);
        }
    }

    /* Try existing file */
    g_cfg_json = slurp_file(g_cfg_path);
    if (g_cfg_json) {
        char *mode = json_find_str_value(g_cfg_json, "\"mode\"");
        if (mode) { snprintf(g_mode, sizeof(g_mode), "%s", mode); free(mode); }
        g_personal_id = json_find_ll_value(g_cfg_json, "\"personal_user_id\"");
        return 1;
    }

    /* Need to create a new config */
    char *dir_only = strdup(g_cfg_path);
    if (!dir_only) return 0;
    char *slash = strrchr(dir_only, '/');
    if (slash) { *slash = '\0'; }
    if (*dir_only) {
        struct stat st;
        if (stat(dir_only, &st) != 0) (void)mkdir(dir_only, 0755);
    }
    free(dir_only);

    /* Discover self user id via tailscale (ip → whois → status fallback) */
    char *ip = tailscale_self_ipv4();
    long long uid = ip ? tailscale_user_id_for_ip(ip) : 0;
    if (!uid && ip) uid = tailscale_user_id_from_status_by_ip(ip);

    /* Default initial config */
    char buf[256];
    snprintf(buf, sizeof(buf),
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
        /* fail-open to EVERYONE if config load/create fails */
        snprintf(g_mode, sizeof(g_mode), "EVERYONE");
        g_personal_id = 0;
    }
    return 1;
}

const char *auth_mode_str(void) {
    return g_mode;
}

/* =========================================================================
 * Remote IP → user mapping + allow/deny
 * ========================================================================= */

#ifndef MG_SOCK_STRINGIFY_IP
#define MG_SOCK_STRINGIFY_IP 1
#endif

static int remote_ip_to_string(struct mg_connection *conn, char *buf, size_t buflen) {
    if (!conn || !buf || buflen < 8) return 0;

    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (!ri) return 0;

    /* Legacy CivetWeb: remote_addr is a char[48] already containing the IP (no port). */
    if (ri->remote_addr[0] == '\0') return 0;

    /* Copy and ensure NUL-termination */
    snprintf(buf, buflen, "%s", ri->remote_addr);

    /* Defensive: strip IPv6 scope id if ever present (unlikely in legacy field) */
    char *pct = strchr(buf, '%');
    if (pct) *pct = '\0';

    return 1;
}



static int is_loopback_ip_str(const char *ip) {
    if (!ip) return 0;
    if (!strncmp(ip, "127.", 4)) return 1; /* 127.0.0.0/8 */
    if (!strcmp(ip, "::1")) return 1;      /* IPv6 loopback */
    return 0;
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
        return json_contains_ll_in_array(g_cfg_json, "\"contacts_user_ids\"", uid_from_request);
    }
    return 0; /* unknown mode → deny */
}

int auth_is_allowed_conn(struct mg_connection *conn, long long *out_user_id) {
    if (out_user_id) *out_user_id = 0;

    /* EVERYONE: always allow; still try to resolve for logging */
    if (strcasecmp(g_mode, "EVERYONE") == 0) {
        char ip[64];
        if (remote_ip_to_string(conn, ip, sizeof(ip))) {
            long long uid = tailscale_user_id_for_ip(ip);
            if (!uid) uid = tailscale_user_id_from_status_by_ip(ip);
            if (out_user_id) *out_user_id = uid;
        }
        return 1;
    }

    /* Resolve caller */
    char ip[64] = {0};
    if (!remote_ip_to_string(conn, ip, sizeof(ip))) {
        return 0;
    }

    long long uid = 0;

    /* Localhost → treat as self (parity with Python behavior) */
    if (is_loopback_ip_str(ip)) {
        uid = g_personal_id;
    } else {
        /* Tailscale whois first, then status fallback */
        uid = tailscale_user_id_for_ip(ip);
        if (!uid) uid = tailscale_user_id_from_status_by_ip(ip);
    }

    if (out_user_id) *out_user_id = uid;
    if (!uid) return 0; /* unresolvable → deny (except EVERYONE) */

    return is_allowed_user_id(uid);
}
