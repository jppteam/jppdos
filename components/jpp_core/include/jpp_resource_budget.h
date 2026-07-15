#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define JPP_RESOURCE_VM_HEAP_TARGET_BYTES 131072u
#define JPP_RESOURCE_VM_HEAP_LIMIT_BYTES 196608u
#define JPP_RESOURCE_VM_STACK_TARGET_WORDS 8192u
#define JPP_RESOURCE_VM_STACK_LIMIT_WORDS 12288u
#define JPP_RESOURCE_VM_QUEUE_TARGET_DEPTH 4u
#define JPP_RESOURCE_VM_QUEUE_LIMIT_DEPTH 8u

#define JPP_RESOURCE_NATIVE_FREE_HEAP_AFTER_BOOT_FLOOR_BYTES 65536u

/* Text rows an app frame may set (rows 0..6 of the 8-page OLED; page 1 is
   reserved for the signature rule when frame_title_rule is set). */
#define JPP_RESOURCE_SDK_FRAME_LINE_TARGET 7u
#define JPP_RESOURCE_SDK_FRAME_LINE_LIMIT 7u

#define JPP_RESOURCE_SDK_HANDLE_TARGET 2u
#define JPP_RESOURCE_SDK_HANDLE_LIMIT 4u

#define JPP_RESOURCE_SDK_BLE_CONN_TARGET 1u
#define JPP_RESOURCE_SDK_BLE_CONN_LIMIT 2u

#define JPP_RESOURCE_SDK_NET_LISTENER_LIMIT 1u  /* concurrent listening sockets */
#define JPP_RESOURCE_SDK_NET_SOCKET_LIMIT 2u    /* concurrent accepted connections */

#define JPP_RESOURCE_BG_SCHEDULE_MAX 8u           /* scheduled tasks across all apps */
#define JPP_RESOURCE_BG_TASK_RUN_QUOTA_MS 30000u  /* wall-clock kill limit per headless run */
#define JPP_RESOURCE_BG_INTERVAL_MIN_S 60u        /* minimum schedule interval */

#define JPP_RESOURCE_BROKER_RESULT_FIELD_TARGET 4u
#define JPP_RESOURCE_BROKER_RESULT_FIELD_LIMIT 8u
/* Lock slots: storage, device, vm_queue, ble_radio, ble_client, ble_host,
   http, network */
#define JPP_RESOURCE_BROKER_LOCK_TARGET 4u
#define JPP_RESOURCE_BROKER_LOCK_LIMIT 8u

/* J++Device Serial Management Protocol (JPPD-SMP) */
#define SMP_TASK_STACK_BYTES   6144u
/* RX ring must comfortably hold one full upload-chunk frame
   (SOF+LEN+payload+CRC ~= SMP_CHUNK_SIZE + ~14 B) even if the smp_rx task is
   briefly starved mid-frame (SD write, WiFi, UI): at 512 B the ring could fill
   partway through a chunk and the USB-Serial-JTAG FIFO would then drop the tail,
   corrupting the frame's CRC so the device silently dropped it and the host
   stalled.  2 KB absorbs a whole frame plus margin. */
#define SMP_RX_BUF_BYTES       2048u
#define SMP_TX_BUF_BYTES       1536u
#define SMP_CHUNK_SIZE         1024u
#define SMP_SESSION_TIMEOUT_MS 30000u
#define SMP_MAX_PAYLOAD_BYTES  1280u
#define SMP_LIST_BUF_BYTES     2048u

#ifdef __cplusplus
}
#endif
