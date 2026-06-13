#pragma once

/*
 * games_ble — BLE device-to-device session for multiplayer games.
 *
 * Discovery is symmetric and automatic: both devices alternate a
 * connectable-advertise phase (waiting for a peer HELLO write) and a scan
 * phase (looking for another advertiser) with random jitter, so two devices
 * that both pick "Multiplayer" settle into host/client roles without the
 * users choosing. The advertiser that receives a HELLO becomes the HOST
 * (game authority); the scanner that connected becomes the CLIENT.
 *
 * Transport: the generic SDK app-host GATT service. The client writes binary
 * packets (hex-encoded) to the host RX characteristic and polls the host TX
 * characteristic; the host publishes its state with host_set_value and drains
 * client packets with host_wait_write. Packet formats, sequence numbers and
 * validation (anti-cheat) are owned by the individual games.
 */

#include "games_api.h"

games_mp_role_t games_ble_discover(jpp_sdk_context_t *ctx, uint8_t game_id);
bool games_ble_host_recv(jpp_sdk_context_t *ctx, uint8_t *buf, size_t buf_len,
                         size_t *out_len, uint32_t timeout_ms);
bool games_ble_host_send(jpp_sdk_context_t *ctx, const uint8_t *data, size_t len);
bool games_ble_client_send(jpp_sdk_context_t *ctx, const uint8_t *data, size_t len);
bool games_ble_client_recv(jpp_sdk_context_t *ctx, uint8_t *buf, size_t buf_len,
                           size_t *out_len);
void games_ble_end(jpp_sdk_context_t *ctx);
