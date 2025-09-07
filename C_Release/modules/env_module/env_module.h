#ifndef SPACEDROP_ENV_MODULE_H
#define SPACEDROP_ENV_MODULE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Load environment variables from a .env-style file.
 * - Lines starting with '#' or blank lines are ignored.
 * - Expected format per line: KEY=VALUE
 * - Whitespace around KEY and VALUE is trimmed.
 * - Surrounding single/double quotes around VALUE are removed.
 * - By default, existing environment variables are not overwritten.
 *
 * @param path       Path to the .env file (e.g., ".env")
 * @param overwrite  0 = keep existing env var values, 1 = overwrite
 * @return           Number of variables successfully set (>=0), or -1 on error.
 */
int env_load_file(const char *path, int overwrite);

/**
 * Convenience: like env_load_file(".env", 0).
 */
int env_load_default(void);

/**
 * Convenience getter that returns env var if present, otherwise the default.
 * Never returns NULL (returns default if missing).
 */
const char *env_get(const char *key, const char *defval);

#ifdef __cplusplus
}
#endif

#endif /* SPACEDROP_ENV_MODULE_H */
