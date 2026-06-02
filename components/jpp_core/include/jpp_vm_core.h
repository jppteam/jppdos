#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jpp_boot_core.h"
#include "jpp_resource_budget.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JPP_VM_QUEUE_CAPACITY_MAX JPP_RESOURCE_VM_QUEUE_LIMIT_DEPTH
#define JPP_VM_TASK_NAME_MAX 32u
#define JPP_VM_APP_ID_MAX 64u
#define JPP_VM_MODULE_NAME_MAX 64u
#define JPP_VM_PATH_MAX 128u

typedef enum {
    JPP_VM_STATE_OFFLINE = 0,
    JPP_VM_STATE_WAITING_FOR_BOOT,
    JPP_VM_STATE_RUNNING,
} jpp_vm_state_t;

typedef enum {
    JPP_VM_STATUS_OK = 0,
    JPP_VM_STATUS_INVALID_ARGUMENT,
    JPP_VM_STATUS_INVALID_CONFIG,
    JPP_VM_STATUS_INVALID_STATE,
    JPP_VM_STATUS_BOOT_NOT_READY,
    JPP_VM_STATUS_QUEUE_FULL,
    JPP_VM_STATUS_ACCESS_DENIED,
    JPP_VM_STATUS_APP_NOT_ALLOWED,
    JPP_VM_STATUS_UNSUPPORTED_IMPORT,
    JPP_VM_STATUS_PATH_NOT_ALLOWED,
} jpp_vm_status_t;

typedef enum {
    JPP_VM_REQUEST_NONE = 0,
    JPP_VM_REQUEST_STARTUP,
    JPP_VM_REQUEST_FOREGROUND,
    JPP_VM_REQUEST_IDLE,
    JPP_VM_REQUEST_BACKGROUND,
    JPP_VM_REQUEST_IMPORT_MODULE,
    JPP_VM_REQUEST_ACTION,     /* handle_action(action_payload) lifecycle hook */
} jpp_vm_request_kind_t;

typedef enum {
    JPP_VM_IMPORT_SOURCE_CORE = 0,
    JPP_VM_IMPORT_SOURCE_SDK,
    JPP_VM_IMPORT_SOURCE_RUNTIME_PATH,
    JPP_VM_IMPORT_SOURCE_APP_PATH,
} jpp_vm_import_source_t;

typedef enum {
    JPP_VM_PATH_ROLE_RUNTIME = 0,
    JPP_VM_PATH_ROLE_SDK,
    JPP_VM_PATH_ROLE_APP,
} jpp_vm_path_role_t;

typedef struct {
    size_t heap_bytes;
    size_t stack_words;
    size_t queue_depth;
} jpp_vm_memory_config_t;

typedef struct {
    const char *app_root;
    const char *runtime_root;
    const char *sd_lib_root;
    const char *const *allowed_modules;
    size_t allowed_module_count;
} jpp_vm_import_surface_t;

typedef struct {
    const char *owner_task_name;
    jpp_vm_memory_config_t memory;
    jpp_vm_import_surface_t import_surface;
} jpp_vm_runtime_config_t;

typedef struct {
    jpp_vm_request_kind_t kind;
    char origin_task_name[JPP_VM_TASK_NAME_MAX];
    char app_id[JPP_VM_APP_ID_MAX];
    char module_name[JPP_VM_MODULE_NAME_MAX];
    char origin_path[JPP_VM_PATH_MAX];
    jpp_vm_import_source_t import_source;
    uint32_t action_payload;   /* used by JPP_VM_REQUEST_ACTION: cast to jpp_sdk_key_event_t */
} jpp_vm_request_t;

typedef struct {
    jpp_vm_runtime_config_t config;
    jpp_vm_state_t state;
    size_t queue_capacity;
    size_t queue_head;
    size_t queue_count;
    jpp_vm_request_t queue[JPP_VM_QUEUE_CAPACITY_MAX];
} jpp_vm_context_t;

void jpp_vm_context_init(jpp_vm_context_t *context);

jpp_vm_status_t jpp_vm_runtime_prepare(
    jpp_vm_context_t *context,
    const jpp_vm_runtime_config_t *config
);
jpp_vm_status_t jpp_vm_runtime_start(
    jpp_vm_context_t *context,
    const jpp_boot_context_t *boot_context
);
bool jpp_vm_boot_ready(const jpp_boot_context_t *boot_context);
bool jpp_vm_task_can_touch_python_objects(
    const jpp_vm_context_t *context,
    const char *task_name
);
jpp_vm_status_t jpp_vm_schedule_request(
    jpp_vm_context_t *context,
    const char *task_name,
    const jpp_vm_request_t *request
);
jpp_vm_status_t jpp_vm_owner_dequeue_request(
    jpp_vm_context_t *context,
    const char *task_name,
    jpp_vm_request_t *request
);
jpp_vm_status_t jpp_vm_check_import(
    const jpp_vm_context_t *context,
    const char *module_name,
    jpp_vm_import_source_t source,
    const char *origin_path
);
jpp_vm_status_t jpp_vm_check_sys_path_entry(
    const jpp_vm_context_t *context,
    const char *path,
    jpp_vm_path_role_t role
);

const char *jpp_vm_state_name(jpp_vm_state_t state);
const char *jpp_vm_status_name(jpp_vm_status_t status);
const char *jpp_vm_request_kind_name(jpp_vm_request_kind_t kind);
const char *jpp_vm_import_source_name(jpp_vm_import_source_t source);
const char *jpp_vm_path_role_name(jpp_vm_path_role_t role);

#ifdef __cplusplus
}
#endif
