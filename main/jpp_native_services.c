#include "jpp_native_services.h"
#include "jpp_file_util.h"
#include "jpp_app_dispatch.h"  /* for s_active_sdk_context */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "cJSON.h"

#include "lwip/sockets.h"

#include "jpp_ble_native.h"
#include "jpp_fileserver_core.h"
#include "jpp_lrv_server.h"
#include "jpp_sdk_bridge.h"
#include "jpp_broker_core.h"
#include "jpp_battery_core.h"
#include "jpp_native_loader_core.h"

static const char *TAG = "native_svc";

jpp_sdk_native_services_t s_native_services;
jpp_broker_lock_set_t     s_broker_locks;
volatile bool             s_sd_ejection_detected = false;

static jpp_rtc_state_t     *s_rtc_state_ptr = NULL;
static jpp_battery_state_t *s_battery_state_ptr = NULL;

void jpp_native_services_set_battery_state(jpp_battery_state_t *state)
{
    s_battery_state_ptr = state;
}

/* ---- Dynamic code module callbacks ---------------------------------------- */
/* Thin wrappers over jpp_native_loader_core. Only meaningful for native apps —
   the loader returns BAD_STATE when no native host app is resident (e.g. a
   MicroPython app somehow reached these). */

static bool module_load_cb(void *context, const char *abs_path, void **out_handle)
{
    (void)context;
    jpp_native_loaded_module_t *module = NULL;
    jpp_native_loader_result_t r = jpp_native_loader_load_module(abs_path, &module);
    if (r != JPP_NATIVE_LOADER_OK) {
        ESP_LOGE(TAG, "MODULE_LOAD_FAILED %s: %s",
                 abs_path, jpp_native_loader_result_name(r));
        return false;
    }
    *out_handle = module;
    return true;
}

static void module_run_cb(void *context, void *handle, void *sdk_ctx, void *api)
{
    (void)context;
    jpp_native_loader_module_run((jpp_native_loaded_module_t *)handle,
                                 (jpp_sdk_context_t *)sdk_ctx, api);
}

static void module_unload_cb(void *context, void *handle)
{
    (void)context;
    jpp_native_loader_module_free((jpp_native_loaded_module_t *)handle);
}

/* ---- Consent-prompt callback --------------------------------------------- */

static bool consent_prompt_cb(void *context, const char *cap, int tier)
{
    (void)context;
    return jpp_app_consent_prompt(s_active_sdk_context, cap, tier);
}

/* ---- Path-prompt callback ------------------------------------------------ */

static bool sd_path_prompt_cb(void *context, const char *path, jpp_sdk_open_mode_t mode)
{
    (void)context;
    if (s_active_sdk_context == NULL) {
        return false;
    }
    /* Use the shared consent surface (signature line + Deny/Allow selector) so a
       file-access prompt looks identical to a capability prompt. The path is
       chunked across up to three rows since confirm does not word-wrap. */
    char head[24];
    char chunk[3][24];
    const char *body[4];
    size_t bn = 0u;
    snprintf(head, sizeof(head), "Allow %s access:",
             mode == JPP_SDK_OPEN_WRITE ? "write" : "read");
    body[bn++] = head;

    size_t plen = strlen(path);
    size_t off  = 0u;
    for (size_t i = 0u; i < 3u && off < plen; i++) {
        size_t take = plen - off;
        if (take > 21u) take = 21u;
        memcpy(chunk[i], path + off, take);
        chunk[i][take] = '\0';
        body[bn++] = chunk[i];
        off += take;
    }

    bool allow = false;
    jpp_sdk_confirm(s_active_sdk_context, "File access", body, bn, false, &allow);
    return allow;
}

/* ---- SD file I/O callbacks ----------------------------------------------- */

/* Static buffer for file content; safe under the "storage" exclusive lock. */
static char s_file_read_buf[4096];

/* Set ejection flag if the error is a hardware/media failure on the SD. */
static void check_sd_ejection(const char *path)
{
    if (path == NULL || strncmp(path, "/sd/", 4) != 0) return;
    if (errno == EIO || errno == ENODEV || errno == ESTALE) {
        s_sd_ejection_detected = true;
        ESP_LOGW(TAG, "SD_EJECTION_DETECTED errno=%d path=%s", errno, path);
    }
}

static jpp_broker_status_t sd_file_read_cb(void *context,
                                            const char *logical_path,
                                            jpp_broker_result_t *result)
{
    (void)context;
    if (result == NULL || logical_path == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }
    FILE *f = fopen(logical_path, "r");
    if (f == NULL) {
        check_sd_ejection(logical_path);
        jpp_broker_error_result(result, "FILE_NOT_FOUND");
        return JPP_BROKER_STATUS_OK;
    }
    size_t n = fread(s_file_read_buf, 1u, sizeof(s_file_read_buf) - 1u, f);
    fclose(f);
    s_file_read_buf[n] = '\0';
    jpp_broker_ok_result(result);
    jpp_broker_result_put(result, "text", s_file_read_buf);
    return JPP_BROKER_STATUS_OK;
}

static jpp_broker_status_t sd_file_write_cb(void *context,
                                             const char *logical_path,
                                             const char *text,
                                             jpp_broker_result_t *result)
{
    (void)context;
    if (result == NULL || logical_path == NULL || text == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }
    jpp_make_parent_dirs(logical_path);
    FILE *f = fopen(logical_path, "w");
    if (f == NULL) {
        jpp_broker_error_result(result, "WRITE_FAILED");
        return JPP_BROKER_STATUS_OK;
    }
    fputs(text, f);
    fclose(f);
    jpp_broker_ok_result(result);
    return JPP_BROKER_STATUS_OK;
}

static jpp_broker_status_t sd_file_list_cb(void *context,
                                            const char *logical_path,
                                            jpp_broker_result_t *result)
{
    (void)context;
    if (result == NULL || logical_path == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }
    DIR *d = opendir(logical_path);
    if (d == NULL) {
        jpp_broker_error_result(result, "DIR_NOT_FOUND");
        return JPP_BROKER_STATUS_OK;
    }
    jpp_broker_ok_result(result);

    /* Two views of the same listing: indexed "entry_N" fields for programmatic
       consumers (e.g. ipc_recv reads "entry_0"), plus a newline-joined "text"
       field so apps and the SDK test screens can display the listing directly.
       Static buffer is safe under the "storage" exclusive lock. The result field
       table holds only JPP_BROKER_RESULT_FIELD_CAPACITY entries, so reserve the
       last slot for "text" and cap entry_N at one fewer. */
    static char s_list_text[1024];
    s_list_text[0] = '\0';
    size_t pos = 0u;
    char key[16];
    size_t idx = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        int w = snprintf(s_list_text + pos, sizeof(s_list_text) - pos,
                         "%s%s", pos > 0u ? "\n" : "", ent->d_name);
        if (w > 0 && (size_t)w < sizeof(s_list_text) - pos) {
            pos += (size_t)w;
        }
        if (idx + 1u < JPP_BROKER_RESULT_FIELD_CAPACITY) {
            snprintf(key, sizeof(key), "entry_%zu", idx);
            if (jpp_broker_result_put(result, key, ent->d_name) == JPP_BROKER_STATUS_OK) {
                idx++;
            }
        }
    }
    closedir(d);
    jpp_broker_result_put(result, "text", s_list_text);
    return JPP_BROKER_STATUS_OK;
}

/* List a directory at abs_path; returns newline-separated entries with dirs
   suffixed by "/". Used internally by jpp_sdk_file_pick(). */
static size_t sd_dir_list_cb(void *context, const char *abs_path,
                               char *buf, size_t buf_len)
{
    (void)context;
    if (abs_path == NULL || buf == NULL || buf_len == 0u) return 0u;
    buf[0] = '\0';

    DIR *d = opendir(abs_path);
    if (d == NULL) return 0u;

    size_t count = 0u;
    size_t pos   = 0u;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        bool is_dir_entry = (ent->d_type == DT_DIR);
        if (ent->d_type == DT_UNKNOWN) {
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", abs_path, ent->d_name);
            struct stat st;
            if (stat(full, &st) == 0) { is_dir_entry = (bool)S_ISDIR(st.st_mode); }
        }

        const char *suffix = is_dir_entry ? "/" : "";
        int written = snprintf(buf + pos, buf_len - pos, "%s%s\n",
                                ent->d_name, suffix);
        if (written <= 0 || (size_t)written >= buf_len - pos) break;
        pos += (size_t)written;
        count++;
    }
    closedir(d);
    if (pos < buf_len) { buf[pos] = '\0'; }
    return count;
}

static jpp_broker_status_t sd_file_delete_cb(void *context,
                                              const char *logical_path,
                                              jpp_broker_result_t *result)
{
    (void)context;
    if (result == NULL || logical_path == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }
    int err = remove(logical_path);
    if (err != 0) {
        jpp_broker_error_result(result, "DELETE_FAILED");
        return JPP_BROKER_STATUS_OK;
    }
    jpp_broker_ok_result(result);
    return JPP_BROKER_STATUS_OK;
}

/* ---- HTTP request callback ----------------------------------------------- */

typedef struct {
    char body[2048];
    int  status_code;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *resp = (http_response_t *)evt->user_data;
    if (resp == NULL) {
        return ESP_OK;
    }
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA: {
        size_t remaining = sizeof(resp->body) - 1u - strlen(resp->body);
        size_t to_copy = evt->data_len < remaining ? (size_t)evt->data_len : remaining;
        strncat(resp->body, (const char *)evt->data, to_copy);
        break;
    }
    default:
        break;
    }
    return ESP_OK;
}

/* ---- network.bind callbacks (TCP server over lwIP) ----------------------- */

/* One app runs at a time, so one static socket table serves the whole SDK. */
static int s_net_listener = -1;
static int s_net_conns[JPP_RESOURCE_SDK_NET_SOCKET_LIMIT] = {-1, -1};

static bool net_sock_is_open(int sock)
{
    for (size_t i = 0u; i < JPP_RESOURCE_SDK_NET_SOCKET_LIMIT; i++) {
        if (s_net_conns[i] == sock && sock >= 0) { return true; }
    }
    return false;
}

static jpp_broker_status_t net_bind_cb(void *context, uint16_t port,
                                       jpp_broker_result_t *result)
{
    (void)context;
    jpp_fileserver_status_t fs_status;
    jpp_fileserver_get_status(&fs_status);
    if (fs_status.state == JPP_FILESERVER_STATE_RUNNING ||
        jpp_lrv_server_is_running()) {
        jpp_broker_error_result(result, "SERVER_ACTIVE");
        return JPP_BROKER_STATUS_OK;
    }
    if (s_net_listener >= 0) {
        jpp_broker_error_result(result, "ALREADY_BOUND");
        return JPP_BROKER_STATUS_OK;
    }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        jpp_broker_error_result(result, "SOCKET_FAILED");
        return JPP_BROKER_STATUS_OK;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 1) != 0) {
        close(fd);
        jpp_broker_error_result(result, "BIND_FAILED");
        return JPP_BROKER_STATUS_OK;
    }

    s_net_listener = fd;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    jpp_broker_ok_result(result);
    jpp_broker_result_put(result, "port", port_str);
    ESP_LOGI(TAG, "NET_BIND port=%u", (unsigned)port);
    return JPP_BROKER_STATUS_OK;
}

/* Wait up to timeout_ms for readability on fd. Returns >0 ready, 0 timeout. */
static int net_wait_readable(int fd, uint32_t timeout_ms)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = {
        .tv_sec  = (time_t)(timeout_ms / 1000u),
        .tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u),
    };
    return select(fd + 1, &rfds, NULL, NULL, &tv);
}

static jpp_broker_status_t net_accept_cb(void *context, uint32_t timeout_ms,
                                         int *out_sock, jpp_broker_result_t *result)
{
    (void)context;
    *out_sock = -1;
    if (s_net_listener < 0) {
        jpp_broker_error_result(result, "NOT_BOUND");
        return JPP_BROKER_STATUS_OK;
    }

    int slot = -1;
    for (size_t i = 0u; i < JPP_RESOURCE_SDK_NET_SOCKET_LIMIT; i++) {
        if (s_net_conns[i] < 0) { slot = (int)i; break; }
    }

    int ready = net_wait_readable(s_net_listener, timeout_ms);
    if (ready <= 0) {
        jpp_broker_ok_result(result);   /* timeout: *out_sock stays -1 */
        return JPP_BROKER_STATUS_OK;
    }

    int conn = accept(s_net_listener, NULL, NULL);
    if (conn < 0) {
        jpp_broker_error_result(result, "ACCEPT_FAILED");
        return JPP_BROKER_STATUS_OK;
    }
    if (slot < 0) {
        /* Connection table full: refuse the peer rather than leak the fd. */
        close(conn);
        jpp_broker_error_result(result, "SOCKET_LIMIT");
        return JPP_BROKER_STATUS_OK;
    }
    s_net_conns[slot] = conn;
    *out_sock = conn;
    jpp_broker_ok_result(result);
    return JPP_BROKER_STATUS_OK;
}

static jpp_broker_status_t net_recv_cb(void *context, int sock, uint8_t *buf,
                                       size_t buf_len, size_t *out_len,
                                       uint32_t timeout_ms, jpp_broker_result_t *result)
{
    (void)context;
    *out_len = 0u;
    if (!net_sock_is_open(sock)) {
        jpp_broker_error_result(result, "INVALID_SOCKET");
        return JPP_BROKER_STATUS_OK;
    }

    int ready = net_wait_readable(sock, timeout_ms);
    if (ready <= 0) {
        jpp_broker_ok_result(result);   /* timeout: *out_len stays 0 */
        return JPP_BROKER_STATUS_OK;
    }

    ssize_t n = recv(sock, buf, buf_len, 0);
    if (n < 0) {
        jpp_broker_error_result(result, "RECV_FAILED");
        return JPP_BROKER_STATUS_OK;
    }
    jpp_broker_ok_result(result);
    if (n == 0) {
        jpp_broker_result_put(result, "closed", "1");   /* peer disconnected */
    }
    *out_len = (size_t)n;
    return JPP_BROKER_STATUS_OK;
}

static jpp_broker_status_t net_send_cb(void *context, int sock,
                                       const uint8_t *data, size_t len,
                                       jpp_broker_result_t *result)
{
    (void)context;
    if (!net_sock_is_open(sock)) {
        jpp_broker_error_result(result, "INVALID_SOCKET");
        return JPP_BROKER_STATUS_OK;
    }
    size_t sent = 0u;
    while (sent < len) {
        ssize_t n = send(sock, data + sent, len - sent, 0);
        if (n <= 0) {
            jpp_broker_error_result(result, "SEND_FAILED");
            return JPP_BROKER_STATUS_OK;
        }
        sent += (size_t)n;
    }
    jpp_broker_ok_result(result);
    return JPP_BROKER_STATUS_OK;
}

static jpp_broker_status_t net_close_cb(void *context, int sock,
                                        jpp_broker_result_t *result)
{
    (void)context;
    if (sock == -1) {
        if (s_net_listener >= 0) {
            close(s_net_listener);
            s_net_listener = -1;
        }
        jpp_broker_ok_result(result);
        return JPP_BROKER_STATUS_OK;
    }
    for (size_t i = 0u; i < JPP_RESOURCE_SDK_NET_SOCKET_LIMIT; i++) {
        if (s_net_conns[i] == sock) {
            close(sock);
            s_net_conns[i] = -1;
            jpp_broker_ok_result(result);
            return JPP_BROKER_STATUS_OK;
        }
    }
    jpp_broker_error_result(result, "INVALID_SOCKET");
    return JPP_BROKER_STATUS_OK;
}

static void net_close_all_cb(void *context)
{
    (void)context;
    for (size_t i = 0u; i < JPP_RESOURCE_SDK_NET_SOCKET_LIMIT; i++) {
        if (s_net_conns[i] >= 0) {
            close(s_net_conns[i]);
            s_net_conns[i] = -1;
        }
    }
    if (s_net_listener >= 0) {
        close(s_net_listener);
        s_net_listener = -1;
        ESP_LOGI(TAG, "NET_CLOSED (teardown)");
    }
}

static jpp_broker_status_t sd_http_request_cb(void *context,
                                               const char *method,
                                               const char *url,
                                               const char *body,
                                               jpp_broker_result_t *result)
{
    (void)context;
    if (result == NULL || method == NULL || url == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }
    static http_response_t s_http_resp;
    memset(&s_http_resp, 0, sizeof(s_http_resp));

    esp_http_client_config_t cfg = {
        .url           = url,
        .event_handler = http_event_handler,
        .user_data     = &s_http_resp,
        .timeout_ms    = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        jpp_broker_error_result(result, "HTTP_INIT_FAILED");
        return JPP_BROKER_STATUS_OK;
    }

    bool is_post = (strcmp(method, "POST") == 0);
    if (is_post && body != NULL) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        s_http_resp.status_code = esp_http_client_get_status_code(client);
    }
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        jpp_broker_error_result(result, "HTTP_PERFORM_FAILED");
        return JPP_BROKER_STATUS_OK;
    }

    static char s_status_code_str[8];
    snprintf(s_status_code_str, sizeof(s_status_code_str), "%d", s_http_resp.status_code);
    jpp_broker_ok_result(result);
    jpp_broker_result_put(result, "status_code", s_status_code_str);
    jpp_broker_result_put(result, "body", s_http_resp.body);
    return JPP_BROKER_STATUS_OK;
}

/* ---- KV store callbacks --------------------------------------------------- */

static jpp_broker_status_t sd_kv_read_cb(void *context,
                                          const char *app_id,
                                          const char *key,
                                          char *out_value,
                                          size_t buf_len,
                                          jpp_broker_result_t *result)
{
    (void)context;
    if (result == NULL || app_id == NULL || key == NULL || out_value == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }
    char path[128];
    snprintf(path, sizeof(path), "/sd/apps/%s/.kv.json", app_id);

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        jpp_broker_error_result(result, "KEY_NOT_FOUND");
        return JPP_BROKER_STATUS_OK;
    }
    char buf[2048];
    size_t n = fread(buf, 1u, sizeof(buf) - 1u, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        jpp_broker_error_result(result, "KV_CORRUPT");
        return JPP_BROKER_STATUS_OK;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        cJSON_Delete(root);
        jpp_broker_error_result(result, "KEY_NOT_FOUND");
        return JPP_BROKER_STATUS_OK;
    }
    strncpy(out_value, item->valuestring, buf_len - 1u);
    out_value[buf_len - 1u] = '\0';
    cJSON_Delete(root);
    jpp_broker_ok_result(result);
    return JPP_BROKER_STATUS_OK;
}

static jpp_broker_status_t sd_kv_write_cb(void *context,
                                           const char *app_id,
                                           const char *key,
                                           const char *value,
                                           jpp_broker_result_t *result)
{
    (void)context;
    if (result == NULL || app_id == NULL || key == NULL || value == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }
    char path[128];
    char tmp_path[136];
    snprintf(path, sizeof(path), "/sd/apps/%s/.kv.json", app_id);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    jpp_make_parent_dirs(path);

    cJSON *root = NULL;
    FILE *f = fopen(path, "r");
    if (f != NULL) {
        char buf[2048];
        size_t n = fread(buf, 1u, sizeof(buf) - 1u, f);
        fclose(f);
        buf[n] = '\0';
        root = cJSON_Parse(buf);
    }
    if (root == NULL) {
        root = cJSON_CreateObject();
    }

    cJSON_DeleteItemFromObject(root, key);
    cJSON_AddStringToObject(root, key, value);

    char *serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (serialized == NULL) {
        jpp_broker_error_result(result, "KV_SERIALIZE_FAILED");
        return JPP_BROKER_STATUS_OK;
    }

    f = fopen(tmp_path, "w");
    if (f != NULL) {
        fputs(serialized, f);
        fclose(f);
        rename(tmp_path, path);
    }
    free(serialized);
    jpp_broker_ok_result(result);
    return JPP_BROKER_STATUS_OK;
}

static jpp_broker_status_t sd_kv_delete_cb(void *context,
                                            const char *app_id,
                                            const char *key,
                                            jpp_broker_result_t *result)
{
    (void)context;
    if (result == NULL || app_id == NULL || key == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }
    char path[128];
    char tmp_path[136];
    snprintf(path, sizeof(path), "/sd/apps/%s/.kv.json", app_id);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        jpp_broker_ok_result(result);
        return JPP_BROKER_STATUS_OK;
    }
    char buf[2048];
    size_t n = fread(buf, 1u, sizeof(buf) - 1u, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        jpp_broker_ok_result(result);
        return JPP_BROKER_STATUS_OK;
    }

    cJSON_DeleteItemFromObject(root, key);
    char *serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (serialized != NULL) {
        f = fopen(tmp_path, "w");
        if (f != NULL) {
            fputs(serialized, f);
            fclose(f);
            rename(tmp_path, path);
        }
        free(serialized);
    }
    jpp_broker_ok_result(result);
    return JPP_BROKER_STATUS_OK;
}

/* ---- RTC reader callback ------------------------------------------------- */

static jpp_broker_status_t rtc_reader_cb(void *context, jpp_broker_result_t *result)
{
    (void)context;

    if (result == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }

    if (s_rtc_state_ptr == NULL || !s_rtc_state_ptr->has_datetime) {
        jpp_broker_error_result(result, "RTC_UNAVAILABLE");
        return JPP_BROKER_STATUS_OK;
    }

    /* Format: "YYYY-MM-DD HH:mm".
       Must be static: jpp_broker_result_put() stores the pointer without copying,
       and the result is read after this callback returns — a stack buffer here
       would dangle. Safe because the "device" exclusive lock serializes access. */
    static char buf[32];
    snprintf(buf, sizeof(buf),
             "%04d-%02d-%02d %02d:%02d",
             (int)s_rtc_state_ptr->datetime.year,
             (int)s_rtc_state_ptr->datetime.month,
             (int)s_rtc_state_ptr->datetime.day,
             (int)s_rtc_state_ptr->datetime.hour,
             (int)s_rtc_state_ptr->datetime.minute);

    jpp_broker_ok_result(result);
    jpp_broker_result_put(result, "text", buf);
    return JPP_BROKER_STATUS_OK;
}

/* ---- Device status callback ---------------------------------------------- */

static jpp_broker_status_t device_status_cb(void *context, jpp_broker_result_t *result)
{
    (void)context;

    if (result == NULL) {
        return JPP_BROKER_STATUS_INVALID_ARGUMENT;
    }

    /* Static: jpp_broker_result_put() stores the pointer without copying; the
       "device" exclusive lock serializes access so reuse is safe. */
    static char battery_pct[8];
    if (s_battery_state_ptr != NULL && s_battery_state_ptr->valid) {
        snprintf(battery_pct, sizeof(battery_pct), "%d", s_battery_state_ptr->percent);
    } else {
        snprintf(battery_pct, sizeof(battery_pct), "-1");
    }

    jpp_broker_ok_result(result);
    jpp_broker_result_put(result, "battery_pct", battery_pct);
    /* The board has no charge-detect line; charging state is unknown ("-1"),
       mirroring the battery_pct convention. */
    jpp_broker_result_put(result, "charging", "-1");
    return JPP_BROKER_STATUS_OK;
}

/* ---- Log writer callback ------------------------------------------------- */

static void log_writer_cb(void *context, const char *app_id, const char *event_name)
{
    (void)context;
    ESP_LOGI("app_log", "[%s] %s",
             app_id != NULL ? app_id : "?",
             event_name != NULL ? event_name : "?");
}

/* ---- Public API ---------------------------------------------------------- */

void jpp_native_services_init(jpp_rtc_state_t *rtc_state)
{
    s_rtc_state_ptr = rtc_state;

    jpp_broker_lock_set_init(&s_broker_locks);

    memset(&s_native_services, 0, sizeof(s_native_services));
    s_native_services.locks = &s_broker_locks;

    jpp_ble_native_get_services(
        &s_native_services.ble_scan,          &s_native_services.ble_scan_context,
        &s_native_services.ble_advertise_start,
        &s_native_services.ble_advertise_stop, &s_native_services.ble_advertise_context,
        &s_native_services.ble_connect,
        &s_native_services.ble_read_char,
        &s_native_services.ble_write_char,
        &s_native_services.ble_disconnect,     &s_native_services.ble_client_context,
        &s_native_services.ble_service_register,
        &s_native_services.ble_service_unregister, &s_native_services.ble_host_context
    );
    jpp_ble_native_get_host_services(
        &s_native_services.ble_host_set_value,
        &s_native_services.ble_host_wait_write,
        &s_native_services.ble_host_clear,
        &s_native_services.ble_set_connectable
    );

    s_native_services.rtc_reader         = rtc_reader_cb;
    s_native_services.rtc_reader_context = NULL;

    s_native_services.device_status         = device_status_cb;
    s_native_services.device_status_context = NULL;

    s_native_services.log_writer  = log_writer_cb;
    s_native_services.log_context = NULL;

    s_native_services.path_prompt          = sd_path_prompt_cb;
    s_native_services.path_prompt_context  = NULL;

    s_native_services.consent_prompt         = consent_prompt_cb;
    s_native_services.consent_prompt_context = NULL;

    s_native_services.read_file          = sd_file_read_cb;
    s_native_services.read_file_context  = NULL;
    s_native_services.write_file         = sd_file_write_cb;
    s_native_services.write_file_context = NULL;
    s_native_services.list_files         = sd_file_list_cb;
    s_native_services.list_files_context = NULL;
    s_native_services.delete_file        = sd_file_delete_cb;
    s_native_services.delete_file_context = NULL;

    s_native_services.http_request        = sd_http_request_cb;
    s_native_services.http_request_context = NULL;

    s_native_services.net_bind      = net_bind_cb;
    s_native_services.net_accept    = net_accept_cb;
    s_native_services.net_recv      = net_recv_cb;
    s_native_services.net_send      = net_send_cb;
    s_native_services.net_close     = net_close_cb;
    s_native_services.net_close_all = net_close_all_cb;
    s_native_services.net_context   = NULL;

    s_native_services.kv_read    = sd_kv_read_cb;
    s_native_services.kv_write   = sd_kv_write_cb;
    s_native_services.kv_delete  = sd_kv_delete_cb;
    s_native_services.kv_context = NULL;

    s_native_services.dir_list         = sd_dir_list_cb;
    s_native_services.dir_list_context = NULL;

    s_native_services.module_load    = module_load_cb;
    s_native_services.module_run     = module_run_cb;
    s_native_services.module_unload  = module_unload_cb;
    s_native_services.module_context = NULL;

    ESP_LOGI(TAG, "BLE_NATIVE_READY");
}
