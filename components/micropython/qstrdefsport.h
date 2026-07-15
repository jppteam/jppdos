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
Q(KEY_CENTER)
Q(KEY_CENTER_LONG)

/* SDK functions */
Q(set_frame)
Q(request_close)
Q(log)
Q(device_status)
Q(get_time)
Q(is_dummy_mode)
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

/* High-level UI helpers and keyword/arg names */
Q(dialog)
Q(list)
Q(input)
Q(items)
Q(title)
Q(multiselect)
Q(placeholder)
Q(type)
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
Q(background_register)
Q(http_request)
Q(net_bind)
Q(net_accept)
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
