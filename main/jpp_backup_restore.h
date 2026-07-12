#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parse and apply a JPPDOS backup JSON string (settings.json + all NVS
 * namespaces).  Returns true on success; the caller is responsible for
 * restarting the device.  On failure, a human-readable error is written to
 * msg (may be NULL to discard).
 */
bool jpp_backup_apply_json(const char *json_buf, char *msg, size_t msg_len);

#ifdef __cplusplus
}
#endif
