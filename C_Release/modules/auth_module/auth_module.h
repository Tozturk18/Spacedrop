#ifndef SPACEDROP_AUTH_MODULE_H
#define SPACEDROP_AUTH_MODULE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <civetweb.h>

/**
 * Initialize auth:
 *  - ensure config exists at ~/.config/spacedrop/config.json
 *  - if missing: create it with {mode:"EVERYONE", personal_user_id:<self>}
 *    where <self> is read from `tailscale ip -4` then `tailscale whois --json <ip>`.
 * Returns 1 on success, 0 on failure (still usable; defaults to EVERYONE).
 */
int auth_init(void);

/**
 * Return 1 if this connection is allowed per config, 0 if denied.
 * If mode is EVERYONE and whois fails, we still allow.
 * Fills out_user_id if resolvable (may be 0 if unknown).
 */
int auth_is_allowed_conn(struct mg_connection *conn, long long *out_user_id);

/**
 * Return current mode as a string ("EVERYONE", "CONTACTS_ONLY", "OFF", "PERSONAL").
 * Useful for logging or debugging endpoints.
 */
const char *auth_mode_str(void);

#ifdef __cplusplus
}
#endif

#endif /* SPACEDROP_AUTH_MODULE_H */
