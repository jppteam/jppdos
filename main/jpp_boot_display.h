#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOOT_DISP_STEPS      4u
#define BOOT_DISP_FIRST_PAGE 4u

typedef struct {
    bool    oled_ok;
    uint8_t next_step;  /* 0 … BOOT_DISP_STEPS */
} boot_disp_t;

void boot_disp_show_splash(boot_disp_t *d);
void boot_disp_step(boot_disp_t *d, const char *name, bool ok);

#ifdef __cplusplus
}
#endif
