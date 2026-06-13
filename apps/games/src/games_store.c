#include "games_store.h"

#include <stdio.h>
#include <stdlib.h>

static jpp_sdk_context_t *s_ctx;

void games_store_init(jpp_sdk_context_t *ctx)
{
    s_ctx = ctx;
}

int games_store_get(const char *key, int fallback)
{
    char value[16];
    if (s_ctx == NULL ||
        jpp_sdk_kv_get(s_ctx, key, value, sizeof(value)) != JPP_SDK_STATUS_OK ||
        value[0] == '\0') {
        return fallback;
    }
    return atoi(value);
}

void games_store_set(const char *key, int value)
{
    char text[16];
    if (s_ctx == NULL) {
        return;
    }
    snprintf(text, sizeof(text), "%d", value);
    jpp_sdk_kv_set(s_ctx, key, text);
}
