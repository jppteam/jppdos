#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jpp_manifest_core.h"
#include "jpp_rtc_core.h"

/*
 * Background task scheduler.
 *
 * Apps declare schedules in their manifest (background.tasks: name +
 * interval_s or cron) and gate enrollment behind the background.register
 * capability (tier 1). After an app session ends, jpp_bg_scheduler_sync_app()
 * writes the app's schedule into /data/bg_schedule.json — present when the
 * grant is persisted, removed when it is not.
 *
 * The main loop polls jpp_bg_scheduler_due() while the device is idle on the
 * launcher (no foreground app, no WebDAV/LRV server, no serial session) and
 * headless-launches the app to run the due task: MicroPython apps get
 * module-level on_task(name), native apps jpp_app_task_entry(ctx, name).
 * A run is killed after JPP_RESOURCE_BG_TASK_RUN_QUOTA_MS of wall-clock time.
 * The scheduler only ticks while the device is awake; deep sleep pauses it.
 */

/* Load /data/bg_schedule.json (missing file = empty schedule). */
void jpp_bg_scheduler_init(void);

/*
 * Replace the schedule entries of app_id with the given manifest tasks
 * (task_count == 0 removes the app's entries). last_run survives for entries
 * whose task name is unchanged. Persists the table.
 */
void jpp_bg_scheduler_sync_app(const char *app_id,
                               const jpp_manifest_bg_task_t *tasks,
                               size_t task_count);

/*
 * True when a task is due at the current RTC time; fills out_app/out_task.
 * Interval tasks are due when now >= last_run + interval; cron tasks when the
 * current minute matches and they have not already run in that minute.
 * Returns false when the RTC has no valid datetime (cron needs wall clock;
 * interval tasks still run, measured against the RTC once it is valid).
 */
bool jpp_bg_scheduler_due(const jpp_rtc_state_t *rtc,
                          char *out_app, size_t app_len,
                          char *out_task, size_t task_len);

/* Record that the task just ran (updates last_run and persists). */
void jpp_bg_scheduler_mark_run(const jpp_rtc_state_t *rtc,
                               const char *app_id, const char *task);
