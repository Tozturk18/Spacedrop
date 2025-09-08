#ifndef SPACEDROP_DROP_MODULE_H
#define SPACEDROP_DROP_MODULE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <civetweb.h>

/**
 * Register /drop POST handler.
 */
void drop_setup_handlers(struct mg_context *ctx);

#ifdef __cplusplus
}
#endif

#endif /* SPACEDROP_DROP_MODULE_H */
