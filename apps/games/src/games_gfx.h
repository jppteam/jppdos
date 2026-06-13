#pragma once

/*
 * games_gfx — hub-side shadow framebuffer over the fullscreen 128×64 canvas.
 * The api->gfx_* table entries in games_api.h point at these functions.
 */

#include <stdbool.h>
#include <stdint.h>

#include "jpp_sdk_bridge.h"

void games_gfx_init(jpp_sdk_context_t *ctx);
void games_gfx_clear(void);
void games_gfx_px(int x, int y, bool on);
void games_gfx_rect(int x, int y, int w, int h, bool on);
void games_gfx_frame(int x, int y, int w, int h);
void games_gfx_hline(int x, int y, int w, bool on);
void games_gfx_vline(int x, int y, int h, bool on);
void games_gfx_text(int x, int y, const char *s);
int  games_gfx_text_w(const char *s);
void games_gfx_flush(void);
void games_gfx_set_rotation(bool rotated);
int  games_gfx_width(void);
int  games_gfx_height(void);
