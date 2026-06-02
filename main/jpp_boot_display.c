#include "jpp_boot_display.h"
#include "ssd1306.h"

#include <stdio.h>

static void boot_disp_draw_step(uint8_t step_idx,
                                 const char *name,
                                 const char *status_tag)
{
    uint8_t page = (uint8_t)(BOOT_DISP_FIRST_PAGE + step_idx);
    char buf[22];
    snprintf(buf, sizeof(buf), " %-14s [%s]", name, status_tag);
    ssd1306_draw_string(page, 0, buf, false);
}

void boot_disp_show_splash(boot_disp_t *d)
{
    if (!d->oled_ok) {
        return;
    }

    ssd1306_clear();

    /*
     * Pages 0-1: large "J++Device" logo
     *
     * "J++Device" at 2× = 9 chars × 12 px = 108 px wide.
     * Centred: (128 - 108) / 2 = 10.
     */
    ssd1306_draw_string_2x(0, 10, "J++Device", false);

    /*
     * Page 2: subtitle on an inverted (white) background.
     * "JPPDOS v1.0" = 11 chars × 6 px = 66 px → centre at col 31.
     */
    ssd1306_fill_page(2, 0xFFu);
    ssd1306_draw_string(2, 31, "JPPDOS v1.0", true);

    /*
     * Page 3: separator — single pixel line at the top of the page
     * (bit-0 = topmost row).
     */
    ssd1306_fill_page(3, 0x01u);

    ssd1306_flush();
}

void boot_disp_step(boot_disp_t *d, const char *name, bool ok)
{
    if (!d->oled_ok) {
        return;
    }
    if (d->next_step >= BOOT_DISP_STEPS) {
        return;
    }
    boot_disp_draw_step(d->next_step, name, ok ? "OK" : "!!");
    d->next_step++;
    ssd1306_flush();
}
