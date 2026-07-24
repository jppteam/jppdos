/*
 * mtproto.h — minimal MTProto client SKELETON for the J++Device.
 *
 * PURPOSE: this app is an envelope-measurement skeleton, not a finished
 * client.  It wires up the real structural code — abridged TCP transport, the
 * PQ/Diffie-Hellman authorization-key handshake, and AES-256-IGE message
 * encryption — against the SDK v2 primitives (net_connect + jpp_crypto_*), so
 * that `mtproto.bin`'s ELF segment sizes reflect what a real minimal
 * send/receive client would cost in the 64 KB app pool.  Handshake response
 * parsing is intentionally reduced to the fields the flow needs; a production
 * client would validate more (nonce echoes, DH params, salts, seq_no).
 */
#pragma once

#include "jpp_sdk_bridge.h"

/* Entry point called by the native loader. */
void mtproto_run(jpp_sdk_context_t *ctx);
