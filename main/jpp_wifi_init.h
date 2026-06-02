#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialise NVS, TCP/IP stack, and esp_wifi in station mode.
 * Reads SSID from settings.json and starts a non-blocking connection.
 * Returns true if Wi-Fi was started (SSID configured); false if skipped.
 */
bool init_wifi(void);

/*
 * Ensure the Wi-Fi driver is started in STA mode (at minimum for scanning)
 * even when no SSID is configured.  Safe to call multiple times; idempotent.
 * Returns true if Wi-Fi is now ready for scanning/connecting.
 */
bool wifi_ensure_started(void);

/*
 * Connect to a specific AP.  Blocks until connected or timeout_ms elapses.
 * Sets out_err_msg (NUL-terminated, max err_msg_len bytes) on failure.
 * Returns true on successful IP acquisition.
 */
bool wifi_connect(const char *ssid, const char *password,
                  uint32_t timeout_ms,
                  char *out_err_msg, size_t err_msg_len);

/* Disconnect from the current AP and suppress auto-reconnect. */
void wifi_disconnect(void);

/* Returns true if the station currently has a valid IP address. */
bool wifi_is_connected(void);

/* Copies the SSID of the currently associated AP into out (NUL-terminated).
   Returns false and clears out if not connected. */
bool wifi_get_connected_ssid(char *out, size_t out_len);

/* Copies the SSID saved in settings.json into out.  Returns false if none is
   configured.  Does not require the Wi-Fi driver to be running. */
bool wifi_get_saved_ssid(char *out, size_t out_len);

/* Returns true while the driver is trying to obtain an IP (autoconnect or
   reconnect in progress but no IP yet). */
bool wifi_is_connecting(void);

#ifdef __cplusplus
}
#endif
