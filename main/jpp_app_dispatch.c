#include "jpp_app_dispatch.h"
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

static const char *TAG = "app_dispatch";

#define SD_APPS_PATH       "/sd/apps"
/* Must be >= the largest capability list any manifest can declare, and
   <= JPP_SDK_PENDING_CAP_MAX. The full SDK surface is 11 capabilities; a smaller
   value silently truncated the manifest list, dropping the trailing caps (e.g.
   ble.connect / ble.host) so they could never be granted. */
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

/* entry_fn: non-NULL only for the legacy registry bridge path. */
typedef void (*jpp_dispatch_entry_fn_t)(jpp_sdk_context_t *);

typedef struct {
    jpp_manifest_app_type_t  app_type;
    char                     entry_path[256];
    jpp_dispatch_entry_fn_t  entry_fn;
} sd_task_args_t;
static sd_task_args_t s_sd_task_args;

/* ---- App discovery ------------------------------------------------------- */

void discover_apps(bool normal_mode,
                   jpp_boot_discovery_summary_t *summary,
                   jpp_ui_shell_t *shell)
{
    memset(summary, 0, sizeof(*summary));

    summary->builtin_count = 2u;
    jpp_ui_shell_add_app(shell, "settings", "Settings",
                         JPP_UI_APP_SOURCE_BUILTIN, true);
    jpp_ui_shell_add_app(shell, "webdav", "WebDAV server",
                         JPP_UI_APP_SOURCE_BUILTIN, true);

    /* Native apps come from the SD card and are discovered by the SD scan below;
       the registry bridge has been retired now that the dynamic loader exists. */

#ifdef JPP_WOKWI_EMBED_APP
    {
        summary->sd_count++;
        jpp_ui_shell_add_app(shell, JPP_WOKWI_EMBED_APP_ID,
                             JPP_WOKWI_EMBED_APP_ID,
                             JPP_UI_APP_SOURCE_SD, true);
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
        char manifest_path[300];
        snprintf(manifest_path, sizeof(manifest_path),
                 "%s/%s/manifest.json", SD_APPS_PATH, ent->d_name);
        ESP_LOGW(TAG, "Loading app manifest from %s", manifest_path);
        if (!file_exists(manifest_path)) {
            ESP_LOGW(TAG, "APP_REJECTED %s: manifest.json missing", ent->d_name);
            summary->rejected_count++;
            continue;
        }

        ESP_LOGI(TAG, "SD app discovered: %s", ent->d_name);
        summary->sd_count++;
        jpp_ui_shell_add_app(shell, ent->d_name, ent->d_name,
                             JPP_UI_APP_SOURCE_SD, true);
    }
    closedir(dir);

    summary->total_count =
        summary->builtin_count +
        summary->sd_count +
        summary->disabled_count +
        summary->rejected_count;
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
    jpp_ui_shell_clear_sd_apps(shell);
    for (size_t i = 0u; i < s_bg_disc_count; i++) {
        jpp_ui_shell_add_app(shell, s_bg_disc_apps[i].id,
                             s_bg_disc_apps[i].name,
                             JPP_UI_APP_SOURCE_SD, true);
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
    char background_mode[16];
    char cap_strs[SD_MANIFEST_CAP_MAX][32];
    const char *cap_ptrs[SD_MANIFEST_CAP_MAX];
    size_t cap_count;
} sd_manifest_strings_t;

static bool load_sd_manifest(const char *app_root,
                              const char *app_id,
                              sd_manifest_strings_t *strs,
                              jpp_manifest_v2_t *out)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/%s/manifest.json", app_root, app_id);

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)size + 1u);
    if (buf == NULL) {
        fclose(f);
        return false;
    }
    fread(buf, 1u, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);

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
    j = cJSON_GetObjectItem(root, "sdk_max");
    out->sdk_max = cJSON_IsNumber(j) ? (int)j->valuedouble : 1;

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
    strncpy(strs->background_mode, "serialized", sizeof(strs->background_mode) - 1u);
    if (cJSON_IsObject(bg)) {
        j = cJSON_GetObjectItem(bg, "enabled");
        out->background.enabled = cJSON_IsTrue(j) ? 1 : 0;
        j = cJSON_GetObjectItem(bg, "mode");
        if (cJSON_IsString(j) && j->valuestring) {
            strncpy(strs->background_mode, j->valuestring,
                    sizeof(strs->background_mode) - 1u);
        }
    }
    out->background.mode = strs->background_mode;

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

    if (a->app_type == JPP_APP_TYPE_MICROPYTHON) {
        jpp_mp_runner_result_t res =
            jpp_mp_runner_run(&s_sd_ctx, &s_sd_vm, a->entry_path);
        if (res != JPP_MP_RUNNER_OK) {
            ESP_LOGE(TAG, "SD_APP_MP_RUNNER %s: %s",
                     s_sd_app_id, jpp_mp_runner_result_name(res));
        }
    } else if (a->entry_fn != NULL) {
        /* Registry bridge: compiled-in entry (kept for special cases; ordinary
           apps should go through the SD card loader path below). */
        a->entry_fn(&s_sd_ctx);
    } else {
        /* Native binary from SD card: load the ELF, run it, then free. */
        jpp_native_loaded_app_t *loaded = NULL;
        jpp_native_loader_result_t lr =
            jpp_native_loader_load(a->entry_path, &loaded);
        if (lr == JPP_NATIVE_LOADER_OK) {
            ESP_LOGI(TAG, "NATIVE_RUN %s", s_sd_app_id);
            jpp_native_loader_run(loaded, &s_sd_ctx);
            jpp_native_loader_free(loaded);
        } else {
            ESP_LOGE(TAG, "NATIVE_LOAD_FAILED %s: %s",
                     s_sd_app_id, jpp_native_loader_result_name(lr));
            /* Surface a brief error message via the SDK dialog so the user
               sees why the app didn't start, then close. */
            {
                const char *reason = jpp_native_loader_result_name(lr);
                char msg[64];
                snprintf(msg, sizeof(msg), "Load failed: %s", reason);
                jpp_sdk_ui_result_t r;
                jpp_sdk_dialog(&s_sd_ctx, "App error", msg, &r);
            }
            s_sd_ctx.close_requested = true;
        }
    }

    /* Signal the main loop that this app has finished. */
    if (!s_sd_ctx.close_requested) {
        s_sd_ctx.close_requested = true;
    }

    /* Suspend until the main loop deletes us (avoid double vTaskDelete). */
    vTaskSuspend(NULL);
}

/* ---- Capability consent -------------------------------------------------- */

static int cap_tier(const char *cap)
{
    if (strcmp(cap, "files.scoped") == 0 || strcmp(cap, "files.shared") == 0 ||
        strcmp(cap, "device.status") == 0) {
        return 0;
    }
    if (strcmp(cap, "ipc.send") == 0 || strcmp(cap, "http.request") == 0 ||
        strcmp(cap, "device.kv") == 0 || strcmp(cap, "ble.scan") == 0 ||
        strcmp(cap, "ble.advertise") == 0) {
        return 1;
    }
    return 2;
}

static const char *cap_description(const char *cap)
{
    if (strcmp(cap, "ipc.send") == 0)      return "exchange messages with other apps";
    if (strcmp(cap, "http.request") == 0)  return "make requests over the network";
    if (strcmp(cap, "device.kv") == 0)     return "store its own saved data";
    if (strcmp(cap, "ble.scan") == 0)      return "scan for nearby Bluetooth devices";
    if (strcmp(cap, "ble.advertise") == 0) return "broadcast over Bluetooth";
    if (strcmp(cap, "files.full") == 0)    return "open any file on the SD card";
    if (strcmp(cap, "network.bind") == 0)  return "open network servers and sockets";
    if (strcmp(cap, "ble.connect") == 0)   return "connect to Bluetooth devices";
    if (strcmp(cap, "ble.host") == 0)      return "host a Bluetooth service";
    return "use an extra device feature";
}

static const char *const TIER1_CAPS[] = {
    "ipc.send", "http.request", "device.kv", "ble.scan", "ble.advertise",
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
 * LEFT = move to Deny, RIGHT = move to Allow, CENTER = confirm, LONG = cancel.
 */
static bool prompt_permission(jpp_sdk_context_t *sdk_ctx, const char *cap, int tier)
{
    if (sdk_ctx == NULL) return false;

    const char *full = cap_description(cap);
    char desc1[22], desc2[22];
    size_t flen = strlen(full);
    if (flen <= 21u) {
        strncpy(desc1, full, 22u);
        desc1[21] = '\0';
        desc2[0] = '\0';
    } else {
        size_t brk = 21u;
        for (size_t k = 20u; k > 0u; k--) {
            if (full[k] == ' ') { brk = k; break; }
        }
        strncpy(desc1, full, brk);
        desc1[brk] = '\0';
        const char *rest = full + brk;
        while (*rest == ' ') rest++;
        strncpy(desc2, rest, 22u);
        desc2[21] = '\0';
    }

    const char *tier_note = (tier == 1) ? "(saved if allowed)" : "(asked each time)";

    /* Render through the shared consent surface so capability prompts and
       file-access prompts look identical (signature line + Deny/Allow selector).
       Still uses jpp_sdk_set_frame + jpp_sdk_wait_key under the hood, as required
       for prompts running from the app task. */
    const char *body[4];
    size_t bn = 0u;
    body[bn++] = "This app wants to:";
    body[bn++] = desc1;
    if (desc2[0] != '\0') {
        body[bn++] = desc2;
    }
    body[bn++] = tier_note;

    bool allow = false;
    jpp_sdk_confirm(sdk_ctx, "Permission", body, bn, false, &allow);
    return allow;
}

/*
 * Partition declared caps into immediately-granted (tier-0 + already-persisted
 * tier-1) and pending (tier-1 not yet persisted + all tier-2).  Pending caps
 * are deferred to first use via jpp_sdk_ensure_cap / consent_prompt_cb.
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
        if (tier == 0) {
            out_caps[n++] = cap;
            ESP_LOGI(TAG, "CONSENT %s %s tier=0 -> GRANT (auto)", app_id, cap);
        } else if (tier == 1 && grant_is_persisted(app_id, cap)) {
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

bool jpp_app_consent_prompt(jpp_sdk_context_t *sdk_ctx, const char *cap, int tier)
{
    if (sdk_ctx == NULL) return false;
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
                             const jpp_boot_context_t *boot)
{
    static sd_manifest_strings_t strs;
    static jpp_manifest_v2_t manifest;

    if (!load_sd_manifest(app_root, app_id, &strs, &manifest)) {
        ESP_LOGE(TAG, "SD_APP_LAUNCH %s: manifest load failed (root=%s)", app_id, app_root);
        return false;
    }

    static jpp_app_entry_record_t entry_rec;
    memset(&entry_rec, 0, sizeof(entry_rec));

    static char abs_entry[256];
    snprintf(abs_entry, sizeof(abs_entry),
             "%s/%s/%s", app_root, app_id, manifest.entry);
    entry_rec.path    = manifest.entry;
    entry_rec.present = file_exists(abs_entry);
    if (entry_rec.present) {
        struct stat st;
        stat(abs_entry, &st);
        entry_rec.size_bytes = (size_t)st.st_size;
    }
    entry_rec.runtime_version = manifest.toolchain.runtime_version;
    entry_rec.cross_version   = manifest.toolchain.cross_version;
    entry_rec.bytecode_abi    = manifest.toolchain.bytecode_abi;

    jpp_app_loader_check_t check =
        jpp_app_loader_preflight(&manifest, &entry_rec);
    if (check.result != JPP_APP_LOADER_OK) {
        ESP_LOGE(TAG, "SD_APP_LAUNCH %s: preflight %s (manifest: %s)",
                 app_id,
                 jpp_app_loader_result_name(check.result),
                 jpp_manifest_result_name(check.manifest_result));
        return false;
    }

    static const char *granted_caps[SD_MANIFEST_CAP_MAX];
    size_t granted_count = 0u;
    static const char *pending_caps[JPP_SDK_PENDING_CAP_MAX];
    static int         pending_tiers[JPP_SDK_PENDING_CAP_MAX];
    size_t pending_count = 0u;
    apply_consent(app_id, (const char *const *)strs.cap_ptrs, strs.cap_count,
                  granted_caps, &granted_count,
                  pending_caps, pending_tiers, &pending_count);

    static jpp_broker_caller_t caller;
    caller.capabilities     = granted_caps;
    caller.capability_count = granted_count;

    strncpy(s_sd_app_id, app_id, sizeof(s_sd_app_id) - 1u);

    if (manifest.app_type == JPP_APP_TYPE_MICROPYTHON) {
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
            return false;
        }
        vst = jpp_vm_runtime_start(&s_sd_vm, boot);
        if (vst != JPP_VM_STATUS_OK) {
            ESP_LOGE(TAG, "SD_APP_LAUNCH %s: vm start: %s",
                     app_id, jpp_vm_status_name(vst));
            return false;
        }

        jpp_sdk_bind(&s_sd_ctx, &s_sd_vm, "sd_app", app_id,
                     &caller, &s_native_services);
        jpp_sdk_set_pending_caps(&s_sd_ctx, pending_caps, pending_tiers, pending_count);
    } else {
        jpp_sdk_bind_native(&s_sd_ctx, app_id, &caller, &s_native_services);
        jpp_sdk_set_pending_caps(&s_sd_ctx, pending_caps, pending_tiers, pending_count);
    }

    s_sd_task_args.app_type  = manifest.app_type;
    s_sd_task_args.entry_fn  = NULL;
    s_sd_is_mp = (manifest.app_type == JPP_APP_TYPE_MICROPYTHON);
    snprintf(s_sd_task_args.entry_path,
             sizeof(s_sd_task_args.entry_path),
             "%s/%s/%s", app_root, app_id, manifest.entry);

    s_active_sdk_context = &s_sd_ctx;
    s_sd_task = xTaskCreateStatic(sd_app_task_fn, "sd_app",
                                  SD_APP_TASK_STACK_BYTES, &s_sd_task_args, 4u,
                                  s_sd_task_stack, &s_sd_task_tcb);
    if (s_sd_task == NULL) {
        s_active_sdk_context = NULL;
        ESP_LOGE(TAG, "SD_APP_LAUNCH %s: xTaskCreateStatic failed", app_id);
        return false;
    }
    ESP_LOGI(TAG, "SD_APP_LAUNCHED %s root=%s (%s)", app_id, app_root,
             manifest.app_type == JPP_APP_TYPE_MICROPYTHON
                 ? "micropython" : "native");
    return true;
}

bool launch_sd_app(const char *app_id, const jpp_boot_context_t *boot)
{
    return launch_app_from(SD_APPS_PATH, app_id, boot);
}

/* Registry-based launch/lookup functions removed: the dynamic loader handles
   all native apps from the SD card. launch_sd_app() now covers both
   MicroPython and native binary apps uniformly. */

void teardown_sd_app(jpp_ui_shell_t *shell)
{
    if (s_sd_task != NULL) {
        vTaskDelete(s_sd_task);
        s_sd_task = NULL;
    }
    s_active_sdk_context = NULL;
    s_sd_app_id[0] = '\0';
    jpp_ui_stack_pop(&shell->stack);
    ESP_LOGI(TAG, "SD_APP_CLOSED");
}

/* ---- Wokwi embedded app launcher ---------------------------------------- */

#ifdef JPP_WOKWI_EMBED_APP
bool launch_wokwi_embed_app(const char *app_id, const jpp_boot_context_t *boot)
{
    return launch_app_from("/data/apps", app_id, boot);
}
#endif
