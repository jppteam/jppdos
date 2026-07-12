#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "jpp_broker_core.h"
#include "jpp_resource_budget.h"
#include "jpp_vm_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JPP_SDK_PENDING_CAP_MAX 16u  /* must be >= SD_MANIFEST_CAP_MAX */

#define JPP_SDK_FRAME_LINE_CAPACITY JPP_RESOURCE_SDK_FRAME_LINE_LIMIT
#define JPP_SDK_FRAME_TEXT_MAX 64u
#define JPP_SDK_APP_ID_MAX JPP_VM_APP_ID_MAX
#define JPP_SDK_PATH_MAX 160u
#define JPP_SDK_HANDLE_CAPACITY JPP_RESOURCE_SDK_HANDLE_LIMIT
#define JPP_SDK_HANDLE_INVALID 0xFFu

/* Canvas constants — 128px wide × 48px content area (pages 2–7) by default.
   With jpp_sdk_canvas_fullscreen(true) the canvas covers all 64 rows
   (pages 0–7) and the frame text rows / title rule are not rendered. */
#define JPP_SDK_CANVAS_ROW_BYTES  16u   /* 128 pixels / 8 = 16 bytes per row */
#define JPP_SDK_CANVAS_ROWS       48u   /* pixel rows in the windowed content area */
#define JPP_SDK_CANVAS_ROWS_FULL  64u   /* pixel rows in fullscreen mode */

/* BLE constants */
#define JPP_SDK_BLE_CONN_CAPACITY JPP_RESOURCE_SDK_BLE_CONN_LIMIT
#define JPP_SDK_BLE_CONN_INVALID  0xFFu
#define JPP_SDK_BLE_ADDR_LEN      18u   /* "AA:BB:CC:DD:EE:FF\0" */
#define JPP_SDK_BLE_UUID_LEN      37u   /* "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX\0" */
#define JPP_SDK_BLE_NAME_MAX      32u
#define JPP_SDK_BLE_AD_PAYLOAD_MAX 31u  /* maximum raw BLE advertisement bytes */
#define JPP_SDK_BLE_SCAN_MAX      20u   /* maximum results returned by one scan */

/*
 * Generic GATT app-host service (ble.host).
 *
 * The firmware pre-registers a single, reusable host service: one readable TX
 * characteristic (the app publishes bytes a connected peer can read) and one
 * writable RX characteristic (a connected peer writes bytes the app receives).
 * An app claims the host role with jpp_sdk_ble_service_register(svc_uuid) using
 * JPP_SDK_BLE_HOST_SVC_UUID, then exchanges raw bytes through the host helpers
 * (jpp_sdk_ble_host_set_value / jpp_sdk_ble_host_wait_write). Apps acting as the
 * GATT client read/write these same UUIDs on the peer. There is nothing app- or
 * protocol-specific about the service; apps distinguish their protocol via their
 * own advertisement payload.
 */
#define JPP_SDK_BLE_HOST_SVC_UUID "4a505300-0000-0000-0000-000000000000"
#define JPP_SDK_BLE_HOST_TX_UUID  "4a505301-0000-0000-0000-000000000000"
#define JPP_SDK_BLE_HOST_RX_UUID  "4a505302-0000-0000-0000-000000000000"
#define JPP_SDK_BLE_HOST_RX_MAX   4096u  /* max bytes one RX write can deliver */

typedef enum {
    JPP_SDK_STATUS_OK = 0,
    JPP_SDK_STATUS_INVALID_ARGUMENT,
    JPP_SDK_STATUS_INVALID_STATE,
    JPP_SDK_STATUS_ACCESS_DENIED,
    JPP_SDK_STATUS_TEXT_TRUNCATED,
    JPP_SDK_STATUS_BROKER_ERROR,
    JPP_SDK_STATUS_HANDLE_LIMIT,
    JPP_SDK_STATUS_INVALID_HANDLE,
    JPP_SDK_STATUS_BLE_CONN_LIMIT,
    JPP_SDK_STATUS_INVALID_BLE_CONN,
    JPP_SDK_STATUS_NO_DATA,          /* nothing to consume (e.g. empty IPC mailbox) */
} jpp_sdk_status_t;

typedef uint8_t jpp_sdk_handle_t;
typedef uint8_t jpp_sdk_ble_conn_t;

/* One BLE scan result entry returned by jpp_sdk_ble_scan(). */
typedef struct {
    char    address[JPP_SDK_BLE_ADDR_LEN];  /* "AA:BB:CC:DD:EE:FF" */
    char    name[JPP_SDK_BLE_NAME_MAX];     /* complete or shortened local name, may be "" */
    int8_t  rssi;                           /* signal strength in dBm */
    uint8_t ad_data[JPP_SDK_BLE_AD_PAYLOAD_MAX];
    size_t  ad_data_len;
} jpp_sdk_ble_scan_result_t;

typedef enum {
    JPP_SDK_OPEN_READ  = 0,
    JPP_SDK_OPEN_WRITE = 1,
} jpp_sdk_open_mode_t;

typedef jpp_broker_status_t (*jpp_sdk_device_status_reader_t)(
    void *context,
    jpp_broker_result_t *result
);

typedef void (*jpp_sdk_log_writer_t)(
    void *context,
    const char *app_id,
    const char *event_name
);

/* Returns UTC time as "YYYY-MM-DD HH:mm" into result->text. */
typedef jpp_broker_status_t (*jpp_sdk_rtc_reader_t)(
    void *context,
    jpp_broker_result_t *result
);

typedef jpp_broker_status_t (*jpp_sdk_file_reader_t)(
    void *context,
    const char *logical_path,
    jpp_broker_result_t *result
);

typedef jpp_broker_status_t (*jpp_sdk_file_writer_t)(
    void *context,
    const char *logical_path,
    const char *text,
    jpp_broker_result_t *result
);

typedef jpp_broker_status_t (*jpp_sdk_file_lister_t)(
    void *context,
    const char *logical_path,
    jpp_broker_result_t *result
);

/* Returns true if the user approved access to path with the given mode. */
typedef bool (*jpp_sdk_path_prompt_t)(
    void *context,
    const char *path,
    jpp_sdk_open_mode_t mode
);

/* ---- BLE native callbacks ---- */

/* Perform a passive scan for duration_ms. Writes up to max_results entries into
   results[] and sets *out_count to the actual number found. */
typedef jpp_broker_status_t (*jpp_sdk_ble_scan_fn_t)(
    void *context,
    uint32_t duration_ms,
    jpp_sdk_ble_scan_result_t *results,
    size_t max_results,
    size_t *out_count,
    jpp_broker_result_t *result
);

/* Start/stop BLE advertisement with the given raw payload bytes. */
typedef jpp_broker_status_t (*jpp_sdk_ble_advertise_start_fn_t)(
    void *context,
    const uint8_t *payload,
    size_t payload_len,
    jpp_broker_result_t *result
);
typedef jpp_broker_status_t (*jpp_sdk_ble_advertise_stop_fn_t)(
    void *context,
    jpp_broker_result_t *result
);

/* Initiate a GATT client connection to address (colon-separated hex). */
typedef jpp_broker_status_t (*jpp_sdk_ble_connect_fn_t)(
    void *context,
    const char *address,
    jpp_broker_result_t *result
);

/* Read/write a GATT characteristic on an open connection. */
typedef jpp_broker_status_t (*jpp_sdk_ble_read_char_fn_t)(
    void *context,
    const char *address,
    const char *svc_uuid,
    const char *char_uuid,
    jpp_broker_result_t *result
);
typedef jpp_broker_status_t (*jpp_sdk_ble_write_char_fn_t)(
    void *context,
    const char *address,
    const char *svc_uuid,
    const char *char_uuid,
    const char *text,
    jpp_broker_result_t *result
);

/* Tear down a GATT client connection. */
typedef jpp_broker_status_t (*jpp_sdk_ble_disconnect_fn_t)(
    void *context,
    const char *address,
    jpp_broker_result_t *result
);

/* Register/unregister a GATT server service identified by svc_uuid. */
typedef jpp_broker_status_t (*jpp_sdk_ble_service_register_fn_t)(
    void *context,
    const char *svc_uuid,
    jpp_broker_result_t *result
);
typedef jpp_broker_status_t (*jpp_sdk_ble_service_unregister_fn_t)(
    void *context,
    jpp_broker_result_t *result
);

/* Publish bytes on the host TX (readable) characteristic. */
typedef void (*jpp_sdk_ble_host_set_value_fn_t)(
    void *context,
    const uint8_t *data,
    size_t len
);
/* Block until a peer writes the host RX characteristic, or timeout_ms elapses.
   Copies up to *len_inout bytes into buf and updates *len_inout. Returns true
   if data arrived. timeout_ms == 0 waits indefinitely. */
typedef bool (*jpp_sdk_ble_host_wait_write_fn_t)(
    void *context,
    uint8_t *buf,
    size_t *len_inout,
    uint32_t timeout_ms
);
/* Discard any pending RX write / buffered data. */
typedef void (*jpp_sdk_ble_host_clear_fn_t)(void *context);
/* Select connectable vs. non-connectable mode for subsequent advertising. */
typedef void (*jpp_sdk_ble_set_connectable_fn_t)(void *context, bool connectable);

/* Performs a broker-serialized HTTP GET or POST request. */
typedef jpp_broker_status_t (*jpp_sdk_http_request_fn_t)(
    void *context,
    const char *method,         /* "GET" or "POST" */
    const char *url,
    const char *body,           /* NULL for GET */
    jpp_broker_result_t *result /* result fields: status_code, body */
);

/*
 * network.bind — TCP server sockets over lwIP. The firmware enforces
 * JPP_RESOURCE_SDK_NET_LISTENER_LIMIT listeners and
 * JPP_RESOURCE_SDK_NET_SOCKET_LIMIT accepted connections, and refuses to bind
 * while the WebDAV or LRV HTTP server is running.
 */
typedef jpp_broker_status_t (*jpp_sdk_net_bind_fn_t)(
    void *context,
    uint16_t port,
    jpp_broker_result_t *result   /* result field: port */
);
typedef jpp_broker_status_t (*jpp_sdk_net_accept_fn_t)(
    void *context,
    uint32_t timeout_ms,
    int *out_sock,                /* -1 when no connection arrived in time */
    jpp_broker_result_t *result
);
typedef jpp_broker_status_t (*jpp_sdk_net_recv_fn_t)(
    void *context,
    int sock,
    uint8_t *buf,
    size_t buf_len,
    size_t *out_len,              /* 0 on timeout; result notes peer close */
    uint32_t timeout_ms,
    jpp_broker_result_t *result
);
typedef jpp_broker_status_t (*jpp_sdk_net_send_fn_t)(
    void *context,
    int sock,
    const uint8_t *data,
    size_t len,
    jpp_broker_result_t *result
);
typedef jpp_broker_status_t (*jpp_sdk_net_close_fn_t)(
    void *context,
    int sock,                     /* a connection socket, or -1 = the listener */
    jpp_broker_result_t *result
);
/* Close the listener and every connection — app teardown path. */
typedef void (*jpp_sdk_net_close_all_fn_t)(void *context);

/*
 * Lazy capability consent — called the first time an app tries to use a
 * capability that was declared in the manifest but not pre-granted at launch.
 * Returns true if the user allows, false if denied.
 */
typedef bool (*jpp_sdk_consent_prompt_t)(
    void *context,
    const char *cap,
    int tier
);

/* Delete a file at logical_path. Used by IPC for consumed messages. */
typedef jpp_broker_status_t (*jpp_sdk_file_deleter_t)(
    void *context,
    const char *logical_path,
    jpp_broker_result_t *result
);

/*
 * Dynamic code modules — load/run/unload a second ELF image (a "plugin" the
 * app ships in its own scoped storage) into the unused tail of the app pool.
 * Implemented by main/ over jpp_native_loader_core; only native apps have
 * these callbacks (the pool tail does not exist under the MicroPython GC
 * heap). sdk_ctx is the calling app's jpp_sdk_context_t; api is an opaque
 * app-defined function table handed to the module entry verbatim.
 */
typedef bool (*jpp_sdk_module_load_fn_t)(
    void *context,
    const char *abs_path,
    void **out_handle
);
typedef void (*jpp_sdk_module_run_fn_t)(
    void *context,
    void *handle,
    void *sdk_ctx,
    void *api
);
typedef void (*jpp_sdk_module_unload_fn_t)(
    void *context,
    void *handle
);

/*
 * List entries in a directory at abs_path (absolute /sd/... path).
 * Writes NUL-terminated newline-separated entries into buf; directories are
 * suffixed with "/". Returns the number of entries written (truncated if buf
 * too small). Used internally by jpp_sdk_file_pick().
 */
typedef size_t (*jpp_sdk_dir_list_fn_t)(
    void *context,
    const char *abs_path,
    char *buf,
    size_t buf_len
);

/* Per-app persistent key-value helper backed by a file in scoped storage. */
typedef jpp_broker_status_t (*jpp_sdk_kv_read_fn_t)(
    void *context,
    const char *app_id,
    const char *key,
    char *out_value,
    size_t buf_len,
    jpp_broker_result_t *result
);
typedef jpp_broker_status_t (*jpp_sdk_kv_write_fn_t)(
    void *context,
    const char *app_id,
    const char *key,
    const char *value,
    jpp_broker_result_t *result
);
typedef jpp_broker_status_t (*jpp_sdk_kv_delete_fn_t)(
    void *context,
    const char *app_id,
    const char *key,
    jpp_broker_result_t *result
);

typedef struct {
    jpp_broker_lock_set_t *locks;
    jpp_sdk_device_status_reader_t device_status;
    void *device_status_context;
    jpp_sdk_file_reader_t read_file;
    void *read_file_context;
    jpp_sdk_file_writer_t write_file;
    void *write_file_context;
    jpp_sdk_file_lister_t list_files;
    void *list_files_context;
    jpp_sdk_path_prompt_t path_prompt;
    void *path_prompt_context;
    jpp_sdk_log_writer_t log_writer;
    void *log_context;
    /* RTC — backs jpp_sdk_get_time() */
    jpp_sdk_rtc_reader_t rtc_reader;
    void *rtc_reader_context;
    /* BLE — ble.scan */
    jpp_sdk_ble_scan_fn_t ble_scan;
    void *ble_scan_context;
    /* BLE — ble.advertise */
    jpp_sdk_ble_advertise_start_fn_t ble_advertise_start;
    jpp_sdk_ble_advertise_stop_fn_t  ble_advertise_stop;
    void *ble_advertise_context;
    /* BLE — ble.connect (GATT client) */
    jpp_sdk_ble_connect_fn_t    ble_connect;
    jpp_sdk_ble_read_char_fn_t  ble_read_char;
    jpp_sdk_ble_write_char_fn_t ble_write_char;
    jpp_sdk_ble_disconnect_fn_t ble_disconnect;
    void *ble_client_context;
    /* BLE — ble.host (GATT server) */
    jpp_sdk_ble_service_register_fn_t   ble_service_register;
    jpp_sdk_ble_service_unregister_fn_t ble_service_unregister;
    jpp_sdk_ble_host_set_value_fn_t     ble_host_set_value;
    jpp_sdk_ble_host_wait_write_fn_t    ble_host_wait_write;
    jpp_sdk_ble_host_clear_fn_t         ble_host_clear;
    jpp_sdk_ble_set_connectable_fn_t    ble_set_connectable;
    void *ble_host_context;
    /* http.request */
    jpp_sdk_http_request_fn_t http_request;
    void *http_request_context;
    /* network.bind — TCP server sockets */
    jpp_sdk_net_bind_fn_t      net_bind;
    jpp_sdk_net_accept_fn_t    net_accept;
    jpp_sdk_net_recv_fn_t      net_recv;
    jpp_sdk_net_send_fn_t      net_send;
    jpp_sdk_net_close_fn_t     net_close;
    jpp_sdk_net_close_all_fn_t net_close_all;
    void *net_context;
    /* IPC — delete file after consuming a received message */
    jpp_sdk_file_deleter_t delete_file;
    void *delete_file_context;
    /* dynamic code modules — native apps only; NULL for MicroPython */
    jpp_sdk_module_load_fn_t   module_load;
    jpp_sdk_module_run_fn_t    module_run;
    jpp_sdk_module_unload_fn_t module_unload;
    void *module_context;
    /* key-value helper — JSON file in the app's scoped storage */
    jpp_sdk_kv_read_fn_t   kv_read;
    jpp_sdk_kv_write_fn_t  kv_write;
    jpp_sdk_kv_delete_fn_t kv_delete;
    void *kv_context;
    /* dir_list — used by jpp_sdk_file_pick() to list arbitrary /sd/ directories */
    jpp_sdk_dir_list_fn_t dir_list;
    void *dir_list_context;
    /* lazy consent — called when a pending cap is first used; may block */
    jpp_sdk_consent_prompt_t consent_prompt;
    void *consent_prompt_context;
} jpp_sdk_native_services_t;

typedef struct {
    char path[JPP_SDK_PATH_MAX];
    jpp_sdk_open_mode_t mode;
    bool active;
} jpp_sdk_handle_entry_t;

/* Session-scoped GATT client connection slot (ble.connect). */
typedef struct {
    char address[JPP_SDK_BLE_ADDR_LEN];
    bool active;
} jpp_sdk_ble_conn_entry_t;

typedef enum {
    JPP_SDK_KEY_NONE        = 0,
    JPP_SDK_KEY_UP,
    JPP_SDK_KEY_DOWN,
    JPP_SDK_KEY_LEFT,
    JPP_SDK_KEY_RIGHT,
    JPP_SDK_KEY_CENTER,
    JPP_SDK_KEY_CENTER_LONG,
} jpp_sdk_key_event_t;

/* ---- High-level UI abstractions ----------------------------------------- */
/*
 * Dialog, List, and Input are blocking, modal helpers built on the frame and
 * key primitives. Each takes over the screen and the d-pad until the user
 * resolves it, then returns. The control scheme is uniform:
 *   d-pad        — move focus
 *   CENTER       — select / confirm / type the focused key
 *   CENTER long  — BACK (dismiss / cancel)
 */

/* Outcome of a modal UI helper. */
typedef enum {
    JPP_SDK_UI_OK   = 0,   /* user confirmed (CENTER) */
    JPP_SDK_UI_BACK = 1,   /* user dismissed with BACK (long-press CENTER) */
} jpp_sdk_ui_result_t;

/* Character set selected for an Input prompt's on-screen keyboard. */
typedef enum {
    JPP_SDK_INPUT_TEXT   = 0,   /* full QWERTY, upper/lower via Shift */
    JPP_SDK_INPUT_NUMBER = 1,   /* digits, sign, decimal point */
    JPP_SDK_INPUT_DATE   = 2,   /* digits and "-" (YYYY-MM-DD) */
    JPP_SDK_INPUT_TIME   = 3,   /* digits and ":" (HH:MM) */
} jpp_sdk_input_type_t;

#define JPP_SDK_LIST_ITEM_MAX   16u   /* maximum items a List prompt accepts */
#define JPP_SDK_INPUT_VALUE_MAX 64u   /* maximum characters an Input returns */

typedef struct {
    char app_id[JPP_SDK_APP_ID_MAX];
    jpp_broker_caller_t caller;
    jpp_sdk_native_services_t services;
    char frame_lines[JPP_SDK_FRAME_LINE_CAPACITY][JPP_SDK_FRAME_TEXT_MAX];
    size_t frame_line_count;
    /* When true, the renderer draws a horizontal "signature" rule across page 1,
       separating the title on row 0 from the body on rows 2+. Set by the modal
       UI helpers (dialog/list/confirm); cleared on every jpp_sdk_set_frame. */
    bool frame_title_rule;
    /* When true, the canvas covers the whole 128×64 display (pages 0–7) and the
       renderer skips frame_lines and the title rule. Set/cleared with
       jpp_sdk_canvas_fullscreen(); jpp_sdk_set_frame() clears it (the modal UI
       helpers therefore drop fullscreen — re-enable it after they return). */
    bool canvas_fullscreen;
    jpp_sdk_handle_entry_t handles[JPP_SDK_HANDLE_CAPACITY];
    jpp_sdk_ble_conn_entry_t ble_conns[JPP_SDK_BLE_CONN_CAPACITY];
    bool ble_host_registered;
    bool close_requested;
    bool bound;
    bool wakelock_held;  /* prevents screen dim / deep sleep when true */
    bool dummy_mode;     /* set when the app is launched as the dummy-mode locked app */
    QueueHandle_t key_queue;  /* FreeRTOS queue, capacity 8, element jpp_sdk_key_event_t */
    /* Pixel canvas — row-major, MSB = leftmost pixel. Rows 0–47 in windowed
       mode (pages 2–7); all 64 rows when canvas_fullscreen is set. */
    uint8_t canvas[JPP_SDK_CANVAS_ROWS_FULL][JPP_SDK_CANVAS_ROW_BYTES];
    /* Mutable backing store for caller.capabilities — holds pre-granted cap pointers
       and is extended in-place as pending caps are lazily granted. */
    const char *granted_cap_ptrs[JPP_SDK_PENDING_CAP_MAX];
    /* Caps declared in the manifest but not yet presented to the user. Each is
       promoted into granted_cap_ptrs the first time the app calls the SDK function
       that requires it; denied caps are dropped and the call returns ACCESS_DENIED. */
    char pending_cap_strs[JPP_SDK_PENDING_CAP_MAX][32];
    int  pending_cap_tiers[JPP_SDK_PENDING_CAP_MAX];
    size_t pending_cap_count;
} jpp_sdk_context_t;

void jpp_sdk_context_init(jpp_sdk_context_t *context);

/*
 * Register capabilities that are declared in the manifest but were not
 * pre-granted at bind time. Call once after jpp_sdk_bind() / jpp_sdk_bind_native()
 * and before the app task starts. caps[i] and tiers[i] must remain valid
 * for the context lifetime (they are copied into the context).
 */
void jpp_sdk_set_pending_caps(
    jpp_sdk_context_t *context,
    const char *const *caps,
    const int *tiers,
    size_t count
);

jpp_sdk_status_t jpp_sdk_bind(
    jpp_sdk_context_t *context,
    const jpp_vm_context_t *vm_context,
    const char *task_name,
    const char *app_id,
    const jpp_broker_caller_t *caller,
    const jpp_sdk_native_services_t *services
);

/* Bind a native C app without a VM context check. */
jpp_sdk_status_t jpp_sdk_bind_native(
    jpp_sdk_context_t *context,
    const char *app_id,
    const jpp_broker_caller_t *caller,
    const jpp_sdk_native_services_t *services
);

jpp_sdk_status_t jpp_sdk_set_frame(
    jpp_sdk_context_t *context,
    const char *const *lines,
    size_t line_count
);

jpp_sdk_status_t jpp_sdk_request_close(jpp_sdk_context_t *context);
jpp_sdk_status_t jpp_sdk_log(jpp_sdk_context_t *context, const char *event_name);

/* Ungated — battery/charging status is available to every app. */
jpp_sdk_status_t jpp_sdk_device_status(jpp_sdk_context_t *context, jpp_broker_result_t *result);

/* Ungated — scoped storage under /sd/apps/<app_id>/ is private to the app. */
jpp_sdk_status_t jpp_sdk_file_read(
    jpp_sdk_context_t *context,
    const char *relative_path,
    jpp_broker_result_t *result
);
jpp_sdk_status_t jpp_sdk_file_write(
    jpp_sdk_context_t *context,
    const char *relative_path,
    const char *text,
    jpp_broker_result_t *result
);
jpp_sdk_status_t jpp_sdk_file_list(
    jpp_sdk_context_t *context,
    const char *relative_dir,
    jpp_broker_result_t *result
);

/* Ungated — shared storage under /sd/shared/<app_id>/ is world-readable. */
jpp_sdk_status_t jpp_sdk_shared_read(
    jpp_sdk_context_t *context,
    const char *relative_path,
    jpp_broker_result_t *result
);
jpp_sdk_status_t jpp_sdk_shared_write(
    jpp_sdk_context_t *context,
    const char *relative_path,
    const char *text,
    jpp_broker_result_t *result
);
jpp_sdk_status_t jpp_sdk_shared_list(
    jpp_sdk_context_t *context,
    const char *relative_dir,
    jpp_broker_result_t *result
);

/*
 * Requires: files.full
 * Prompts the user to approve access to `path` (absolute under /sd/).
 * On approval, writes a handle index to *out_handle; the handle stays
 * valid until jpp_sdk_handle_close or the app session ends.
 */
jpp_sdk_status_t jpp_sdk_file_open(
    jpp_sdk_context_t *context,
    const char *path,
    jpp_sdk_open_mode_t mode,
    jpp_sdk_handle_t *out_handle
);
jpp_sdk_status_t jpp_sdk_handle_read(
    jpp_sdk_context_t *context,
    jpp_sdk_handle_t handle,
    jpp_broker_result_t *result
);
jpp_sdk_status_t jpp_sdk_handle_write(
    jpp_sdk_context_t *context,
    jpp_sdk_handle_t handle,
    const char *text,
    jpp_broker_result_t *result
);
jpp_sdk_status_t jpp_sdk_handle_list(
    jpp_sdk_context_t *context,
    jpp_sdk_handle_t handle,
    jpp_broker_result_t *result
);
jpp_sdk_status_t jpp_sdk_handle_close(
    jpp_sdk_context_t *context,
    jpp_sdk_handle_t handle
);

/*
 * Requires: background.register
 * Request permission to run the manifest-declared background.tasks schedule.
 * The call itself only triggers the consent prompt; enrollment happens when
 * the app exits — the firmware syncs the schedule table from the manifest iff
 * the background.register grant is persisted.
 */
jpp_sdk_status_t jpp_sdk_background_register(
    jpp_sdk_context_t *context,
    jpp_broker_result_t *result
);

/*
 * Requires: ble.scan
 * Perform a passive BLE scan for duration_ms milliseconds. Up to max_results
 * entries are written into results[]. *out_count receives the actual count.
 */
jpp_sdk_status_t jpp_sdk_ble_scan(
    jpp_sdk_context_t *context,
    uint32_t duration_ms,
    jpp_sdk_ble_scan_result_t *results,
    size_t max_results,
    size_t *out_count,
    jpp_broker_result_t *result
);

/*
 * Requires: ble.advertise
 * Start broadcasting a raw BLE advertisement payload (max JPP_SDK_BLE_AD_PAYLOAD_MAX bytes).
 * Only one app may advertise at a time; the broker serializes access.
 */
jpp_sdk_status_t jpp_sdk_ble_advertise_start(
    jpp_sdk_context_t *context,
    const uint8_t *payload,
    size_t payload_len,
    jpp_broker_result_t *result
);
jpp_sdk_status_t jpp_sdk_ble_advertise_stop(
    jpp_sdk_context_t *context,
    jpp_broker_result_t *result
);

/*
 * Requires: ble.connect
 * Open a GATT client connection to address ("AA:BB:CC:DD:EE:FF").
 * On success writes a session-scoped handle to *out_conn.
 * The handle is valid until jpp_sdk_ble_disconnect or app stop.
 */
jpp_sdk_status_t jpp_sdk_ble_connect(
    jpp_sdk_context_t *context,
    const char *address,
    jpp_sdk_ble_conn_t *out_conn
);
jpp_sdk_status_t jpp_sdk_ble_read_char(
    jpp_sdk_context_t *context,
    jpp_sdk_ble_conn_t conn,
    const char *svc_uuid,
    const char *char_uuid,
    jpp_broker_result_t *result
);
jpp_sdk_status_t jpp_sdk_ble_write_char(
    jpp_sdk_context_t *context,
    jpp_sdk_ble_conn_t conn,
    const char *svc_uuid,
    const char *char_uuid,
    const char *text,
    jpp_broker_result_t *result
);
jpp_sdk_status_t jpp_sdk_ble_disconnect(
    jpp_sdk_context_t *context,
    jpp_sdk_ble_conn_t conn
);

/*
 * Requires: ble.host
 * Register a GATT server service identified by svc_uuid. Only one service may
 * be registered per app session; a second call returns ACCESS_DENIED until the
 * first is unregistered.
 */
jpp_sdk_status_t jpp_sdk_ble_service_register(
    jpp_sdk_context_t *context,
    const char *svc_uuid,
    jpp_broker_result_t *result
);
jpp_sdk_status_t jpp_sdk_ble_service_unregister(
    jpp_sdk_context_t *context,
    jpp_broker_result_t *result
);

/*
 * Requires: ble.host
 * Publish up to JPP_SDK_BLE_HOST_RX_MAX bytes on the host TX characteristic.
 * A peer that has connected and registered the host service UUID can read them.
 */
jpp_sdk_status_t jpp_sdk_ble_host_set_value(
    jpp_sdk_context_t *context,
    const uint8_t *data,
    size_t len,
    jpp_broker_result_t *result
);
/*
 * Requires: ble.host
 * Block until a connected peer writes the host RX characteristic, or until
 * timeout_ms elapses (0 = wait forever). Copies up to *len_inout bytes into buf
 * and updates *len_inout with the byte count. *out_received is set to whether a
 * write arrived before the timeout.
 */
jpp_sdk_status_t jpp_sdk_ble_host_wait_write(
    jpp_sdk_context_t *context,
    uint8_t *buf,
    size_t *len_inout,
    uint32_t timeout_ms,
    bool *out_received,
    jpp_broker_result_t *result
);
/* Requires: ble.host — discard any pending/buffered RX write. */
jpp_sdk_status_t jpp_sdk_ble_host_clear(
    jpp_sdk_context_t *context,
    jpp_broker_result_t *result
);
/*
 * Requires: ble.advertise
 * Choose connectable vs. non-connectable mode for subsequent
 * jpp_sdk_ble_advertise_start() calls. A GATT-host app must be connectable so a
 * peer can open a connection; the default is non-connectable.
 */
jpp_sdk_status_t jpp_sdk_ble_set_connectable(
    jpp_sdk_context_t *context,
    bool connectable,
    jpp_broker_result_t *result
);

/* Ungated — returns UTC time as "YYYY-MM-DD HH:mm" */
jpp_sdk_status_t jpp_sdk_get_time(jpp_sdk_context_t *context, jpp_broker_result_t *result);

/* Requires: http.request — method is "GET" or "POST"; body may be NULL for GET */
jpp_sdk_status_t jpp_sdk_http_request(
    jpp_sdk_context_t *context,
    const char *method,
    const char *url,
    const char *body,
    jpp_broker_result_t *result
);

/* Canvas pixel drawing — no capability required, always available */
jpp_sdk_status_t jpp_sdk_canvas_write(
    jpp_sdk_context_t *context,
    uint8_t row,              /* 0–47 (0–63 in fullscreen mode) */
    const uint8_t *pixels     /* JPP_SDK_CANVAS_ROW_BYTES bytes, MSB = leftmost */
);
jpp_sdk_status_t jpp_sdk_canvas_clear(jpp_sdk_context_t *context);
jpp_sdk_status_t jpp_sdk_canvas_draw_pixel(
    jpp_sdk_context_t *context,
    uint8_t x,  /* 0–127 */
    uint8_t y,  /* 0–47 (0–63 in fullscreen mode) */
    bool on
);
/*
 * Toggle fullscreen canvas mode. When on, the canvas covers the entire
 * 128×64 display (rows 0–63, pages 0–7) and the renderer skips the frame
 * text rows and title rule. The canvas is cleared on every mode change.
 * jpp_sdk_set_frame() (and therefore every modal UI helper) switches back
 * to windowed mode; call this again after a dialog/list/input returns.
 */
jpp_sdk_status_t jpp_sdk_canvas_fullscreen(jpp_sdk_context_t *context, bool on);

/* ---- Dynamic code modules (native apps only, ungated) --------------------- */
/*
 * Load a second ELF binary from the app's own scoped storage
 * (/sd/apps/<app_id>/<relative_path>) into the unused tail of the app pool.
 * The module is built like an app binary but exports
 *     void jpp_module_entry(jpp_sdk_context_t *ctx, void *api);
 * One module may be loaded at a time; unload before loading the next. The
 * module runs in the app's own task with the app's own capabilities — it is
 * the app's code, just paged in on demand, so no capability gate applies.
 * Returns INVALID_STATE for MicroPython apps (no module service) and
 * BROKER_ERROR when loading fails (bad ELF, no room in the pool tail —
 * the app keeps running either way).
 */
jpp_sdk_status_t jpp_sdk_module_load(
    jpp_sdk_context_t *context,
    const char *relative_path,
    void **out_module
);
/* Call the module's jpp_module_entry(context, api); blocks until it returns. */
jpp_sdk_status_t jpp_sdk_module_run(
    jpp_sdk_context_t *context,
    void *module,
    void *api
);
jpp_sdk_status_t jpp_sdk_module_unload(
    jpp_sdk_context_t *context,
    void *module
);

/*
 * Requires: network.bind — TCP server sockets over lwIP.
 * net_bind opens the single listener on `port`; net_accept waits up to
 * timeout_ms for an inbound connection and returns a socket id; net_recv /
 * net_send move bytes on an accepted socket (net_recv writes 0 to *out_len on
 * timeout and sets result field "closed" when the peer disconnected);
 * net_close closes one socket (sock = -1 closes the listener). All sockets
 * are closed automatically when the app session ends. Binding fails with
 * SERVER_ACTIVE while the WebDAV or LRV server runs.
 */
jpp_sdk_status_t jpp_sdk_net_bind(
    jpp_sdk_context_t *context,
    uint16_t port,
    jpp_broker_result_t *result
);
jpp_sdk_status_t jpp_sdk_net_accept(
    jpp_sdk_context_t *context,
    uint32_t timeout_ms,
    int *out_sock,
    jpp_broker_result_t *result
);
jpp_sdk_status_t jpp_sdk_net_recv(
    jpp_sdk_context_t *context,
    int sock,
    uint8_t *buf,
    size_t buf_len,
    size_t *out_len,
    uint32_t timeout_ms,
    jpp_broker_result_t *result
);
jpp_sdk_status_t jpp_sdk_net_send(
    jpp_sdk_context_t *context,
    int sock,
    const uint8_t *data,
    size_t len,
    jpp_broker_result_t *result
);
jpp_sdk_status_t jpp_sdk_net_close(
    jpp_sdk_context_t *context,
    int sock,
    jpp_broker_result_t *result
);
/* Close the listener and every open connection (no capability check — used by
   the app teardown path; a no-op when nothing is open). */
void jpp_sdk_net_close_all(jpp_sdk_context_t *context);

/* Ungated — mailbox files live in the recipient's scoped storage. */
#define JPP_SDK_IPC_PAYLOAD_MAX 512u
jpp_sdk_status_t jpp_sdk_ipc_send(
    jpp_sdk_context_t *context,
    const char *recipient_app_id,
    const char *payload,
    jpp_broker_result_t *result
);
/* On success: result field "sender" is the sending app_id.
   Returns NO_DATA if no messages are pending (result->ok = false). */
jpp_sdk_status_t jpp_sdk_ipc_recv(
    jpp_sdk_context_t *context,
    char *out_payload,
    size_t payload_buf_len,
    jpp_broker_result_t *result
);

/* Ungated — key-value helper backed by a JSON file in scoped storage. */
#define JPP_SDK_KV_KEY_MAX   64u
#define JPP_SDK_KV_VALUE_MAX 256u
jpp_sdk_status_t jpp_sdk_kv_get(
    jpp_sdk_context_t *context,
    const char *key,
    char *out_value,
    size_t value_buf_len
);
jpp_sdk_status_t jpp_sdk_kv_set(
    jpp_sdk_context_t *context,
    const char *key,
    const char *value
);
jpp_sdk_status_t jpp_sdk_kv_delete(
    jpp_sdk_context_t *context,
    const char *key
);

/* Non-blocking: returns the next queued key event, or KEY_NONE if empty. */
jpp_sdk_status_t jpp_sdk_poll_key(jpp_sdk_context_t *context, jpp_sdk_key_event_t *out_event);

/* Blocking: waits up to timeout_ms for a key event. Returns KEY_NONE on timeout.
   timeout_ms == 0 waits indefinitely (use jpp_sdk_poll_key for a non-blocking check). */
jpp_sdk_status_t jpp_sdk_wait_key(jpp_sdk_context_t *context, uint32_t timeout_ms, jpp_sdk_key_event_t *out_event);

/* Called by the main loop to push a key event into the active app's queue. */
void jpp_sdk_push_key(jpp_sdk_context_t *context, jpp_sdk_key_event_t event);

/* ---- High-level UI helpers (no capability required) ---------------------- */

/*
 * Dialog — a modal message box. `title` may be NULL. `text` is word-wrapped
 * across the body rows. Blocks until the user presses CENTER (returns
 * *out_result = JPP_SDK_UI_OK) or long-presses CENTER (JPP_SDK_UI_BACK).
 */
jpp_sdk_status_t jpp_sdk_dialog(
    jpp_sdk_context_t *context,
    const char *title,                  /* may be NULL */
    const char *text,
    jpp_sdk_ui_result_t *out_result
);

/*
 * List — prompt the user to choose from `items` (an array of `item_count`
 * strings). `title` may be NULL.
 *   single-select (multiselect=false): CENTER on an item confirms; on OK,
 *     *out_count is 1 and out_indices[0] is the chosen index.
 *   multi-select (multiselect=true): CENTER toggles the focused item; a
 *     trailing "Done" row confirms. On OK, out_indices[0..*out_count-1] holds
 *     the checked indices in ascending order.
 * Long-press CENTER cancels: *out_result = JPP_SDK_UI_BACK, *out_count = 0.
 * out_indices must have room for at least out_capacity entries.
 */
jpp_sdk_status_t jpp_sdk_list(
    jpp_sdk_context_t *context,
    const char *title,                  /* may be NULL */
    const char *const *items,
    size_t item_count,
    bool multiselect,
    size_t *out_indices,
    size_t out_capacity,
    size_t *out_count,
    jpp_sdk_ui_result_t *out_result
);

/*
 * Input — prompt the user to type a value on an on-screen d-pad keyboard.
 * `title` is shown as the prompt. `placeholder` (may be NULL) is shown while
 * the field is empty. `type` selects the keyboard character set. On OK the
 * entered string is written to out_value (NUL-terminated). Long-press CENTER
 * cancels: *out_result = JPP_SDK_UI_BACK and out_value is set to "".
 */
jpp_sdk_status_t jpp_sdk_input(
    jpp_sdk_context_t *context,
    const char *title,
    const char *placeholder,            /* may be NULL */
    jpp_sdk_input_type_t type,
    char *out_value,
    size_t value_buf_len,
    jpp_sdk_ui_result_t *out_result
);

/*
 * Confirm — a unified Allow/Deny consent prompt. Renders `title` on row 0 with a
 * signature rule beneath it, the body_lines wrapped below, and a horizontal
 * "Deny / Allow" selector on the bottom row. d-pad LEFT/RIGHT move the selector,
 * CENTER confirms the highlighted choice, long-press CENTER denies. `default_allow`
 * sets the initially highlighted side. Writes the choice to *out_allow.
 *
 * This is the single consent surface shared by capability and file-access prompts
 * so every permission decision looks and behaves identically.
 */
jpp_sdk_status_t jpp_sdk_confirm(
    jpp_sdk_context_t *context,
    const char *title,
    const char *const *body_lines,
    size_t body_count,
    bool default_allow,
    bool *out_allow
);

/*
 * Word-wrap text into up to max_lines rows of at most 21 visible characters.
 * Words longer than a row are hard-split; text past max_lines is dropped.
 * Returns the number of rows used. Used by jpp_sdk_dialog internally and by
 * callers preparing body_lines for jpp_sdk_confirm.
 */
size_t jpp_sdk_wrap_text(
    const char *text,
    char lines[][JPP_SDK_FRAME_TEXT_MAX],
    size_t max_lines
);

/*
 * File pick — prompt the user to browse the SD card (/sd) and select a file.
 * Requires: files.full
 * Navigable directory listing; directories are shown with a "/" suffix and ".."
 * appears at the top of every non-root directory. Long names scroll as marquees.
 * On OK: *out_result = JPP_SDK_UI_OK and out_path holds the absolute /sd/ path.
 * On cancel (long-press CENTER / BACK at root): *out_result = JPP_SDK_UI_BACK.
 * out_path_len must be at least JPP_SDK_PATH_MAX.
 */
jpp_sdk_status_t jpp_sdk_file_pick(
    jpp_sdk_context_t *context,
    char *out_path,
    size_t out_path_len,
    jpp_sdk_ui_result_t *out_result
);

/* ---- Wakelock ------------------------------------------------------------ */
/* Prevent the OS from dimming the screen or entering deep sleep.
   Each acquire must be matched by a release.  Nested pairs are supported. */
jpp_sdk_status_t jpp_sdk_wakelock_acquire(jpp_sdk_context_t *context);
jpp_sdk_status_t jpp_sdk_wakelock_release(jpp_sdk_context_t *context);

/* Returns true when the firmware has locked the device to this app (dummy
   mode).  Apps can use this to hide their own "Exit" option when they know
   the firmware will re-launch them immediately after close. */
bool jpp_sdk_is_dummy_mode(const jpp_sdk_context_t *ctx);

/* ---- Buzzer -------------------------------------------------------------- */
#include "jpp_buzzer_core.h"

/* Play a predefined named sound asynchronously (returns immediately). */
jpp_sdk_status_t jpp_sdk_buzzer_play(jpp_sdk_context_t *context,
                                      jpp_buzzer_sound_t sound);

/* Play a frequency+duration tone (blocking for duration_ms). */
jpp_sdk_status_t jpp_sdk_buzzer_tone(jpp_sdk_context_t *context,
                                      uint32_t freq_hz,
                                      uint32_t duration_ms);

/* Play a caller-supplied note sequence (blocking). */
jpp_sdk_status_t jpp_sdk_buzzer_play_sequence(jpp_sdk_context_t *context,
                                               const jpp_buzzer_note_t *notes,
                                               size_t count);

/* Play a caller-supplied note sequence asynchronously (returns immediately;
   the sequence is copied, so the caller's buffer need not stay valid).
   A new async sequence — or jpp_sdk_buzzer_stop() — preempts the current one. */
jpp_sdk_status_t jpp_sdk_buzzer_play_sequence_async(jpp_sdk_context_t *context,
                                                     const jpp_buzzer_note_t *notes,
                                                     size_t count);

/* Stop any playing sound immediately. */
jpp_sdk_status_t jpp_sdk_buzzer_stop(jpp_sdk_context_t *context);

const char *jpp_sdk_status_name(jpp_sdk_status_t status);

#ifdef __cplusplus
}
#endif
