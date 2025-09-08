/* =============================================================================
 * Spacedrop (C edition) — server demo using CivetWeb
 *
 * This file is the main entry point for a simple web server demo using the
 * CivetWeb library.
 *
 *
 * TL;DR for contributors:
 *   - Uses C17 standard.
 *   - Depends on CivetWeb (https://github.com/civetweb/civetweb)
 *   - Uses a simple env var module to load .env files (see modules/env_module)
 *   - Compile with e.g.:
 *     ** make submodule
 *     ** make build
 *     ** make run
 * =============================================================================
*/

/* --- Imports --- */
// System Imports
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <civetweb.h>

// Local Imports
#include "modules/env_module/env_module.h"
#include "modules/pages_module/pages_module.h"
#include "modules/drop_module/drop_module.h"
#include "modules/auth_module/auth_module.h"
#include "modules/clip_module/clip_module.h"
/* --- End of Imports --- */

// Global running flag for graceful shutdown
static volatile int g_running = 1;

/* --- handle_sigint() --- */
/* This function handles the SIGINT signal (Ctrl+C) for graceful shutdown. 
 * Parameters:
 *   sig - The signal number.
 * Returns: void
 */
static void handle_sigint(int sig) {
    (void)sig;
    printf("\nCaught SIGINT (Ctrl+C). Exiting gracefully. Please wait...\n");
    g_running = 0;
}

int main(void) {
    // Setup signal handlers
    signal(SIGPIPE, SIG_IGN);       // Ignore broken pipe signals
    signal(SIGINT, handle_sigint);  // Handle Ctrl+C for graceful shutdown

    // Load .env if present (does not overwrite existing env vars)
    int loaded = env_load_default();
    if (loaded >= 0) {
        printf("Loaded %d env var(s) from .env (non-overwriting)\n", loaded);
    } else {
        printf("No .env file found (continuing with defaults)\n");
    }

    // Init auth (creates config.json on first run)
    auth_init();
    printf("Auth mode: %s\n", auth_mode_str());

    // Resolve HTTP options
    int   thrs              = env_get_int("SPACEDROP_THREADS",      2);
    int   keep_alive        = env_get_bool("SPACEDROP_KEEP_ALIVE",  0);
    const char *port        = env_get("SPACEDROP_PORT",             "8080");
    const char *doc_root    = env_get("SPACEDROP_DOCROOT",          ".");
    const char *access_log  = env_get("SPACEDROP_ACCESS_LOG",       "-");
    const char *error_log   = env_get("SPACEDROP_ERROR_LOG",        "-");
    int debug               = env_get_bool("SPACEDROP_DEBUG",       0);

    // Convert typed values back to strings for CivetWeb (expects string options)
    char thrs_s[16]; snprintf(thrs_s, sizeof(thrs_s), "%d", thrs);
    const char *ka_s = keep_alive ? "yes" : "no";

    const char *options[] = {
        "listening_ports",   port,
        "document_root",     doc_root,
        "num_threads",       thrs_s,
        "enable_keep_alive", ka_s,
        "access_log_file",   access_log,
        "error_log_file",    error_log,
        NULL
    };


    if (debug) {
        char *conf_path = env_get_path_expanded("SPACEDROP_CONFIG", NULL);
        if (!conf_path) conf_path = env_get_path_expanded("SPACEDROP_CONF_PATH", NULL);
        if (!conf_path) conf_path = env_get_path_expanded("SPACEDROP_CONF_DIR", "~/.config/spacedrop");
        printf("[debug] Port=%s Threads=%s KeepAlive=%s DocRoot=%s\n", port, thrs_s, ka_s, doc_root);
        printf("[debug] AccessLog=%s ErrorLog=%s\n", access_log, error_log);
        if (conf_path) { printf("[debug] Config=%s\n", conf_path); free(conf_path); }
    }

    // Start CivetWeb
    struct mg_callbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));

    // Start the HTTP Web server
    struct mg_context *ctx = mg_start(&callbacks, NULL, options);
    if (!ctx) {
        fprintf(stderr, "Failed to start CivetWeb.\n");
        return 1;
    }

    // Setup request handlers
    setup_handlers(ctx);
    drop_setup_handlers(ctx);
    clip_setup_handlers(ctx);

    // Main loop
    printf("Spacedrop C running on http://localhost:%s  (Ctrl+C to stop)\n", port);
    while (g_running) { sleep(1); }

    // Shutdown
    mg_stop(ctx);
    return 0;
}
