#include "../include/jpp_vm_core.h"
#include "../include/jpp_string_util.h"

#include <string.h>

static bool jpp_vm_task_name_matches(const char *left, const char *right)
{
    return jpp_str_eq(left, right) && jpp_str_nonempty(left);
}

static bool jpp_vm_module_is_blocked(const char *module_name);

static bool jpp_vm_name_is_valid(const char *name)
{
    size_t index;
    bool previous_was_dot = false;

    if (!jpp_str_nonempty(name)) {
        return false;
    }
    if (name[0] == '.') {
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

static bool jpp_vm_app_id_is_valid(const char *app_id)
{
    if (!jpp_vm_name_is_valid(app_id)) {
        return false;
    }
    return strcmp(app_id, "settings") != 0;
}

static bool jpp_vm_has_parent_traversal(const char *path)
{
    size_t index;

    if (!jpp_str_nonempty(path)) {
        return true;
    }
    for (index = 0u; path[index] != '\0'; ++index) {
        if (path[index] != '.') {
            continue;
        }
        if (path[index + 1u] != '.') {
            continue;
        }
        if ((index == 0u || path[index - 1u] == '/')
            && (path[index + 2u] == '\0' || path[index + 2u] == '/')) {
            return true;
        }
    }
    return false;
}

static bool jpp_vm_path_is_under_root(const char *path, const char *root)
{
    size_t root_length;

    if (!jpp_str_nonempty(path) || !jpp_str_nonempty(root) || path[0] != '/') {
        return false;
    }
    if (jpp_vm_has_parent_traversal(path)) {
        return false;
    }
    root_length = strlen(root);
    if (strncmp(path, root, root_length) != 0) {
        return false;
    }
    return path[root_length] == '\0' || path[root_length] == '/';
}

static bool jpp_vm_app_root_is_valid(const char *path)
{
    static const char sd_prefix[]   = "/sd/apps/";
    static const char data_prefix[] = "/data/apps/";
    size_t prefix_length;

    if (!jpp_str_nonempty(path)) {
        return false;
    }
    if (strncmp(path, sd_prefix, sizeof(sd_prefix) - 1u) == 0) {
        prefix_length = sizeof(sd_prefix) - 1u;
    } else if (strncmp(path, data_prefix, sizeof(data_prefix) - 1u) == 0) {
        prefix_length = sizeof(data_prefix) - 1u;
    } else {
        return false;
    }
    if (path[prefix_length] == '\0') {
        return false;
    }
    return !jpp_vm_has_parent_traversal(path);
}

static bool jpp_vm_import_surface_is_valid(const jpp_vm_import_surface_t *surface)
{
    size_t index;

    if (surface == NULL) {
        return false;
    }
    if (!jpp_vm_app_root_is_valid(surface->app_root)) {
        return false;
    }
    if (!jpp_vm_task_name_matches(surface->runtime_root, "/lib")) {
        return false;
    }
    if (!jpp_vm_task_name_matches(surface->sd_lib_root, "/sd/lib")) {
        return false;
    }
    if (surface->allowed_modules == NULL || surface->allowed_module_count == 0u) {
        return false;
    }
    for (index = 0u; index < surface->allowed_module_count; ++index) {
        if (!jpp_vm_name_is_valid(surface->allowed_modules[index])) {
            return false;
        }
        if (jpp_vm_module_is_blocked(surface->allowed_modules[index])) {
            return false;
        }
    }
    return true;
}

static bool jpp_vm_runtime_config_is_valid(const jpp_vm_runtime_config_t *config)
{
    if (config == NULL) {
        return false;
    }
    if (!jpp_str_nonempty(config->owner_task_name)) {
        return false;
    }
    if (config->memory.heap_bytes == 0u || config->memory.stack_words == 0u) {
        return false;
    }
    if (config->memory.heap_bytes > JPP_RESOURCE_VM_HEAP_LIMIT_BYTES) {
        return false;
    }
    if (config->memory.stack_words > JPP_RESOURCE_VM_STACK_LIMIT_WORDS) {
        return false;
    }
    if (config->memory.queue_depth == 0u || config->memory.queue_depth > JPP_VM_QUEUE_CAPACITY_MAX) {
        return false;
    }
    return jpp_vm_import_surface_is_valid(&config->import_surface);
}

static bool jpp_vm_allowed_module_matches(const char *allowed_module, const char *module_name)
{
    size_t length;

    if (!jpp_str_nonempty(allowed_module) || !jpp_str_nonempty(module_name)) {
        return false;
    }
    length = strlen(allowed_module);
    if (strncmp(allowed_module, module_name, length) != 0) {
        return false;
    }
    return module_name[length] == '\0' || module_name[length] == '.';
}

static bool jpp_vm_module_is_blocked(const char *module_name)
{
    static const char *const blocked_modules[] = {
        "machine",
        "network",
        "socket",
        "os",
        "uos",
        "esp",
        "esp32",
    };
    size_t index;

    if (!jpp_str_nonempty(module_name)) {
        return true;
    }
    for (index = 0u; index < sizeof(blocked_modules) / sizeof(blocked_modules[0]); ++index) {
        if (jpp_vm_allowed_module_matches(blocked_modules[index], module_name)) {
            return true;
        }
    }
    return false;
}

static bool jpp_vm_module_is_allowed(const jpp_vm_import_surface_t *surface, const char *module_name)
{
    size_t index;

    if (surface == NULL || !jpp_vm_name_is_valid(module_name) || jpp_vm_module_is_blocked(module_name)) {
        return false;
    }
    for (index = 0u; index < surface->allowed_module_count; ++index) {
        if (jpp_vm_allowed_module_matches(surface->allowed_modules[index], module_name)) {
            return true;
        }
    }
    return false;
}

static bool jpp_vm_request_targets_sd_app(const jpp_vm_request_t *request)
{
    if (request == NULL) {
        return false;
    }
    switch (request->kind) {
    case JPP_VM_REQUEST_STARTUP:
    case JPP_VM_REQUEST_FOREGROUND:
    case JPP_VM_REQUEST_IDLE:
    case JPP_VM_REQUEST_BACKGROUND:
    case JPP_VM_REQUEST_IMPORT_MODULE:
    case JPP_VM_REQUEST_ACTION:
        return jpp_vm_app_id_is_valid(request->app_id);
    case JPP_VM_REQUEST_NONE:
        return false;
    }
    return false;
}

static bool jpp_vm_request_is_valid(const jpp_vm_request_t *request)
{
    if (!jpp_vm_request_targets_sd_app(request)) {
        return false;
    }
    if (request->kind == JPP_VM_REQUEST_IMPORT_MODULE) {
        if (!jpp_str_nonempty(request->module_name)) {
            return false;
        }
        if (request->import_source > JPP_VM_IMPORT_SOURCE_APP_PATH) {
            return false;
        }
    }
    return true;
}

void jpp_vm_context_init(jpp_vm_context_t *context)
{
    if (context == NULL) {
        return;
    }
    memset(context, 0, sizeof(*context));
    context->state = JPP_VM_STATE_OFFLINE;
}

jpp_vm_status_t jpp_vm_runtime_prepare(
    jpp_vm_context_t *context,
    const jpp_vm_runtime_config_t *config
)
{
    if (context == NULL || config == NULL) {
        return JPP_VM_STATUS_INVALID_ARGUMENT;
    }
    if (!jpp_vm_runtime_config_is_valid(config)) {
        return JPP_VM_STATUS_INVALID_CONFIG;
    }
    jpp_vm_context_init(context);
    context->config = *config;
    context->queue_capacity = config->memory.queue_depth;
    context->state = JPP_VM_STATE_WAITING_FOR_BOOT;
    return JPP_VM_STATUS_OK;
}

bool jpp_vm_boot_ready(const jpp_boot_context_t *boot_context)
{
    if (boot_context == NULL) {
        return false;
    }
    if (!boot_context->flash_layout_ready || !boot_context->settings_available) {
        return false;
    }
    if (!boot_context->runtime_paths_ready || !boot_context->services_ready) {
        return false;
    }
    if (!boot_context->system_ready) {
        return false;
    }
    return true;
}

jpp_vm_status_t jpp_vm_runtime_start(
    jpp_vm_context_t *context,
    const jpp_boot_context_t *boot_context
)
{
    if (context == NULL || boot_context == NULL) {
        return JPP_VM_STATUS_INVALID_ARGUMENT;
    }
    if (context->state != JPP_VM_STATE_WAITING_FOR_BOOT) {
        return JPP_VM_STATUS_INVALID_STATE;
    }
    if (!jpp_vm_boot_ready(boot_context)) {
        return JPP_VM_STATUS_BOOT_NOT_READY;
    }
    context->state = JPP_VM_STATE_RUNNING;
    return JPP_VM_STATUS_OK;
}

bool jpp_vm_task_can_touch_python_objects(
    const jpp_vm_context_t *context,
    const char *task_name
)
{
    if (context == NULL || context->state != JPP_VM_STATE_RUNNING) {
        return false;
    }
    return jpp_vm_task_name_matches(context->config.owner_task_name, task_name);
}

jpp_vm_status_t jpp_vm_schedule_request(
    jpp_vm_context_t *context,
    const char *task_name,
    const jpp_vm_request_t *request
)
{
    size_t insert_index;

    if (context == NULL || request == NULL || !jpp_str_nonempty(task_name)) {
        return JPP_VM_STATUS_INVALID_ARGUMENT;
    }
    if (context->state != JPP_VM_STATE_RUNNING) {
        return JPP_VM_STATUS_INVALID_STATE;
    }
    if (!jpp_vm_request_is_valid(request)) {
        return JPP_VM_STATUS_APP_NOT_ALLOWED;
    }
    if (context->queue_count >= context->queue_capacity) {
        return JPP_VM_STATUS_QUEUE_FULL;
    }
    insert_index = (context->queue_head + context->queue_count) % context->queue_capacity;
    context->queue[insert_index] = *request;
    strncpy(context->queue[insert_index].origin_task_name, task_name, JPP_VM_TASK_NAME_MAX - 1u);
    context->queue[insert_index].origin_task_name[JPP_VM_TASK_NAME_MAX - 1u] = '\0';
    context->queue_count += 1u;
    return JPP_VM_STATUS_OK;
}

jpp_vm_status_t jpp_vm_owner_dequeue_request(
    jpp_vm_context_t *context,
    const char *task_name,
    jpp_vm_request_t *request
)
{
    if (context == NULL || request == NULL || !jpp_str_nonempty(task_name)) {
        return JPP_VM_STATUS_INVALID_ARGUMENT;
    }
    if (context->state != JPP_VM_STATE_RUNNING) {
        return JPP_VM_STATUS_INVALID_STATE;
    }
    if (!jpp_vm_task_can_touch_python_objects(context, task_name)) {
        return JPP_VM_STATUS_ACCESS_DENIED;
    }
    if (context->queue_count == 0u) {
        return JPP_VM_STATUS_INVALID_STATE;
    }
    *request = context->queue[context->queue_head];
    context->queue_head = (context->queue_head + 1u) % context->queue_capacity;
    context->queue_count -= 1u;
    return JPP_VM_STATUS_OK;
}

jpp_vm_status_t jpp_vm_check_import(
    const jpp_vm_context_t *context,
    const char *module_name,
    jpp_vm_import_source_t source,
    const char *origin_path
)
{
    const jpp_vm_import_surface_t *surface;

    if (context == NULL || !jpp_str_nonempty(module_name)) {
        return JPP_VM_STATUS_INVALID_ARGUMENT;
    }
    if (context->state != JPP_VM_STATE_RUNNING) {
        return JPP_VM_STATUS_INVALID_STATE;
    }
    if (!jpp_vm_name_is_valid(module_name)) {
        return JPP_VM_STATUS_INVALID_ARGUMENT;
    }
    if (jpp_vm_module_is_blocked(module_name)) {
        return JPP_VM_STATUS_UNSUPPORTED_IMPORT;
    }
    surface = &context->config.import_surface;
    switch (source) {
    case JPP_VM_IMPORT_SOURCE_CORE:
    case JPP_VM_IMPORT_SOURCE_SDK:
        return jpp_vm_module_is_allowed(surface, module_name)
            ? JPP_VM_STATUS_OK
            : JPP_VM_STATUS_UNSUPPORTED_IMPORT;
    case JPP_VM_IMPORT_SOURCE_RUNTIME_PATH:
        if (!jpp_vm_module_is_allowed(surface, module_name)) {
            return JPP_VM_STATUS_UNSUPPORTED_IMPORT;
        }
        if (jpp_vm_path_is_under_root(origin_path, surface->runtime_root)
            || jpp_vm_path_is_under_root(origin_path, surface->sd_lib_root)) {
            return JPP_VM_STATUS_OK;
        }
        return JPP_VM_STATUS_PATH_NOT_ALLOWED;
    case JPP_VM_IMPORT_SOURCE_APP_PATH:
        if (jpp_vm_path_is_under_root(origin_path, surface->app_root)) {
            return JPP_VM_STATUS_OK;
        }
        return JPP_VM_STATUS_PATH_NOT_ALLOWED;
    }
    return JPP_VM_STATUS_INVALID_ARGUMENT;
}

jpp_vm_status_t jpp_vm_check_sys_path_entry(
    const jpp_vm_context_t *context,
    const char *path,
    jpp_vm_path_role_t role
)
{
    const jpp_vm_import_surface_t *surface;

    if (context == NULL || !jpp_str_nonempty(path)) {
        return JPP_VM_STATUS_INVALID_ARGUMENT;
    }
    if (context->state != JPP_VM_STATE_RUNNING) {
        return JPP_VM_STATUS_INVALID_STATE;
    }
    surface = &context->config.import_surface;
    switch (role) {
    case JPP_VM_PATH_ROLE_RUNTIME:
        return jpp_vm_path_is_under_root(path, surface->runtime_root)
            ? JPP_VM_STATUS_OK
            : JPP_VM_STATUS_PATH_NOT_ALLOWED;
    case JPP_VM_PATH_ROLE_SDK:
        return jpp_vm_path_is_under_root(path, surface->sd_lib_root)
            ? JPP_VM_STATUS_OK
            : JPP_VM_STATUS_PATH_NOT_ALLOWED;
    case JPP_VM_PATH_ROLE_APP:
        return jpp_vm_path_is_under_root(path, surface->app_root)
            ? JPP_VM_STATUS_OK
            : JPP_VM_STATUS_PATH_NOT_ALLOWED;
    }
    return JPP_VM_STATUS_INVALID_ARGUMENT;
}

const char *jpp_vm_state_name(jpp_vm_state_t state)
{
    switch (state) {
    case JPP_VM_STATE_OFFLINE:
        return "OFFLINE";
    case JPP_VM_STATE_WAITING_FOR_BOOT:
        return "WAITING_FOR_BOOT";
    case JPP_VM_STATE_RUNNING:
        return "RUNNING";
    }
    return "UNKNOWN";
}

const char *jpp_vm_status_name(jpp_vm_status_t status)
{
    switch (status) {
    case JPP_VM_STATUS_OK:
        return "OK";
    case JPP_VM_STATUS_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case JPP_VM_STATUS_INVALID_CONFIG:
        return "INVALID_CONFIG";
    case JPP_VM_STATUS_INVALID_STATE:
        return "INVALID_STATE";
    case JPP_VM_STATUS_BOOT_NOT_READY:
        return "BOOT_NOT_READY";
    case JPP_VM_STATUS_QUEUE_FULL:
        return "QUEUE_FULL";
    case JPP_VM_STATUS_ACCESS_DENIED:
        return "ACCESS_DENIED";
    case JPP_VM_STATUS_APP_NOT_ALLOWED:
        return "APP_NOT_ALLOWED";
    case JPP_VM_STATUS_UNSUPPORTED_IMPORT:
        return "UNSUPPORTED_IMPORT";
    case JPP_VM_STATUS_PATH_NOT_ALLOWED:
        return "PATH_NOT_ALLOWED";
    }
    return "UNKNOWN";
}

const char *jpp_vm_request_kind_name(jpp_vm_request_kind_t kind)
{
    switch (kind) {
    case JPP_VM_REQUEST_NONE:
        return "NONE";
    case JPP_VM_REQUEST_STARTUP:
        return "STARTUP";
    case JPP_VM_REQUEST_FOREGROUND:
        return "FOREGROUND";
    case JPP_VM_REQUEST_IDLE:
        return "IDLE";
    case JPP_VM_REQUEST_BACKGROUND:
        return "BACKGROUND";
    case JPP_VM_REQUEST_IMPORT_MODULE:
        return "IMPORT_MODULE";
    case JPP_VM_REQUEST_ACTION:
        return "ACTION";
    }
    return "UNKNOWN";
}

const char *jpp_vm_import_source_name(jpp_vm_import_source_t source)
{
    switch (source) {
    case JPP_VM_IMPORT_SOURCE_CORE:
        return "core";
    case JPP_VM_IMPORT_SOURCE_SDK:
        return "sdk";
    case JPP_VM_IMPORT_SOURCE_RUNTIME_PATH:
        return "runtime_path";
    case JPP_VM_IMPORT_SOURCE_APP_PATH:
        return "app_path";
    }
    return "unknown";
}

const char *jpp_vm_path_role_name(jpp_vm_path_role_t role)
{
    switch (role) {
    case JPP_VM_PATH_ROLE_RUNTIME:
        return "runtime";
    case JPP_VM_PATH_ROLE_SDK:
        return "sdk";
    case JPP_VM_PATH_ROLE_APP:
        return "app";
    }
    return "unknown";
}
