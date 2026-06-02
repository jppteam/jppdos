#include "../include/jpp_sdk_bridge.h"
#include "../include/jpp_buzzer_core.h"
#include "../include/jpp_kbd_core.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *JPP_SDK_TAG = "jpp_sdk";

static bool jpp_sdk_text_present(const char *text)
{
    return text != NULL && text[0] != '\0';
}

static bool jpp_sdk_name_is_valid(const char *name)
{
    size_t index;
    bool previous_was_dot = false;

    if (!jpp_sdk_text_present(name) || name[0] == '.') {
        return false;
    }
    for (index = 0u; name[index] != '\0'; ++index) {
        char current = name[index];
        bool is_lower = current >= 'a' && current <= 'z';
        bool is_upper = current >= 'A' && current <= 'Z';
        bool is_digit = current >= '0' && current <= '9';
        if (current == '.') {
            if (previous_was_dot || name[index + 1u] == '\0') {
                return false;
            }
            previous_was_dot = true;
            continue;
        }
        if (is_lower || is_upper || is_digit || current == '_') {
            previous_was_dot = false;
            continue;
        }
        return false;
    }
    return true;
}

static bool jpp_sdk_relative_path_is_valid(const char *path)
{
    size_t index = 0u;

    if (!jpp_sdk_text_present(path)) {
        return false;
    }
    while (path[index] == '/') {
        index += 1u;
    }
    if (path[index] == '\0') {
        return false;
    }
    for (; path[index] != '\0'; ++index) {
        if (path[index] != '.') {
            continue;
        }
        if (path[index + 1u] != '.') {
            continue;
        }
        if ((index == 0u || path[index - 1u] == '/') &&
            (path[index + 2u] == '\0' || path[index + 2u] == '/')) {
            return false;
        }
    }
    return true;
}

static bool jpp_sdk_full_path_is_valid(const char *path)
{
    static const char sd_prefix[] = "/sd/";
    size_t prefix_len = sizeof(sd_prefix) - 1u;
    size_t index;

    if (path == NULL || path[0] == '\0') {
        return false;
    }
    if (strncmp(path, sd_prefix, prefix_len) != 0) {
        return false;
    }
    /* Must have at least one character after /sd/ */
    if (path[prefix_len] == '\0') {
        return false;
    }
    for (index = prefix_len; path[index] != '\0'; ++index) {
        if (path[index] != '.') {
            continue;
        }
        if (path[index + 1u] != '.') {
            continue;
        }
        if ((path[index - 1u] == '/') &&
            (path[index + 2u] == '\0' || path[index + 2u] == '/')) {
            return false;
        }
    }
    return true;
}

static jpp_sdk_status_t jpp_sdk_copy_text(char *dest, size_t dest_size, const char *source)
{
    size_t length;

    if (dest == NULL || dest_size == 0u || source == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    length = strlen(source);
    if (length >= dest_size) {
        memcpy(dest, source, dest_size - 1u);
        dest[dest_size - 1u] = '\0';
        return JPP_SDK_STATUS_TEXT_TRUNCATED;
    }
    memcpy(dest, source, length + 1u);
    return JPP_SDK_STATUS_OK;
}

static bool jpp_sdk_app_id_is_valid(const char *app_id)
{
    if (!jpp_sdk_name_is_valid(app_id) || strcmp(app_id, "settings") == 0) {
        return false;
    }
    return true;
}

static jpp_sdk_status_t jpp_sdk_ensure_bound(const jpp_sdk_context_t *context)
{
    if (context == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    return context->bound ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_INVALID_STATE;
}

/*
 * Builds /sd/apps/<app_id>/<relative_path> into out_path.
 * Strips any leading slashes from relative_path.
 */
static jpp_sdk_status_t jpp_sdk_build_scoped_path(
    const jpp_sdk_context_t *context,
    const char *relative_path,
    char *out_path,
    size_t out_path_size
)
{
    static const char prefix[] = "/sd/apps/";
    size_t prefix_length = sizeof(prefix) - 1u;
    size_t app_id_length;
    size_t relative_length;

    if (context == NULL || out_path == NULL || out_path_size == 0u ||
        !jpp_sdk_relative_path_is_valid(relative_path)) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    while (relative_path[0] == '/') {
        relative_path += 1;
    }
    app_id_length = strlen(context->app_id);
    relative_length = strlen(relative_path);
    if (prefix_length + app_id_length + 1u + relative_length + 1u > out_path_size) {
        return JPP_SDK_STATUS_TEXT_TRUNCATED;
    }
    memcpy(out_path, prefix, prefix_length);
    memcpy(out_path + prefix_length, context->app_id, app_id_length);
    out_path[prefix_length + app_id_length] = '/';
    memcpy(out_path + prefix_length + app_id_length + 1u, relative_path, relative_length + 1u);
    return JPP_SDK_STATUS_OK;
}

/*
 * Builds /sd/shared/<app_id>/<relative_path> into out_path.
 */
static jpp_sdk_status_t jpp_sdk_build_shared_path(
    const jpp_sdk_context_t *context,
    const char *relative_path,
    char *out_path,
    size_t out_path_size
)
{
    static const char prefix[] = "/sd/shared/";
    size_t prefix_length = sizeof(prefix) - 1u;
    size_t app_id_length;
    size_t relative_length;

    if (context == NULL || out_path == NULL || out_path_size == 0u ||
        !jpp_sdk_relative_path_is_valid(relative_path)) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    while (relative_path[0] == '/') {
        relative_path += 1;
    }
    app_id_length = strlen(context->app_id);
    relative_length = strlen(relative_path);
    if (prefix_length + app_id_length + 1u + relative_length + 1u > out_path_size) {
        return JPP_SDK_STATUS_TEXT_TRUNCATED;
    }
    memcpy(out_path, prefix, prefix_length);
    memcpy(out_path + prefix_length, context->app_id, app_id_length);
    out_path[prefix_length + app_id_length] = '/';
    memcpy(out_path + prefix_length + app_id_length + 1u, relative_path, relative_length + 1u);
    return JPP_SDK_STATUS_OK;
}

static jpp_sdk_status_t jpp_sdk_ensure_cap(jpp_sdk_context_t *context,
                                            const char *cap);

/* ---- broker operation wrappers ---- */

typedef struct {
    jpp_sdk_file_reader_t reader;
    void *reader_context;
    const char *logical_path;
} jpp_sdk_file_read_context_t;

typedef struct {
    jpp_sdk_file_writer_t writer;
    void *writer_context;
    const char *logical_path;
    const char *text;
} jpp_sdk_file_write_context_t;

typedef struct {
    jpp_sdk_file_lister_t lister;
    void *lister_context;
    const char *logical_path;
} jpp_sdk_file_list_context_t;

typedef struct {
    jpp_vm_context_t *vm_context;
    const char *task_name;
    const char *app_id;
    const char *name;
} jpp_sdk_background_register_context_t;

static jpp_broker_status_t jpp_sdk_file_read_operation(void *context, jpp_broker_result_t *result)
{
    jpp_sdk_file_read_context_t *ctx = (jpp_sdk_file_read_context_t *)context;
    return ctx->reader(ctx->reader_context, ctx->logical_path, result);
}

static jpp_broker_status_t jpp_sdk_file_write_operation(void *context, jpp_broker_result_t *result)
{
    jpp_sdk_file_write_context_t *ctx = (jpp_sdk_file_write_context_t *)context;
    return ctx->writer(ctx->writer_context, ctx->logical_path, ctx->text, result);
}

static jpp_broker_status_t jpp_sdk_file_list_operation(void *context, jpp_broker_result_t *result)
{
    jpp_sdk_file_list_context_t *ctx = (jpp_sdk_file_list_context_t *)context;
    return ctx->lister(ctx->lister_context, ctx->logical_path, result);
}

static jpp_broker_status_t jpp_sdk_background_register_operation(
    void *context,
    jpp_broker_result_t *result
)
{
    jpp_sdk_background_register_context_t *register_context;
    jpp_vm_request_t request;
    jpp_vm_status_t vm_status;

    if (context == NULL || result == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }
    register_context = (jpp_sdk_background_register_context_t *)context;
    memset(&request, 0, sizeof(request));
    request.kind = JPP_VM_REQUEST_BACKGROUND;
    (void)jpp_sdk_copy_text(request.app_id, sizeof(request.app_id), register_context->app_id);
    (void)jpp_sdk_copy_text(request.module_name, sizeof(request.module_name), register_context->name);
    vm_status = jpp_vm_schedule_request(register_context->vm_context, register_context->task_name, &request);
    if (vm_status != JPP_VM_STATUS_OK) {
        const char *code = vm_status == JPP_VM_STATUS_QUEUE_FULL ? "QUEUE_FULL" : "VM_ERROR";
        jpp_broker_status_t status = jpp_broker_error_result(result, code);
        if (status != JPP_BROKER_STATUS_OK) {
            return status;
        }
        status = jpp_broker_result_put(result, "task", register_context->name);
        if (status != JPP_BROKER_STATUS_OK) {
            return status;
        }
        return jpp_broker_result_put(result, "vm_status", jpp_vm_status_name(vm_status));
    }
    if (jpp_broker_ok_result(result) != JPP_BROKER_STATUS_OK) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }
    if (jpp_broker_result_put(result, "task", register_context->name) != JPP_BROKER_STATUS_OK) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }
    return jpp_broker_result_put(result, "vm_status", jpp_vm_status_name(JPP_VM_STATUS_OK));
}

/* ---- generic file helpers ---- */

static jpp_sdk_status_t jpp_sdk_do_read(
    jpp_sdk_context_t *context,
    const char *logical_path,
    const char *capability,
    jpp_broker_result_t *result
)
{
    jpp_broker_status_t broker_status;
    jpp_sdk_file_read_context_t file_context;

    if (result == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.locks == NULL || context->services.read_file == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, capability);
        if (cap_st != JPP_SDK_STATUS_OK) {
            jpp_broker_error_result(result, "ACCESS_DENIED");
            return cap_st;
        }
    }
    file_context.reader = context->services.read_file;
    file_context.reader_context = context->services.read_file_context;
    file_context.logical_path = logical_path;
    broker_status = jpp_broker_run_exclusive(
        context->services.locks, "storage",
        jpp_sdk_file_read_operation, &file_context, result
    );
    return broker_status == JPP_BROKER_STATUS_OK ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_BROKER_ERROR;
}

static jpp_sdk_status_t jpp_sdk_do_write(
    jpp_sdk_context_t *context,
    const char *logical_path,
    const char *text,
    const char *capability,
    jpp_broker_result_t *result
)
{
    jpp_broker_status_t broker_status;
    jpp_sdk_file_write_context_t file_context;

    if (result == NULL || text == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.locks == NULL || context->services.write_file == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, capability);
        if (cap_st != JPP_SDK_STATUS_OK) {
            jpp_broker_error_result(result, "ACCESS_DENIED");
            return cap_st;
        }
    }
    file_context.writer = context->services.write_file;
    file_context.writer_context = context->services.write_file_context;
    file_context.logical_path = logical_path;
    file_context.text = text;
    broker_status = jpp_broker_run_exclusive(
        context->services.locks, "storage",
        jpp_sdk_file_write_operation, &file_context, result
    );
    return broker_status == JPP_BROKER_STATUS_OK ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_BROKER_ERROR;
}

static jpp_sdk_status_t jpp_sdk_do_list(
    jpp_sdk_context_t *context,
    const char *logical_path,
    const char *capability,
    jpp_broker_result_t *result
)
{
    jpp_broker_status_t broker_status;
    jpp_sdk_file_list_context_t list_context;

    if (result == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.locks == NULL || context->services.list_files == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, capability);
        if (cap_st != JPP_SDK_STATUS_OK) {
            jpp_broker_error_result(result, "ACCESS_DENIED");
            return cap_st;
        }
    }
    list_context.lister = context->services.list_files;
    list_context.lister_context = context->services.list_files_context;
    list_context.logical_path = logical_path;
    broker_status = jpp_broker_run_exclusive(
        context->services.locks, "storage",
        jpp_sdk_file_list_operation, &list_context, result
    );
    return broker_status == JPP_BROKER_STATUS_OK ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_BROKER_ERROR;
}

/* ---- handle management ---- */

static jpp_sdk_handle_t jpp_sdk_find_free_handle(const jpp_sdk_context_t *context)
{
    uint8_t index;
    for (index = 0u; index < JPP_SDK_HANDLE_CAPACITY; ++index) {
        if (!context->handles[index].active) {
            return (jpp_sdk_handle_t)index;
        }
    }
    return (jpp_sdk_handle_t)JPP_SDK_HANDLE_INVALID;
}

static jpp_sdk_handle_entry_t *jpp_sdk_resolve_handle(
    jpp_sdk_context_t *context,
    jpp_sdk_handle_t handle
)
{
    if (handle >= JPP_SDK_HANDLE_CAPACITY) {
        return NULL;
    }
    if (!context->handles[handle].active) {
        return NULL;
    }
    return &context->handles[handle];
}

/* ---- public API ---- */

void jpp_sdk_context_init(jpp_sdk_context_t *context)
{
    if (context == NULL) {
        return;
    }
    memset(context, 0, sizeof(*context));
    context->key_queue = xQueueCreate(8u, sizeof(jpp_sdk_key_event_t));
}

jpp_sdk_status_t jpp_sdk_bind(
    jpp_sdk_context_t *context,
    const jpp_vm_context_t *vm_context,
    const char *task_name,
    const char *app_id,
    const jpp_broker_caller_t *caller,
    const jpp_sdk_native_services_t *services
)
{
    if (context == NULL || vm_context == NULL || caller == NULL || services == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (!jpp_vm_task_can_touch_python_objects(vm_context, task_name)) {
        return JPP_SDK_STATUS_ACCESS_DENIED;
    }
    if (!jpp_sdk_app_id_is_valid(app_id)) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    jpp_sdk_context_init(context);
    if (jpp_sdk_copy_text(context->app_id, sizeof(context->app_id), app_id) != JPP_SDK_STATUS_OK) {
        return JPP_SDK_STATUS_TEXT_TRUNCATED;
    }
    /* Copy the pre-granted cap pointers into the context's own backing array so
       that jpp_sdk_ensure_cap() can extend the list in-place at first use. */
    size_t n = caller->capability_count < JPP_SDK_PENDING_CAP_MAX
               ? caller->capability_count : JPP_SDK_PENDING_CAP_MAX;
    for (size_t i = 0u; i < n; i++) {
        context->granted_cap_ptrs[i] = caller->capabilities[i];
    }
    context->caller.capabilities     = (const char *const *)context->granted_cap_ptrs;
    context->caller.capability_count = n;
    context->services = *services;
    context->bound = true;
    return JPP_SDK_STATUS_OK;
}

jpp_sdk_status_t jpp_sdk_bind_native(
    jpp_sdk_context_t *context,
    const char *app_id,
    const jpp_broker_caller_t *caller,
    const jpp_sdk_native_services_t *services
)
{
    if (context == NULL || caller == NULL || services == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (!jpp_sdk_app_id_is_valid(app_id)) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    jpp_sdk_context_init(context);
    if (jpp_sdk_copy_text(context->app_id, sizeof(context->app_id), app_id) != JPP_SDK_STATUS_OK) {
        return JPP_SDK_STATUS_TEXT_TRUNCATED;
    }
    size_t n = caller->capability_count < JPP_SDK_PENDING_CAP_MAX
               ? caller->capability_count : JPP_SDK_PENDING_CAP_MAX;
    for (size_t i = 0u; i < n; i++) {
        context->granted_cap_ptrs[i] = caller->capabilities[i];
    }
    context->caller.capabilities     = (const char *const *)context->granted_cap_ptrs;
    context->caller.capability_count = n;
    context->services = *services;
    context->bound = true;
    return JPP_SDK_STATUS_OK;
}

jpp_sdk_status_t jpp_sdk_set_frame(
    jpp_sdk_context_t *context,
    const char *const *lines,
    size_t line_count
)
{
    size_t index;
    size_t stored_count;
    jpp_sdk_status_t final_status = JPP_SDK_STATUS_OK;

    if (jpp_sdk_ensure_bound(context) != JPP_SDK_STATUS_OK || (line_count > 0u && lines == NULL)) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    /* A plain frame has no signature rule; modal helpers re-enable it after. */
    context->frame_title_rule = false;
    stored_count = line_count < JPP_SDK_FRAME_LINE_CAPACITY ? line_count : JPP_SDK_FRAME_LINE_CAPACITY;
    memset(context->frame_lines, 0, sizeof(context->frame_lines));
    for (index = 0u; index < stored_count; ++index) {
        const char *line = lines[index] != NULL ? lines[index] : "";
        if (jpp_sdk_copy_text(context->frame_lines[index], JPP_SDK_FRAME_TEXT_MAX, line) ==
            JPP_SDK_STATUS_TEXT_TRUNCATED) {
            final_status = JPP_SDK_STATUS_TEXT_TRUNCATED;
        }
    }
    context->frame_line_count = stored_count;
    if (line_count > JPP_SDK_FRAME_LINE_CAPACITY) {
        final_status = JPP_SDK_STATUS_TEXT_TRUNCATED;
    }
    return final_status;
}

jpp_sdk_status_t jpp_sdk_request_close(jpp_sdk_context_t *context)
{
    jpp_sdk_status_t status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    context->close_requested = true;
    return JPP_SDK_STATUS_OK;
}

jpp_sdk_status_t jpp_sdk_log(jpp_sdk_context_t *context, const char *event_name)
{
    jpp_sdk_status_t status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (!jpp_sdk_text_present(event_name)) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.log_writer != NULL) {
        context->services.log_writer(context->services.log_context, context->app_id, event_name);
    }
    return JPP_SDK_STATUS_OK;
}

void jpp_sdk_set_pending_caps(
    jpp_sdk_context_t *context,
    const char *const *caps,
    const int *tiers,
    size_t count
)
{
    if (context == NULL || count == 0u) {
        return;
    }
    size_t n = count < JPP_SDK_PENDING_CAP_MAX ? count : JPP_SDK_PENDING_CAP_MAX;
    for (size_t i = 0u; i < n; i++) {
        strncpy(context->pending_cap_strs[i], caps[i], 31u);
        context->pending_cap_strs[i][31] = '\0';
        context->pending_cap_tiers[i] = tiers[i];
    }
    context->pending_cap_count = n;
}

/*
 * Ensure `cap` is in the broker caller's capability list. If it is already
 * there, returns OK immediately. If it is in the pending set, calls the
 * consent_prompt callback (which may block for user input). On grant the cap
 * is moved into the caller list and — for tier-1 — persistence is handled by
 * the callback. On deny, returns ACCESS_DENIED. If the cap is not in either
 * set (undeclared), returns ACCESS_DENIED without prompting.
 */
static jpp_sdk_status_t jpp_sdk_ensure_cap(jpp_sdk_context_t *context,
                                            const char *cap)
{
    if (jpp_broker_caller_has_capability(&context->caller, cap)) {
        return JPP_SDK_STATUS_OK;
    }
    for (size_t i = 0u; i < context->pending_cap_count; i++) {
        if (strcmp(context->pending_cap_strs[i], cap) != 0) {
            continue;
        }
        bool granted = false;
        if (context->services.consent_prompt != NULL) {
            granted = context->services.consent_prompt(
                context->services.consent_prompt_context,
                cap,
                context->pending_cap_tiers[i]
            );
        }
        if (granted) {
            /* Append to the broker caller list, pointing at the pending slot's
               stable backing string. The pending entry is intentionally left in
               place: the caller-list check above short-circuits all future
               lookups for this cap, so it is never re-prompted this session.
               (The previous swap-with-last compaction corrupted granted_cap_ptrs
               because it overwrote the very buffer the new caller entry pointed
               at, silently substituting a different capability.) */
            size_t n = context->caller.capability_count;
            if (n < JPP_SDK_PENDING_CAP_MAX) {
                context->granted_cap_ptrs[n] = context->pending_cap_strs[i];
                context->caller.capability_count = n + 1u;
            } else {
                ESP_LOGW(JPP_SDK_TAG, "CAP_GRANT_OVERFLOW %s %s",
                         context->app_id, cap);
                return JPP_SDK_STATUS_ACCESS_DENIED;
            }
            return JPP_SDK_STATUS_OK;
        }
        ESP_LOGW(JPP_SDK_TAG, "CAP_DENIED %s %s (user declined)",
                 context->app_id, cap);
        return JPP_SDK_STATUS_ACCESS_DENIED;
    }
    ESP_LOGW(JPP_SDK_TAG, "CAP_DENIED %s %s (not declared in manifest)",
             context->app_id, cap);
    return JPP_SDK_STATUS_ACCESS_DENIED;
}

jpp_sdk_status_t jpp_sdk_device_status(jpp_sdk_context_t *context, jpp_broker_result_t *result)
{
    jpp_broker_status_t broker_status;
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (result == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.locks == NULL || context->services.device_status == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "device.status");
        if (cap_st != JPP_SDK_STATUS_OK) {
            jpp_broker_error_result(result, "ACCESS_DENIED");
            return cap_st;
        }
    }
    broker_status = jpp_broker_run_exclusive(
        context->services.locks, "device",
        context->services.device_status, context->services.device_status_context, result
    );
    return broker_status == JPP_BROKER_STATUS_OK ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_BROKER_ERROR;
}

/* ---- files.scoped ---- */

jpp_sdk_status_t jpp_sdk_file_read(
    jpp_sdk_context_t *context,
    const char *relative_path,
    jpp_broker_result_t *result
)
{
    jpp_sdk_status_t status;
    char path[JPP_SDK_PATH_MAX];

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    status = jpp_sdk_build_scoped_path(context, relative_path, path, sizeof(path));
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    return jpp_sdk_do_read(context, path, "files.scoped", result);
}

jpp_sdk_status_t jpp_sdk_file_write(
    jpp_sdk_context_t *context,
    const char *relative_path,
    const char *text,
    jpp_broker_result_t *result
)
{
    jpp_sdk_status_t status;
    char path[JPP_SDK_PATH_MAX];

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    status = jpp_sdk_build_scoped_path(context, relative_path, path, sizeof(path));
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    return jpp_sdk_do_write(context, path, text, "files.scoped", result);
}

jpp_sdk_status_t jpp_sdk_file_list(
    jpp_sdk_context_t *context,
    const char *relative_dir,
    jpp_broker_result_t *result
)
{
    jpp_sdk_status_t status;
    char path[JPP_SDK_PATH_MAX];

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    /* An empty/NULL dir means "the app's own root". "." resolves to the scoped
       base directory and lists cleanly; without this the path validator rejected
       "" as INVALID_ARGUMENT. */
    if (relative_dir == NULL || relative_dir[0] == '\0') {
        relative_dir = ".";
    }
    status = jpp_sdk_build_scoped_path(context, relative_dir, path, sizeof(path));
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    return jpp_sdk_do_list(context, path, "files.scoped", result);
}

/* ---- files.shared ---- */

jpp_sdk_status_t jpp_sdk_shared_read(
    jpp_sdk_context_t *context,
    const char *relative_path,
    jpp_broker_result_t *result
)
{
    jpp_sdk_status_t status;
    char path[JPP_SDK_PATH_MAX];

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    status = jpp_sdk_build_shared_path(context, relative_path, path, sizeof(path));
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    return jpp_sdk_do_read(context, path, "files.shared", result);
}

jpp_sdk_status_t jpp_sdk_shared_write(
    jpp_sdk_context_t *context,
    const char *relative_path,
    const char *text,
    jpp_broker_result_t *result
)
{
    jpp_sdk_status_t status;
    char path[JPP_SDK_PATH_MAX];

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    status = jpp_sdk_build_shared_path(context, relative_path, path, sizeof(path));
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    return jpp_sdk_do_write(context, path, text, "files.shared", result);
}

jpp_sdk_status_t jpp_sdk_shared_list(
    jpp_sdk_context_t *context,
    const char *relative_dir,
    jpp_broker_result_t *result
)
{
    jpp_sdk_status_t status;
    char path[JPP_SDK_PATH_MAX];

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    /* Empty/NULL dir means "the shared root for this app" — see jpp_sdk_file_list. */
    if (relative_dir == NULL || relative_dir[0] == '\0') {
        relative_dir = ".";
    }
    status = jpp_sdk_build_shared_path(context, relative_dir, path, sizeof(path));
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    return jpp_sdk_do_list(context, path, "files.shared", result);
}

/* ---- files.full ---- */

jpp_sdk_status_t jpp_sdk_file_open(
    jpp_sdk_context_t *context,
    const char *path,
    jpp_sdk_open_mode_t mode,
    jpp_sdk_handle_t *out_handle
)
{
    jpp_sdk_status_t status;
    jpp_sdk_handle_t slot;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (out_handle == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (!jpp_sdk_full_path_is_valid(path)) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (mode != JPP_SDK_OPEN_READ && mode != JPP_SDK_OPEN_WRITE) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.path_prompt == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "files.full");
        if (cap_st != JPP_SDK_STATUS_OK) {
            return cap_st;
        }
    }
    slot = jpp_sdk_find_free_handle(context);
    if (slot == (jpp_sdk_handle_t)JPP_SDK_HANDLE_INVALID) {
        return JPP_SDK_STATUS_HANDLE_LIMIT;
    }
    /* Prompt runs outside the storage lock — it may block for user input. */
    if (!context->services.path_prompt(context->services.path_prompt_context, path, mode)) {
        return JPP_SDK_STATUS_ACCESS_DENIED;
    }
    (void)jpp_sdk_copy_text(context->handles[slot].path, JPP_SDK_PATH_MAX, path);
    context->handles[slot].mode = mode;
    context->handles[slot].active = true;
    *out_handle = slot;
    return JPP_SDK_STATUS_OK;
}

jpp_sdk_status_t jpp_sdk_handle_read(
    jpp_sdk_context_t *context,
    jpp_sdk_handle_t handle,
    jpp_broker_result_t *result
)
{
    jpp_sdk_status_t status;
    jpp_sdk_handle_entry_t *entry;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    entry = jpp_sdk_resolve_handle(context, handle);
    if (entry == NULL) {
        return JPP_SDK_STATUS_INVALID_HANDLE;
    }
    if (entry->mode != JPP_SDK_OPEN_READ) {
        return JPP_SDK_STATUS_ACCESS_DENIED;
    }
    return jpp_sdk_do_read(context, entry->path, "files.full", result);
}

jpp_sdk_status_t jpp_sdk_handle_write(
    jpp_sdk_context_t *context,
    jpp_sdk_handle_t handle,
    const char *text,
    jpp_broker_result_t *result
)
{
    jpp_sdk_status_t status;
    jpp_sdk_handle_entry_t *entry;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    entry = jpp_sdk_resolve_handle(context, handle);
    if (entry == NULL) {
        return JPP_SDK_STATUS_INVALID_HANDLE;
    }
    if (entry->mode != JPP_SDK_OPEN_WRITE) {
        return JPP_SDK_STATUS_ACCESS_DENIED;
    }
    return jpp_sdk_do_write(context, entry->path, text, "files.full", result);
}

jpp_sdk_status_t jpp_sdk_handle_list(
    jpp_sdk_context_t *context,
    jpp_sdk_handle_t handle,
    jpp_broker_result_t *result
)
{
    jpp_sdk_status_t status;
    jpp_sdk_handle_entry_t *entry;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    entry = jpp_sdk_resolve_handle(context, handle);
    if (entry == NULL) {
        return JPP_SDK_STATUS_INVALID_HANDLE;
    }
    if (entry->mode != JPP_SDK_OPEN_READ) {
        return JPP_SDK_STATUS_ACCESS_DENIED;
    }
    return jpp_sdk_do_list(context, entry->path, "files.full", result);
}

jpp_sdk_status_t jpp_sdk_handle_close(
    jpp_sdk_context_t *context,
    jpp_sdk_handle_t handle
)
{
    jpp_sdk_status_t status;
    jpp_sdk_handle_entry_t *entry;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    entry = jpp_sdk_resolve_handle(context, handle);
    if (entry == NULL) {
        return JPP_SDK_STATUS_INVALID_HANDLE;
    }
    memset(entry, 0, sizeof(*entry));
    return JPP_SDK_STATUS_OK;
}

/* ---- background tasks ---- */

jpp_sdk_status_t jpp_sdk_add_background_task(
    jpp_sdk_context_t *context,
    jpp_vm_context_t *vm_context,
    const char *task_name,
    const char *name,
    jpp_broker_result_t *result
)
{
    jpp_broker_status_t broker_status;
    jpp_sdk_background_register_context_t register_context;
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (vm_context == NULL || result == NULL || !jpp_sdk_name_is_valid(name)) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.locks == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "background.register");
        if (cap_st != JPP_SDK_STATUS_OK) {
            jpp_broker_error_result(result, "ACCESS_DENIED");
            return cap_st;
        }
    }
    if (context->background_task_count >= JPP_SDK_BACKGROUND_TASK_CAPACITY) {
        if (jpp_broker_error_result(result, "TASK_LIMIT") != JPP_BROKER_STATUS_OK) {
            return JPP_SDK_STATUS_BROKER_ERROR;
        }
        if (jpp_broker_result_put(result, "task", name) != JPP_BROKER_STATUS_OK) {
            return JPP_SDK_STATUS_BROKER_ERROR;
        }
        return JPP_SDK_STATUS_BROKER_ERROR;
    }
    register_context.vm_context = vm_context;
    register_context.task_name = task_name;
    register_context.app_id = context->app_id;
    register_context.name = name;
    broker_status = jpp_broker_run_exclusive(
        context->services.locks, "vm_queue",
        jpp_sdk_background_register_operation, &register_context, result
    );
    if (broker_status != JPP_BROKER_STATUS_OK) {
        return JPP_SDK_STATUS_BROKER_ERROR;
    }
    if (!result->ok) {
        return JPP_SDK_STATUS_BROKER_ERROR;
    }
    status = jpp_sdk_copy_text(
        context->background_tasks[context->background_task_count].name,
        sizeof(context->background_tasks[context->background_task_count].name),
        name
    );
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    context->background_tasks[context->background_task_count].strikes = 0u;
    context->background_task_count += 1u;
    return JPP_SDK_STATUS_OK;
}

/* ---- BLE connection handle management ---- */

static jpp_sdk_ble_conn_t jpp_sdk_find_free_ble_conn(const jpp_sdk_context_t *context)
{
    uint8_t index;
    for (index = 0u; index < JPP_SDK_BLE_CONN_CAPACITY; ++index) {
        if (!context->ble_conns[index].active) {
            return (jpp_sdk_ble_conn_t)index;
        }
    }
    return (jpp_sdk_ble_conn_t)JPP_SDK_BLE_CONN_INVALID;
}

static jpp_sdk_ble_conn_entry_t *jpp_sdk_resolve_ble_conn(
    jpp_sdk_context_t *context,
    jpp_sdk_ble_conn_t conn
)
{
    if (conn >= JPP_SDK_BLE_CONN_CAPACITY) {
        return NULL;
    }
    if (!context->ble_conns[conn].active) {
        return NULL;
    }
    return &context->ble_conns[conn];
}

/* ---- BLE broker operation contexts ---- */

typedef struct {
    jpp_sdk_ble_scan_fn_t scan;
    void *scan_context;
    uint32_t duration_ms;
    jpp_sdk_ble_scan_result_t *results;
    size_t max_results;
    size_t *out_count;
} jpp_sdk_ble_scan_op_t;

typedef struct {
    jpp_sdk_ble_advertise_start_fn_t start;
    void *adv_context;
    const uint8_t *payload;
    size_t payload_len;
} jpp_sdk_ble_adv_start_op_t;

typedef struct {
    jpp_sdk_ble_advertise_stop_fn_t stop;
    void *adv_context;
} jpp_sdk_ble_adv_stop_op_t;

typedef struct {
    jpp_sdk_ble_connect_fn_t connect;
    void *client_context;
    const char *address;
} jpp_sdk_ble_connect_op_t;

typedef struct {
    jpp_sdk_ble_read_char_fn_t read_char;
    void *client_context;
    const char *address;
    const char *svc_uuid;
    const char *char_uuid;
} jpp_sdk_ble_read_char_op_t;

typedef struct {
    jpp_sdk_ble_write_char_fn_t write_char;
    void *client_context;
    const char *address;
    const char *svc_uuid;
    const char *char_uuid;
    const char *text;
} jpp_sdk_ble_write_char_op_t;

typedef struct {
    jpp_sdk_ble_disconnect_fn_t disconnect;
    void *client_context;
    const char *address;
} jpp_sdk_ble_disconnect_op_t;

typedef struct {
    jpp_sdk_ble_service_register_fn_t service_register;
    void *host_context;
    const char *svc_uuid;
} jpp_sdk_ble_service_register_op_t;

typedef struct {
    jpp_sdk_ble_service_unregister_fn_t service_unregister;
    void *host_context;
} jpp_sdk_ble_service_unregister_op_t;

/* ---- BLE broker operation wrappers ---- */

static jpp_broker_status_t jpp_sdk_ble_scan_operation(void *context, jpp_broker_result_t *result)
{
    jpp_sdk_ble_scan_op_t *op = (jpp_sdk_ble_scan_op_t *)context;
    return op->scan(op->scan_context, op->duration_ms, op->results, op->max_results,
                    op->out_count, result);
}

static jpp_broker_status_t jpp_sdk_ble_adv_start_operation(
    void *context,
    jpp_broker_result_t *result
)
{
    jpp_sdk_ble_adv_start_op_t *op = (jpp_sdk_ble_adv_start_op_t *)context;
    return op->start(op->adv_context, op->payload, op->payload_len, result);
}

static jpp_broker_status_t jpp_sdk_ble_adv_stop_operation(
    void *context,
    jpp_broker_result_t *result
)
{
    jpp_sdk_ble_adv_stop_op_t *op = (jpp_sdk_ble_adv_stop_op_t *)context;
    return op->stop(op->adv_context, result);
}

static jpp_broker_status_t jpp_sdk_ble_connect_operation(
    void *context,
    jpp_broker_result_t *result
)
{
    jpp_sdk_ble_connect_op_t *op = (jpp_sdk_ble_connect_op_t *)context;
    return op->connect(op->client_context, op->address, result);
}

static jpp_broker_status_t jpp_sdk_ble_read_char_operation(
    void *context,
    jpp_broker_result_t *result
)
{
    jpp_sdk_ble_read_char_op_t *op = (jpp_sdk_ble_read_char_op_t *)context;
    return op->read_char(op->client_context, op->address, op->svc_uuid, op->char_uuid, result);
}

static jpp_broker_status_t jpp_sdk_ble_write_char_operation(
    void *context,
    jpp_broker_result_t *result
)
{
    jpp_sdk_ble_write_char_op_t *op = (jpp_sdk_ble_write_char_op_t *)context;
    return op->write_char(op->client_context, op->address, op->svc_uuid, op->char_uuid,
                          op->text, result);
}

static jpp_broker_status_t jpp_sdk_ble_disconnect_operation(
    void *context,
    jpp_broker_result_t *result
)
{
    jpp_sdk_ble_disconnect_op_t *op = (jpp_sdk_ble_disconnect_op_t *)context;
    return op->disconnect(op->client_context, op->address, result);
}

static jpp_broker_status_t jpp_sdk_ble_service_register_operation(
    void *context,
    jpp_broker_result_t *result
)
{
    jpp_sdk_ble_service_register_op_t *op = (jpp_sdk_ble_service_register_op_t *)context;
    return op->service_register(op->host_context, op->svc_uuid, result);
}

static jpp_broker_status_t jpp_sdk_ble_service_unregister_operation(
    void *context,
    jpp_broker_result_t *result
)
{
    jpp_sdk_ble_service_unregister_op_t *op = (jpp_sdk_ble_service_unregister_op_t *)context;
    return op->service_unregister(op->host_context, result);
}

/* ---- ble.scan ---- */

jpp_sdk_status_t jpp_sdk_ble_scan(
    jpp_sdk_context_t *context,
    uint32_t duration_ms,
    jpp_sdk_ble_scan_result_t *results,
    size_t max_results,
    size_t *out_count,
    jpp_broker_result_t *result
)
{
    jpp_broker_status_t broker_status;
    jpp_sdk_ble_scan_op_t op;
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (result == NULL || out_count == NULL || (max_results > 0u && results == NULL)) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.locks == NULL || context->services.ble_scan == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "ble.scan");
        if (cap_st != JPP_SDK_STATUS_OK) {
            jpp_broker_error_result(result, "ACCESS_DENIED");
            return cap_st;
        }
    }
    op.scan = context->services.ble_scan;
    op.scan_context = context->services.ble_scan_context;
    op.duration_ms = duration_ms;
    op.results = results;
    op.max_results = max_results;
    op.out_count = out_count;
    broker_status = jpp_broker_run_exclusive(
        context->services.locks, "ble_radio",
        jpp_sdk_ble_scan_operation, &op, result
    );
    return broker_status == JPP_BROKER_STATUS_OK ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_BROKER_ERROR;
}

/* ---- ble.advertise ---- */

jpp_sdk_status_t jpp_sdk_ble_advertise_start(
    jpp_sdk_context_t *context,
    const uint8_t *payload,
    size_t payload_len,
    jpp_broker_result_t *result
)
{
    jpp_broker_status_t broker_status;
    jpp_sdk_ble_adv_start_op_t op;
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (result == NULL || (payload_len > 0u && payload == NULL)) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (payload_len > JPP_SDK_BLE_AD_PAYLOAD_MAX) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.locks == NULL || context->services.ble_advertise_start == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "ble.advertise");
        if (cap_st != JPP_SDK_STATUS_OK) {
            jpp_broker_error_result(result, "ACCESS_DENIED");
            return cap_st;
        }
    }
    op.start = context->services.ble_advertise_start;
    op.adv_context = context->services.ble_advertise_context;
    op.payload = payload;
    op.payload_len = payload_len;
    broker_status = jpp_broker_run_exclusive(
        context->services.locks, "ble_radio",
        jpp_sdk_ble_adv_start_operation, &op, result
    );
    return broker_status == JPP_BROKER_STATUS_OK ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_BROKER_ERROR;
}

jpp_sdk_status_t jpp_sdk_ble_advertise_stop(
    jpp_sdk_context_t *context,
    jpp_broker_result_t *result
)
{
    jpp_broker_status_t broker_status;
    jpp_sdk_ble_adv_stop_op_t op;
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (result == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.locks == NULL || context->services.ble_advertise_stop == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "ble.advertise");
        if (cap_st != JPP_SDK_STATUS_OK) {
            jpp_broker_error_result(result, "ACCESS_DENIED");
            return cap_st;
        }
    }
    op.stop = context->services.ble_advertise_stop;
    op.adv_context = context->services.ble_advertise_context;
    broker_status = jpp_broker_run_exclusive(
        context->services.locks, "ble_radio",
        jpp_sdk_ble_adv_stop_operation, &op, result
    );
    return broker_status == JPP_BROKER_STATUS_OK ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_BROKER_ERROR;
}

/* ---- ble.connect ---- */

jpp_sdk_status_t jpp_sdk_ble_connect(
    jpp_sdk_context_t *context,
    const char *address,
    jpp_sdk_ble_conn_t *out_conn
)
{
    jpp_broker_result_t op_result;
    jpp_broker_status_t broker_status;
    jpp_sdk_ble_connect_op_t op;
    jpp_sdk_ble_conn_t slot;
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (address == NULL || address[0] == '\0' || out_conn == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.locks == NULL || context->services.ble_connect == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "ble.connect");
        if (cap_st != JPP_SDK_STATUS_OK) {
            return cap_st;
        }
    }
    slot = jpp_sdk_find_free_ble_conn(context);
    if (slot == (jpp_sdk_ble_conn_t)JPP_SDK_BLE_CONN_INVALID) {
        return JPP_SDK_STATUS_BLE_CONN_LIMIT;
    }
    op.connect = context->services.ble_connect;
    op.client_context = context->services.ble_client_context;
    op.address = address;
    broker_status = jpp_broker_run_exclusive(
        context->services.locks, "ble_client",
        jpp_sdk_ble_connect_operation, &op, &op_result
    );
    if (broker_status != JPP_BROKER_STATUS_OK || !op_result.ok) {
        return JPP_SDK_STATUS_BROKER_ERROR;
    }
    (void)jpp_sdk_copy_text(
        context->ble_conns[slot].address, JPP_SDK_BLE_ADDR_LEN, address
    );
    context->ble_conns[slot].active = true;
    *out_conn = slot;
    return JPP_SDK_STATUS_OK;
}

jpp_sdk_status_t jpp_sdk_ble_read_char(
    jpp_sdk_context_t *context,
    jpp_sdk_ble_conn_t conn,
    const char *svc_uuid,
    const char *char_uuid,
    jpp_broker_result_t *result
)
{
    jpp_broker_status_t broker_status;
    jpp_sdk_ble_conn_entry_t *entry;
    jpp_sdk_ble_read_char_op_t op;
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (result == NULL || svc_uuid == NULL || char_uuid == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    entry = jpp_sdk_resolve_ble_conn(context, conn);
    if (entry == NULL) {
        return JPP_SDK_STATUS_INVALID_BLE_CONN;
    }
    if (context->services.locks == NULL || context->services.ble_read_char == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "ble.connect");
        if (cap_st != JPP_SDK_STATUS_OK) {
            jpp_broker_error_result(result, "ACCESS_DENIED");
            return cap_st;
        }
    }
    op.read_char = context->services.ble_read_char;
    op.client_context = context->services.ble_client_context;
    op.address = entry->address;
    op.svc_uuid = svc_uuid;
    op.char_uuid = char_uuid;
    broker_status = jpp_broker_run_exclusive(
        context->services.locks, "ble_client",
        jpp_sdk_ble_read_char_operation, &op, result
    );
    return broker_status == JPP_BROKER_STATUS_OK ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_BROKER_ERROR;
}

jpp_sdk_status_t jpp_sdk_ble_write_char(
    jpp_sdk_context_t *context,
    jpp_sdk_ble_conn_t conn,
    const char *svc_uuid,
    const char *char_uuid,
    const char *text,
    jpp_broker_result_t *result
)
{
    jpp_broker_status_t broker_status;
    jpp_sdk_ble_conn_entry_t *entry;
    jpp_sdk_ble_write_char_op_t op;
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (result == NULL || svc_uuid == NULL || char_uuid == NULL || text == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    entry = jpp_sdk_resolve_ble_conn(context, conn);
    if (entry == NULL) {
        return JPP_SDK_STATUS_INVALID_BLE_CONN;
    }
    if (context->services.locks == NULL || context->services.ble_write_char == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "ble.connect");
        if (cap_st != JPP_SDK_STATUS_OK) {
            jpp_broker_error_result(result, "ACCESS_DENIED");
            return cap_st;
        }
    }
    op.write_char = context->services.ble_write_char;
    op.client_context = context->services.ble_client_context;
    op.address = entry->address;
    op.svc_uuid = svc_uuid;
    op.char_uuid = char_uuid;
    op.text = text;
    broker_status = jpp_broker_run_exclusive(
        context->services.locks, "ble_client",
        jpp_sdk_ble_write_char_operation, &op, result
    );
    return broker_status == JPP_BROKER_STATUS_OK ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_BROKER_ERROR;
}

jpp_sdk_status_t jpp_sdk_ble_disconnect(
    jpp_sdk_context_t *context,
    jpp_sdk_ble_conn_t conn
)
{
    jpp_broker_result_t op_result;
    jpp_broker_status_t broker_status;
    jpp_sdk_ble_conn_entry_t *entry;
    jpp_sdk_ble_disconnect_op_t op;
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    entry = jpp_sdk_resolve_ble_conn(context, conn);
    if (entry == NULL) {
        return JPP_SDK_STATUS_INVALID_BLE_CONN;
    }
    if (context->services.locks == NULL || context->services.ble_disconnect == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "ble.connect");
        if (cap_st != JPP_SDK_STATUS_OK) { return cap_st; }
    }
    op.disconnect = context->services.ble_disconnect;
    op.client_context = context->services.ble_client_context;
    op.address = entry->address;
    broker_status = jpp_broker_run_exclusive(
        context->services.locks, "ble_client",
        jpp_sdk_ble_disconnect_operation, &op, &op_result
    );
    /* Zero the slot regardless of whether the native disconnect reported an error,
       so a failed peripheral can't leave the slot permanently occupied. */
    memset(entry, 0, sizeof(*entry));
    return broker_status == JPP_BROKER_STATUS_OK ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_BROKER_ERROR;
}

/* ---- ble.host ---- */

jpp_sdk_status_t jpp_sdk_ble_service_register(
    jpp_sdk_context_t *context,
    const char *svc_uuid,
    jpp_broker_result_t *result
)
{
    jpp_broker_status_t broker_status;
    jpp_sdk_ble_service_register_op_t op;
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (result == NULL || svc_uuid == NULL || svc_uuid[0] == '\0') {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.locks == NULL || context->services.ble_service_register == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    /* Only one service registration per app session. */
    if (context->ble_host_registered) {
        return JPP_SDK_STATUS_ACCESS_DENIED;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "ble.host");
        if (cap_st != JPP_SDK_STATUS_OK) {
            jpp_broker_error_result(result, "ACCESS_DENIED");
            return cap_st;
        }
    }
    op.service_register = context->services.ble_service_register;
    op.host_context = context->services.ble_host_context;
    op.svc_uuid = svc_uuid;
    broker_status = jpp_broker_run_exclusive(
        context->services.locks, "ble_host",
        jpp_sdk_ble_service_register_operation, &op, result
    );
    if (broker_status == JPP_BROKER_STATUS_OK && result->ok) {
        context->ble_host_registered = true;
    }
    return broker_status == JPP_BROKER_STATUS_OK ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_BROKER_ERROR;
}

jpp_sdk_status_t jpp_sdk_ble_service_unregister(
    jpp_sdk_context_t *context,
    jpp_broker_result_t *result
)
{
    jpp_broker_status_t broker_status;
    jpp_sdk_ble_service_unregister_op_t op;
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (result == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.locks == NULL || context->services.ble_service_unregister == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    if (!context->ble_host_registered) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "ble.host");
        if (cap_st != JPP_SDK_STATUS_OK) {
            jpp_broker_error_result(result, "ACCESS_DENIED");
            return cap_st;
        }
    }
    op.service_unregister = context->services.ble_service_unregister;
    op.host_context = context->services.ble_host_context;
    broker_status = jpp_broker_run_exclusive(
        context->services.locks, "ble_host",
        jpp_sdk_ble_service_unregister_operation, &op, result
    );
    if (broker_status == JPP_BROKER_STATUS_OK) {
        context->ble_host_registered = false;
    }
    return broker_status == JPP_BROKER_STATUS_OK ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_BROKER_ERROR;
}

/*
 * Host TX/RX byte exchange and connectable mode.
 *
 * These operate on the single shared host service the firmware pre-registers, so
 * they do not take the "ble_host" exclusive lock the way register/unregister do:
 * jpp_sdk_ble_host_wait_write() blocks for the caller-supplied timeout, and
 * holding the lock across a multi-second wait would needlessly serialise the
 * owning app against itself. The capability check still gates every call.
 */
jpp_sdk_status_t jpp_sdk_ble_host_set_value(
    jpp_sdk_context_t *context,
    const uint8_t *data,
    size_t len,
    jpp_broker_result_t *result
)
{
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (result == NULL || (data == NULL && len > 0u)) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.ble_host_set_value == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "ble.host");
        if (cap_st != JPP_SDK_STATUS_OK) {
            jpp_broker_error_result(result, "ACCESS_DENIED");
            return cap_st;
        }
    }
    context->services.ble_host_set_value(context->services.ble_host_context, data, len);
    jpp_broker_ok_result(result);
    return JPP_SDK_STATUS_OK;
}

jpp_sdk_status_t jpp_sdk_ble_host_wait_write(
    jpp_sdk_context_t *context,
    uint8_t *buf,
    size_t *len_inout,
    uint32_t timeout_ms,
    bool *out_received,
    jpp_broker_result_t *result
)
{
    jpp_sdk_status_t status;
    bool received;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (result == NULL || buf == NULL || len_inout == NULL || out_received == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.ble_host_wait_write == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "ble.host");
        if (cap_st != JPP_SDK_STATUS_OK) {
            jpp_broker_error_result(result, "ACCESS_DENIED");
            return cap_st;
        }
    }
    received = context->services.ble_host_wait_write(
        context->services.ble_host_context, buf, len_inout, timeout_ms);
    *out_received = received;
    jpp_broker_ok_result(result);
    return JPP_SDK_STATUS_OK;
}

jpp_sdk_status_t jpp_sdk_ble_host_clear(
    jpp_sdk_context_t *context,
    jpp_broker_result_t *result
)
{
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (result == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.ble_host_clear == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "ble.host");
        if (cap_st != JPP_SDK_STATUS_OK) {
            jpp_broker_error_result(result, "ACCESS_DENIED");
            return cap_st;
        }
    }
    context->services.ble_host_clear(context->services.ble_host_context);
    jpp_broker_ok_result(result);
    return JPP_SDK_STATUS_OK;
}

jpp_sdk_status_t jpp_sdk_ble_set_connectable(
    jpp_sdk_context_t *context,
    bool connectable,
    jpp_broker_result_t *result
)
{
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (result == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.ble_set_connectable == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "ble.advertise");
        if (cap_st != JPP_SDK_STATUS_OK) {
            jpp_broker_error_result(result, "ACCESS_DENIED");
            return cap_st;
        }
    }
    context->services.ble_set_connectable(context->services.ble_host_context, connectable);
    jpp_broker_ok_result(result);
    return JPP_SDK_STATUS_OK;
}

/* ---- device.rtc ---- */

jpp_sdk_status_t jpp_sdk_get_time(jpp_sdk_context_t *context, jpp_broker_result_t *result)
{
    jpp_broker_status_t broker_status;
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (result == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.locks == NULL || context->services.rtc_reader == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "device.status");
        if (cap_st != JPP_SDK_STATUS_OK) {
            jpp_broker_error_result(result, "ACCESS_DENIED");
            return cap_st;
        }
    }
    broker_status = jpp_broker_run_exclusive(
        context->services.locks, "device",
        context->services.rtc_reader, context->services.rtc_reader_context, result
    );
    return broker_status == JPP_BROKER_STATUS_OK ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_BROKER_ERROR;
}

/* ---- http.request ---- */

typedef struct {
    jpp_sdk_http_request_fn_t fn;
    void *fn_context;
    const char *method;
    const char *url;
    const char *body;
} jpp_sdk_http_request_op_t;

static jpp_broker_status_t jpp_sdk_http_request_operation(
    void *context,
    jpp_broker_result_t *result
)
{
    jpp_sdk_http_request_op_t *op = (jpp_sdk_http_request_op_t *)context;
    return op->fn(op->fn_context, op->method, op->url, op->body, result);
}

jpp_sdk_status_t jpp_sdk_http_request(
    jpp_sdk_context_t *context,
    const char *method,
    const char *url,
    const char *body,
    jpp_broker_result_t *result
)
{
    jpp_broker_status_t broker_status;
    jpp_sdk_http_request_op_t op;
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (result == NULL || url == NULL || url[0] == '\0') {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (method == NULL ||
        (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0)) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.locks == NULL || context->services.http_request == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "http.request");
        if (cap_st != JPP_SDK_STATUS_OK) {
            jpp_broker_error_result(result, "ACCESS_DENIED");
            return cap_st;
        }
    }
    op.fn = context->services.http_request;
    op.fn_context = context->services.http_request_context;
    op.method = method;
    op.url = url;
    op.body = body;
    broker_status = jpp_broker_run_exclusive(
        context->services.locks, "http",
        jpp_sdk_http_request_operation, &op, result
    );
    return broker_status == JPP_BROKER_STATUS_OK ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_BROKER_ERROR;
}

/* ---- canvas ---- */

jpp_sdk_status_t jpp_sdk_canvas_write(
    jpp_sdk_context_t *context,
    uint8_t row,
    const uint8_t *pixels
)
{
    jpp_sdk_status_t status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (pixels == NULL || row >= JPP_SDK_CANVAS_ROWS) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    memcpy(context->canvas[row], pixels, JPP_SDK_CANVAS_ROW_BYTES);
    return JPP_SDK_STATUS_OK;
}

jpp_sdk_status_t jpp_sdk_canvas_clear(jpp_sdk_context_t *context)
{
    jpp_sdk_status_t status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    memset(context->canvas, 0, sizeof(context->canvas));
    return JPP_SDK_STATUS_OK;
}

jpp_sdk_status_t jpp_sdk_canvas_draw_pixel(
    jpp_sdk_context_t *context,
    uint8_t x,
    uint8_t y,
    bool on
)
{
    jpp_sdk_status_t status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (x >= 128u || y >= JPP_SDK_CANVAS_ROWS) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    uint8_t *byte = &context->canvas[y][x / 8u];
    uint8_t bit = (uint8_t)(0x80u >> (x % 8u));
    if (on) {
        *byte |= bit;
    } else {
        *byte &= (uint8_t)~bit;
    }
    return JPP_SDK_STATUS_OK;
}

/* ---- ipc.send ---- */

typedef struct {
    jpp_sdk_file_writer_t writer;
    void *writer_context;
    const char *logical_path;
    const char *text;
} jpp_sdk_ipc_write_context_t;

typedef struct {
    jpp_sdk_file_lister_t lister;
    void *lister_context;
    const char *logical_path;
} jpp_sdk_ipc_list_context_t;

typedef struct {
    jpp_sdk_file_reader_t reader;
    void *reader_context;
    const char *logical_path;
} jpp_sdk_ipc_read_context_t;

typedef struct {
    jpp_sdk_file_deleter_t deleter;
    void *deleter_context;
    const char *logical_path;
} jpp_sdk_ipc_delete_context_t;

static jpp_broker_status_t jpp_sdk_ipc_write_op(void *context, jpp_broker_result_t *result)
{
    jpp_sdk_ipc_write_context_t *ctx = (jpp_sdk_ipc_write_context_t *)context;
    return ctx->writer(ctx->writer_context, ctx->logical_path, ctx->text, result);
}

static jpp_broker_status_t jpp_sdk_ipc_list_op(void *context, jpp_broker_result_t *result)
{
    jpp_sdk_ipc_list_context_t *ctx = (jpp_sdk_ipc_list_context_t *)context;
    return ctx->lister(ctx->lister_context, ctx->logical_path, result);
}

static jpp_broker_status_t jpp_sdk_ipc_read_op(void *context, jpp_broker_result_t *result)
{
    jpp_sdk_ipc_read_context_t *ctx = (jpp_sdk_ipc_read_context_t *)context;
    return ctx->reader(ctx->reader_context, ctx->logical_path, result);
}

static jpp_broker_status_t jpp_sdk_ipc_delete_op(void *context, jpp_broker_result_t *result)
{
    jpp_sdk_ipc_delete_context_t *ctx = (jpp_sdk_ipc_delete_context_t *)context;
    return ctx->deleter(ctx->deleter_context, ctx->logical_path, result);
}

static bool jpp_sdk_app_id_segment_is_safe(const char *id)
{
    /* No path separators or dots — prevents directory traversal in IPC paths. */
    size_t i;
    if (id == NULL || id[0] == '\0') {
        return false;
    }
    for (i = 0u; id[i] != '\0'; i++) {
        if (id[i] == '/' || id[i] == '.' || id[i] == '\\') {
            return false;
        }
    }
    return true;
}

jpp_sdk_status_t jpp_sdk_ipc_send(
    jpp_sdk_context_t *context,
    const char *recipient_app_id,
    const char *payload,
    jpp_broker_result_t *result
)
{
    jpp_broker_status_t broker_status;
    jpp_sdk_ipc_write_context_t op;
    jpp_sdk_status_t status;
    char path[JPP_SDK_PATH_MAX];
    size_t app_id_len;
    size_t recipient_len;
    TickType_t tick;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (result == NULL || payload == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (!jpp_sdk_app_id_segment_is_safe(recipient_app_id)) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (strlen(payload) >= JPP_SDK_IPC_PAYLOAD_MAX) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.locks == NULL || context->services.write_file == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "ipc.send");
        if (cap_st != JPP_SDK_STATUS_OK) {
            jpp_broker_error_result(result, "ACCESS_DENIED");
            return cap_st;
        }
    }
    /* Build /sd/ipc/<recipient>/<sender>/msg_<tick>.json */
    app_id_len = strlen(context->app_id);
    recipient_len = strlen(recipient_app_id);
    tick = xTaskGetTickCount();
    if (sizeof("/sd/ipc///.json") + recipient_len + app_id_len + 20u > sizeof(path)) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    snprintf(path, sizeof(path), "/sd/ipc/%s/%s/msg_%lu.json",
             recipient_app_id, context->app_id, (unsigned long)tick);
    op.writer = context->services.write_file;
    op.writer_context = context->services.write_file_context;
    op.logical_path = path;
    op.text = payload;
    broker_status = jpp_broker_run_exclusive(
        context->services.locks, "storage",
        jpp_sdk_ipc_write_op, &op, result
    );
    return broker_status == JPP_BROKER_STATUS_OK ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_BROKER_ERROR;
}

jpp_sdk_status_t jpp_sdk_ipc_recv(
    jpp_sdk_context_t *context,
    char *out_payload,
    size_t payload_buf_len,
    jpp_broker_result_t *result
)
{
    jpp_broker_result_t list_result;
    jpp_broker_result_t read_result;
    jpp_broker_result_t del_result;
    jpp_broker_status_t broker_status;
    jpp_sdk_status_t status;
    char dir_path[JPP_SDK_PATH_MAX];
    char msg_path[JPP_SDK_PATH_MAX];
    const char *first_entry;
    const char *sender;
    jpp_sdk_ipc_list_context_t list_op;
    jpp_sdk_ipc_read_context_t read_op;
    jpp_sdk_ipc_delete_context_t del_op;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (out_payload == NULL || result == NULL || payload_buf_len == 0u) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.locks == NULL ||
        context->services.list_files == NULL ||
        context->services.read_file == NULL ||
        context->services.delete_file == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "ipc.send");
        if (cap_st != JPP_SDK_STATUS_OK) {
            jpp_broker_error_result(result, "ACCESS_DENIED");
            return cap_st;
        }
    }
    /* List /sd/ipc/<own_id>/ to find sender directories */
    snprintf(dir_path, sizeof(dir_path), "/sd/ipc/%s", context->app_id);
    list_op.lister = context->services.list_files;
    list_op.lister_context = context->services.list_files_context;
    list_op.logical_path = dir_path;
    broker_status = jpp_broker_run_exclusive(
        context->services.locks, "storage",
        jpp_sdk_ipc_list_op, &list_op, &list_result
    );
    if (broker_status != JPP_BROKER_STATUS_OK || !list_result.ok) {
        jpp_broker_error_result(result, "NO_MESSAGES");
        return JPP_SDK_STATUS_ACCESS_DENIED;
    }
    /* list_result should have field "entry_0" or similar — take first entry */
    first_entry = jpp_broker_result_get(&list_result, "entry_0");
    if (first_entry == NULL || first_entry[0] == '\0') {
        jpp_broker_error_result(result, "NO_MESSAGES");
        return JPP_SDK_STATUS_ACCESS_DENIED;
    }
    /* first_entry is the sender app_id; find its first message */
    sender = first_entry;
    snprintf(dir_path, sizeof(dir_path), "/sd/ipc/%s/%s", context->app_id, sender);
    list_op.logical_path = dir_path;
    broker_status = jpp_broker_run_exclusive(
        context->services.locks, "storage",
        jpp_sdk_ipc_list_op, &list_op, &list_result
    );
    if (broker_status != JPP_BROKER_STATUS_OK || !list_result.ok) {
        jpp_broker_error_result(result, "NO_MESSAGES");
        return JPP_SDK_STATUS_ACCESS_DENIED;
    }
    first_entry = jpp_broker_result_get(&list_result, "entry_0");
    if (first_entry == NULL || first_entry[0] == '\0') {
        jpp_broker_error_result(result, "NO_MESSAGES");
        return JPP_SDK_STATUS_ACCESS_DENIED;
    }
    snprintf(msg_path, sizeof(msg_path), "/sd/ipc/%s/%s/%s",
             context->app_id, sender, first_entry);
    /* Read the message */
    read_op.reader = context->services.read_file;
    read_op.reader_context = context->services.read_file_context;
    read_op.logical_path = msg_path;
    broker_status = jpp_broker_run_exclusive(
        context->services.locks, "storage",
        jpp_sdk_ipc_read_op, &read_op, &read_result
    );
    if (broker_status != JPP_BROKER_STATUS_OK || !read_result.ok) {
        jpp_broker_error_result(result, "READ_FAILED");
        return JPP_SDK_STATUS_BROKER_ERROR;
    }
    const char *text = jpp_broker_result_get(&read_result, "text");
    if (text == NULL) text = "";
    jpp_sdk_copy_text(out_payload, payload_buf_len, text);
    /* Delete the consumed message */
    del_op.deleter = context->services.delete_file;
    del_op.deleter_context = context->services.delete_file_context;
    del_op.logical_path = msg_path;
    jpp_broker_run_exclusive(
        context->services.locks, "storage",
        jpp_sdk_ipc_delete_op, &del_op, &del_result
    );
    /* Report success with sender field */
    if (jpp_broker_ok_result(result) != JPP_BROKER_STATUS_OK) {
        return JPP_SDK_STATUS_BROKER_ERROR;
    }
    (void)jpp_broker_result_put(result, "sender", sender);
    return JPP_SDK_STATUS_OK;
}

/* ---- device.kv ---- */

typedef struct {
    jpp_sdk_kv_read_fn_t fn;
    void *ctx;
    const char *app_id;
    const char *key;
    char *out_value;
    size_t buf_len;
} jpp_sdk_kv_read_op_t;

typedef struct {
    jpp_sdk_kv_write_fn_t fn;
    void *ctx;
    const char *app_id;
    const char *key;
    const char *value;
} jpp_sdk_kv_write_op_t;

typedef struct {
    jpp_sdk_kv_delete_fn_t fn;
    void *ctx;
    const char *app_id;
    const char *key;
} jpp_sdk_kv_delete_op_t;

static jpp_broker_status_t jpp_sdk_kv_read_operation(void *context, jpp_broker_result_t *result)
{
    jpp_sdk_kv_read_op_t *op = (jpp_sdk_kv_read_op_t *)context;
    return op->fn(op->ctx, op->app_id, op->key, op->out_value, op->buf_len, result);
}

static jpp_broker_status_t jpp_sdk_kv_write_operation(void *context, jpp_broker_result_t *result)
{
    jpp_sdk_kv_write_op_t *op = (jpp_sdk_kv_write_op_t *)context;
    return op->fn(op->ctx, op->app_id, op->key, op->value, result);
}

static jpp_broker_status_t jpp_sdk_kv_delete_operation(void *context, jpp_broker_result_t *result)
{
    jpp_sdk_kv_delete_op_t *op = (jpp_sdk_kv_delete_op_t *)context;
    return op->fn(op->ctx, op->app_id, op->key, result);
}

static jpp_sdk_status_t jpp_sdk_kv_require(jpp_sdk_context_t *context)
{
    return jpp_sdk_ensure_cap(context, "device.kv");
}

jpp_sdk_status_t jpp_sdk_kv_get(
    jpp_sdk_context_t *context,
    const char *key,
    char *out_value,
    size_t value_buf_len
)
{
    jpp_broker_result_t result;
    jpp_sdk_kv_read_op_t op;
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (key == NULL || key[0] == '\0' || out_value == NULL || value_buf_len == 0u) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (strlen(key) >= JPP_SDK_KV_KEY_MAX) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.locks == NULL || context->services.kv_read == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    status = jpp_sdk_kv_require(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    op.fn = context->services.kv_read;
    op.ctx = context->services.kv_context;
    op.app_id = context->app_id;
    op.key = key;
    op.out_value = out_value;
    op.buf_len = value_buf_len;
    jpp_broker_status_t bs = jpp_broker_run_exclusive(
        context->services.locks, "kv",
        jpp_sdk_kv_read_operation, &op, &result
    );
    if (bs != JPP_BROKER_STATUS_OK) {
        return JPP_SDK_STATUS_BROKER_ERROR;
    }
    /* The KV callback reports a missing key with result.ok == false (e.g.
       "KEY_NOT_FOUND"). Surface that as a non-OK status so callers can tell a
       present empty value apart from an absent key — the read buffer is only
       valid when the key exists. Without this, a get after a delete returned OK
       with a stale/empty buffer and the key looked like it was still there. */
    if (!result.ok) {
        out_value[0] = '\0';
        return JPP_SDK_STATUS_BROKER_ERROR;
    }
    return JPP_SDK_STATUS_OK;
}

jpp_sdk_status_t jpp_sdk_kv_set(
    jpp_sdk_context_t *context,
    const char *key,
    const char *value
)
{
    jpp_broker_result_t result;
    jpp_sdk_kv_write_op_t op;
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (key == NULL || key[0] == '\0' || value == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (strlen(key) >= JPP_SDK_KV_KEY_MAX || strlen(value) >= JPP_SDK_KV_VALUE_MAX) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.locks == NULL || context->services.kv_write == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    status = jpp_sdk_kv_require(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    op.fn = context->services.kv_write;
    op.ctx = context->services.kv_context;
    op.app_id = context->app_id;
    op.key = key;
    op.value = value;
    jpp_broker_status_t bs = jpp_broker_run_exclusive(
        context->services.locks, "kv",
        jpp_sdk_kv_write_operation, &op, &result
    );
    return bs == JPP_BROKER_STATUS_OK ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_BROKER_ERROR;
}

jpp_sdk_status_t jpp_sdk_kv_delete(
    jpp_sdk_context_t *context,
    const char *key
)
{
    jpp_broker_result_t result;
    jpp_sdk_kv_delete_op_t op;
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (key == NULL || key[0] == '\0') {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.locks == NULL || context->services.kv_delete == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    status = jpp_sdk_kv_require(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    op.fn = context->services.kv_delete;
    op.ctx = context->services.kv_context;
    op.app_id = context->app_id;
    op.key = key;
    jpp_broker_status_t bs = jpp_broker_run_exclusive(
        context->services.locks, "kv",
        jpp_sdk_kv_delete_operation, &op, &result
    );
    return bs == JPP_BROKER_STATUS_OK ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_BROKER_ERROR;
}

/* ---- keypad event routing ---- */

jpp_sdk_status_t jpp_sdk_poll_key(jpp_sdk_context_t *context, jpp_sdk_key_event_t *out_event)
{
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (out_event == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->key_queue == NULL) {
        *out_event = JPP_SDK_KEY_NONE;
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    jpp_sdk_key_event_t ev = JPP_SDK_KEY_NONE;
    xQueueReceive(context->key_queue, &ev, 0);
    *out_event = ev;
    return JPP_SDK_STATUS_OK;
}

jpp_sdk_status_t jpp_sdk_wait_key(jpp_sdk_context_t *context, uint32_t timeout_ms, jpp_sdk_key_event_t *out_event)
{
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (out_event == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->key_queue == NULL) {
        *out_event = JPP_SDK_KEY_NONE;
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    jpp_sdk_key_event_t ev = JPP_SDK_KEY_NONE;
    /* timeout_ms == 0 means "wait indefinitely" (use jpp_sdk_poll_key for a
       non-blocking check). Mapping 0 to 0 ticks made this a non-blocking poll,
       which caused apps with an idle screen to fall straight through to their
       "no key" branch and exit the instant they launched. */
    TickType_t ticks = (timeout_ms == 0u) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    xQueueReceive(context->key_queue, &ev, ticks);
    *out_event = ev;
    return JPP_SDK_STATUS_OK;
}

void jpp_sdk_push_key(jpp_sdk_context_t *context, jpp_sdk_key_event_t event)
{
    if (context == NULL || context->key_queue == NULL) {
        return;
    }
    /* Non-blocking: drop if full — the main loop never blocks on this. */
    xQueueSendToBack(context->key_queue, &event, 0);
}

/* -------------------------------------------------------------------------- */
/* High-level UI helpers: Dialog, List, Input                                 */
/*                                                                            */
/* These run in the calling app's task. They drive the screen with           */
/* jpp_sdk_set_frame and read the d-pad with jpp_sdk_wait_key, blocking until */
/* the user resolves the prompt. The main render task draws the frame they    */
/* publish, so they need no separate rendering path.                          */
/* -------------------------------------------------------------------------- */

#define JPP_SDK_DISPLAY_COLS 21u   /* visible characters per row */

/* Block until the next key event, ignoring spurious KEY_NONE wakeups. */
static jpp_sdk_key_event_t jpp_sdk_ui_next_key(jpp_sdk_context_t *context)
{
    jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
    do {
        if (jpp_sdk_wait_key(context, 0u, &key) != JPP_SDK_STATUS_OK) {
            return JPP_SDK_KEY_NONE;
        }
    } while (key == JPP_SDK_KEY_NONE);
    return key;
}

/*
 * Word-wrap `text` into up to `max_lines` rows of at most JPP_SDK_DISPLAY_COLS
 * characters. Returns the number of rows used. Words longer than a row are
 * hard-split; text past max_lines is dropped.
 */
static size_t jpp_sdk_ui_wrap(const char *text,
                              char lines[][JPP_SDK_FRAME_TEXT_MAX],
                              size_t max_lines)
{
    if (text == NULL || max_lines == 0u) {
        return 0u;
    }
    size_t line = 0u;
    size_t col  = 0u;
    lines[0][0] = '\0';
    for (size_t i = 0u; text[i] != '\0' && line < max_lines; ) {
        if (text[i] == ' ') {       /* collapse the gap between words */
            i++;
            continue;
        }
        size_t word_len = 0u;
        while (text[i + word_len] != '\0' && text[i + word_len] != ' ') {
            word_len++;
        }
        size_t need = (col > 0u ? 1u : 0u) + word_len;
        if (col > 0u && col + need > JPP_SDK_DISPLAY_COLS) {
            if (++line >= max_lines) {
                break;
            }
            lines[line][0] = '\0';
            col = 0u;
        }
        if (col > 0u) {
            lines[line][col++] = ' ';
        }
        for (size_t k = 0u; k < word_len && col < JPP_SDK_DISPLAY_COLS; k++) {
            lines[line][col++] = text[i + k];
        }
        lines[line][col] = '\0';
        i += word_len;
    }
    return (col > 0u || line > 0u) ? line + 1u : 0u;
}

jpp_sdk_status_t jpp_sdk_dialog(
    jpp_sdk_context_t *context,
    const char *title,
    const char *text,
    jpp_sdk_ui_result_t *out_result)
{
    jpp_sdk_status_t status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (out_result == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }

    char body[JPP_SDK_FRAME_LINE_CAPACITY][JPP_SDK_FRAME_TEXT_MAX];
    const char *lines[JPP_SDK_FRAME_LINE_CAPACITY];
    size_t n = 0u;
    bool has_title = jpp_sdk_text_present(title);

    if (has_title) {
        /* Title on row 0; row 1 stays blank so the renderer can draw the
           signature line there, matching the launcher/settings/WebDAV header. */
        jpp_sdk_copy_text(body[0], JPP_SDK_FRAME_TEXT_MAX, title);
        body[1][0] = '\0';
        n = 2u;
    }
    n += jpp_sdk_ui_wrap(text, &body[n], JPP_SDK_FRAME_LINE_CAPACITY - n);
    for (size_t i = 0u; i < n; i++) {
        lines[i] = body[i];
    }
    jpp_sdk_set_frame(context, lines, n);
    context->frame_title_rule = has_title;

    for (;;) {
        jpp_sdk_key_event_t key = jpp_sdk_ui_next_key(context);
        if (key == JPP_SDK_KEY_CENTER) {
            *out_result = JPP_SDK_UI_OK;
            return JPP_SDK_STATUS_OK;
        }
        if (key == JPP_SDK_KEY_CENTER_LONG) {
            *out_result = JPP_SDK_UI_BACK;
            return JPP_SDK_STATUS_OK;
        }
    }
}

jpp_sdk_status_t jpp_sdk_confirm(
    jpp_sdk_context_t *context,
    const char *title,
    const char *const *body_lines,
    size_t body_count,
    bool default_allow,
    bool *out_allow)
{
    jpp_sdk_status_t status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (out_allow == NULL || (body_count > 0u && body_lines == NULL)) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }

    bool allow = default_allow;
    char frame[JPP_SDK_FRAME_LINE_CAPACITY][JPP_SDK_FRAME_TEXT_MAX];
    const char *lines[JPP_SDK_FRAME_LINE_CAPACITY];
    bool has_title = jpp_sdk_text_present(title);

    for (;;) {
        size_t n = 0u;
        if (has_title) {
            jpp_sdk_copy_text(frame[0], JPP_SDK_FRAME_TEXT_MAX, title);
            frame[1][0] = '\0';   /* blank row for the signature line */
            n = 2u;
        }
        /* Reserve the final row for the Deny/Allow selector. */
        size_t max_body = JPP_SDK_FRAME_LINE_CAPACITY - n - 1u;
        size_t bc = body_count < max_body ? body_count : max_body;
        for (size_t i = 0u; i < bc; i++) {
            jpp_sdk_copy_text(frame[n], JPP_SDK_FRAME_TEXT_MAX,
                              body_lines[i] != NULL ? body_lines[i] : "");
            n++;
        }
        jpp_sdk_copy_text(frame[n], JPP_SDK_FRAME_TEXT_MAX,
                          allow ? " Deny       >Allow" : ">Deny        Allow");
        n++;
        for (size_t i = 0u; i < n; i++) {
            lines[i] = frame[i];
        }
        jpp_sdk_set_frame(context, lines, n);
        context->frame_title_rule = has_title;

        jpp_sdk_key_event_t key = jpp_sdk_ui_next_key(context);
        switch (key) {
        case JPP_SDK_KEY_LEFT:        allow = false; break;
        case JPP_SDK_KEY_RIGHT:       allow = true;  break;
        case JPP_SDK_KEY_CENTER:      *out_allow = allow; return JPP_SDK_STATUS_OK;
        case JPP_SDK_KEY_CENTER_LONG: *out_allow = false; return JPP_SDK_STATUS_OK;
        default: break;
        }
    }
}

jpp_sdk_status_t jpp_sdk_list(
    jpp_sdk_context_t *context,
    const char *title,
    const char *const *items,
    size_t item_count,
    bool multiselect,
    size_t *out_indices,
    size_t out_capacity,
    size_t *out_count,
    jpp_sdk_ui_result_t *out_result)
{
    jpp_sdk_status_t status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) {
        return status;
    }
    if (items == NULL || item_count == 0u || item_count > JPP_SDK_LIST_ITEM_MAX ||
        out_indices == NULL || out_capacity == 0u || out_count == NULL ||
        out_result == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }

    bool   checked[JPP_SDK_LIST_ITEM_MAX] = {0};
    bool   has_title  = jpp_sdk_text_present(title);
    size_t total_rows = item_count + (multiselect ? 1u : 0u);  /* +1 "Done" row */
    /* With a title we reserve row 0 (title) and row 1 (signature line); items
       start on row 2. Without a title, items use every row. */
    size_t header     = has_title ? 2u : 0u;
    size_t visible    = JPP_SDK_FRAME_LINE_CAPACITY - header;
    size_t cursor     = 0u;
    size_t top        = 0u;

    for (;;) {
        if (cursor < top) {
            top = cursor;
        } else if (cursor >= top + visible) {
            top = cursor - visible + 1u;
        }

        char body[JPP_SDK_FRAME_LINE_CAPACITY][JPP_SDK_FRAME_TEXT_MAX];
        const char *lines[JPP_SDK_FRAME_LINE_CAPACITY];
        size_t n = 0u;
        if (header) {
            jpp_sdk_copy_text(body[0], JPP_SDK_FRAME_TEXT_MAX, title);
            lines[0] = body[0];
            body[1][0] = '\0';        /* blank row for the signature line */
            lines[1] = body[1];
            n = 2u;
        }
        for (size_t v = 0u; v < visible && (top + v) < total_rows; v++) {
            size_t idx  = top + v;
            const char *mark = (idx == cursor) ? ">" : " ";
            bool is_done = multiselect && idx == item_count;
            if (is_done) {
                snprintf(body[n], JPP_SDK_FRAME_TEXT_MAX, "%s<Done>", mark);
            } else if (multiselect) {
                snprintf(body[n], JPP_SDK_FRAME_TEXT_MAX, "%s[%c]%s",
                         mark, checked[idx] ? 'x' : ' ', items[idx]);
            } else {
                snprintf(body[n], JPP_SDK_FRAME_TEXT_MAX, "%s%s", mark, items[idx]);
            }
            lines[n] = body[n];
            n++;
        }
        jpp_sdk_set_frame(context, lines, n);
        context->frame_title_rule = has_title;

        jpp_sdk_key_event_t key = jpp_sdk_ui_next_key(context);
        switch (key) {
        case JPP_SDK_KEY_UP:
            cursor = (cursor == 0u) ? total_rows - 1u : cursor - 1u;
            break;
        case JPP_SDK_KEY_DOWN:
            cursor = (cursor + 1u) % total_rows;
            break;
        case JPP_SDK_KEY_CENTER:
            if (!multiselect) {
                out_indices[0] = cursor;
                *out_count = 1u;
                *out_result = JPP_SDK_UI_OK;
                return JPP_SDK_STATUS_OK;
            }
            if (cursor == item_count) {   /* "Done" row confirms */
                size_t c = 0u;
                for (size_t i = 0u; i < item_count && c < out_capacity; i++) {
                    if (checked[i]) {
                        out_indices[c++] = i;
                    }
                }
                *out_count = c;
                *out_result = JPP_SDK_UI_OK;
                return JPP_SDK_STATUS_OK;
            }
            checked[cursor] = !checked[cursor];
            break;
        case JPP_SDK_KEY_CENTER_LONG:
            *out_count = 0u;
            *out_result = JPP_SDK_UI_BACK;
            return JPP_SDK_STATUS_OK;
        default:
            break;
        }
    }
}

/* ---- jpp_sdk_input -------------------------------------------------------
 * The on-screen keyboard renders into context->canvas (which has the same
 * uint8_t[48][16] layout as jpp_kbd_pixbuf_t) using jpp_kbd_core.          */

/* On-screen keyboard entry (the QWERTY/number grid). `prefill` (may be NULL)
   seeds the field — used by the date/time picker's "123" manual-entry mode. */
static jpp_sdk_status_t jpp_sdk_keyboard_entry(
    jpp_sdk_context_t *context,
    const char *title,
    const char *placeholder,
    jpp_sdk_input_type_t type,
    const char *prefill,
    char *out_value,
    size_t value_buf_len,
    jpp_sdk_ui_result_t *out_result)
{
    jpp_kbd_key_t rows[JPP_KBD_MAX_ROWS][JPP_KBD_MAX_COLS];
    size_t row_len[JPP_KBD_MAX_ROWS];
    size_t row_count = 0u;
    jpp_kbd_layout((jpp_kbd_input_type_t)type, rows, row_len, &row_count);

    jpp_kbd_cursor_t cursor;
    jpp_kbd_cursor_init(&cursor, prefill);
    size_t cap = value_buf_len - 1u;
    if (cap > JPP_KBD_VALUE_MAX) { cap = JPP_KBD_VALUE_MAX; }

    const char *title_str = jpp_sdk_text_present(title) ? title : "Enter value";

    for (;;) {
        const char *lines[1] = { title_str };
        jpp_sdk_set_frame(context, lines, 1u);

        bool is_ghost = (cursor.len == 0u && jpp_sdk_text_present(placeholder));
        const char *field = is_ghost ? placeholder : cursor.value;

        /* Render keyboard directly into context->canvas (same layout as
           jpp_kbd_pixbuf_t: uint8_t[48][16], row-major, MSB=left).     */
        jpp_kbd_render(context->canvas, rows, row_len, row_count,
                       field, is_ghost, &cursor);

        jpp_sdk_key_event_t key = jpp_sdk_ui_next_key(context);

        jpp_kbd_result_t res = jpp_kbd_step(&cursor, rows, row_len, row_count,
                                             (int)key, cap);
        if (res == JPP_KBD_RESULT_DONE) {
            size_t n = cursor.len;
            if (n >= value_buf_len) { n = value_buf_len - 1u; }
            memcpy(out_value, cursor.value, n);
            out_value[n] = '\0';
            *out_result = JPP_SDK_UI_OK;
            return JPP_SDK_STATUS_OK;
        }
        if (res == JPP_KBD_RESULT_CANCEL) {
            out_value[0] = '\0';
            *out_result = JPP_SDK_UI_BACK;
            return JPP_SDK_STATUS_OK;
        }
    }
}

/* ---- jpp_sdk_input date/time picker --------------------------------------
 * A field-based spinner for DATE (YYYY-MM-DD) and TIME (HH:MM:SS, seconds
 * included). LEFT/RIGHT move between fields and the button row, UP/DOWN adjust
 * the focused field by one. The button row offers: "123" (masked manual entry
 * via the number keyboard), "now" (fill from the RTC), Cancel, and OK.        */

#define SDK_DT_FIELDS 3u   /* year/month/day or hour/minute/second */
enum { SDK_DT_BTN_123 = SDK_DT_FIELDS, SDK_DT_BTN_NOW, SDK_DT_BTN_CANCEL,
       SDK_DT_BTN_OK, SDK_DT_FOCUS_COUNT };

static int sdk_dt_days_in_month(int year, int month)
{
    static const int days[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month < 1 || month > 12) { return 31; }
    if (month == 2) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return days[month - 1];
}

/* Read up to `max` unsigned integers from `s`, skipping any non-digit runs. */
static size_t sdk_dt_parse_nums(const char *s, int *out, size_t max)
{
    size_t count = 0u;
    while (*s != '\0' && count < max) {
        if (*s >= '0' && *s <= '9') {
            int v = 0;
            while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
            out[count++] = v;
        } else {
            s++;
        }
    }
    return count;
}

static void sdk_dt_clamp(jpp_sdk_input_type_t type, int *v)
{
    if (type == JPP_SDK_INPUT_DATE) {
        if (v[0] < 1970) v[0] = 1970;
        if (v[0] > 2099) v[0] = 2099;
        if (v[1] < 1) v[1] = 1;
        if (v[1] > 12) v[1] = 12;
        int dim = sdk_dt_days_in_month(v[0], v[1]);
        if (v[2] < 1) v[2] = 1;
        if (v[2] > dim) v[2] = dim;
    } else {
        if (v[0] < 0) { v[0] = 0; }
        if (v[0] > 23) { v[0] = 23; }
        if (v[1] < 0) { v[1] = 0; }
        if (v[1] > 59) { v[1] = 59; }
        if (v[2] < 0) { v[2] = 0; }
        if (v[2] > 59) { v[2] = 59; }
    }
}

static void sdk_dt_format(jpp_sdk_input_type_t type, const int *v,
                          char *out, size_t out_len)
{
    if (type == JPP_SDK_INPUT_DATE) {
        snprintf(out, out_len, "%04d-%02d-%02d", v[0], v[1], v[2]);
    } else {
        snprintf(out, out_len, "%02d:%02d:%02d", v[0], v[1], v[2]);
    }
}

/* Step the focused field by delta (+1/-1) with wraparound, re-clamping day. */
static void sdk_dt_step_field(jpp_sdk_input_type_t type, int *v, size_t f, int delta)
{
    int lo, hi;
    if (type == JPP_SDK_INPUT_DATE) {
        if (f == 0u)      { lo = 1970; hi = 2099; }
        else if (f == 1u) { lo = 1; hi = 12; }
        else              { lo = 1; hi = sdk_dt_days_in_month(v[0], v[1]); }
    } else {
        lo = 0;
        hi = (f == 0u) ? 23 : 59;
    }
    int range = hi - lo + 1;
    v[f] = lo + ((v[f] - lo + delta) % range + range) % range;
    sdk_dt_clamp(type, v);
}

static jpp_sdk_status_t jpp_sdk_datetime_picker(
    jpp_sdk_context_t *context,
    const char *title,
    jpp_sdk_input_type_t type,
    char *out_value,
    size_t value_buf_len,
    jpp_sdk_ui_result_t *out_result)
{
    bool is_date = (type == JPP_SDK_INPUT_DATE);

    /* Field geometry: char offset within the formatted string, and char width. */
    const int field_off[SDK_DT_FIELDS] = { 0, is_date ? 5 : 3, is_date ? 8 : 6 };
    const int field_w[SDK_DT_FIELDS]   = { is_date ? 4 : 2, 2, 2 };
    int value_chars = is_date ? 10 : 8;
    int base_x = (128 - value_chars * 6) / 2;
    const int valy = 12;

    int v[SDK_DT_FIELDS];
    if (is_date) { v[0] = 2026; v[1] = 1; v[2] = 1; }
    else         { v[0] = 0; v[1] = 0; v[2] = 0; }
    sdk_dt_clamp(type, v);

    size_t focus = 0u;

    const char *title_str = jpp_sdk_text_present(title)
                            ? title : (is_date ? "Set date" : "Set time");

    static const char *const btn_labels[4] = { "123", "now", "X", "OK" };
    const int btn_cell = 32;  /* 4 cells across 128 px */

    for (;;) {
        const char *lines[1] = { title_str };
        jpp_sdk_set_frame(context, lines, 1u);
        context->frame_title_rule = true;

        memset(context->canvas, 0, sizeof(context->canvas));

        char str[16];
        sdk_dt_format(type, v, str, sizeof(str));
        jpp_kbd_text(context->canvas, base_x, valy, str, 128, false);

        /* Left/right navigation hint arrows flanking the value. */
        jpp_kbd_glyph(context->canvas, base_x - 11, valy, '<', false);
        jpp_kbd_glyph(context->canvas, base_x + value_chars * 6 + 1, valy, '>', false);

        /* Focused field: box + up/down carets. */
        if (focus < SDK_DT_FIELDS) {
            int fx0 = base_x + field_off[focus] * 6;
            int fx1 = fx0 + field_w[focus] * 6 - 2;
            jpp_kbd_frame_rect(context->canvas, fx0 - 1, valy - 2, fx1 + 1, valy + 9);
            int cx = (fx0 + fx1) / 2 - 2;
            jpp_kbd_glyph(context->canvas, cx, valy - 11, '^', false);
            jpp_kbd_glyph(context->canvas, cx, valy + 13, 'v', false);
        }

        /* Button row. */
        for (size_t b = 0u; b < 4u; b++) {
            int x0 = (int)b * btn_cell + 1;
            int x1 = x0 + btn_cell - 3;
            int y0 = 34, y1 = 45;
            bool sel = (focus == SDK_DT_BTN_123 + b);
            int label_px = (int)strlen(btn_labels[b]) * 6 - 1;
            int tx = x0 + (x1 - x0 - label_px) / 2;
            if (sel) {
                jpp_kbd_fill_rect(context->canvas, x0, y0, x1, y1, true);
            } else {
                jpp_kbd_frame_rect(context->canvas, x0, y0, x1, y1);
            }
            jpp_kbd_text(context->canvas, tx, y0 + 2, btn_labels[b], x1, sel);
        }

        jpp_sdk_key_event_t key = jpp_sdk_ui_next_key(context);
        switch (key) {
        case JPP_SDK_KEY_LEFT:
            focus = (focus == 0u) ? SDK_DT_FOCUS_COUNT - 1u : focus - 1u;
            break;
        case JPP_SDK_KEY_RIGHT:
            focus = (focus + 1u) % SDK_DT_FOCUS_COUNT;
            break;
        case JPP_SDK_KEY_UP:
            if (focus < SDK_DT_FIELDS) { sdk_dt_step_field(type, v, focus, +1); }
            break;
        case JPP_SDK_KEY_DOWN:
            if (focus < SDK_DT_FIELDS) { sdk_dt_step_field(type, v, focus, -1); }
            break;
        case JPP_SDK_KEY_CENTER:
            if (focus < SDK_DT_FIELDS || focus == SDK_DT_BTN_OK) {
                sdk_dt_format(type, v, out_value, value_buf_len);
                *out_result = JPP_SDK_UI_OK;
                return JPP_SDK_STATUS_OK;
            }
            if (focus == SDK_DT_BTN_CANCEL) {
                out_value[0] = '\0';
                *out_result = JPP_SDK_UI_BACK;
                return JPP_SDK_STATUS_OK;
            }
            if (focus == SDK_DT_BTN_123) {
                char prefill[16];
                char typed[32];
                jpp_sdk_ui_result_t r = JPP_SDK_UI_BACK;
                sdk_dt_format(type, v, prefill, sizeof(prefill));
                jpp_sdk_keyboard_entry(context, title_str, NULL, type, prefill,
                                       typed, sizeof(typed), &r);
                if (r == JPP_SDK_UI_OK) {
                    int parsed[SDK_DT_FIELDS] = { v[0], v[1], v[2] };
                    sdk_dt_parse_nums(typed, parsed, SDK_DT_FIELDS);
                    v[0] = parsed[0]; v[1] = parsed[1]; v[2] = parsed[2];
                    sdk_dt_clamp(type, v);
                }
            } else if (focus == SDK_DT_BTN_NOW) {
                if (context->services.rtc_reader != NULL &&
                    context->services.locks != NULL) {
                    jpp_broker_result_t r;
                    if (jpp_broker_run_exclusive(
                            context->services.locks, "device",
                            context->services.rtc_reader,
                            context->services.rtc_reader_context, &r)
                            == JPP_BROKER_STATUS_OK && r.ok) {
                        /* "YYYY-MM-DD HH:mm" — RTC has no seconds. */
                        const char *t = jpp_broker_result_get(&r, "text");
                        if (t != NULL) {
                            int nums[5] = {0};
                            size_t got = sdk_dt_parse_nums(t, nums, 5u);
                            if (is_date && got >= 3u) {
                                v[0] = nums[0]; v[1] = nums[1]; v[2] = nums[2];
                            } else if (!is_date && got >= 5u) {
                                v[0] = nums[3]; v[1] = nums[4]; v[2] = 0;
                            }
                            sdk_dt_clamp(type, v);
                        }
                    }
                }
            }
            break;
        case JPP_SDK_KEY_CENTER_LONG:
            out_value[0] = '\0';
            *out_result = JPP_SDK_UI_BACK;
            return JPP_SDK_STATUS_OK;
        default:
            break;
        }
    }
}

jpp_sdk_status_t jpp_sdk_input(
    jpp_sdk_context_t *context,
    const char *title,
    const char *placeholder,
    jpp_sdk_input_type_t type,
    char *out_value,
    size_t value_buf_len,
    jpp_sdk_ui_result_t *out_result)
{
    jpp_sdk_status_t status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) { return status; }
    if (out_value == NULL || value_buf_len == 0u || out_result == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }

    if (type == JPP_SDK_INPUT_DATE || type == JPP_SDK_INPUT_TIME) {
        return jpp_sdk_datetime_picker(context, title, type,
                                       out_value, value_buf_len, out_result);
    }
    return jpp_sdk_keyboard_entry(context, title, placeholder, type, NULL,
                                  out_value, value_buf_len, out_result);
}


/* ---- jpp_sdk_file_pick ---------------------------------------------------
 * Navigable SD card file browser using the text frame and dir_list callback.
 * Requires: files.full
 * Marquee cadence: 400 ms per tick via jpp_sdk_wait_key timeout.            */

#define SDK_FP_ENTRY_MAX   32u
#define SDK_FP_NAME_MAX    65u   /* display name incl. "/" suffix and NUL */
#define SDK_FP_VISIBLE      6u   /* JPP_RESOURCE_SDK_FRAME_LINE_LIMIT - 1 header */
#define SDK_FP_ITEM_CHARS  20u   /* chars available after ">" / " " prefix */
#define SDK_FP_MARQUEE_MS  400u
#define SDK_FP_PAUSE_TICKS   3u

typedef struct {
    char   root[JPP_SDK_PATH_MAX];
    char   dir[JPP_SDK_PATH_MAX];
    char   names[SDK_FP_ENTRY_MAX][SDK_FP_NAME_MAX];
    bool   is_dir[SDK_FP_ENTRY_MAX];
    size_t count;
    bool   has_parent;
    size_t cursor;
    size_t scroll;
    uint32_t marquee;
} sdk_fp_state_t;

static size_t sdk_fp_total(const sdk_fp_state_t *s)
{
    return s->count + (s->has_parent ? 1u : 0u);
}

static const char *sdk_fp_item_name(const sdk_fp_state_t *s, size_t i)
{
    if (s->has_parent) { if (i == 0u) return ".."; return s->names[i - 1u]; }
    return s->names[i];
}

static bool sdk_fp_item_is_dir(const sdk_fp_state_t *s, size_t i)
{
    if (s->has_parent && i == 0u) return true;
    size_t ei = s->has_parent ? i - 1u : i;
    return s->is_dir[ei];
}

static size_t sdk_fp_marquee_off(uint32_t tick, size_t namelen)
{
    if (namelen <= SDK_FP_ITEM_CHARS) return 0u;
    size_t max_off = namelen - SDK_FP_ITEM_CHARS;
    size_t cycle   = 2u * SDK_FP_PAUSE_TICKS + max_off;
    size_t phase   = (size_t)(tick % (uint32_t)cycle);
    if (phase < SDK_FP_PAUSE_TICKS)                    return 0u;
    if (phase < SDK_FP_PAUSE_TICKS + max_off)          return phase - SDK_FP_PAUSE_TICKS;
    return max_off;
}

static void sdk_fp_load(sdk_fp_state_t *s, jpp_sdk_context_t *ctx)
{
    s->count = 0u;
    if (ctx->services.dir_list == NULL) return;

    static char s_dir_buf[2048];
    ctx->services.dir_list(ctx->services.dir_list_context,
                            s->dir, s_dir_buf, sizeof(s_dir_buf));

    char *p = s_dir_buf;
    while (*p != '\0' && s->count < SDK_FP_ENTRY_MAX) {
        char *nl  = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len == 0u) { if (nl) { p = nl + 1u; } else { break; } continue; }
        bool is_d = (p[len - 1u] == '/');
        if (len >= SDK_FP_NAME_MAX) { len = SDK_FP_NAME_MAX - 1u; }
        memcpy(s->names[s->count], p, len);
        s->names[s->count][len] = '\0';
        s->is_dir[s->count]     = is_d;
        s->count++;
        if (nl) { p = nl + 1u; } else { break; }
    }

    /* Bubble sort by name. */
    for (size_t i = 0u; i < s->count; i++) {
        for (size_t j = 0u; j + 1u < s->count - i; j++) {
            if (strcmp(s->names[j], s->names[j + 1u]) > 0) {
                char tmp[SDK_FP_NAME_MAX];
                memcpy(tmp, s->names[j], SDK_FP_NAME_MAX);
                memcpy(s->names[j], s->names[j + 1u], SDK_FP_NAME_MAX);
                memcpy(s->names[j + 1u], tmp, SDK_FP_NAME_MAX);
                bool bt         = s->is_dir[j];
                s->is_dir[j]           = s->is_dir[j + 1u];
                s->is_dir[j + 1u]      = bt;
            }
        }
    }

    s->has_parent = (strcmp(s->dir, s->root) != 0);
}

static void sdk_fp_go_up(sdk_fp_state_t *s, jpp_sdk_context_t *ctx)
{
    char *slash = strrchr(s->dir, '/');
    if (slash != NULL && slash != s->dir) { *slash = '\0'; }
    sdk_fp_load(s, ctx);
    s->cursor  = 0u;
    s->scroll  = 0u;
    s->marquee = 0u;
}

jpp_sdk_status_t jpp_sdk_file_pick(
    jpp_sdk_context_t   *context,
    char                *out_path,
    size_t               out_path_len,
    jpp_sdk_ui_result_t *out_result)
{
    jpp_sdk_status_t status;

    status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) { return status; }
    if (out_path == NULL || out_path_len == 0u || out_result == NULL) {
        return JPP_SDK_STATUS_INVALID_ARGUMENT;
    }
    if (context->services.dir_list == NULL) {
        return JPP_SDK_STATUS_INVALID_STATE;
    }
    {
        jpp_sdk_status_t cap_st = jpp_sdk_ensure_cap(context, "files.full");
        if (cap_st != JPP_SDK_STATUS_OK) { return cap_st; }
    }

    static sdk_fp_state_t fp;
    memset(&fp, 0, sizeof(fp));
    jpp_sdk_copy_text(fp.root, JPP_SDK_PATH_MAX, "/sd");
    jpp_sdk_copy_text(fp.dir,  JPP_SDK_PATH_MAX, "/sd");
    sdk_fp_load(&fp, context);

    for (;;) {
        size_t total = sdk_fp_total(&fp);

        /* Keep cursor in view. */
        if (fp.cursor < fp.scroll) {
            fp.scroll = fp.cursor;
        } else if (total > SDK_FP_VISIBLE && fp.cursor >= fp.scroll + SDK_FP_VISIBLE) {
            fp.scroll = fp.cursor - SDK_FP_VISIBLE + 1u;
        }
        if (total >= SDK_FP_VISIBLE && fp.scroll + SDK_FP_VISIBLE > total) {
            fp.scroll = total - SDK_FP_VISIBLE;
        }

        /* Build text frame: row 0 = header, rows 1-6 = list items. */
        char body[JPP_SDK_FRAME_LINE_CAPACITY][JPP_SDK_FRAME_TEXT_MAX];
        const char *lines[JPP_SDK_FRAME_LINE_CAPACITY];
        jpp_sdk_copy_text(body[0], JPP_SDK_FRAME_TEXT_MAX, "Select a file:");
        lines[0] = body[0];
        size_t n = 1u;

        for (size_t v = 0u; v < SDK_FP_VISIBLE && n < JPP_SDK_FRAME_LINE_CAPACITY; v++) {
            size_t idx = fp.scroll + v;
            if (idx >= total) break;
            bool        sel  = (idx == fp.cursor);
            const char *name = sdk_fp_item_name(&fp, idx);
            size_t      nlen = strlen(name);
            size_t      off  = sdk_fp_marquee_off(fp.marquee, nlen);
            snprintf(body[n], JPP_SDK_FRAME_TEXT_MAX, "%c%.20s",
                     sel ? '>' : ' ', name + off);
            lines[n] = body[n];
            n++;
        }
        jpp_sdk_set_frame(context, lines, n);

        /* Wait for key with marquee timeout. */
        jpp_sdk_key_event_t key = JPP_SDK_KEY_NONE;
        jpp_sdk_wait_key(context, SDK_FP_MARQUEE_MS, &key);

        if (key == JPP_SDK_KEY_NONE) {
            fp.marquee++;
            continue;
        }

        fp.marquee = 0u;

        switch (key) {
        case JPP_SDK_KEY_UP:
            if (fp.cursor > 0u) { fp.cursor--; }
            break;
        case JPP_SDK_KEY_DOWN:
            if (fp.cursor + 1u < total) { fp.cursor++; }
            break;
        case JPP_SDK_KEY_CENTER:
            if (total == 0u) break;
            if (sdk_fp_item_is_dir(&fp, fp.cursor)) {
                if (fp.has_parent && fp.cursor == 0u) {
                    sdk_fp_go_up(&fp, context);
                } else {
                    size_t ei = fp.has_parent ? fp.cursor - 1u : fp.cursor;
                    char subname[SDK_FP_NAME_MAX];
                    strncpy(subname, fp.names[ei], SDK_FP_NAME_MAX - 1u);
                    subname[SDK_FP_NAME_MAX - 1u] = '\0';
                    size_t slen = strlen(subname);
                    if (slen > 0u && subname[slen - 1u] == '/') {
                        subname[slen - 1u] = '\0';
                    }
                    char newdir[JPP_SDK_PATH_MAX * 2];
                    snprintf(newdir, sizeof(newdir), "%s/%s", fp.dir, subname);
                    jpp_sdk_copy_text(fp.dir, JPP_SDK_PATH_MAX, newdir);
                    sdk_fp_load(&fp, context);
                    fp.cursor  = 0u;
                    fp.scroll  = 0u;
                    fp.marquee = 0u;
                }
            } else {
                size_t ei = fp.has_parent ? fp.cursor - 1u : fp.cursor;
                snprintf(out_path, out_path_len, "%s/%s", fp.dir, fp.names[ei]);
                *out_result = JPP_SDK_UI_OK;
                return JPP_SDK_STATUS_OK;
            }
            break;
        case JPP_SDK_KEY_CENTER_LONG:
            if (fp.has_parent) {
                sdk_fp_go_up(&fp, context);
            } else {
                out_path[0] = '\0';
                *out_result = JPP_SDK_UI_BACK;
                return JPP_SDK_STATUS_OK;
            }
            break;
        default:
            break;
        }
    }
}

/* ---- Wakelock ------------------------------------------------------------ */

jpp_sdk_status_t jpp_sdk_wakelock_acquire(jpp_sdk_context_t *context)
{
    jpp_sdk_status_t status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) { return status; }
    context->wakelock_held = true;
    return JPP_SDK_STATUS_OK;
}

jpp_sdk_status_t jpp_sdk_wakelock_release(jpp_sdk_context_t *context)
{
    jpp_sdk_status_t status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) { return status; }
    context->wakelock_held = false;
    return JPP_SDK_STATUS_OK;
}

/* ---- Buzzer -------------------------------------------------------------- */

jpp_sdk_status_t jpp_sdk_buzzer_play(jpp_sdk_context_t *context,
                                      jpp_buzzer_sound_t sound)
{
    jpp_sdk_status_t status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) { return status; }
    jpp_buzzer_status_t bs = jpp_buzzer_play(sound);
    return (bs == JPP_BUZZER_STATUS_OK) ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_INVALID_STATE;
}

jpp_sdk_status_t jpp_sdk_buzzer_tone(jpp_sdk_context_t *context,
                                      uint32_t freq_hz,
                                      uint32_t duration_ms)
{
    jpp_sdk_status_t status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) { return status; }
    jpp_buzzer_status_t bs = jpp_buzzer_tone(freq_hz, duration_ms);
    return (bs == JPP_BUZZER_STATUS_OK) ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_INVALID_STATE;
}

jpp_sdk_status_t jpp_sdk_buzzer_play_sequence(jpp_sdk_context_t *context,
                                               const jpp_buzzer_note_t *notes,
                                               size_t count)
{
    jpp_sdk_status_t status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) { return status; }
    if (notes == NULL && count > 0u) { return JPP_SDK_STATUS_INVALID_ARGUMENT; }
    jpp_buzzer_status_t bs = jpp_buzzer_play_sequence(notes, count);
    return (bs == JPP_BUZZER_STATUS_OK) ? JPP_SDK_STATUS_OK : JPP_SDK_STATUS_INVALID_STATE;
}

jpp_sdk_status_t jpp_sdk_buzzer_stop(jpp_sdk_context_t *context)
{
    jpp_sdk_status_t status = jpp_sdk_ensure_bound(context);
    if (status != JPP_SDK_STATUS_OK) { return status; }
    jpp_buzzer_stop();
    return JPP_SDK_STATUS_OK;
}

const char *jpp_sdk_status_name(jpp_sdk_status_t status)
{
    switch (status) {
    case JPP_SDK_STATUS_OK:
        return "OK";
    case JPP_SDK_STATUS_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case JPP_SDK_STATUS_INVALID_STATE:
        return "INVALID_STATE";
    case JPP_SDK_STATUS_ACCESS_DENIED:
        return "ACCESS_DENIED";
    case JPP_SDK_STATUS_TEXT_TRUNCATED:
        return "TEXT_TRUNCATED";
    case JPP_SDK_STATUS_BROKER_ERROR:
        return "BROKER_ERROR";
    case JPP_SDK_STATUS_HANDLE_LIMIT:
        return "HANDLE_LIMIT";
    case JPP_SDK_STATUS_INVALID_HANDLE:
        return "INVALID_HANDLE";
    case JPP_SDK_STATUS_BLE_CONN_LIMIT:
        return "BLE_CONN_LIMIT";
    case JPP_SDK_STATUS_INVALID_BLE_CONN:
        return "INVALID_BLE_CONN";
    }
    return "UNKNOWN";
}
