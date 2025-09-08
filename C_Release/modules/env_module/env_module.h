#ifndef SPACEDROP_ENV_MODULE_H
#define SPACEDROP_ENV_MODULE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Load KEY=VALUE lines from a file into the process env.
 * overwrite=0 -> do not overwrite existing env vars. Returns #vars set, or -1 if file missing. */
int         env_load_file(const char *path, int overwrite);

/* Convenience: load from ".env" in CWD, non-overwriting. */
int         env_load_default(void);

/* Get raw env value (NULL-safe). Falls back to defval if unset/empty. */
const char *env_get(const char *key, const char *defval);

/* Parse common types from env (with defaults) */
int         env_get_int(const char *key, int defval);
int         env_get_bool(const char *key, int defval /*0/1*/);

/* Expand leading "~" in paths (returns malloc'd string; caller frees). If value unset, expands defval. */
char       *env_get_path_expanded(const char *key, const char *defval);

#ifdef __cplusplus
}
#endif

#endif /* SPACEDROP_ENV_MODULE_H */
