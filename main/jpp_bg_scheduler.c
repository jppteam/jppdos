#include "jpp_bg_scheduler.h"
#include "jpp_file_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "cJSON.h"

#include "jpp_resource_budget.h"
#include "jpp_string_util.h"

static const char *TAG = "bg_sched";

#define BG_SCHEDULE_PATH "/data/bg_schedule.json"

typedef struct {
    char     app_id[32];
    char     task[32];
    uint32_t interval_s;   /* 0 = cron */
    char     cron[24];
    uint32_t last_run;     /* epoch seconds, 0 = never */
} bg_entry_t;

static bg_entry_t s_entries[JPP_RESOURCE_BG_SCHEDULE_MAX];
static size_t     s_entry_count = 0u;

/* ---- Time helpers --------------------------------------------------------- */

/* Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). */
static int32_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (uint32_t)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

static uint32_t datetime_to_epoch(const jpp_rtc_datetime_t *dt)
{
    int32_t days = days_from_civil(dt->year, dt->month, dt->day);
    return (uint32_t)days * 86400u +
           (uint32_t)dt->hour * 3600u +
           (uint32_t)dt->minute * 60u +
           (uint32_t)dt->second;
}

/* ---- Cron matching -------------------------------------------------------- */

/* One field: "*" matches anything; otherwise compare the number. Advances
   *cursor past the field and its trailing spaces. */
static bool cron_field_matches(const char **cursor, int value)
{
    const char *p = *cursor;
    bool match;
    if (*p == '*') {
        match = true;
        p++;
    } else {
        int v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            p++;
        }
        match = (v == value);
    }
    while (*p == ' ') {
        p++;
    }
    *cursor = p;
    return match;
}

/* True when the (validated) cron expression matches the given datetime. */
static bool cron_matches(const char *cron, const jpp_rtc_datetime_t *dt)
{
    /* 1970-01-01 was a Thursday; cron day-of-week 0 = Sunday. */
    int dow = (int)(((uint32_t)days_from_civil(dt->year, dt->month, dt->day) + 4u) % 7u);

    const char *p = cron;
    bool ok = true;
    ok &= cron_field_matches(&p, dt->minute);
    ok &= cron_field_matches(&p, dt->hour);
    ok &= cron_field_matches(&p, dt->day);
    ok &= cron_field_matches(&p, dt->month);
    ok &= cron_field_matches(&p, dow);
    return ok;
}

/* ---- Persistence ----------------------------------------------------------- */

static void save_schedule(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) { return; }
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON *arr = cJSON_AddArrayToObject(root, "entries");
    for (size_t i = 0u; i < s_entry_count; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "app",  s_entries[i].app_id);
        cJSON_AddStringToObject(e, "task", s_entries[i].task);
        cJSON_AddNumberToObject(e, "interval_s", (double)s_entries[i].interval_s);
        cJSON_AddStringToObject(e, "cron", s_entries[i].cron);
        cJSON_AddNumberToObject(e, "last_run", (double)s_entries[i].last_run);
        cJSON_AddItemToArray(arr, e);
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) { return; }

    FILE *f = fopen(BG_SCHEDULE_PATH, "w");
    if (f != NULL) {
        fputs(out, f);
        fclose(f);
    }
    free(out);
}

void jpp_bg_scheduler_init(void)
{
    s_entry_count = 0u;

    char *buf = jpp_read_file(BG_SCHEDULE_PATH, NULL);
    if (buf == NULL) { return; }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) { return; }

    cJSON *arr = cJSON_GetObjectItem(root, "entries");
    cJSON *e;
    cJSON_ArrayForEach(e, arr) {
        if (s_entry_count >= JPP_RESOURCE_BG_SCHEDULE_MAX) { break; }
        cJSON *app  = cJSON_GetObjectItem(e, "app");
        cJSON *task = cJSON_GetObjectItem(e, "task");
        cJSON *ivl  = cJSON_GetObjectItem(e, "interval_s");
        cJSON *cron = cJSON_GetObjectItem(e, "cron");
        cJSON *last = cJSON_GetObjectItem(e, "last_run");
        if (!cJSON_IsString(app) || !cJSON_IsString(task)) { continue; }

        bg_entry_t *ent = &s_entries[s_entry_count];
        memset(ent, 0, sizeof(*ent));
        jpp_str_copy(ent->app_id, sizeof(ent->app_id), app->valuestring);
        jpp_str_copy(ent->task, sizeof(ent->task), task->valuestring);
        ent->interval_s = cJSON_IsNumber(ivl) ? (uint32_t)ivl->valuedouble : 0u;
        if (cJSON_IsString(cron) && cron->valuestring) {
            jpp_str_copy(ent->cron, sizeof(ent->cron), cron->valuestring);
        }
        ent->last_run = cJSON_IsNumber(last) ? (uint32_t)last->valuedouble : 0u;
        s_entry_count++;
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "BG_SCHEDULE loaded: %u entr%s",
             (unsigned)s_entry_count, s_entry_count == 1u ? "y" : "ies");
}

void jpp_bg_scheduler_sync_app(const char *app_id,
                               const jpp_manifest_bg_task_t *tasks,
                               size_t task_count)
{
    if (app_id == NULL) { return; }

    /* Snapshot last_run values, then drop every entry for this app. */
    uint32_t prev_last[JPP_RESOURCE_BG_SCHEDULE_MAX];
    char     prev_task[JPP_RESOURCE_BG_SCHEDULE_MAX][32];
    size_t   prev_count = 0u;

    size_t w = 0u;
    for (size_t i = 0u; i < s_entry_count; i++) {
        if (jpp_str_eq(s_entries[i].app_id, app_id)) {
            prev_last[prev_count] = s_entries[i].last_run;
            jpp_str_copy(prev_task[prev_count], sizeof(prev_task[0]),
                         s_entries[i].task);
            prev_count++;
            continue;
        }
        if (w != i) { s_entries[w] = s_entries[i]; }
        w++;
    }
    s_entry_count = w;

    for (size_t i = 0u; i < task_count; i++) {
        if (s_entry_count >= JPP_RESOURCE_BG_SCHEDULE_MAX) {
            ESP_LOGW(TAG, "BG_SCHEDULE full: %s/%s dropped",
                     app_id, tasks[i].name);
            break;
        }
        bg_entry_t *ent = &s_entries[s_entry_count];
        memset(ent, 0, sizeof(*ent));
        jpp_str_copy(ent->app_id, sizeof(ent->app_id), app_id);
        jpp_str_copy(ent->task, sizeof(ent->task), tasks[i].name);
        ent->interval_s = tasks[i].interval_s;
        if (jpp_str_nonempty(tasks[i].cron)) {
            jpp_str_copy(ent->cron, sizeof(ent->cron), tasks[i].cron);
        }
        for (size_t j = 0u; j < prev_count; j++) {
            if (jpp_str_eq(prev_task[j], tasks[i].name)) {
                ent->last_run = prev_last[j];
                break;
            }
        }
        s_entry_count++;
    }

    save_schedule();
    ESP_LOGI(TAG, "BG_SCHEDULE sync %s: %u task(s)", app_id, (unsigned)task_count);
}

bool jpp_bg_scheduler_due(const jpp_rtc_state_t *rtc,
                          char *out_app, size_t app_len,
                          char *out_task, size_t task_len)
{
    if (s_entry_count == 0u || rtc == NULL) { return false; }

    jpp_rtc_datetime_t now;
    if (jpp_rtc_get_current(rtc, &now) != JPP_RTC_STATUS_OK) {
        return false;
    }
    uint32_t epoch = datetime_to_epoch(&now);

    for (size_t i = 0u; i < s_entry_count; i++) {
        bg_entry_t *ent = &s_entries[i];
        bool due;
        if (ent->interval_s > 0u) {
            due = (ent->last_run == 0u) ||
                  (epoch >= ent->last_run + ent->interval_s);
        } else {
            /* Cron: fire once within the matching minute. */
            due = cron_matches(ent->cron, &now) &&
                  (epoch / 60u) != (ent->last_run / 60u);
        }
        if (due) {
            jpp_str_copy(out_app, app_len, ent->app_id);
            jpp_str_copy(out_task, task_len, ent->task);
            return true;
        }
    }
    return false;
}

void jpp_bg_scheduler_mark_run(const jpp_rtc_state_t *rtc,
                               const char *app_id, const char *task)
{
    jpp_rtc_datetime_t now;
    uint32_t epoch = 0u;
    if (rtc != NULL && jpp_rtc_get_current(rtc, &now) == JPP_RTC_STATUS_OK) {
        epoch = datetime_to_epoch(&now);
    }
    for (size_t i = 0u; i < s_entry_count; i++) {
        if (jpp_str_eq(s_entries[i].app_id, app_id) &&
            jpp_str_eq(s_entries[i].task, task)) {
            s_entries[i].last_run = epoch;
            break;
        }
    }
    save_schedule();
}
