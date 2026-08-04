"""Guards on the App SDK's ABI and on the C/Python capability mirror.

Both checks exist because both defects actually shipped into a branch:

  * `net_connect` was added in the middle of `jpp_sdk_native_services_t`, which
    is embedded by value in `jpp_sdk_context_t` above every field an app reads
    directly.  Native apps are separately built ELF32 binaries loaded from SD,
    so that silently shifted `canvas_fullscreen` / `close_requested` for every
    already-deployed `.bin` with no load-time error.
  * `network.connect` was added to the Python manifest mirror but not to the C
    whitelist, so the host tests passed while the device rejected the manifest.

Neither is visible in a normal build, which is why they are pinned here.
"""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SDK_HEADER = REPO / "components" / "jpp_core" / "include" / "jpp_sdk_bridge.h"
MANIFEST_C = REPO / "components" / "jpp_core" / "src" / "jpp_manifest_core.c"
MANIFEST_H = REPO / "components" / "jpp_core" / "include" / "jpp_manifest_core.h"


def _struct_fields(source: str, name: str) -> list[str]:
    """Field declarations of `typedef struct { ... } name;`, comments stripped."""
    end = source.index("} " + name + ";")
    start = source.rindex("typedef struct {", 0, end)
    body = source[start + len("typedef struct {") : end]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    return [
        re.sub(r"\s+", " ", line.strip())
        for line in body.split("\n")
        if line.strip() and not line.strip().startswith("//")
    ]


# The v1 service table, frozen. Apps compiled against any firmware read
# jpp_sdk_context_t fields that sit *after* this struct, so adding, removing or
# reordering anything here moves those fields under already-deployed binaries.
# New service callbacks belong in jpp_sdk_services_v2_t instead.
FROZEN_SERVICES_V1 = (
    "jpp_broker_lock_set_t *locks;",
    "jpp_sdk_device_status_reader_t device_status;",
    "void *device_status_context;",
    "jpp_sdk_file_reader_t read_file;",
    "void *read_file_context;",
    "jpp_sdk_file_writer_t write_file;",
    "void *write_file_context;",
    "jpp_sdk_file_lister_t list_files;",
    "void *list_files_context;",
    "jpp_sdk_path_prompt_t path_prompt;",
    "void *path_prompt_context;",
    "jpp_sdk_log_writer_t log_writer;",
    "void *log_context;",
    "jpp_sdk_rtc_reader_t rtc_reader;",
    "void *rtc_reader_context;",
    "jpp_sdk_ble_scan_fn_t ble_scan;",
    "void *ble_scan_context;",
    "jpp_sdk_ble_advertise_start_fn_t ble_advertise_start;",
    "jpp_sdk_ble_advertise_stop_fn_t ble_advertise_stop;",
    "void *ble_advertise_context;",
    "jpp_sdk_ble_connect_fn_t ble_connect;",
    "jpp_sdk_ble_read_char_fn_t ble_read_char;",
    "jpp_sdk_ble_write_char_fn_t ble_write_char;",
    "jpp_sdk_ble_disconnect_fn_t ble_disconnect;",
    "void *ble_client_context;",
    "jpp_sdk_ble_service_register_fn_t ble_service_register;",
    "jpp_sdk_ble_service_unregister_fn_t ble_service_unregister;",
    "jpp_sdk_ble_host_set_value_fn_t ble_host_set_value;",
    "jpp_sdk_ble_host_wait_write_fn_t ble_host_wait_write;",
    "jpp_sdk_ble_host_clear_fn_t ble_host_clear;",
    "jpp_sdk_ble_set_connectable_fn_t ble_set_connectable;",
    "void *ble_host_context;",
    "jpp_sdk_espnow_send_fn_t espnow_send;",
    "jpp_sdk_espnow_recv_fn_t espnow_recv;",
    "void *espnow_context;",
    "jpp_sdk_http_request_fn_t http_request;",
    "void *http_request_context;",
    "jpp_sdk_net_bind_fn_t net_bind;",
    "jpp_sdk_net_accept_fn_t net_accept;",
    "jpp_sdk_net_recv_fn_t net_recv;",
    "jpp_sdk_net_send_fn_t net_send;",
    "jpp_sdk_net_close_fn_t net_close;",
    "jpp_sdk_net_close_all_fn_t net_close_all;",
    "void *net_context;",
    "jpp_sdk_file_deleter_t delete_file;",
    "void *delete_file_context;",
    "jpp_sdk_module_load_fn_t module_load;",
    "jpp_sdk_module_run_fn_t module_run;",
    "jpp_sdk_module_unload_fn_t module_unload;",
    "void *module_context;",
    "jpp_sdk_kv_read_fn_t kv_read;",
    "jpp_sdk_kv_write_fn_t kv_write;",
    "jpp_sdk_kv_delete_fn_t kv_delete;",
    "void *kv_context;",
    "jpp_sdk_dir_list_fn_t dir_list;",
    "void *dir_list_context;",
    "jpp_sdk_consent_prompt_t consent_prompt;",
    "void *consent_prompt_context;",
)

# jpp_sdk_context_t is append-only: these fields must keep these positions.
# A new field goes after the last entry, never between two of them.
FROZEN_CONTEXT_PREFIX = (
    "char app_id[JPP_SDK_APP_ID_MAX];",
    "jpp_broker_caller_t caller;",
    "jpp_sdk_native_services_t services;",
    "char frame_lines[JPP_SDK_FRAME_LINE_CAPACITY][JPP_SDK_FRAME_TEXT_MAX];",
    "size_t frame_line_count;",
    "bool frame_title_rule;",
    "bool canvas_fullscreen;",
    "jpp_sdk_handle_entry_t handles[JPP_SDK_HANDLE_CAPACITY];",
    "jpp_sdk_ble_conn_entry_t ble_conns[JPP_SDK_BLE_CONN_CAPACITY];",
    "bool ble_host_registered;",
    "bool close_requested;",
    "bool bound;",
    "bool wakelock_held;",
    "bool dummy_mode;",
    "QueueHandle_t key_queue;",
    "uint8_t canvas[JPP_SDK_CANVAS_ROWS_FULL][JPP_SDK_CANVAS_ROW_BYTES];",
    "const char *granted_cap_ptrs[JPP_SDK_PENDING_CAP_MAX];",
    "char pending_cap_strs[JPP_SDK_PENDING_CAP_MAX][32];",
    "int pending_cap_tiers[JPP_SDK_PENDING_CAP_MAX];",
    "size_t pending_cap_count;",
    "uint8_t ok_claim;",
)


def test_v1_service_table_is_frozen():
    fields = _struct_fields(SDK_HEADER.read_text(), "jpp_sdk_native_services_t")
    assert tuple(fields) == FROZEN_SERVICES_V1, (
        "jpp_sdk_native_services_t changed. It is embedded by value in "
        "jpp_sdk_context_t above fields that deployed app binaries read "
        "directly, so any change here shifts their offsets with no load-time "
        "error. Put new service callbacks in jpp_sdk_services_v2_t."
    )


def test_context_struct_is_append_only():
    fields = _struct_fields(SDK_HEADER.read_text(), "jpp_sdk_context_t")
    prefix = tuple(fields[: len(FROZEN_CONTEXT_PREFIX)])
    assert prefix == FROZEN_CONTEXT_PREFIX, (
        "jpp_sdk_context_t fields moved. Deployed app .bin files read these at "
        "fixed offsets; append new fields at the tail instead."
    )


def _c_capabilities() -> set[str]:
    """Capability strings accepted by jpp_manifest_v2_is_allowed_capability()."""
    source = MANIFEST_C.read_text()
    start = source.index("int jpp_manifest_v2_is_allowed_capability")
    end = source.index("int jpp_manifest_v2_is_valid_entry_path")
    return set(re.findall(r'jpp_str_eq\(capability,\s*"([^"]+)"\)', source[start:end]))


def test_c_and_python_capability_lists_agree():
    from validate_manifests import ALLOWED_CAPABILITIES

    assert _c_capabilities() == set(ALLOWED_CAPABILITIES), (
        "The C whitelist in jpp_manifest_core.c and the Python mirror in "
        "tests/validate_manifests.py disagree. A capability in only one of them "
        "either passes the host tests and is rejected on device, or vice versa."
    )


def test_c_and_python_sdk_version_agree():
    from validate_manifests import SDK_VERSION

    match = re.search(r"#define JPP_SDK_VERSION\s+(\d+)", MANIFEST_H.read_text())
    assert match is not None
    assert int(match.group(1)) == SDK_VERSION, (
        "JPP_SDK_VERSION and validate_manifests.SDK_VERSION disagree; the host "
        "tests would accept or reject a different sdk_min than the firmware."
    )
