#include "modules/pages_module/pages_module.h"

#include <stdio.h>
#include <string.h>

// ---------------- Internal handler functions ----------------

static int handle_root(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    mg_printf(conn,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n\r\n");
    mg_printf(conn, "Hello from Spacedrop (C + CivetWeb)!\n");
    return 1; // non-zero = handled
}

static int handle_health(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    const char *json = "{\"ok\":true,\"name\":\"spacedrop-c\",\"version\":1}";
    mg_printf(conn,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Content-Length: %zu\r\n\r\n%s",
        strlen(json), json);
    return 1;
}

// ---------------- Public registration API -------------------
/* --- setup_handles() --- */
/* This function sets up the request handlers for the various endpoints. 
 * Parameters:
 *   ctx - The CivetWeb context.
 * Returns: void
 */
void setup_handlers(struct mg_context *ctx) {
    mg_set_request_handler(ctx, "/",       handle_root,   NULL);
    mg_set_request_handler(ctx, "/health", handle_health, NULL);
}
