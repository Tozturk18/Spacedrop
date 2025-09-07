#include "env_module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---- small helpers ------------------------------------------------------ */

static char *strtrim(char *s) {
    if (!s) return s;
    // left trim
    while (*s && isspace((unsigned char)*s)) s++;
    // right trim
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
        if ((a == '"'  && b == '"') ||
            (a == '\'' && b == '\'')) {
            s[n - 1] = '\0';
            return s + 1;
        }
    }
    return s;
}

static int set_env_kv(const char *key, const char *val, int overwrite) {
#if defined(_WIN32)
    // Windows _putenv_s copies the value
    return _putenv_s(key, val ? val : "") == 0 ? 1 : 0;
#else
    // POSIX setenv copies the value (safe to pass stack strings)
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
        // Strip newline
        line[strcspn(line, "\r\n")] = 0;

        // Skip blanks & comments
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') continue;

        // Find KEY=VALUE
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = strtrim(p);
        char *val = strtrim(eq + 1);
        val = strip_quotes(val);

        if (*key == '\0') continue;

        // Respect overwrite flag
#if !defined(_WIN32)
        if (!overwrite && getenv(key) != NULL) continue;
#else
        // _dupenv_s to check existence on Windows; skip complexity here
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
