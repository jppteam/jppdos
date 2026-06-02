#include "../include/jpp_broker_core.h"
#include "../include/jpp_string_util.h"

#include <string.h>

jpp_broker_status_t jpp_broker_ok_result(jpp_broker_result_t *result)
{
    if (result == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    result->ok = true;
    result->code = "OK";
    return JPP_BROKER_STATUS_OK;
}

jpp_broker_status_t jpp_broker_error_result(jpp_broker_result_t *result, const char *code)
{
    if (result == NULL || code == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    result->ok = false;
    result->code = code;
    return JPP_BROKER_STATUS_OK;
}

jpp_broker_status_t jpp_broker_result_put(
    jpp_broker_result_t *result,
    const char *key,
    const char *value
)
{
    if (result == NULL || key == NULL || value == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }
    if (result->field_count >= JPP_BROKER_RESULT_FIELD_CAPACITY) {
        return JPP_BROKER_STATUS_FIELD_OVERFLOW;
    }
    result->fields[result->field_count].key = key;
    result->fields[result->field_count].value = value;
    result->field_count += 1u;
    return JPP_BROKER_STATUS_OK;
}

const char *jpp_broker_result_get(const jpp_broker_result_t *result, const char *key)
{
    size_t index;
    if (result == NULL || key == NULL) {
        return NULL;
    }
    for (index = 0u; index < result->field_count; ++index) {
        if (jpp_str_eq(result->fields[index].key, key)) {
            return result->fields[index].value;
        }
    }
    return NULL;
}

bool jpp_broker_caller_has_capability(const jpp_broker_caller_t *caller, const char *capability)
{
    size_t index;
    if (caller == NULL || capability == NULL) {
        return false;
    }
    for (index = 0u; index < caller->capability_count; ++index) {
        if (jpp_str_eq(caller->capabilities[index], capability)) {
            return true;
        }
    }
    return false;
}

static jpp_broker_lock_entry_t *jpp_broker_find_lock(
    jpp_broker_lock_set_t *lock_set,
    const char *resource
)
{
    size_t index;
    for (index = 0u; index < lock_set->entry_count; ++index) {
        if (jpp_str_eq(lock_set->entries[index].resource, resource)) {
            return &lock_set->entries[index];
        }
    }
    return NULL;
}

jpp_broker_status_t jpp_broker_lock_set_init(jpp_broker_lock_set_t *lock_set)
{
    if (lock_set == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }
    memset(lock_set, 0, sizeof(*lock_set));
    return JPP_BROKER_STATUS_OK;
}

jpp_broker_status_t jpp_broker_run_exclusive(
    jpp_broker_lock_set_t *lock_set,
    const char *resource,
    jpp_broker_exclusive_operation_t operation,
    void *context,
    jpp_broker_result_t *result
)
{
    jpp_broker_lock_entry_t *entry;
    jpp_broker_status_t status;

    if (lock_set == NULL || resource == NULL || result == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }
    entry = jpp_broker_find_lock(lock_set, resource);
    if (entry != NULL && entry->locked) {
        status = jpp_broker_error_result(result, "BUSY");
        if (status != JPP_BROKER_STATUS_OK) {
            return status;
        }
        return jpp_broker_result_put(result, "resource", resource);
    }
    if (operation == NULL) {
        status = jpp_broker_error_result(result, "INVALID_OPERATION");
        if (status != JPP_BROKER_STATUS_OK) {
            return status;
        }
        return jpp_broker_result_put(result, "resource", resource);
    }
    if (entry == NULL) {
        if (lock_set->entry_count >= JPP_BROKER_LOCK_CAPACITY) {
            return JPP_BROKER_STATUS_FIELD_OVERFLOW;
        }
        entry = &lock_set->entries[lock_set->entry_count];
        entry->resource = resource;
        entry->locked = false;
        lock_set->entry_count += 1u;
    }

    entry->locked = true;
    status = operation(context, result);
    entry->locked = false;
    return status;
}

jpp_broker_status_t jpp_broker_require(
    const jpp_broker_caller_t *caller,
    const char *required,
    const char *operation,
    jpp_broker_result_t *result
)
{
    jpp_broker_status_t status;

    if (required == NULL || operation == NULL || result == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }
    if (jpp_broker_caller_has_capability(caller, "system") ||
        jpp_broker_caller_has_capability(caller, required)) {
        status = jpp_broker_ok_result(result);
        if (status != JPP_BROKER_STATUS_OK) {
            return status;
        }
        return jpp_broker_result_put(result, "required", required);
    }
    status = jpp_broker_error_result(result, "ACCESS_DENIED");
    if (status != JPP_BROKER_STATUS_OK) {
        return status;
    }
    status = jpp_broker_result_put(result, "required", required);
    if (status != JPP_BROKER_STATUS_OK) {
        return status;
    }
    return jpp_broker_result_put(result, "operation", operation);
}

const char *jpp_broker_status_name(jpp_broker_status_t status)
{
    switch (status) {
    case JPP_BROKER_STATUS_OK:
        return "OK";
    case JPP_BROKER_STATUS_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case JPP_BROKER_STATUS_FIELD_OVERFLOW:
        return "FIELD_OVERFLOW";
    }
    return "UNKNOWN";
}
