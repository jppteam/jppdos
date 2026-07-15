#include "meetapp_identity.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "jpp_broker_core.h"

static const char *TAG = "meetapp_id";

#define IDENTITY_PATH "identity.bin"
#define IDENTITY_SIZE (1u + MEETAPP_NICKNAME_MAX + JPP_CRYPTO_PUBKEY_BYTES + JPP_CRYPTO_SECKEY_BYTES)

/* ---- Persistence --------------------------------------------------------- */

static bool save_identity(jpp_sdk_context_t *ctx, const meetapp_identity_t *id)
{
    uint8_t buf[IDENTITY_SIZE];
    memset(buf, 0, sizeof(buf));

    uint8_t nk_len = (uint8_t)strnlen(id->nickname, MEETAPP_NICKNAME_MAX);
    buf[0] = nk_len;
    memcpy(&buf[1], id->nickname, nk_len);
    memcpy(&buf[1 + MEETAPP_NICKNAME_MAX], id->pubkey, JPP_CRYPTO_PUBKEY_BYTES);
    memcpy(&buf[1 + MEETAPP_NICKNAME_MAX + JPP_CRYPTO_PUBKEY_BYTES],
           id->seckey, JPP_CRYPTO_SECKEY_BYTES);

    /* Hex-encode for jpp_sdk_file_write (text-only API). */
    static char hex[IDENTITY_SIZE * 2u + 1u];
    for (size_t i = 0u; i < IDENTITY_SIZE; i++) {
        snprintf(&hex[i * 2u], 3u, "%02x", buf[i]);
    }
    hex[IDENTITY_SIZE * 2u] = '\0';

    jpp_broker_result_t result;
    jpp_sdk_status_t st = jpp_sdk_file_write(ctx, IDENTITY_PATH, hex, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        ESP_LOGW(TAG, "Failed to save identity: st=%d code=%s",
                 st, result.ok ? "ok" : result.code);
        return false;
    }
    return true;
}

static bool load_identity(jpp_sdk_context_t *ctx, meetapp_identity_t *id)
{
    jpp_broker_result_t result;
    jpp_sdk_status_t st = jpp_sdk_file_read(ctx, IDENTITY_PATH, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        return false;
    }
    const char *hex = jpp_broker_result_get(&result, "text");
    if (hex == NULL) return false;

    size_t hex_len = strlen(hex);
    if (hex_len < IDENTITY_SIZE * 2u) {
        ESP_LOGW(TAG, "Identity file too short: %zu hex chars", hex_len);
        return false;
    }

    uint8_t buf[IDENTITY_SIZE];
    for (size_t i = 0u; i < IDENTITY_SIZE; i++) {
        unsigned int byte;
        if (sscanf(&hex[i * 2u], "%02x", &byte) != 1) return false;
        buf[i] = (uint8_t)byte;
    }

    uint8_t nk_len = buf[0];
    if (nk_len > MEETAPP_NICKNAME_MAX) nk_len = MEETAPP_NICKNAME_MAX;
    memcpy(id->nickname, &buf[1], nk_len);
    id->nickname[nk_len] = '\0';
    memcpy(id->pubkey, &buf[1 + MEETAPP_NICKNAME_MAX], JPP_CRYPTO_PUBKEY_BYTES);
    memcpy(id->seckey, &buf[1 + MEETAPP_NICKNAME_MAX + JPP_CRYPTO_PUBKEY_BYTES],
           JPP_CRYPTO_SECKEY_BYTES);
    id->loaded = true;
    return true;
}

/* ---- Nickname prompt ------------------------------------------------------ */

bool meetapp_identity_prompt_nickname(jpp_sdk_context_t *ctx,
                                      meetapp_identity_t *identity)
{
    /* Collect the nickname through the App SDK Input prompt (QWERTY keyboard).
       Re-ask until a non-empty value is entered; BACK cancels. */
    char buf[MEETAPP_NICKNAME_MAX + 1u];
    while (true) {
        jpp_sdk_ui_result_t res = JPP_SDK_UI_BACK;
        buf[0] = '\0';
        jpp_sdk_input(ctx, "Enter nickname", NULL, JPP_SDK_INPUT_TEXT,
                      buf, sizeof(buf), &res);
        if (res == JPP_SDK_UI_BACK) {
            return false;   /* user cancelled */
        }

        /* Strip leading/trailing spaces; require at least one character. */
        size_t start = 0u;
        while (buf[start] == ' ') start++;
        size_t len = strnlen(&buf[start], MEETAPP_NICKNAME_MAX);
        while (len > 0u && buf[start + len - 1u] == ' ') len--;
        if (len == 0u) {
            jpp_sdk_ui_result_t r2;
            jpp_sdk_dialog(ctx, "Nickname needed",
                           "Please enter at least one character.", &r2);
            continue;
        }
        memmove(buf, &buf[start], len);
        buf[len] = '\0';
        strncpy(identity->nickname, buf, MEETAPP_NICKNAME_MAX);
        identity->nickname[MEETAPP_NICKNAME_MAX] = '\0';
        return true;
    }
}

/* ---- Reset --------------------------------------------------------------- */

bool meetapp_identity_reset(jpp_sdk_context_t *ctx,
                            meetapp_identity_t *identity)
{
    meetapp_identity_t fresh;
    memset(&fresh, 0, sizeof(fresh));

    /* Prompt for the new nickname first; cancelling here leaves the existing
       identity untouched. */
    if (!meetapp_identity_prompt_nickname(ctx, &fresh)) {
        return false;
    }

    if (jpp_crypto_keygen(fresh.pubkey, fresh.seckey) != JPP_CRYPTO_OK) {
        ESP_LOGE(TAG, "reset keygen failed");
        return false;
    }
    fresh.loaded = true;

    if (!save_identity(ctx, &fresh)) {
        return false;
    }

    *identity = fresh;
    ESP_LOGI(TAG, "Identity reset: nick=%s", identity->nickname);
    return true;
}

/* ---- Device username default ---------------------------------------------- */

/*
 * On first run, default the nickname to the device's username (Settings >
 * User's name) rather than prompting — "default", not "pre-fill": the SDK's
 * input field only shows a placeholder as ghost text (pressing OK on an
 * empty field submits "", not the ghost text), so there is no clean way to
 * offer an editable default without changing the shared jpp_sdk_input() API
 * for every other caller. "Reset identity" still always prompts, so users
 * who want a different nickname than their device username can set one.
 */
static bool try_default_nickname_from_device(jpp_sdk_context_t *ctx,
                                              meetapp_identity_t *identity)
{
    jpp_broker_result_t result;
    jpp_sdk_status_t st = jpp_sdk_device_status(ctx, &result);
    if (st != JPP_SDK_STATUS_OK || !result.ok) {
        return false;
    }
    const char *username = jpp_broker_result_get(&result, "username");
    if (username == NULL || username[0] == '\0') {
        return false;
    }
    strncpy(identity->nickname, username, MEETAPP_NICKNAME_MAX);
    identity->nickname[MEETAPP_NICKNAME_MAX] = '\0';
    return true;
}

/* ---- Public API ---------------------------------------------------------- */

bool meetapp_identity_load_or_create(jpp_sdk_context_t *ctx,
                                     meetapp_identity_t *identity)
{
    memset(identity, 0, sizeof(*identity));

    /* Try to load from SD first. */
    if (load_identity(ctx, identity)) {
        ESP_LOGI(TAG, "Identity loaded: nick=%s", identity->nickname);
        return true;
    }

    /* First run: default to the device username; fall back to prompting. */
    if (!try_default_nickname_from_device(ctx, identity) &&
        !meetapp_identity_prompt_nickname(ctx, identity)) {
        return false;
    }

    /* Generate Ed25519 keypair. */
    if (jpp_crypto_keygen(identity->pubkey, identity->seckey) != JPP_CRYPTO_OK) {
        ESP_LOGE(TAG, "keygen failed");
        return false;
    }
    identity->loaded = true;

    if (!save_identity(ctx, identity)) {
        return false;
    }

    ESP_LOGI(TAG, "New identity created: nick=%s", identity->nickname);
    return true;
}
