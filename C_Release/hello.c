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
#include <civetweb.h>

// Local Imports
#include "modules/env_module/env_module.h"
#include "modules/pages_module/pages_module.h"
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

    // Resolve CivetWeb options from env with sensible defaults
    const char *port        = env_get("SPACEDROP_PORT",        "8080");
    const char *doc_root    = env_get("SPACEDROP_DOCROOT",     ".");
    const char *threads     = env_get("SPACEDROP_THREADS",     "2");
    const char *keep_alive  = env_get("SPACEDROP_KEEP_ALIVE",  "no");
    const char *access_log  = env_get("SPACEDROP_ACCESS_LOG",  "-");
    const char *error_log   = env_get("SPACEDROP_ERROR_LOG",   "-");

    // CivetWeb options
    const char *options[] = {
        "listening_ports",   port,
        "document_root",     doc_root,
        "num_threads",       threads,
        "enable_keep_alive", keep_alive,
        "access_log_file",   access_log,
        "error_log_file",    error_log,
        NULL
    };

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

    // Main loop
    printf("Spacedrop C running on http://localhost:%s  (Ctrl+C to stop)\n", port);
    while (g_running) { sleep(1); }

    // Shutdown
    mg_stop(ctx);
    return 0;
}
