#include "env_module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---- small helpers ------------------------------------------------------ */

static char *strtrim(char *s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    return s;
}

static char *strip_quotes(char *s) {
    if (!s) return s;
    size_t n = strlen(s);
    if (n >= 2) {
        char a = s[0], b = s[n - 1];
        if ((a == '"'  && b == '"') || (a == '\'' && b == '\'')) {
            s[n - 1] = '\0';
            return s + 1;
        }
    }
    return s;
}

static int set_env_kv(const char *key, const char *val, int overwrite) {
#if defined(_WIN32)
    return _putenv_s(key, val ? val : "") == 0 ? 1 : 0;
#else
    return setenv(key, val ? val : "", overwrite ? 1 : 0) == 0 ? 1 : 0;
#endif
}

/* ---- public API --------------------------------------------------------- */

int env_load_file(const char *path, int overwrite) {
    if (!path) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    int count = 0;
    char line[1024];

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;

        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = strtrim(p);
        char *val = strtrim(eq + 1);
        val = strip_quotes(val);

        if (*key == '\0') continue;

#if !defined(_WIN32)
        if (!overwrite && getenv(key) != NULL) continue;
#else
        if (!overwrite) {
            char *dummy = getenv(key);
            if (dummy) continue;
        }
#endif
        if (set_env_kv(key, val, /*overwrite*/1)) count++;
    }

    fclose(f);
    return count;
}

int env_load_default(void) {
    return env_load_file(".env", /*overwrite*/0);
}

const char *env_get(const char *key, const char *defval) {
    const char *v = key ? getenv(key) : NULL;
    return (v && *v) ? v : (defval ? defval : "");
}

/* ---- type helpers ------------------------------------------------------- */

int env_get_int(const char *key, int defval) {
    const char *v = env_get(key, NULL);
    if (!v || !*v) return defval;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (!end || *end != '\0') return defval;
    return (int)n;
}

int env_get_bool(const char *key, int defval) {
    const char *v = env_get(key, NULL);
    if (!v || !*v) return defval;
    if (!strcasecmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on"))  return 1;
    if (!strcasecmp(v, "0") || !strcasecmp(v, "false")|| !strcasecmp(v, "no")  || !strcasecmp(v, "off")) return 0;
    return defval;
}

/* Expand "~" using HOME. Caller must free the returned pointer. */
static char *expand_home(const char *path) {
    if (!path) return NULL;
    if (path[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) home = "";
        size_t len = strlen(home) + strlen(path);
        char *out = (char*)malloc(len);
        if (!out) return NULL;
        snprintf(out, len, "%s%s", home, path + 1);
        return out;
    }
    return strdup(path);
}

char *env_get_path_expanded(const char *key, const char *defval) {
    const char *raw = env_get(key, defval);
    if (!raw || !*raw) return NULL;
    return expand_home(raw);
}
