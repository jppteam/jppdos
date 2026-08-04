/* Port-specific qstr definitions for JPPDOS.
 *
 * All Q(name) entries here become MP_QSTR_name constants, available to
 * jpp_mp_sdk_module.c and jpp_mp_runner.c which are compiled outside the
 * micropython component and therefore not scanned by makeqstrdefs.py.
 */

/* jppsdk module name */
Q(jppsdk)
Q(create_app)

/* Exception types exposed to Python apps */
Q(SdkError)
Q(SdkPermissionError)

/* Module constants */
Q(OPEN_READ)
Q(OPEN_WRITE)
Q(KEY_NONE)
Q(KEY_UP)
Q(KEY_DOWN)
Q(KEY_LEFT)
Q(KEY_RIGHT)
Q(KEY_OK)
Q(KEY_OK_LONG)

/* SDK functions */
Q(set_frame)
Q(request_close)
Q(log)
Q(device_status)
Q(get_time)
Q(is_dummy_mode)
Q(claim_ok)
Q(KEY_BACK)
Q(KEY_OK_HOLD)
Q(KEY_OK_DOUBLE)
Q(OK_CLAIM_NONE)
Q(OK_CLAIM_HOLD)
Q(OK_CLAIM_DOUBLE)

/* Deprecated pre-rename names (5th keypad button: CENTER -> OK), same
 * values/objects as their OK_* counterparts above — kept so .mpy files
 * compiled before the rename keep resolving jppsdk.claim_center /
 * jppsdk.KEY_CENTER* / jppsdk.CENTER_CLAIM_* without a rebuild. */
Q(claim_center)
Q(KEY_CENTER)
Q(KEY_CENTER_LONG)
Q(KEY_CENTER_HOLD)
Q(KEY_CENTER_DOUBLE)
Q(CENTER_CLAIM_NONE)
Q(CENTER_CLAIM_HOLD)
Q(CENTER_CLAIM_DOUBLE)
Q(file_read)
Q(file_write)
Q(file_list)
Q(shared_read)
Q(shared_write)
Q(shared_list)
Q(file_open)
Q(handle_read)
Q(handle_write)
Q(handle_list)
Q(handle_close)
Q(poll_key)
Q(wait_key)
Q(request_cap)

/* High-level UI helpers and keyword/arg names */
Q(dialog)
Q(list)
Q(input)
Q(confirm)
Q(wrap_text)
Q(file_pick)
Q(items)
Q(title)
Q(multiselect)
Q(placeholder)
Q(type)
Q(lines)
Q(default_allow)
Q(INPUT_TEXT)
Q(INPUT_NUMBER)
Q(INPUT_DATE)
Q(INPUT_TIME)

Q(ble_scan)
Q(ble_advertise_start)
Q(ble_advertise_stop)
Q(espnow_send)
Q(espnow_recv)
Q(ble_connect)
Q(ble_read_char)
Q(ble_write_char)
Q(ble_disconnect)
Q(ble_service_register)
Q(ble_service_unregister)
Q(ble_set_connectable)
Q(ble_host_set_value)
Q(ble_host_wait_write)
Q(ble_host_clear)
Q(background_register)
Q(http_request)
Q(https_request)
Q(net_bind)
Q(net_accept)
Q(net_connect)
Q(net_recv)
Q(net_send)
Q(net_close)
Q(on_task)
Q(canvas_write)
Q(canvas_clear)
Q(canvas_draw_pixel)
Q(canvas_fullscreen)
Q(ipc_send)
Q(ipc_recv)
Q(kv_get)
Q(kv_set)
Q(kv_delete)

/* BLE scan result dict keys */
Q(address)
Q(rssi)
Q(ad_data)

/* Wakelock */
Q(wakelock_acquire)
Q(wakelock_release)

/* Buzzer */
Q(buzzer_play)
Q(buzzer_tone)
Q(buzzer_play_sequence)
Q(buzzer_play_sequence_async)
Q(buzzer_stop)
Q(SOUND_SUCCESS)
Q(SOUND_FAILURE)
Q(SOUND_NOTIFY)
Q(SOUND_STARTUP)
Q(SOUND_CLICK)

/* LED */
Q(led_set_color)
Q(led_off)

/* Crypto primitives */
Q(crypto_sha256)
Q(crypto_sha1)
Q(crypto_aes256_ige_encrypt)
Q(crypto_aes256_ige_decrypt)
Q(crypto_modexp)
Q(crypto_rsa_encrypt)
Q(crypto_dh_compute)
