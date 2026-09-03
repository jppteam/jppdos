#include "jpp_app_dispatch.h"
#include "jpp_file_util.h"
#include "jpp_native_services.h"
#include "jpp_settings_load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "jpp_manifest_core.h"
#include "jpp_app_loader_core.h"
#include "jpp_mp_runner.h"
#include "jpp_broker_core.h"
#include "jpp_vm_core.h"
#include "jpp_sdk_bridge.h"
#include "jpp_ui_core.h"
#include "jpp_native_loader_core.h"
#include "jpp_bg_scheduler.h"

static const char *TAG = "app_dispatch";

#define SD_APPS_PATH       "/sd/apps"
/* Must be >= the largest capability list any manifest can declare, and
   <= JPP_SDK_PENDING_CAP_MAX. A smaller value silently truncates the manifest
   list, dropping the trailing caps so they can never be granted. */
#define SD_MANIFEST_CAP_MAX 16u

/* ---- Shared runtime state (externs declared in jpp_app_dispatch.h) ------- */

jpp_sdk_context_t  *s_active_sdk_context = NULL;
jpp_sdk_context_t   s_sd_ctx;
jpp_vm_context_t    s_sd_vm;
TaskHandle_t        s_sd_task   = NULL;
bool                s_sd_is_mp  = false;

static char s_sd_app_id[64];
static char s_sd_app_root[160];

/* Static task stack and TCB — avoids heap allocation failure from post-WiFi/BLE
 * fragmentation (same root cause as the shared jpp_app_pool).
 * Kept at 12 KB (not 16 KB) to leave headroom for WiFi association state. */
#define SD_APP_TASK_STACK_BYTES 12288u
static StackType_t  s_sd_task_stack[SD_APP_TASK_STACK_BYTES / sizeof(StackType_t)];
static StaticTask_t s_sd_task_tcb;

typedef struct {
    jpp_manifest_app_type_t  app_type;
    char                     entry_path[256];
    char                     bg_task[32];   /* "" = foreground session */
} sd_task_args_t;
static sd_task_args_t s_sd_task_args;

/* Headless background-run state (one run at a time, same task slot). */
static volatile bool s_bg_run_active = false;
static volatile bool s_bg_run_done   = false;

static bool grant_is_persisted(const char *app_id, const char *cap);

/* ---- Crash reporting ------------------------------------------------------ */

#define CRASH_LOG_PATH "/data/ui_crash.log"

static volatile bool s_crash_pending = false;
static char          s_crash_app[64];     /* snapshot: teardown clears s_sd_app_id */
static char          s_crash_reason[32];

/* Record an app failure: emit the APP_CRASH marker, append a line to the
   crash log, and arm the flag the main loop consumes via jpp_app_crash_take()
   to show the crash dialog after teardown.  Runs on the app task. */
static void record_app_crash(const char *reason)
{
    ESP_LOGE(TAG, "APP_CRASH app=%s reason=%s", s_sd_app_id, reason);

    FILE *f = fopen(CRASH_LOG_PATH, "a");
    if (f != NULL) {
        fprintf(f, "app=%s reason=%s\n", s_sd_app_id, reason);
        fclose(f);
    }

    strncpy(s_crash_app, s_sd_app_id, sizeof(s_crash_app) - 1u);
    s_crash_app[sizeof(s_crash_app) - 1u] = '\0';
    strncpy(s_crash_reason, reason, sizeof(s_crash_reason) - 1u);
    s_crash_reason[sizeof(s_crash_reason) - 1u] = '\0';
    s_crash_pending = true;
}

bool jpp_app_crash_take(char *app_id, size_t app_id_len,
                        char *reason, size_t reason_len)
{
    if (!s_crash_pending) {
        return false;
    }
    if (app_id != NULL && app_id_len > 0u) {
        strncpy(app_id, s_crash_app, app_id_len - 1u);
        app_id[app_id_len - 1u] = '\0';
    }
    if (reason != NULL && reason_len > 0u) {
        strncpy(reason, s_crash_reason, reason_len - 1u);
        reason[reason_len - 1u] = '\0';
    }
    s_crash_pending = false;
    return true;
}

/* ---- App discovery ------------------------------------------------------- */

/*
 * Best-effort read of manifest.json's "name" field for the launcher label.
 * Leaves out_name untouched (callers pre-fill it with app_id as a fallback)
 * if the file is missing, unparsable, or the field is absent/empty --
 * discovery intentionally skips full manifest validation here.
 */
static void read_manifest_display_name(const char *app_root, const char *app_id,
                                        char *out_name, size_t out_name_len)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/%s/manifest.json", app_root, app_id);

    char *buf = jpp_read_file(path, NULL);
    if (buf == NULL) {
        return;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        return;
    }

    cJSON *j = cJSON_GetObjectItem(root, "name");
    if (cJSON_IsString(j) && j->valuestring && j->valuestring[0] != '\0') {
        strncpy(out_name, j->valuestring, out_name_len - 1u);
        out_name[out_name_len - 1u] = '\0';
    }
    cJSON_Delete(root);
}

void discover_apps(bool normal_mode,
                   jpp_boot_discovery_summary_t *summary,
                   jpp_ui_shell_t *shell)
{
    memset(summary, 0, sizeof(*summary));

    summary->builtin_count = 2u;
    jpp_ui_shell_add_app(shell, "settings", "Settings",
                         JPP_UI_APP_SOURCE_BUILTIN);
    jpp_ui_shell_add_app(shell, "webdav", "WebDAV server",
                         JPP_UI_APP_SOURCE_BUILTIN);

#ifdef JPP_WOKWI_EMBED_APP
    {
        summary->sd_count++;
        jpp_ui_shell_add_app(shell, JPP_WOKWI_EMBED_APP_ID,
                             JPP_WOKWI_EMBED_APP_ID,
                             JPP_UI_APP_SOURCE_SD);
        ESP_LOGI(TAG, "EMBED app registered: %s", JPP_WOKWI_EMBED_APP_ID);
    }
#endif

    if (!normal_mode) {
        summary->total_count = summary->builtin_count;
        return;
    }

    DIR *dir = opendir(SD_APPS_PATH);
    if (dir == NULL) {
        summary->total_count = summary->builtin_count;
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type != DT_DIR) {
            continue;
        }
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (jpp_manifest_v2_is_reserved_app_id(ent->d_name)) {
            ESP_LOGW(TAG, "APP_REJECTED %s: RESERVED_APP_ID", ent->d_name);
            summary->rejected_count++;
            continue;
        }
        char manifest_path[300];
        snprintf(manifest_path, sizeof(manifest_path),
                 "%s/%s/manifest.json", SD_APPS_PATH, ent->d_name);
        ESP_LOGI(TAG, "Loading app manifest from %s", manifest_path);
        if (!file_exists(manifest_path)) {
            ESP_LOGW(TAG, "APP_REJECTED %s: manifest.json missing", ent->d_name);
            summary->rejected_count++;
            continue;
        }

        ESP_LOGI(TAG, "SD app discovered: %s", ent->d_name);
        summary->sd_count++;
        char display_name[JPP_UI_TEXT_LIMIT];
        strncpy(display_name, ent->d_name, sizeof(display_name) - 1u);
        display_name[sizeof(display_name) - 1u] = '\0';
        read_manifest_display_name(SD_APPS_PATH, ent->d_name,
                                   display_name, sizeof(display_name));
        jpp_ui_shell_add_app(shell, ent->d_name, display_name,
                             JPP_UI_APP_SOURCE_SD);
    }
    closedir(dir);

    summary->total_count =
        summary->builtin_count +
        summary->sd_count +
        summary->disabled_count +
        summary->rejected_count;

    /* Boot population, not a rebuild: open the launcher at the top of the list.
       (jpp_ui_shell_add_app() otherwise keeps the cursor on whichever app it
       was already on, which in "system apps at the bottom" order would leave it
       pinned to Settings — index 0 at the time the first SD app was inserted.) */
    shell->selected_app = 0u;
}

/* ---- Background app re-discovery ----------------------------------------- */

#define BG_DISC_APP_MAX 6u

typedef struct {
    char id[JPP_UI_TEXT_LIMIT];
    char name[JPP_UI_TEXT_LIMIT];
} bg_disc_entry_t;

static bg_disc_entry_t     s_bg_disc_apps[BG_DISC_APP_MAX];
static size_t              s_bg_disc_count   = 0u;
static volatile bool       s_bg_disc_running = false;
static volatile bool       s_bg_disc_ready   = false;
static bool                s_bg_disc_normal  = false;

static void bg_discover_task(void *arg)
{
    (void)arg;
    s_bg_disc_count = 0u;

    if (s_bg_disc_normal) {
        DIR *dir = opendir(SD_APPS_PATH);
        if (dir != NULL) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL &&
                   s_bg_disc_count < BG_DISC_APP_MAX) {
                if (ent->d_type != DT_DIR) { continue; }
                if (strcmp(ent->d_name, ".") == 0 ||
                    strcmp(ent->d_name, "..") == 0) { continue; }
                if (jpp_manifest_v2_is_reserved_app_id(ent->d_name)) { continue; }
                char mpath[300];
                snprintf(mpath, sizeof(mpath), "%s/%s/manifest.json",
                         SD_APPS_PATH, ent->d_name);
                if (!file_exists(mpath)) { continue; }
                strncpy(s_bg_disc_apps[s_bg_disc_count].id,
                        ent->d_name, JPP_UI_TEXT_LIMIT - 1u);
                s_bg_disc_apps[s_bg_disc_count].id[JPP_UI_TEXT_LIMIT - 1u] = '\0';
                strncpy(s_bg_disc_apps[s_bg_disc_count].name,
                        ent->d_name, JPP_UI_TEXT_LIMIT - 1u);
                s_bg_disc_apps[s_bg_disc_count].name[JPP_UI_TEXT_LIMIT - 1u] = '\0';
                read_manifest_display_name(SD_APPS_PATH, ent->d_name,
                                           s_bg_disc_apps[s_bg_disc_count].name,
                                           JPP_UI_TEXT_LIMIT);
                s_bg_disc_count++;
            }
            closedir(dir);
        }
    }

    s_bg_disc_running = false;
    s_bg_disc_ready   = true;
    vTaskDelete(NULL);
}

void discover_apps_background_start(bool normal_mode)
{
    if (s_bg_disc_running) { return; }
    s_bg_disc_running = true;
    s_bg_disc_ready   = false;
    s_bg_disc_normal  = normal_mode;
    xTaskCreate(bg_discover_task, "bg_disc", 4096, NULL, 2, NULL);
}

bool discover_apps_background_ready(void)
{
    if (!s_bg_disc_ready) { return false; }
    s_bg_disc_ready = false;
    return true;
}

void discover_apps_apply_to_shell(jpp_ui_shell_t *shell)
{
    /* Rebuilding the catalogue shuffles indices (SD apps sit *before* the
       system group when those are shown at the bottom), so hold the cursor by
       app_id rather than by index. */
    char selected_id[JPP_UI_TEXT_LIMIT] = "";
    if (shell->selected_app < shell->app_count) {
        strncpy(selected_id, shell->apps[shell->selected_app].app_id,
                sizeof(selected_id) - 1u);
    }

    jpp_ui_shell_clear_sd_apps(shell);
    for (size_t i = 0u; i < s_bg_disc_count; i++) {
        jpp_ui_shell_add_app(shell, s_bg_disc_apps[i].id,
                             s_bg_disc_apps[i].name,
                             JPP_UI_APP_SOURCE_SD);
    }

    for (size_t i = 0u; i < shell->app_count; i++) {
        if (strcmp(shell->apps[i].app_id, selected_id) == 0) {
            shell->selected_app = i;
            break;
        }
    }
}

/* ---- Manifest loading ---------------------------------------------------- */

typedef struct {
    char app_id[64];
    char name[64];
    char version[32];
    char entry[160];
    char runtime_version[32];
    char cross_version[32];
    char cap_strs[SD_MANIFEST_CAP_MAX][32];
    const char *cap_ptrs[SD_MANIFEST_CAP_MAX];
    size_t cap_count;
    char bg_task_names[JPP_MANIFEST_V2_BG_TASK_MAX][32];
    char bg_task_crons[JPP_MANIFEST_V2_BG_TASK_MAX][24];
    jpp_manifest_bg_task_t bg_tasks[JPP_MANIFEST_V2_BG_TASK_MAX];
} sd_manifest_strings_t;

/* Manifest of the most recently launched app — file-scope statics so the app
   task can read the background.tasks list when the session ends. */
static sd_manifest_strings_t s_sd_manifest_strs;
static jpp_manifest_v2_t     s_sd_manifest;

static bool load_sd_manifest(const char *app_root,
                              const char *app_id,
                              sd_manifest_strings_t *strs,
                              jpp_manifest_v2_t *out)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/%s/manifest.json", app_root, app_id);

    char *buf = jpp_read_file(path, NULL);
    if (buf == NULL) {
        return false;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        return false;
    }

    memset(strs, 0, sizeof(*strs));
    memset(out, 0, sizeof(*out));

#define STR_FROM(key, dst) do { \
    cJSON *_j = cJSON_GetObjectItem(root, (key)); \
    if (cJSON_IsString(_j) && _j->valuestring) { \
        strncpy((dst), _j->valuestring, sizeof(dst) - 1u); \
    } \
} while (0)
#define STR_FROM_OBJ(obj, key, dst) do { \
    cJSON *_j = cJSON_GetObjectItem((obj), (key)); \
    if (cJSON_IsString(_j) && _j->valuestring) { \
        strncpy((dst), _j->valuestring, sizeof(dst) - 1u); \
    } \
} while (0)

    strncpy(strs->app_id, app_id, sizeof(strs->app_id) - 1u);
    STR_FROM("name",    strs->name);
    STR_FROM("version", strs->version);
    STR_FROM("entry",   strs->entry);

    out->app_type = JPP_APP_TYPE_MICROPYTHON;
    cJSON *j_type = cJSON_GetObjectItem(root, "app_type");
    if (cJSON_IsString(j_type) && j_type->valuestring &&
        strcmp(j_type->valuestring, "native") == 0) {
        out->app_type = JPP_APP_TYPE_NATIVE;
    }

    cJSON *j = cJSON_GetObjectItem(root, "schema_version");
    out->schema_version = cJSON_IsNumber(j) ? (int)j->valuedouble : 2;
    j = cJSON_GetObjectItem(root, "sdk_min");
    out->sdk_min = cJSON_IsNumber(j) ? (int)j->valuedouble : 1;

    cJSON *caps = cJSON_GetObjectItem(root, "capabilities");
    if (cJSON_IsArray(caps)) {
        cJSON *cap;
        cJSON_ArrayForEach(cap, caps) {
            if (strs->cap_count >= SD_MANIFEST_CAP_MAX) {
                break;
            }
            if (cJSON_IsString(cap) && cap->valuestring) {
                strncpy(strs->cap_strs[strs->cap_count],
                        cap->valuestring,
                        sizeof(strs->cap_strs[0]) - 1u);
                strs->cap_ptrs[strs->cap_count] =
                    strs->cap_strs[strs->cap_count];
                strs->cap_count++;
            }
        }
    }

    cJSON *bg = cJSON_GetObjectItem(root, "background");
    out->background.enabled = 0;
    out->background.tasks = strs->bg_tasks;
    out->background.task_count = 0u;
    if (cJSON_IsObject(bg)) {
        j = cJSON_GetObjectItem(bg, "enabled");
        out->background.enabled = cJSON_IsTrue(j) ? 1 : 0;
        cJSON *tasks = cJSON_GetObjectItem(bg, "tasks");
        cJSON *t;
        cJSON_ArrayForEach(t, tasks) {
            size_t n = out->background.task_count;
            if (n >= JPP_MANIFEST_V2_BG_TASK_MAX) { break; }
            cJSON *tname = cJSON_GetObjectItem(t, "name");
            cJSON *tivl  = cJSON_GetObjectItem(t, "interval_s");
            cJSON *tcron = cJSON_GetObjectItem(t, "cron");
            if (!cJSON_IsString(tname) || tname->valuestring == NULL) { continue; }
            strncpy(strs->bg_task_names[n], tname->valuestring,
                    sizeof(strs->bg_task_names[n]) - 1u);
            strs->bg_task_names[n][sizeof(strs->bg_task_names[n]) - 1u] = '\0';
            strs->bg_task_crons[n][0] = '\0';
            if (cJSON_IsString(tcron) && tcron->valuestring) {
                strncpy(strs->bg_task_crons[n], tcron->valuestring,
                        sizeof(strs->bg_task_crons[n]) - 1u);
                strs->bg_task_crons[n][sizeof(strs->bg_task_crons[n]) - 1u] = '\0';
            }
            strs->bg_tasks[n].name       = strs->bg_task_names[n];
            strs->bg_tasks[n].interval_s = cJSON_IsNumber(tivl)
                                           ? (unsigned)tivl->valuedouble : 0u;
            strs->bg_tasks[n].cron       = strs->bg_task_crons[n];
            out->background.task_count = n + 1u;
        }
    }

    cJSON *tc = cJSON_GetObjectItem(root, "toolchain");
    if (cJSON_IsObject(tc)) {
        STR_FROM_OBJ(tc, "runtime_version", strs->runtime_version);
        STR_FROM_OBJ(tc, "cross_version",   strs->cross_version);
        j = cJSON_GetObjectItem(tc, "bytecode_abi");
        out->toolchain.bytecode_abi = cJSON_IsNumber(j) ? (int)j->valuedouble : 0;
    }
    out->toolchain.runtime_version = strs->runtime_version;
    out->toolchain.cross_version   = strs->cross_version;

#undef STR_FROM
#undef STR_FROM_OBJ

    cJSON_Delete(root);

    out->app_id           = strs->app_id;
    out->name             = strs->name;
    out->version          = strs->version;
    out->entry            = strs->entry;
    out->capabilities     = (const char *const *)strs->cap_ptrs;
    out->capability_count = strs->cap_count;

    return true;
}

/* ---- SD app task --------------------------------------------------------- */

static void sd_app_task_fn(void *arg)
{
    sd_task_args_t *a = (sd_task_args_t *)arg;
    bool is_bg = (a->bg_task[0] != '\0');

    if (a->app_type == JPP_APP_TYPE_MICROPYTHON) {
        jpp_mp_runner_result_t res = is_bg
            ? jpp_mp_runner_run_task(&s_sd_ctx, &s_sd_vm, a->entry_path,
                                     a->bg_task)
            : jpp_mp_runner_run(&s_sd_ctx, &s_sd_vm, a->entry_path);
        if (res != JPP_MP_RUNNER_OK) {
            if (is_bg) {
                ESP_LOGE(TAG, "BG_TASK_ERROR %s/%s: %s", s_sd_app_id,
                         a->bg_task, jpp_mp_runner_result_name(res));
            } else {
                record_app_crash(jpp_mp_runner_result_name(res));
            }
        }
    } else {
        /* Native binary from SD card: load the ELF, run it, then free. */
        jpp_native_loaded_app_t *loaded = NULL;
        jpp_native_loader_result_t lr =
            jpp_native_loader_load(a->entry_path, &loaded);
        if (lr == JPP_NATIVE_LOADER_OK) {
            if (is_bg) {
                ESP_LOGI(TAG, "BG_TASK_RUN %s/%s", s_sd_app_id, a->bg_task);
                if (!jpp_native_loader_run_task(loaded, &s_sd_ctx, a->bg_task)) {
                    ESP_LOGE(TAG, "BG_TASK_ERROR %s/%s: NO_TASK_ENTRY",
                             s_sd_app_id, a->bg_task);
                }
            } else {
                ESP_LOGI(TAG, "NATIVE_RUN %s", s_sd_app_id);
                jpp_native_loader_run(loaded, &s_sd_ctx);
            }
            jpp_native_loader_free(loaded);
        } else {
            if (is_bg) {
                ESP_LOGE(TAG, "BG_TASK_ERROR %s/%s: %s", s_sd_app_id,
                         a->bg_task, jpp_native_loader_result_name(lr));
            } else {
                record_app_crash(jpp_native_loader_result_name(lr));
            }
            s_sd_ctx.close_requested = true;
        }
    }

    /* Sync this app's background schedule: entries exist while the
       background.register grant is persisted and the manifest has
       background.enabled; otherwise the app's entries are removed. */
    if (!is_bg) {
        bool scheduled = s_sd_manifest.background.enabled != 0 &&
                         grant_is_persisted(s_sd_app_id, "background.register");
        jpp_bg_scheduler_sync_app(
            s_sd_app_id,
            scheduled ? s_sd_manifest.background.tasks : NULL,
            scheduled ? s_sd_manifest.background.task_count : 0u);
    }

    /* Signal the main loop that this app has finished. */
    if (!s_sd_ctx.close_requested) {
        s_sd_ctx.close_requested = true;
    }
    if (is_bg) {
        s_bg_run_done = true;
    }

    /* Suspend until the main loop deletes us (avoid double vTaskDelete). */
    vTaskSuspend(NULL);
}

/* ---- Capability consent -------------------------------------------------- */

/*
 * Tier 1 capabilities prompt once and persist the grant; every other declared
 * capability is tier 2 (prompted on first use, each session).  Capabilities
 * outside the manifest whitelist never reach here — preflight rejects them.
 */
static int cap_tier(const char *cap)
{
    if (strcmp(cap, "http.request") == 0 || strcmp(cap, "https.request") == 0 ||
        strcmp(cap, "ble.scan") == 0 ||
        strcmp(cap, "ble.advertise") == 0 ||
        strcmp(cap, "background.register") == 0 ||
        strcmp(cap, "esp_now") == 0) {
        return 1;
    }
    return 2;
}

static const char *cap_description(const char *cap)
{
    if (strcmp(cap, "http.request") == 0)  return "make requests over the network";
    /* Deliberately vague about *which* site: the per-origin prompt that follows
       names the actual server, and that is the prompt worth reading. */
    if (strcmp(cap, "https.request") == 0) return "make secure (HTTPS) web requests";
    if (strcmp(cap, "ble.scan") == 0)      return "scan for nearby Bluetooth devices";
    if (strcmp(cap, "ble.advertise") == 0) return "broadcast over Bluetooth";
    if (strcmp(cap, "background.register") == 0)
        return "run scheduled tasks in the background";
    if (strcmp(cap, "esp_now") == 0)       return "send and receive ESP-NOW messages";
    if (strcmp(cap, "files.full") == 0)    return "open any file on the SD card";
    if (strcmp(cap, "network.bind") == 0)  return "open network servers and sockets";
    if (strcmp(cap, "network.connect") == 0)
        return "open outbound network connections";
    if (strcmp(cap, "ble.connect") == 0)   return "connect to Bluetooth devices";
    if (strcmp(cap, "ble.host") == 0)      return "host a Bluetooth service";
    return "use an extra device feature";
}

static const char *const TIER1_CAPS[] = {
    "http.request", "https.request", "ble.scan", "ble.advertise",
    "background.register", "esp_now",
};
#define TIER1_CAP_COUNT (sizeof(TIER1_CAPS) / sizeof(TIER1_CAPS[0]))

static bool grant_is_persisted(const char *app_id, const char *cap)
{
    char path[96];
    snprintf(path, sizeof(path), "/data/grants/%s.json", app_id);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    char buf[256];
    size_t n = fread(buf, 1u, sizeof(buf) - 1u, f);
    buf[n] = '\0';
    fclose(f);
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", cap);
    return strstr(buf, needle) != NULL;
}

static void grant_persist(const char *app_id, const char *cap)
{
    bool granted[TIER1_CAP_COUNT];
    for (size_t i = 0u; i < TIER1_CAP_COUNT; i++) {
        granted[i] = grant_is_persisted(app_id, TIER1_CAPS[i]) ||
                     strcmp(cap, TIER1_CAPS[i]) == 0;
    }
    mkdir("/data/grants", 0755);
    char path[96];
    snprintf(path, sizeof(path), "/data/grants/%s.json", app_id);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGW(TAG, "GRANT_PERSIST_FAILED %s", app_id);
        return;
    }
    fputc('[', f);
    bool first = true;
    for (size_t i = 0u; i < TIER1_CAP_COUNT; i++) {
        if (granted[i]) {
            fprintf(f, "%s\"%s\"", first ? "" : ",", TIER1_CAPS[i]);
            first = false;
        }
    }
    fputc(']', f);
    fclose(f);
}

/*
 * Runs from within the app task via jpp_app_consent_prompt. Uses the SDK
 * frame/key system so it renders through the normal main-loop path and reads
 * keys from context->key_queue (populated by the keypad routing in app_main).
 * LEFT = move to Deny, RIGHT = move to Allow, OK = confirm, LONG = cancel.
 */
static bool prompt_permission(jpp_sdk_context_t *sdk_ctx, const char *cap, int tier)
{
    if (sdk_ctx == NULL) return false;

    char desc[2][JPP_SDK_FRAME_TEXT_MAX];
    size_t desc_count = jpp_sdk_wrap_text(cap_description(cap), desc, 2u);

    const char *tier_note = (tier == 1) ? "(saved if allowed)" : "(asked each time)";

    /* Render through the shared consent surface so capability prompts and
       file-access prompts look identical (signature line + Deny/Allow selector).
       Still uses jpp_sdk_set_frame + jpp_sdk_wait_key under the hood, as required
       for prompts running from the app task. */
    const char *body[4];
    size_t bn = 0u;
    body[bn++] = "This app wants to:";
    for (size_t i = 0u; i < desc_count; i++) {
        body[bn++] = desc[i];
    }
    body[bn++] = tier_note;

    bool allow = false;
    jpp_sdk_confirm(sdk_ctx, "Permission", body, bn, false, &allow);
    return allow;
}

/*
 * Partition declared caps into immediately-granted (already-persisted tier-1)
 * and pending (tier-1 not yet persisted + all tier-2).  Pending caps are
 * deferred to first use via jpp_sdk_ensure_cap / consent_prompt_cb.
 */
static void apply_consent(const char *app_id,
                          const char *const *declared, size_t declared_count,
                          const char **out_caps, size_t *out_count,
                          const char **out_pending_caps, int *out_pending_tiers,
                          size_t *out_pending_count)
{
    size_t n = 0u;
    size_t p = 0u;
    for (size_t i = 0u; i < declared_count; i++) {
        const char *cap = declared[i];
        int tier = cap_tier(cap);
        if (tier == 1 && grant_is_persisted(app_id, cap)) {
            out_caps[n++] = cap;
            ESP_LOGI(TAG, "CONSENT %s %s tier=1 -> GRANT (persisted)", app_id, cap);
        } else if (p < JPP_SDK_PENDING_CAP_MAX) {
            out_pending_caps[p]  = cap;
            out_pending_tiers[p] = tier;
            p++;
            ESP_LOGI(TAG, "CONSENT %s %s tier=%d -> PENDING", app_id, cap, tier);
        }
    }
    *out_count         = n;
    *out_pending_count = p;
}

/* ---- Per-origin consent for https.request -------------------------------- */

/*
 * Origin grants live in a sibling file to the capability grants rather than in
 * <app_id>.json, because grant_persist() rebuilds that file from the fixed
 * TIER1_CAPS array and would silently drop anything else stored there.
 * Format is one origin per line; a missing or unreadable file simply means
 * "nothing granted yet", which costs a re-prompt and never a false allow.
 */
#define ORIGIN_GRANTS_MAX_BYTES 1024u

static void origin_grants_path(const char *app_id, char *out, size_t out_len)
{
    snprintf(out, out_len, "/data/grants/%s.origins", app_id);
}

static bool origin_is_persisted(const char *app_id, const char *origin)
{
    char path[96];
    origin_grants_path(app_id, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    char line[JPP_SDK_ORIGIN_MAX + 2u];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f) != NULL) {
        size_t len = strlen(line);
        while (len > 0u && (line[len - 1u] == '\n' || line[len - 1u] == '\r')) {
            line[--len] = '\0';
        }
        /* Whole-line compare: a substring match would let "https://evil.com"
           be satisfied by a stored "https://evil.com.trusted.net". */
        found = (strcmp(line, origin) == 0);
    }
    fclose(f);
    return found;
}

static void origin_persist(const char *app_id, const char *origin)
{
    if (origin_is_persisted(app_id, origin)) {
        return;
    }
    char path[96];
    origin_grants_path(app_id, path, sizeof(path));
    struct stat st;
    /* Bounded so a misbehaving app cannot grow the file without limit; at the
       cap the user simply gets asked again for further origins. */
    if (stat(path, &st) == 0 &&
        (size_t)st.st_size + strlen(origin) + 1u > ORIGIN_GRANTS_MAX_BYTES) {
        ESP_LOGW(TAG, "ORIGIN_GRANTS_FULL %s", app_id);
        return;
    }
    mkdir("/data/grants", 0755);
    FILE *f = fopen(path, "a");
    if (f == NULL) {
        ESP_LOGW(TAG, "ORIGIN_PERSIST_FAILED %s", app_id);
        return;
    }
    fprintf(f, "%s\n", origin);
    fclose(f);
}

/*
 * Renders the origin across up to three rows — jpp_sdk_confirm does not
 * word-wrap, and a hostname is exactly the string the user must be able to read
 * in full to make this decision, so it is chunked rather than truncated.
 */
static bool prompt_origin(jpp_sdk_context_t *sdk_ctx, const char *origin)
{
    char chunk[3][24];
    const char *body[4];
    size_t bn = 0u;
    body[bn++] = "Connect securely to:";

    /* The scheme is constant for this capability and costs a row, so show the
       host[:port] only. */
    const char *shown = origin;
    if (strncmp(shown, "https://", 8) == 0) {
        shown += 8;
    }
    size_t olen = strlen(shown);
    size_t off  = 0u;
    for (size_t i = 0u; i < 3u && off < olen; i++) {
        size_t take = olen - off;
        if (take > 21u) take = 21u;
        memcpy(chunk[i], shown + off, take);
        chunk[i][take] = '\0';
        body[bn++] = chunk[i];
        off += take;
    }

    bool allow = false;
    jpp_sdk_confirm(sdk_ctx, "Web request", body, bn, false, &allow);
    return allow;
}

bool jpp_app_origin_prompt(jpp_sdk_context_t *sdk_ctx, const char *origin)
{
    if (sdk_ctx == NULL || origin == NULL || origin[0] == '\0') {
        return false;
    }
    if (origin_is_persisted(sdk_ctx->app_id, origin)) {
        return true;
    }
    if (s_bg_run_active) {
        /* Same rule as capability consent: a headless run has no way to ask, so
           only origins already approved interactively are usable. */
        ESP_LOGW(TAG, "ORIGIN_HEADLESS_DENY %s %s", sdk_ctx->app_id, origin);
        return false;
    }
    bool granted = prompt_origin(sdk_ctx, origin);
    if (granted) {
        origin_persist(sdk_ctx->app_id, origin);
    }
    ESP_LOGI(TAG, "ORIGIN_CONSENT %s %s -> %s",
             sdk_ctx->app_id, origin, granted ? "GRANT" : "DENY");
    return granted;
}

bool jpp_app_consent_prompt(jpp_sdk_context_t *sdk_ctx, const char *cap, int tier)
{
    if (sdk_ctx == NULL) return false;
    if (s_bg_run_active) {
        /* Headless runs have no consent surface: only capabilities already
           persisted at launch are available; everything else is denied. */
        ESP_LOGW(TAG, "CONSENT_HEADLESS_DENY %s %s", sdk_ctx->app_id, cap);
        return false;
    }
    bool granted = prompt_permission(sdk_ctx, cap, tier);
    if (granted && tier == 1) {
        grant_persist(sdk_ctx->app_id, cap);
    }
    ESP_LOGI(TAG, "CONSENT_LAZY %s %s tier=%d -> %s",
             sdk_ctx->app_id, cap, tier, granted ? "GRANT" : "DENY");
    return granted;
}

/* ---- App launch ---------------------------------------------------------- */

static bool launch_app_from(const char *app_root,
                             const char *app_id,
                             const jpp_boot_context_t *boot,
                             const char *bg_task)
{
    sd_manifest_strings_t *strs_p = &s_sd_manifest_strs;
    jpp_manifest_v2_t *manifest_p = &s_sd_manifest;

    /* Set before any failure path so record_app_crash() can attribute every
       pre-launch failure (not just runtime crashes) to the right app id. */
    strncpy(s_sd_app_id, app_id, sizeof(s_sd_app_id) - 1u);

    if (!load_sd_manifest(app_root, app_id, strs_p, manifest_p)) {
        ESP_LOGE(TAG, "SD_APP_LAUNCH %s: manifest load failed (root=%s)", app_id, app_root);
        record_app_crash("MANIFEST_LOAD_FAILED");
        return false;
    }

    static jpp_app_entry_record_t entry_rec;
    memset(&entry_rec, 0, sizeof(entry_rec));

    static char abs_entry[256];
    snprintf(abs_entry, sizeof(abs_entry),
             "%s/%s/%s", app_root, app_id, manifest_p->entry);
    entry_rec.path    = manifest_p->entry;
    entry_rec.present = file_exists(abs_entry);
    if (entry_rec.present) {
        struct stat st;
        stat(abs_entry, &st);
        entry_rec.size_bytes = (size_t)st.st_size;
    }
    entry_rec.runtime_version = manifest_p->toolchain.runtime_version;
    entry_rec.cross_version   = manifest_p->toolchain.cross_version;
    entry_rec.bytecode_abi    = manifest_p->toolchain.bytecode_abi;

    jpp_app_loader_check_t check =
        jpp_app_loader_preflight(manifest_p, &entry_rec);
    if (check.result != JPP_APP_LOADER_OK) {
        ESP_LOGE(TAG, "SD_APP_LAUNCH %s: preflight %s (manifest: %s)",
                 app_id,
                 jpp_app_loader_result_name(check.result),
                 jpp_manifest_result_name(check.manifest_result));
        record_app_crash(jpp_app_loader_result_name(check.result));
        return false;
    }

    static const char *granted_caps[SD_MANIFEST_CAP_MAX];
    size_t granted_count = 0u;
    static const char *pending_caps[JPP_SDK_PENDING_CAP_MAX];
    static int         pending_tiers[JPP_SDK_PENDING_CAP_MAX];
    size_t pending_count = 0u;
    apply_consent(app_id, (const char *const *)strs_p->cap_ptrs, strs_p->cap_count,
                  granted_caps, &granted_count,
                  pending_caps, pending_tiers, &pending_count);

    static jpp_broker_caller_t caller;
    caller.capabilities     = granted_caps;
    caller.capability_count = granted_count;

    if (manifest_p->app_type == JPP_APP_TYPE_MICROPYTHON) {
        snprintf(s_sd_app_root, sizeof(s_sd_app_root),
                 "%s/%s", app_root, app_id);

        static const char *const allowed[] = {
            "jppsdk", "json", "struct", "math", "array",
            "collections", "io", "micropython", "gc", "sys",
        };
        jpp_vm_context_init(&s_sd_vm);
        jpp_vm_runtime_config_t vm_cfg = {
            .owner_task_name = "sd_app",
            .memory = {
                .heap_bytes   = 32u * 1024u,
                .stack_words  = JPP_RESOURCE_VM_STACK_TARGET_WORDS,
                .queue_depth  = 8,
            },
            .import_surface = {
                .app_root             = s_sd_app_root,
                .runtime_root         = "/lib",
                .sd_lib_root          = "/sd/lib",
                .allowed_modules      = allowed,
                .allowed_module_count =
                    sizeof(allowed) / sizeof(allowed[0]),
            },
        };
        jpp_vm_status_t vst = jpp_vm_runtime_prepare(&s_sd_vm, &vm_cfg);
        if (vst != JPP_VM_STATUS_OK) {
            ESP_LOGE(TAG, "SD_APP_LAUNCH %s: vm prepare: %s",
                     app_id, jpp_vm_status_name(vst));
            record_app_crash(jpp_vm_status_name(vst));
            return false;
        }
        vst = jpp_vm_runtime_start(&s_sd_vm, boot);
        if (vst != JPP_VM_STATUS_OK) {
            ESP_LOGE(TAG, "SD_APP_LAUNCH %s: vm start: %s",
                     app_id, jpp_vm_status_name(vst));
            record_app_crash(jpp_vm_status_name(vst));
            return false;
        }

        jpp_sdk_bind(&s_sd_ctx, &s_sd_vm, "sd_app", app_id,
                     &caller, &s_native_services);
        jpp_sdk_set_services_v2(&s_sd_ctx, &s_native_services_v2);
        jpp_sdk_set_pending_caps(&s_sd_ctx, pending_caps, pending_tiers, pending_count);
    } else {
        jpp_sdk_bind_native(&s_sd_ctx, app_id, &caller, &s_native_services);
        jpp_sdk_set_services_v2(&s_sd_ctx, &s_native_services_v2);
        jpp_sdk_set_pending_caps(&s_sd_ctx, pending_caps, pending_tiers, pending_count);
    }

    s_sd_task_args.app_type  = manifest_p->app_type;
    s_sd_is_mp = (manifest_p->app_type == JPP_APP_TYPE_MICROPYTHON);
    snprintf(s_sd_task_args.entry_path,
             sizeof(s_sd_task_args.entry_path),
             "%s/%s/%s", app_root, app_id, manifest_p->entry);
    s_sd_task_args.bg_task[0] = '\0';
    if (bg_task != NULL) {
        strncpy(s_sd_task_args.bg_task, bg_task,
                sizeof(s_sd_task_args.bg_task) - 1u);
        s_sd_task_args.bg_task[sizeof(s_sd_task_args.bg_task) - 1u] = '\0';
        s_bg_run_active = true;
        s_bg_run_done   = false;
    }

    s_active_sdk_context = &s_sd_ctx;
    s_sd_task = xTaskCreateStatic(sd_app_task_fn, "sd_app",
                                  SD_APP_TASK_STACK_BYTES, &s_sd_task_args, 4u,
                                  s_sd_task_stack, &s_sd_task_tcb);
    if (s_sd_task == NULL) {
        s_active_sdk_context = NULL;
        ESP_LOGE(TAG, "SD_APP_LAUNCH %s: xTaskCreateStatic failed", app_id);
        record_app_crash("TASK_CREATE_FAILED");
        return false;
    }
    if (bg_task != NULL) {
        ESP_LOGI(TAG, "BG_TASK_LAUNCHED %s/%s", app_id, bg_task);
    } else {
        ESP_LOGI(TAG, "SD_APP_LAUNCHED %s root=%s (%s)", app_id, app_root,
                 manifest_p->app_type == JPP_APP_TYPE_MICROPYTHON
                     ? "micropython" : "native");
    }
    return true;
}

bool launch_sd_app(const char *app_id, const jpp_boot_context_t *boot)
{
    return launch_app_from(SD_APPS_PATH, app_id, boot, NULL);
}

void teardown_sd_app(jpp_ui_shell_t *shell)
{
    if (s_sd_task != NULL) {
        vTaskDelete(s_sd_task);
        s_sd_task = NULL;
    }
    jpp_sdk_net_close_all(&s_sd_ctx);
    s_active_sdk_context = NULL;
    s_sd_app_id[0] = '\0';
    jpp_ui_stack_pop(&shell->stack);
    ESP_LOGI(TAG, "SD_APP_CLOSED");
}

/* ---- Headless background-task runs ---------------------------------------- */

bool jpp_app_bg_launch(const char *app_id, const char *task_name,
                       const jpp_boot_context_t *boot)
{
    if (s_sd_task != NULL || s_bg_run_active) {
        return false;
    }
    if (!launch_app_from(SD_APPS_PATH, app_id, boot, task_name)) {
        s_bg_run_active = false;
        return false;
    }
    return true;
}

bool jpp_app_bg_running(void)
{
    return s_bg_run_active;
}

bool jpp_app_bg_finished(void)
{
    return s_bg_run_done;
}

void jpp_app_bg_teardown(void)
{
    if (s_sd_task != NULL) {
        vTaskDelete(s_sd_task);
        s_sd_task = NULL;
    }
    jpp_sdk_net_close_all(&s_sd_ctx);
    s_active_sdk_context = NULL;
    s_sd_app_id[0] = '\0';
    s_bg_run_active = false;
    s_bg_run_done   = false;
    ESP_LOGI(TAG, "BG_TASK_CLOSED");
}

/* ---- Wokwi embedded app launcher ---------------------------------------- */

#ifdef JPP_WOKWI_EMBED_APP
bool launch_wokwi_embed_app(const char *app_id, const jpp_boot_context_t *boot)
{
    return launch_app_from("/data/apps", app_id, boot, NULL);
}
#endif
