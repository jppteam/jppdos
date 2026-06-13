#pragma once

/* games_store — integer get/set over the hub's SDK key-value file. */

#include "jpp_sdk_bridge.h"

void games_store_init(jpp_sdk_context_t *ctx);
int  games_store_get(const char *key, int fallback);
void games_store_set(const char *key, int value);
