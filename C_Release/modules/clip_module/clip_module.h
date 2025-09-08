#ifndef SPACEDROP_CLIP_MODULE_H
#define SPACEDROP_CLIP_MODULE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <civetweb.h>

/**
 * Register POST /clip/push handler.
 * - kind=text + text=...     (x-www-form-urlencoded)
 * - image=@file               (multipart/form-data)
 */
void clip_setup_handlers(struct mg_context *ctx);

#ifdef __cplusplus
}
#endif

#endif /* SPACEDROP_CLIP_MODULE_H */
