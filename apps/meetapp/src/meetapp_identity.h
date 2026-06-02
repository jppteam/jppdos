#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "jpp_sdk_bridge.h"
#include "jpp_crypto_core.h"

#define MEETAPP_NICKNAME_MAX 16u

typedef struct {
    char    nickname[MEETAPP_NICKNAME_MAX + 1u];
    uint8_t pubkey[JPP_CRYPTO_PUBKEY_BYTES];
    uint8_t seckey[JPP_CRYPTO_SECKEY_BYTES];
    bool    loaded;
} meetapp_identity_t;

/*
 * Load identity from /sd/apps/meetapp/identity.bin, or create and persist a
 * new one if the file does not exist.  Returns true on success.
 *
 * Identity file layout (binary, 113 bytes):
 *   [0]       nickname length (1 byte)
 *   [1..16]   nickname (up to 16 bytes, no NUL)
 *   [17..48]  pubkey (32 bytes)
 *   [49..112] seckey (64 bytes)
 */
bool meetapp_identity_load_or_create(jpp_sdk_context_t *ctx,
                                     meetapp_identity_t *identity);

/*
 * Display a prompt to collect a new nickname from the user and persist the
 * identity.  Returns true if the user confirmed a valid nickname.
 */
bool meetapp_identity_prompt_nickname(jpp_sdk_context_t *ctx,
                                      meetapp_identity_t *identity);

/*
 * Reset the identity: prompt for a new nickname, generate a fresh keypair, and
 * persist it (overwriting any existing identity). On success *identity holds the
 * new identity. Returns false if the user cancelled or the write failed, in
 * which case *identity is left unchanged.
 */
bool meetapp_identity_reset(jpp_sdk_context_t *ctx,
                            meetapp_identity_t *identity);
