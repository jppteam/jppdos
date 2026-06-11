/*
 * jpp_mp_runner — executes a MicroPython app inside the JPPDOS runtime.
 *
 * Sequence:
 *   1. Allocate MicroPython GC heap from vm config memory limits.
 *   2. Initialize the interpreter (mp_init).
 *   3. Set the jppsdk module context to the provided sdk pointer.
 *   4. Install an import hook that calls jpp_vm_check_import() — any import
 *      that fails policy is surfaced as an ImportError in Python.
 *   5. Validate and populate sys.path via jpp_vm_check_sys_path_entry().
 *   6. Import the entry module by name (after its directory is on sys.path).
 *   7. Call create_app(jppsdk) to obtain the app object.
 *   8. Drive the lifecycle loop until sdk->close_requested is set.
 *   9. Call on_stop(), deinit the interpreter.
 */

#include "../include/jpp_mp_runner.h"
#include "../include/jpp_mp_sdk_module.h"
#include "../include/jpp_vm_core.h"
#include "../include/jpp_sdk_bridge.h"

/* STATIC was removed from MicroPython v1.20+; define it here for jpp_core usage. */
#define STATIC static

#include "py/gc.h"
#include "py/mphal.h"
#include "py/objexcept.h"
#include "py/obj.h"
#include "py/objexcept.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "py/builtin.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "jpp_app_pool.h"

#include <string.h>

static const char *TAG = "jpp_mp_runner";

/* jpp_mp_sdk_module is defined in jpp_mp_sdk_module.c (same component).
 * MP_REGISTER_MODULE in that file is only processed by makemoduledefs.py,
 * which does not scan jpp_core sources.  We register it manually into
 * sys.modules after mp_init() so Python code can do "import jppsdk". */
extern const mp_obj_module_t jpp_mp_sdk_module;

/* The GC heap uses the shared jpp_app_pool (see jpp_app_pool.h) — a single
 * static `.bss` buffer also used by the native loader for executable code.
 * Native and MicroPython apps are mutually exclusive, so one pool serves both,
 * keeping the static footprint out of the general heap that WiFi
 * management-frame buffers and lwIP pbufs are drawn from. */

/* -------------------------------------------------------------------------- */
/* Import hook                                                                 */
/* -------------------------------------------------------------------------- */

/* VM context pointer used by the import hook — set before Python runs. */
static jpp_vm_context_t *s_import_vm_ctx;

/*
 * Custom __import__ that validates each attempt against jpp_vm_core policy
 * before delegating to the standard MicroPython built-in loader.
 */
STATIC mp_obj_t mp_jpp_import(size_t n_args, const mp_obj_t *args)
{
    const char *name = mp_obj_str_get_str(args[0]);

    if (s_import_vm_ctx != NULL) {
        /* Determine the calling module's origin path from the 5th argument
           (__import__(name, globals, locals, fromlist, level) — if level == 0
           this is an absolute import; origin_path is irrelevant for the APP_PATH
           check but we supply it anyway so the policy log is useful. */
        const char *origin = "";
        if (n_args >= 5 && mp_obj_is_str(args[4])) {
            origin = mp_obj_str_get_str(args[4]);
        }

        jpp_vm_status_t st = jpp_vm_check_import(
            s_import_vm_ctx,
            name,
            JPP_VM_IMPORT_SOURCE_CORE,  /* conservative; policy checks name first */
            origin);
        if (st != JPP_VM_STATUS_OK) {
            mp_raise_msg_varg(&mp_type_ImportError,
                              MP_ERROR_TEXT("import '%s' blocked by policy"), name);
        }
    }

    return mp_builtin___import__(n_args, args);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_jpp_import_obj, 1, 5, mp_jpp_import);

/* -------------------------------------------------------------------------- */
/* sys.path population                                                         */
/* -------------------------------------------------------------------------- */

static void populate_sys_path(jpp_vm_context_t *vm)
{
    const jpp_vm_import_surface_t *surface = &vm->config.import_surface;

    const char *paths[] = {
        surface->app_root,
        surface->runtime_root,
        surface->sd_lib_root,
    };
    const jpp_vm_path_role_t roles[] = {
        JPP_VM_PATH_ROLE_APP,
        JPP_VM_PATH_ROLE_RUNTIME,
        JPP_VM_PATH_ROLE_SDK,
    };

    volatile size_t i;
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        nlr_buf_t nlr;
        if (paths[i] == NULL || paths[i][0] == '\0') {
            continue;
        }
        if (jpp_vm_check_sys_path_entry(vm, paths[i], roles[i]) != JPP_VM_STATUS_OK) {
            ESP_LOGW(TAG, "sys.path entry rejected: %s", paths[i]);
            continue;
        }
        if (nlr_push(&nlr) == 0) {
            mp_obj_list_append(mp_sys_path,
                mp_obj_new_str(paths[i], strlen(paths[i])));
            nlr_pop();
        } else {
            ESP_LOGW(TAG, "sys.path append failed: %s", paths[i]);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Lifecycle helpers                                                           */
/* -------------------------------------------------------------------------- */

/*
 * Attempt to call a named method on app_obj.
 * Returns true if the hook succeeded or is absent.
 * Returns false and logs the traceback if a Python exception escapes.
 */
static bool call_hook(jpp_sdk_context_t *sdk,
                      mp_obj_t app_obj,
                      const char *hook_name)
{
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t dest[2];
        mp_load_method_maybe(app_obj, qstr_from_str(hook_name), dest);
        if (dest[0] != MP_OBJ_NULL) {
            if (dest[1] != MP_OBJ_NULL) {
                mp_call_method_n_kw(0, 0, dest);
            } else {
                mp_call_function_0(dest[0]);
            }
        }
        nlr_pop();
        return true;
    } else {
        mp_obj_t exc = (mp_obj_t)nlr.ret_val;
        nlr_buf_t inner;
        if (nlr_push(&inner) == 0) {
            vstr_t vstr;
            mp_print_t print;
            vstr_init_print(&vstr, 128, &print);
            mp_obj_print_exception(&print, exc);
            ESP_LOGE(TAG, "hook %s raised: %.*s", hook_name, (int)vstr.len, vstr.buf);
            jpp_sdk_log(sdk, vstr.buf);
            vstr_clear(&vstr);
            nlr_pop();
        } else {
            ESP_LOGE(TAG, "hook %s: exception (formatting failed)", hook_name);
        }
        return false;
    }
}

/*
 * Call handle_action(key_int) on app_obj with one integer argument.
 * Same nlr pattern as call_hook — absent hook is OK, exception is logged.
 */
static bool call_hook_with_int(jpp_sdk_context_t *sdk,
                               mp_obj_t app_obj,
                               const char *hook_name,
                               mp_int_t arg)
{
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t dest[2];
        mp_load_method_maybe(app_obj, qstr_from_str(hook_name), dest);
        if (dest[0] != MP_OBJ_NULL) {
            mp_obj_t arg_obj = mp_obj_new_int(arg);
            if (dest[1] != MP_OBJ_NULL) {
                mp_obj_t call_args[3] = { dest[0], dest[1], arg_obj };
                mp_call_method_n_kw(1, 0, call_args);
            } else {
                mp_call_function_1(dest[0], arg_obj);
            }
        }
        nlr_pop();
        return true;
    } else {
        mp_obj_t exc = (mp_obj_t)nlr.ret_val;
        nlr_buf_t inner;
        if (nlr_push(&inner) == 0) {
            vstr_t vstr;
            mp_print_t print;
            vstr_init_print(&vstr, 128, &print);
            mp_obj_print_exception(&print, exc);
            ESP_LOGE(TAG, "hook %s raised: %.*s", hook_name, (int)vstr.len, vstr.buf);
            jpp_sdk_log(sdk, vstr.buf);
            vstr_clear(&vstr);
            nlr_pop();
        } else {
            ESP_LOGE(TAG, "hook %s: exception (formatting failed)", hook_name);
        }
        return false;
    }
}

/* -------------------------------------------------------------------------- */
/* Runner entry point                                                          */
/* -------------------------------------------------------------------------- */

/* Shared body for the foreground lifecycle run (bg_task_name == NULL) and a
   headless background-task run (bg_task_name set: call on_task(name) once). */
static jpp_mp_runner_result_t runner_run_internal(
    jpp_sdk_context_t *sdk,
    jpp_vm_context_t  *vm,
    const char        *entry_mpy_path,
    const char        *bg_task_name)
{
    volatile jpp_mp_runner_result_t outcome = JPP_MP_RUNNER_OK;

    /* Acquire the shared app pool for the GC heap.  Native apps use the same
       pool for code, but apps are mutually exclusive so it is free here. */
    size_t pool_sz = 0u;
    void  *heap    = jpp_app_pool_acquire(0u, &pool_sz);
    if (heap == NULL) {
        ESP_LOGE(TAG, "GC heap: app pool unavailable");
        return JPP_MP_RUNNER_LOAD_FAILED;
    }

    size_t heap_bytes = vm->config.memory.heap_bytes;
    if (heap_bytes == 0 || heap_bytes > pool_sz) {
        heap_bytes = pool_sz;
    }

    mp_stack_ctrl_init();
    /* Set Python recursion depth limit.  MICROPY_STACK_CHECK compares
     * mp_stack_usage() against this value; without a non-zero limit the first
     * check always fires (usage >= 0).  The task stack is SD_APP_TASK_STACK_BYTES
     * (12 KB); reserve ~5 KB for C frames below the ctrl_init point and the
     * POSIX file-reader call chain during .mpy loading, leaving 7 KB for Python. */
    mp_stack_set_limit(7u * 1024u);
    gc_init(heap, (uint8_t *)heap + heap_bytes);
    mp_init();

    /* Register jppsdk in sys.modules.  makemoduledefs.py only scans the
     * micropython component, so MP_REGISTER_MODULE in jpp_mp_sdk_module.c
     * (jpp_core) never makes it into the generated module table. */
    mp_obj_dict_store(
        MP_OBJ_FROM_PTR(&MP_STATE_VM(mp_loaded_modules_dict)),
        MP_OBJ_NEW_QSTR(MP_QSTR_jppsdk),
        MP_OBJ_FROM_PTR(&jpp_mp_sdk_module));

    jpp_mp_sdk_module_set_context(sdk);

    s_import_vm_ctx = vm;
    mp_store_name(MP_QSTR___import__, MP_OBJ_FROM_PTR(&mp_jpp_import_obj));

    populate_sys_path(vm);

    /* Derive the module name from the entry path basename, stripping ".mpy".
       e.g. "main.mpy" → "main", "myapp/core.mpy" → "core" */
    const char *basename = strrchr(entry_mpy_path, '/');
    basename = (basename != NULL) ? basename + 1 : entry_mpy_path;
    char module_name[64] = {0};
    const char *dot = strrchr(basename, '.');
    size_t name_len = (dot != NULL)
        ? (size_t)(dot - basename)
        : strlen(basename);
    if (name_len >= sizeof(module_name)) {
        name_len = sizeof(module_name) - 1u;
    }
    memcpy(module_name, basename, name_len);

    mp_obj_t app_obj = MP_OBJ_NULL;

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        /* Import the entry module by name — MicroPython will find its .mpy
           file on sys.path and invoke the persistent-code loader. */
        mp_obj_t name_obj = mp_obj_new_str(module_name, strlen(module_name));
        mp_obj_t import_args[] = { name_obj };
        mp_obj_t module = mp_builtin___import__(1, import_args);

        if (module == MP_OBJ_NULL || module == mp_const_none) {
            outcome = JPP_MP_RUNNER_LOAD_FAILED;
        } else if (bg_task_name != NULL) {
            /* Headless: call module-level on_task(name) once. mp_load_attr
               raises (caught below) when the module exports no on_task. */
            mp_obj_t hook = mp_load_attr(module, MP_QSTR_on_task);
            mp_call_function_1(hook,
                mp_obj_new_str(bg_task_name, strlen(bg_task_name)));
        } else {
            /* Resolve create_app from the module. */
            mp_obj_t factory = mp_load_attr(module, MP_QSTR_create_app);
            if (factory == MP_OBJ_NULL || factory == mp_const_none) {
                outcome = JPP_MP_RUNNER_NO_FACTORY;
            } else {
                /* Import jppsdk and pass it to create_app(sdk). */
                mp_obj_t sdk_name_obj = mp_obj_new_str("jppsdk", 6);
                mp_obj_t sdk_import_args[] = { sdk_name_obj };
                mp_obj_t jppsdk_obj =
                    mp_builtin___import__(1, sdk_import_args);
                app_obj = mp_call_function_1(factory, jppsdk_obj);
            }
        }
        nlr_pop();
    } else {
        mp_obj_t exc = (mp_obj_t)nlr.ret_val;
        /* Log exception type without any heap allocation — safe even under OOM. */
        ESP_LOGE(TAG, "load error type: %s",
                 qstr_str(mp_obj_get_type(exc)->name));
        nlr_buf_t inner;
        if (nlr_push(&inner) == 0) {
            vstr_t vstr;
            mp_print_t print;
            vstr_init_print(&vstr, 256, &print);
            mp_obj_print_exception(&print, exc);
            ESP_LOGE(TAG, "load error: %.*s", (int)vstr.len, vstr.buf);
            jpp_sdk_log(sdk, vstr.buf);
            vstr_clear(&vstr);
            nlr_pop();
        } else {
            /* GC heap too tight for mp_obj_print_exception — extract
             * traceback and message from pre-allocated data (no GC). */
            ESP_LOGE(TAG, "load error: %s (traceback unavailable)",
                     qstr_str(mp_obj_get_type(exc)->name));
            size_t tb_n = 0;
            size_t *tb_v = NULL;
            mp_obj_exception_get_traceback(exc, &tb_n, &tb_v);
            for (size_t j = 0; j + 2 < tb_n; j += 3) {
                ESP_LOGE(TAG, "  File \"%s\", line %zu, in %s",
                         qstr_str((qstr)tb_v[j]),
                         tb_v[j + 1],
                         qstr_str((qstr)tb_v[j + 2]));
            }
            if (mp_obj_is_exception_instance(exc)) {
                mp_obj_exception_t *ep = MP_OBJ_TO_PTR(exc);
                if (ep->args != NULL && ep->args->len > 0) {
                    size_t msg_len = 0;
                    const char *msg =
                        mp_obj_str_get_data(ep->args->items[0], &msg_len);
                    if (msg != NULL) {
                        ESP_LOGE(TAG, "  msg: %.*s", (int)msg_len, msg);
                    }
                }
            }
        }
        outcome = JPP_MP_RUNNER_LOAD_FAILED;
    }

    /* Headless runs are done after on_task; the lifecycle loop is for the
       foreground path only. */
    if (outcome != JPP_MP_RUNNER_OK || app_obj == MP_OBJ_NULL) {
        goto cleanup;
    }

    /* Lifecycle: on_start */
    if (!call_hook(sdk, app_obj, "on_start")) {
        outcome = JPP_MP_RUNNER_HOOK_ERROR;
        goto stop_hook;
    }

    /* Main lifecycle loop — drain the VM request queue. */
    const char *owner_task = pcTaskGetName(NULL);
    while (!sdk->close_requested) {
        jpp_vm_request_t req;
        memset(&req, 0, sizeof(req));
        jpp_vm_status_t vst =
            jpp_vm_owner_dequeue_request(vm, owner_task, &req);

        if (vst != JPP_VM_STATUS_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        bool hook_ok = true;
        switch (req.kind) {
        case JPP_VM_REQUEST_IDLE:
            hook_ok = call_hook(sdk, app_obj, "on_idle");
            break;
        case JPP_VM_REQUEST_ACTION:
            hook_ok = call_hook_with_int(sdk, app_obj, "handle_action",
                                         (mp_int_t)req.action_payload);
            break;
        default:
            break;
        }

        if (!hook_ok) {
            outcome = JPP_MP_RUNNER_HOOK_ERROR;
            break;
        }
    }

stop_hook:
    call_hook(sdk, app_obj, "on_stop");

cleanup:
    s_import_vm_ctx = NULL;
    jpp_mp_sdk_module_set_context(NULL);
    mp_deinit();
    gc_sweep_all();
    jpp_app_pool_release();

    return outcome;
}

jpp_mp_runner_result_t jpp_mp_runner_run(
    jpp_sdk_context_t *sdk,
    jpp_vm_context_t  *vm,
    const char        *entry_mpy_path)
{
    return runner_run_internal(sdk, vm, entry_mpy_path, NULL);
}

jpp_mp_runner_result_t jpp_mp_runner_run_task(
    jpp_sdk_context_t *sdk,
    jpp_vm_context_t  *vm,
    const char        *entry_mpy_path,
    const char        *task_name)
{
    return runner_run_internal(sdk, vm, entry_mpy_path, task_name);
}

const char *jpp_mp_runner_result_name(jpp_mp_runner_result_t result)
{
    switch (result) {
    case JPP_MP_RUNNER_OK:           return "OK";
    case JPP_MP_RUNNER_LOAD_FAILED:  return "LOAD_FAILED";
    case JPP_MP_RUNNER_NO_FACTORY:   return "NO_FACTORY";
    case JPP_MP_RUNNER_HOOK_ERROR:   return "HOOK_ERROR";
    default:                         return "UNKNOWN";
    }
}
