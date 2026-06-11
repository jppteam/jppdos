#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "jpp_resource_budget.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JPP_BROKER_RESULT_FIELD_CAPACITY JPP_RESOURCE_BROKER_RESULT_FIELD_LIMIT
#define JPP_BROKER_LOCK_CAPACITY JPP_RESOURCE_BROKER_LOCK_LIMIT

typedef enum {
    JPP_BROKER_STATUS_OK = 0,
    JPP_BROKER_STATUS_INVALID_ARGUMENT,
    JPP_BROKER_STATUS_FIELD_OVERFLOW,
} jpp_broker_status_t;

typedef struct {
    const char *key;
    const char *value;
} jpp_broker_result_field_t;

typedef struct {
    bool ok;
    const char *code;
    size_t field_count;
    jpp_broker_result_field_t fields[JPP_BROKER_RESULT_FIELD_CAPACITY];
} jpp_broker_result_t;

typedef struct {
    const char *const *capabilities;
    size_t capability_count;
} jpp_broker_caller_t;

typedef struct {
    const char *resource;
    bool locked;
} jpp_broker_lock_entry_t;

typedef struct {
    size_t entry_count;
    jpp_broker_lock_entry_t entries[JPP_BROKER_LOCK_CAPACITY];
} jpp_broker_lock_set_t;

typedef jpp_broker_status_t (*jpp_broker_exclusive_operation_t)(
    void *context,
    jpp_broker_result_t *result
);

jpp_broker_status_t jpp_broker_ok_result(jpp_broker_result_t *result);
jpp_broker_status_t jpp_broker_error_result(jpp_broker_result_t *result, const char *code);
jpp_broker_status_t jpp_broker_result_put(
    jpp_broker_result_t *result,
    const char *key,
    const char *value
);
const char *jpp_broker_result_get(const jpp_broker_result_t *result, const char *key);

bool jpp_broker_caller_has_capability(const jpp_broker_caller_t *caller, const char *capability);

jpp_broker_status_t jpp_broker_lock_set_init(jpp_broker_lock_set_t *lock_set);
jpp_broker_status_t jpp_broker_run_exclusive(
    jpp_broker_lock_set_t *lock_set,
    const char *resource,
    jpp_broker_exclusive_operation_t operation,
    void *context,
    jpp_broker_result_t *result
);

const char *jpp_broker_status_name(jpp_broker_status_t status);

#ifdef __cplusplus
}
#endif
