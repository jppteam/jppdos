#include "../include/jpp_ui_core.h"
#include "../include/jpp_fileserver_core.h"
#include "../include/jpp_string_util.h"

#include <string.h>
#include <stdio.h>

#include "esp_timer.h"

static const char *JPP_UI_SCREEN_LAUNCHER         = "launcher";
static const char *JPP_UI_SCREEN_SETTINGS         = "settings";
static const char *JPP_UI_SCREEN_DIALOG           = "dialog";
static const char *JPP_UI_SCREEN_CRASH            = "app_crash";
static const char *JPP_UI_SCREEN_WEBDAV           = "webdav";
static const char *JPP_UI_SCREEN_WEBDAV_PASSCONFIG = "webdav_passconfig";
static const char *JPP_UI_SCREEN_SD_EJECTED       = "sd_ejected";

/* ---- Helpers ------------------------------------------------------------ */

static bool jpp_ui_frame_equals(const jpp_ui_frame_t *left, const jpp_ui_frame_t *right)
{
    size_t i;
    if (left == NULL || right == NULL) { return left == right; }
    for (i = 0; i < JPP_UI_FRAME_LINES; i++) {
        if (strcmp(left->lines[i], right->lines[i]) != 0) { return false; }
    }
    return true;
}

static void jpp_ui_set_line(char lines[JPP_UI_FRAME_LINES][JPP_UI_FRAME_CHARS + 1u],
                             size_t row, const char *text)
{
    if (row >= JPP_UI_FRAME_LINES) { return; }
    jpp_str_copy(lines[row], JPP_UI_FRAME_CHARS + 1u, text);
}

static void jpp_ui_menu_move(size_t *selected_index, size_t item_count, int delta)
{
    if (selected_index == NULL || item_count == 0u) { return; }
    if (delta < 0 && *selected_index > 0u) { *selected_index -= 1u; }
    else if (delta > 0 && *selected_index + 1u < item_count) { *selected_index += 1u; }
}

static bool jpp_ui_is_direction(const char *key, jpp_ui_action_t *action)
{
    if (jpp_str_eq(key, "UP"))    { *action = JPP_UI_ACTION_UP;    return true; }
    if (jpp_str_eq(key, "DOWN"))  { *action = JPP_UI_ACTION_DOWN;  return true; }
    if (jpp_str_eq(key, "LEFT"))  { *action = JPP_UI_ACTION_LEFT;  return true; }
    if (jpp_str_eq(key, "RIGHT")) { *action = JPP_UI_ACTION_RIGHT; return true; }
    return false;
}

/* ---- Launcher list geometry --------------------------------------------- */

/* The launcher list occupies frame rows 2–7 (row 0 is the status bar, row 1
   carries the header rule). */
#define JPP_UI_LAUNCHER_FIRST_ROW 2u
#define JPP_UI_LAUNCHER_ROWS      6u

/* True when apps of this source are rendered above the other group. */
static bool jpp_ui_source_is_first_group(const jpp_ui_shell_t *shell,
                                          jpp_ui_app_source_t source)
{
    bool builtin = (source == JPP_UI_APP_SOURCE_BUILTIN);
    return builtin != shell->system_apps_bottom;
}

/* Number of apps in the group rendered first, i.e. the index of the boundary
   between the two groups. */
static size_t jpp_ui_launcher_split(const jpp_ui_shell_t *shell)
{
    size_t n = 0u;
    while (n < shell->app_count &&
           jpp_ui_source_is_first_group(shell, shell->apps[n].source)) {
        n++;
    }
    return n;
}

/* Index range of the app entries currently visible in the list window. */
static void jpp_ui_launcher_window(const jpp_ui_shell_t *shell,
                                    size_t *start, size_t *end)
{
    size_t s = shell->selected_app > 2u ? shell->selected_app - 2u : 0u;
    if (shell->app_count > JPP_UI_LAUNCHER_ROWS &&
        s + JPP_UI_LAUNCHER_ROWS > shell->app_count) {
        s = shell->app_count - JPP_UI_LAUNCHER_ROWS;
    }
    *start = s;
    *end = shell->app_count < s + JPP_UI_LAUNCHER_ROWS ? shell->app_count
                                                       : s + JPP_UI_LAUNCHER_ROWS;
}

int jpp_ui_shell_launcher_divider_row(const jpp_ui_shell_t *shell)
{
    size_t split, start, end, boundary_row;

    if (shell == NULL || shell->app_count == 0u) { return -1; }
    split = jpp_ui_launcher_split(shell);
    /* Nothing to divide when either group is empty. */
    if (split == 0u || split >= shell->app_count) { return -1; }

    jpp_ui_launcher_window(shell, &start, &end);
    boundary_row = split - 1u;  /* last row of the group rendered first */
    if (boundary_row < start || boundary_row >= end) { return -1; }
    return (int)(JPP_UI_LAUNCHER_FIRST_ROW + boundary_row - start);
}

/* ---- Status bar --------------------------------------------------------- */

static void jpp_ui_build_status_bar(const jpp_ui_shell_t *shell,
                                     char buf[JPP_UI_FRAME_CHARS + 1u])
{
    /* Layout (128 px wide, 21 chars × 6 px):
     *   chars  0- 4  (col   0- 29): "HH:MM"
     *   col  36-43               : Wi-Fi icon bitmap (display layer, when connected)
     *   chars 14-17  (col  84-107): battery % text e.g. " 75%" or "100%"
     *   col 108-109              : 2 px gap (space char at pos 18)
     *   col 110-121              : 12×8 battery icon bitmaps (display layer) */
    memset(buf, ' ', JPP_UI_FRAME_CHARS);
    buf[JPP_UI_FRAME_CHARS] = '\0';
    if (shell->status_time[0] != '\0') {
        size_t len = strlen(shell->status_time);
        memcpy(buf, shell->status_time, len);
    }
    if (shell->status_battery_pct >= 0) {
        char pct[16];
        snprintf(pct, sizeof(pct), "%3d%%", shell->status_battery_pct);
        memcpy(buf + 14u, pct, 4u);
    }
}

/* ---- Public API --------------------------------------------------------- */

void jpp_ui_frame_clear(jpp_ui_frame_t *frame)
{
    if (frame == NULL) { return; }
    memset(frame, 0, sizeof(*frame));
}

jpp_ui_status_t jpp_ui_frame_from_lines(jpp_ui_frame_t *frame,
                                          const char *const *lines,
                                          size_t line_count)
{
    size_t row;
    if (frame == NULL || (lines == NULL && line_count > 0u)) {
        return JPP_UI_STATUS_INVALID_ARGUMENT;
    }
    jpp_ui_frame_clear(frame);
    for (row = 0; row < JPP_UI_FRAME_LINES && row < line_count; row++) {
        const char *source = lines[row] != NULL ? lines[row] : "";
        size_t col;
        for (col = 0; col < JPP_UI_FRAME_CHARS && source[col] != '\0'; col++) {
            frame->lines[row][col] = source[col] == '\n' ? ' ' : source[col];
        }
        frame->lines[row][col] = '\0';
    }
    return JPP_UI_STATUS_OK;
}

void jpp_ui_display_init(jpp_ui_display_t *display)
{
    if (display == NULL) { return; }
    memset(display, 0, sizeof(*display));
}

jpp_ui_status_t jpp_ui_display_render_lines(jpp_ui_display_t *display,
                                              const char *const *lines,
                                              size_t line_count,
                                              jpp_ui_frame_t *rendered_frame,
                                              bool *changed)
{
    jpp_ui_frame_t frame;
    jpp_ui_status_t status;

    if (display == NULL || rendered_frame == NULL || changed == NULL) {
        return JPP_UI_STATUS_INVALID_ARGUMENT;
    }
    status = jpp_ui_frame_from_lines(&frame, lines, line_count);
    if (status != JPP_UI_STATUS_OK) { return status; }
    *changed = !display->has_last_frame || !jpp_ui_frame_equals(&display->last_frame, &frame);
    display->last_frame = frame;
    display->has_last_frame = true;
    *rendered_frame = frame;
    return JPP_UI_STATUS_OK;
}

jpp_ui_action_t jpp_ui_normalize_action(const jpp_keypad_event_t *event)
{
    jpp_ui_action_t action;
    if (event == NULL || event->kind == JPP_KEYPAD_KIND_NO_EVENT) {
        return JPP_UI_ACTION_NONE;
    }
    if (event->kind == JPP_KEYPAD_KIND_OK_SHORT || jpp_str_eq(event->mapped, "OK")) {
        return JPP_UI_ACTION_OK;
    }
    /* OK_LONG / OK_DOUBLE are deliberately not mapped here: whether a
       hold or a double-click means "Back" depends on the user's Settings >
       Controls preference and on what the foreground app has claimed, and
       neither is visible from a single keypad event. keypad_task() in
       main/app_main.c resolves them before calling this. */
    if ((event->kind == JPP_KEYPAD_KIND_PRESS || event->kind == JPP_KEYPAD_KIND_REPEAT) &&
        jpp_ui_is_direction(event->key, &action)) {
        return action;
    }
    return JPP_UI_ACTION_NONE;
}

const char *jpp_ui_action_name(jpp_ui_action_t action)
{
    switch (action) {
    case JPP_UI_ACTION_NONE:  return "NONE";
    case JPP_UI_ACTION_UP:    return "UP";
    case JPP_UI_ACTION_DOWN:  return "DOWN";
    case JPP_UI_ACTION_LEFT:  return "LEFT";
    case JPP_UI_ACTION_RIGHT: return "RIGHT";
    case JPP_UI_ACTION_OK:    return "OK";
    case JPP_UI_ACTION_BACK:  return "BACK";
    }
    return "UNKNOWN";
}

const char *jpp_ui_status_name(jpp_ui_status_t status)
{
    switch (status) {
    case JPP_UI_STATUS_OK:               return "OK";
    case JPP_UI_STATUS_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case JPP_UI_STATUS_STACK_OVERFLOW:   return "STACK_OVERFLOW";
    case JPP_UI_STATUS_CATALOG_FULL:     return "CATALOG_FULL";
    }
    return "UNKNOWN";
}

/* ---- Stack -------------------------------------------------------------- */

jpp_ui_status_t jpp_ui_stack_init(jpp_ui_stack_t *stack, const char *root_screen)
{
    if (stack == NULL || root_screen == NULL) { return JPP_UI_STATUS_INVALID_ARGUMENT; }
    memset(stack, 0, sizeof(*stack));
    stack->screens[0] = root_screen;
    stack->depth = 1u;
    return JPP_UI_STATUS_OK;
}

jpp_ui_status_t jpp_ui_stack_push(jpp_ui_stack_t *stack, const char *screen)
{
    if (stack == NULL || screen == NULL || stack->depth == 0u) {
        return JPP_UI_STATUS_INVALID_ARGUMENT;
    }
    if (stack->depth >= JPP_UI_STACK_LIMIT) { return JPP_UI_STATUS_STACK_OVERFLOW; }
    stack->screens[stack->depth] = screen;
    stack->depth += 1u;
    return JPP_UI_STATUS_OK;
}

const char *jpp_ui_stack_pop(jpp_ui_stack_t *stack)
{
    if (stack == NULL || stack->depth == 0u) { return NULL; }
    if (stack->depth == 1u) { return stack->screens[0]; }
    stack->depth -= 1u;
    return stack->screens[stack->depth];
}

const char *jpp_ui_stack_top(const jpp_ui_stack_t *stack)
{
    if (stack == NULL || stack->depth == 0u) { return NULL; }
    return stack->screens[stack->depth - 1u];
}

jpp_ui_status_t jpp_ui_stack_reset_to_root(jpp_ui_stack_t *stack)
{
    if (stack == NULL || stack->depth == 0u) { return JPP_UI_STATUS_INVALID_ARGUMENT; }
    stack->depth = 1u;
    return JPP_UI_STATUS_OK;
}

/* ---- Shell init --------------------------------------------------------- */

void jpp_ui_shell_init(jpp_ui_shell_t *shell, const char *boot_mode)
{
    if (shell == NULL) { return; }
    memset(shell, 0, sizeof(*shell));
    jpp_ui_display_init(&shell->display);
    (void)jpp_ui_stack_init(&shell->stack, JPP_UI_SCREEN_LAUNCHER);
    jpp_str_copy(shell->boot_mode, sizeof(shell->boot_mode),
                 boot_mode != NULL ? boot_mode : "normal");
    shell->status_battery_pct = -1;
    shell->dim_time_s      = 60;
    shell->poweroff_time_s = 300;
    shell->last_activity_us = esp_timer_get_time();
}

void jpp_ui_shell_set_status(jpp_ui_shell_t *shell,
                               const char *time_str,
                               int battery_pct)
{
    if (shell == NULL) { return; }
    if (time_str != NULL) {
        jpp_str_copy(shell->status_time, sizeof(shell->status_time), time_str);
    } else {
        shell->status_time[0] = '\0';
    }
    shell->status_battery_pct = battery_pct;
}

jpp_ui_status_t jpp_ui_shell_add_app(jpp_ui_shell_t *shell,
                                       const char *app_id,
                                       const char *name,
                                       jpp_ui_app_source_t source)
{
    jpp_ui_app_entry_t *entry;
    if (shell == NULL || app_id == NULL || app_id[0] == '\0') {
        return JPP_UI_STATUS_INVALID_ARGUMENT;
    }
    if (jpp_str_eq(app_id, "launcher")) { return JPP_UI_STATUS_OK; }
    if (shell->app_count >= JPP_UI_APP_LIMIT) { return JPP_UI_STATUS_CATALOG_FULL; }

    /* apps[] is kept in display order, so the entry goes at the end of its own
       group rather than the end of the list: the group rendered first is the
       builtins, or the SD apps when system_apps_bottom is set. */
    size_t insert = shell->app_count;
    if (jpp_ui_source_is_first_group(shell, source)) {
        insert = 0u;
        while (insert < shell->app_count && shell->apps[insert].source == source) {
            insert++;
        }
        for (size_t i = shell->app_count; i > insert; i--) {
            shell->apps[i] = shell->apps[i - 1u];
        }
        /* Keep the cursor on the app it was already pointing at. */
        if (shell->app_count > 0u && insert <= shell->selected_app) {
            shell->selected_app += 1u;
        }
    }

    entry = &shell->apps[insert];
    jpp_str_copy(entry->app_id, sizeof(entry->app_id), app_id);
    jpp_str_copy(entry->name, sizeof(entry->name),
                 name != NULL && name[0] != '\0' ? name : app_id);
    entry->source  = source;
    shell->app_count += 1u;
    return JPP_UI_STATUS_OK;
}

void jpp_ui_shell_set_system_apps_bottom(jpp_ui_shell_t *shell, bool bottom)
{
    jpp_ui_app_entry_t ordered[JPP_UI_APP_LIMIT];
    char selected_id[JPP_UI_TEXT_LIMIT];
    size_t n = 0u;

    if (shell == NULL || shell->system_apps_bottom == bottom) { return; }
    shell->system_apps_bottom = bottom;

    selected_id[0] = '\0';
    if (shell->selected_app < shell->app_count) {
        jpp_str_copy(selected_id, sizeof(selected_id),
                     shell->apps[shell->selected_app].app_id);
    }

    /* Stable partition: first group, then the other, each in its existing order. */
    for (size_t pass = 0u; pass < 2u; pass++) {
        for (size_t i = 0u; i < shell->app_count; i++) {
            bool first = jpp_ui_source_is_first_group(shell, shell->apps[i].source);
            if (first == (pass == 0u)) { ordered[n++] = shell->apps[i]; }
        }
    }
    memcpy(shell->apps, ordered, n * sizeof(ordered[0]));

    for (size_t i = 0u; i < shell->app_count; i++) {
        if (jpp_str_eq(shell->apps[i].app_id, selected_id)) {
            shell->selected_app = i;
            break;
        }
    }
}

void jpp_ui_shell_clear_sd_apps(jpp_ui_shell_t *shell)
{
    if (shell == NULL) { return; }
    size_t new_count = 0u;
    for (size_t i = 0u; i < shell->app_count; i++) {
        if (shell->apps[i].source == JPP_UI_APP_SOURCE_BUILTIN) {
            shell->apps[new_count++] = shell->apps[i];
        }
    }
    shell->app_count = new_count;
    if (shell->selected_app >= shell->app_count && shell->app_count > 0u) {
        shell->selected_app = shell->app_count - 1u;
    }
}

/* ---- Power management --------------------------------------------------- */

void jpp_ui_shell_note_activity(jpp_ui_shell_t *shell)
{
    if (shell == NULL) { return; }
    shell->last_activity_us = esp_timer_get_time();
    shell->power_state = JPP_UI_POWER_ACTIVE;
}

bool jpp_ui_shell_tick_power(jpp_ui_shell_t *shell)
{
    if (shell == NULL) { return false; }

    jpp_ui_power_state_t prev = shell->power_state;
    int64_t elapsed_s = (esp_timer_get_time() - shell->last_activity_us) / 1000000LL;

    if (shell->poweroff_time_s > 0 && elapsed_s >= (int64_t)shell->poweroff_time_s) {
        shell->power_state = JPP_UI_POWER_OFF;
    } else if (shell->dim_time_s > 0 && elapsed_s >= (int64_t)shell->dim_time_s) {
        shell->power_state = JPP_UI_POWER_DIM;
    }
    return shell->power_state != prev;
}

/* ---- Screen renderers --------------------------------------------------- */

static void jpp_ui_shell_launcher_lines(
    const jpp_ui_shell_t *shell,
    char lines[JPP_UI_FRAME_LINES][JPP_UI_FRAME_CHARS + 1u])
{
    size_t row, start, end;

    jpp_ui_build_status_bar(shell, lines[0]);
    /* line 1 is left blank — the display layer draws a pixel divider there */

    if (shell->app_count == 0u) {
        jpp_ui_set_line(lines, 2u, ">(no apps)");
        return;
    }
    jpp_ui_launcher_window(shell, &start, &end);
    for (row = start; row < end; row++) {
        const jpp_ui_app_entry_t *app = &shell->apps[row];
        size_t frame_row = JPP_UI_LAUNCHER_FIRST_ROW + row - start;
        char item[JPP_UI_TEXT_LIMIT + 4u];
        (void)snprintf(item, sizeof(item), "%c%s",
                       (row == shell->selected_app) ? '>' : ' ',
                       app->name);
        jpp_ui_set_line(lines, frame_row, item);
    }

    if (shell->back_at_root_flash > 0u) {
        jpp_ui_set_line(lines, 7u, "Can't go further back");
    }
}

static void jpp_ui_shell_webdav_lines(
    const jpp_ui_shell_t *shell,
    char lines[JPP_UI_FRAME_LINES][JPP_UI_FRAME_CHARS + 1u])
{
    char buf[JPP_UI_FRAME_CHARS + 1u];

    if (shell->fileserver_ip[0] == '\0') {
        jpp_ui_set_line(lines, 0u, "WebDAV server");
        jpp_ui_set_line(lines, 4u, "This app needs Wi-Fi");
        jpp_ui_set_line(lines, 5u, "connection.");
    } else if (!shell->fileserver_running) {
        jpp_ui_set_line(lines, 0u, "WebDAV server STOPPED");
        jpp_ui_set_line(lines, 3u, "Use this app to mana-");
        jpp_ui_set_line(lines, 4u, "ge files on SD card");
        jpp_ui_set_line(lines, 5u, "via the Wi-Fi network");
        jpp_ui_set_line(lines, 7u, "Press OK to start");
    } else {
        jpp_ui_set_line(lines, 0u, "WebDAV server  ACTIVE");
        snprintf(buf, sizeof(buf), "IP:   %s", shell->fileserver_ip);
        jpp_ui_set_line(lines, 2u, buf);
        jpp_ui_set_line(lines, 3u, "User: jppd");
        snprintf(buf, sizeof(buf), "Pass: %.15s", shell->fileserver_password);
        jpp_ui_set_line(lines, 4u, buf);
        jpp_ui_set_line(lines, 6u,
            shell->webdav_menu_sel == 0u ? ">Password settings"
                                         : " Password settings");
        jpp_ui_set_line(lines, 7u,
            shell->webdav_menu_sel == 1u ? ">Stop server"
                                         : " Stop server");
    }
}

static void jpp_ui_shell_webdav_passconfig_lines(
    const jpp_ui_shell_t *shell,
    char lines[JPP_UI_FRAME_LINES][JPP_UI_FRAME_CHARS + 1u])
{
    jpp_ui_set_line(lines, 0u, "Password settings");
    jpp_ui_set_line(lines, 2u,
        shell->webdav_passconfig_sel == 0u ? ">Random password"
                                           : " Random password");
    jpp_ui_set_line(lines, 3u,
        shell->webdav_passconfig_sel == 1u ? ">Static password"
                                           : " Static password");
    jpp_ui_set_line(lines, 4u,
        shell->webdav_passconfig_sel == 2u ? ">Back"
                                           : " Back");
}

static void jpp_ui_shell_dialog_lines(
    const jpp_ui_dialog_t *dialog,
    char lines[JPP_UI_FRAME_LINES][JPP_UI_FRAME_CHARS + 1u])
{
    size_t row;
    jpp_ui_set_line(lines, 0u, dialog->title);
    for (row = 0u; row < dialog->body_count && row < JPP_UI_DIALOG_BODY_LINES; row++) {
        jpp_ui_set_line(lines, 2u + row, dialog->body[row]);
    }
}

static void jpp_ui_shell_sd_ejected_lines(
    char lines[JPP_UI_FRAME_LINES][JPP_UI_FRAME_CHARS + 1u])
{
    jpp_ui_set_line(lines, 1u, "    /\\  SD REMOVED");
    jpp_ui_set_line(lines, 2u, "   /!!\\");
    jpp_ui_set_line(lines, 3u, "  /----\\");
    jpp_ui_set_line(lines, 5u, "Insert SD + OK");
    jpp_ui_set_line(lines, 6u, "to restart JPPDOS");
}

static bool jpp_ui_shell_screen_is_app(const jpp_ui_shell_t *shell, const char *screen)
{
    size_t i;
    if (shell == NULL || screen == NULL) { return false; }
    for (i = 0u; i < shell->app_count; i++) {
        if (shell->apps[i].source == JPP_UI_APP_SOURCE_BUILTIN) { continue; }
        if (jpp_str_eq(shell->apps[i].app_id, screen)) { return true; }
    }
    return false;
}

/* ---- Render ------------------------------------------------------------- */

jpp_ui_status_t jpp_ui_shell_render(jpp_ui_shell_t *shell,
                                     jpp_ui_frame_t *frame,
                                     bool *changed)
{
    char lines[JPP_UI_FRAME_LINES][JPP_UI_FRAME_CHARS + 1u];
    const char *line_ptrs[JPP_UI_FRAME_LINES];
    const char *screen;
    size_t row;

    if (shell == NULL || frame == NULL || changed == NULL) {
        return JPP_UI_STATUS_INVALID_ARGUMENT;
    }
    memset(lines, 0, sizeof(lines));

    screen = jpp_ui_stack_top(&shell->stack);

    if (jpp_str_eq(screen, JPP_UI_SCREEN_SD_EJECTED)) {
        jpp_ui_shell_sd_ejected_lines(lines);
    } else if (jpp_str_eq(screen, JPP_UI_SCREEN_SETTINGS)) {
        /* Rendered by jpp_settings_screen — blank frame here, handled externally */
    } else if (jpp_str_eq(screen, JPP_UI_SCREEN_WEBDAV)) {
        jpp_ui_shell_webdav_lines(shell, lines);
    } else if (jpp_str_eq(screen, JPP_UI_SCREEN_WEBDAV_PASSCONFIG)) {
        jpp_ui_shell_webdav_passconfig_lines(shell, lines);
    } else if (jpp_str_eq(screen, JPP_UI_SCREEN_DIALOG) ||
               jpp_str_eq(screen, JPP_UI_SCREEN_CRASH)) {
        jpp_ui_shell_dialog_lines(&shell->dialog, lines);
    } else if (jpp_ui_shell_screen_is_app(shell, screen)) {
        /* Blank — app SDK frame rendered directly by display layer */
    } else {
        jpp_ui_shell_launcher_lines(shell, lines);
        if (shell->back_at_root_flash > 0u) {
            shell->back_at_root_flash--;
            if (shell->back_at_root_flash == 0u) {
                shell->display.has_last_frame = false;
            }
        }
    }

    for (row = 0u; row < JPP_UI_FRAME_LINES; row++) { line_ptrs[row] = lines[row]; }
    return jpp_ui_display_render_lines(&shell->display, line_ptrs, JPP_UI_FRAME_LINES,
                                        frame, changed);
}

/* ---- Dialog ------------------------------------------------------------- */

jpp_ui_status_t jpp_ui_shell_show_dialog(jpp_ui_shell_t *shell,
                                           const char *title,
                                           const char *const *body_lines,
                                           size_t body_count)
{
    size_t row;
    if (shell == NULL || title == NULL || (body_lines == NULL && body_count > 0u)) {
        return JPP_UI_STATUS_INVALID_ARGUMENT;
    }
    memset(&shell->dialog, 0, sizeof(shell->dialog));
    jpp_str_copy(shell->dialog.title, sizeof(shell->dialog.title), title);
    shell->dialog.body_count = body_count < JPP_UI_DIALOG_BODY_LINES ? body_count
                                                                       : JPP_UI_DIALOG_BODY_LINES;
    for (row = 0u; row < shell->dialog.body_count; row++) {
        jpp_str_copy(shell->dialog.body[row], sizeof(shell->dialog.body[row]), body_lines[row]);
    }
    return jpp_ui_stack_push(&shell->stack, JPP_UI_SCREEN_DIALOG);
}

/* ---- Action handler ----------------------------------------------------- */

static jpp_ui_status_t jpp_ui_shell_open_selected(jpp_ui_shell_t *shell)
{
    const jpp_ui_app_entry_t *entry;

    if (shell->app_count == 0u) { return JPP_UI_STATUS_OK; }
    entry = &shell->apps[shell->selected_app];
    jpp_str_copy(shell->last_opened_app_id, sizeof(shell->last_opened_app_id), entry->app_id);

    if (jpp_str_eq(entry->app_id, "settings")) {
        return jpp_ui_stack_push(&shell->stack, JPP_UI_SCREEN_SETTINGS);
    }
    if (jpp_str_eq(entry->app_id, "webdav")) {
        return jpp_ui_stack_push(&shell->stack, JPP_UI_SCREEN_WEBDAV);
    }
    if (entry->source == JPP_UI_APP_SOURCE_SD) {
        return jpp_ui_stack_push(&shell->stack, entry->app_id);
    }
    return JPP_UI_STATUS_OK;
}

jpp_ui_status_t jpp_ui_shell_handle_action(jpp_ui_shell_t *shell, jpp_ui_action_t action)
{
    const char *screen;

    if (shell == NULL) { return JPP_UI_STATUS_INVALID_ARGUMENT; }
    if (action == JPP_UI_ACTION_NONE) { return JPP_UI_STATUS_OK; }

    screen = jpp_ui_stack_top(&shell->stack);

    /* SD ejection fatal screen: OK to restart (caller handles reboot) */
    if (jpp_str_eq(screen, JPP_UI_SCREEN_SD_EJECTED)) {
        return JPP_UI_STATUS_OK;
    }

    if (jpp_str_eq(screen, JPP_UI_SCREEN_DIALOG) ||
        jpp_str_eq(screen, JPP_UI_SCREEN_CRASH)) {
        if (action == JPP_UI_ACTION_OK || action == JPP_UI_ACTION_BACK) {
            (void)jpp_ui_stack_pop(&shell->stack);
        }
        return JPP_UI_STATUS_OK;
    }

    if (jpp_str_eq(screen, JPP_UI_SCREEN_SETTINGS)) {
        /* Delegated to jpp_settings_screen in app_main */
        return JPP_UI_STATUS_OK;
    }

    if (jpp_str_eq(screen, JPP_UI_SCREEN_WEBDAV)) {
        if (action == JPP_UI_ACTION_BACK) {
            /* The server is a foreground activity: it runs out of the shared
               app pool, so leaving the screen has to hand that memory back
               rather than leave a task serving in the background. */
            if (shell->fileserver_running &&
                jpp_fileserver_stop() == JPP_FILESERVER_RESULT_OK) {
                shell->fileserver_running     = false;
                shell->fileserver_password[0] = '\0';
            }
            /* A failed stop leaves the flag alone: the main loop's 2 s status
               poll re-reports the truth rather than showing a stopped server
               that is in fact still holding the pool. */
            (void)jpp_ui_stack_pop(&shell->stack);
        } else if (shell->fileserver_running) {
            if (action == JPP_UI_ACTION_UP) {
                jpp_ui_menu_move(&shell->webdav_menu_sel, 2u, -1);
            } else if (action == JPP_UI_ACTION_DOWN) {
                jpp_ui_menu_move(&shell->webdav_menu_sel, 2u, 1);
            } else if (action == JPP_UI_ACTION_OK) {
                if (shell->webdav_menu_sel == 0u) {
                    (void)jpp_ui_stack_push(&shell->stack, JPP_UI_SCREEN_WEBDAV_PASSCONFIG);
                } else {
                    jpp_fileserver_stop();
                }
            }
        } else if (action == JPP_UI_ACTION_OK && shell->fileserver_ip[0] != '\0') {
            if (shell->webdav_pass_is_static && shell->webdav_static_pass[0] != '\0') {
                jpp_fileserver_start_with_password(shell->webdav_static_pass);
            } else {
                jpp_fileserver_start();
            }
        }
        return JPP_UI_STATUS_OK;
    }

    if (jpp_str_eq(screen, JPP_UI_SCREEN_WEBDAV_PASSCONFIG)) {
        if (action == JPP_UI_ACTION_BACK) {
            (void)jpp_ui_stack_pop(&shell->stack);
        } else if (action == JPP_UI_ACTION_UP) {
            jpp_ui_menu_move(&shell->webdav_passconfig_sel, 3u, -1);
        } else if (action == JPP_UI_ACTION_DOWN) {
            jpp_ui_menu_move(&shell->webdav_passconfig_sel, 3u, 1);
        } else if (action == JPP_UI_ACTION_OK) {
            if (shell->webdav_passconfig_sel == 0u) {
                shell->webdav_pass_is_static    = false;
                shell->webdav_static_pass[0]    = '\0';
                shell->webdav_pass_config_changed = true;
                if (shell->fileserver_running) {
                    jpp_fileserver_stop();
                    jpp_fileserver_start();
                }
            } else if (shell->webdav_passconfig_sel == 1u) {
                shell->webdav_needs_pass_input = true;
            }
            /* sel==2 (Back) and BACK both just pop */
            (void)jpp_ui_stack_pop(&shell->stack);
        }
        return JPP_UI_STATUS_OK;
    }

    if (action == JPP_UI_ACTION_UP) {
        jpp_ui_menu_move(&shell->selected_app, shell->app_count, -1);
    } else if (action == JPP_UI_ACTION_DOWN) {
        jpp_ui_menu_move(&shell->selected_app, shell->app_count, 1);
    } else if (action == JPP_UI_ACTION_OK) {
        return jpp_ui_shell_open_selected(shell);
    } else if (action == JPP_UI_ACTION_BACK) {
        shell->back_at_root_flash = 5u; /* ~0.5s at 100 ms/tick */
        shell->display.has_last_frame = false;
    }
    return JPP_UI_STATUS_OK;
}

/* ---- Crash recording ---------------------------------------------------- */

jpp_ui_status_t jpp_ui_shell_record_crash(jpp_ui_shell_t *shell,
                                           const char *title,
                                           const char *screen,
                                           const char *error_name)
{
    const char *body[3];
    jpp_ui_status_t status;

    if (shell == NULL || title == NULL || screen == NULL || error_name == NULL) {
        return JPP_UI_STATUS_INVALID_ARGUMENT;
    }
    (void)jpp_ui_stack_reset_to_root(&shell->stack);
    jpp_str_copy(shell->crash_log_path, sizeof(shell->crash_log_path), "/data/ui_crash.log");
    (void)snprintf(shell->crash_log, sizeof(shell->crash_log),
                   "screen=%s\nerror=%s\nlog=/data/ui_crash.log\n", screen, error_name);
    body[0] = screen;
    body[1] = error_name;
    body[2] = shell->crash_log_path;
    status = jpp_ui_shell_show_dialog(shell, title, body, 3u);
    if (status == JPP_UI_STATUS_OK) {
        shell->stack.screens[shell->stack.depth - 1u] = JPP_UI_SCREEN_CRASH;
    }
    return status;
}

/* ---- Misc getters ------------------------------------------------------- */

const char *jpp_ui_shell_screen(const jpp_ui_shell_t *shell)
{
    if (shell == NULL) { return NULL; }
    return jpp_ui_stack_top(&shell->stack);
}

const char *jpp_ui_shell_selected_app_id(const jpp_ui_shell_t *shell)
{
    if (shell == NULL || shell->app_count == 0u) { return ""; }
    return shell->apps[shell->selected_app].app_id;
}

void jpp_ui_shell_set_fileserver_state(jpp_ui_shell_t *shell,
                                        bool            running,
                                        const char     *ip,
                                        uint16_t        port,
                                        const char     *password)
{
    if (shell == NULL) { return; }
    shell->fileserver_running = running;
    shell->fileserver_port    = port;
    if (ip != NULL) {
        jpp_str_copy(shell->fileserver_ip, sizeof(shell->fileserver_ip), ip);
    } else {
        shell->fileserver_ip[0] = '\0';
    }
    if (password != NULL) {
        jpp_str_copy(shell->fileserver_password,
                     sizeof(shell->fileserver_password), password);
    } else {
        shell->fileserver_password[0] = '\0';
    }
}

/* ---- Generic list-view helpers -------------------------------------------- */

size_t jpp_ui_scroll_clamp(size_t cursor, size_t total, size_t visible, size_t scroll)
{
    if (cursor < scroll) {
        scroll = cursor;
    } else if (cursor >= scroll + visible) {
        scroll = cursor - visible + 1u;
    }
    if (total <= visible) {
        scroll = 0u;
    } else if (scroll + visible > total) {
        scroll = total - visible;
    }
    return scroll;
}

size_t jpp_ui_marquee_offset(uint32_t tick, size_t text_len, size_t window, size_t pause_ticks)
{
    if (text_len <= window) {
        return 0u;
    }
    size_t max_off = text_len - window;
    size_t cycle   = 2u * pause_ticks + max_off;
    size_t phase   = (size_t)(tick % (uint32_t)cycle);
    if (phase < pause_ticks) {
        return 0u;
    }
    if (phase < pause_ticks + max_off) {
        return phase - pause_ticks;
    }
    return max_off;
}

const char *jpp_ui_consent_selector_row(bool allow)
{
    return allow ? " Deny       >Allow" : ">Deny        Allow";
}
