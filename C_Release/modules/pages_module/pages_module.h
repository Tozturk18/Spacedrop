#ifndef SPACEDROP_PAGES_MODULE_H
#define SPACEDROP_PAGES_MODULE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <civetweb.h>

/**
 * Register all HTTP handlers/endpoints for the Spacedrop demo server.
 * Call this once after mg_start().
 */
void setup_handlers(struct mg_context *ctx);

#ifdef __cplusplus
}
#endif

#endif /* SPACEDROP_PAGES_MODULE_H */
