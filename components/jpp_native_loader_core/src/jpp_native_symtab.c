/*
 * jpp_native_symtab — firmware symbol table exported to native app binaries.
 *
 * Every symbol an app may call through the PLT (because it was compiled with
 * -fPIC -shared and calls an external function) must appear here. The loader
 * resolves UNDEF symbols in the app's .dynsym by scanning this table.
 *
 * Maintenance rule: if an app is rejected with UNRESOLVED_SYM at boot, add the
 * missing symbol here. Keep this table sorted by group for readability; order
 * does not matter for correctness (linear scan).
 */

#include "../include/jpp_native_loader_core.h"

#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sodium/crypto_hash_sha512.h"

/* libgcc soft-arithmetic helpers. */
extern long long __udivdi3(long long, long long);
extern long long __divdi3(long long, long long);
extern long long __umoddi3(long long, long long);
extern long long __moddi3(long long, long long);
extern long long __muldi3(long long, long long);
extern long long __ashldi3(long long, int);
extern long long __ashrdi3(long long, int);
extern unsigned long long __lshrdi3(unsigned long long, int);
extern int __clzsi2(unsigned int);
extern int __clzdi2(unsigned long long);
extern int __ctzsi2(unsigned int);
extern int __popcountsi2(unsigned int);

#include "jpp_sdk_bridge.h"
#include "jpp_broker_core.h"
#include "jpp_crypto_core.h"
#include "sodium/randombytes.h"

typedef struct {
    const char *name;
    void       *addr;
} jpp_native_sym_t;

/* ---- Symbol table -------------------------------------------------------- */

static const jpp_native_sym_t s_symtab[] = {

    /* ---- App SDK (jpp_sdk_bridge) --------------------------------------- */
    { "jpp_sdk_context_init",          (void *)jpp_sdk_context_init          },
    { "jpp_sdk_bind_native",           (void *)jpp_sdk_bind_native           },
    { "jpp_sdk_set_frame",             (void *)jpp_sdk_set_frame             },
    { "jpp_sdk_request_close",         (void *)jpp_sdk_request_close         },
    { "jpp_sdk_request_cap",           (void *)jpp_sdk_request_cap           },
    { "jpp_sdk_log",                   (void *)jpp_sdk_log                   },
    { "jpp_sdk_status_name",           (void *)jpp_sdk_status_name           },

    /* UI helpers */
    { "jpp_sdk_dialog",                (void *)jpp_sdk_dialog                },
    { "jpp_sdk_list",                  (void *)jpp_sdk_list                  },
    { "jpp_sdk_input",                 (void *)jpp_sdk_input                 },
    { "jpp_sdk_file_pick",             (void *)jpp_sdk_file_pick             },

    /* Key input */
    { "jpp_sdk_poll_key",              (void *)jpp_sdk_poll_key              },
    { "jpp_sdk_wait_key",              (void *)jpp_sdk_wait_key              },
    { "jpp_sdk_push_key",              (void *)jpp_sdk_push_key              },

    /* Canvas */
    { "jpp_sdk_canvas_clear",          (void *)jpp_sdk_canvas_clear          },
    { "jpp_sdk_canvas_draw_pixel",     (void *)jpp_sdk_canvas_draw_pixel     },
    { "jpp_sdk_canvas_write",          (void *)jpp_sdk_canvas_write          },
    { "jpp_sdk_canvas_fullscreen",     (void *)jpp_sdk_canvas_fullscreen     },

    /* Dynamic code modules */
    { "jpp_sdk_module_load",           (void *)jpp_sdk_module_load           },
    { "jpp_sdk_module_run",            (void *)jpp_sdk_module_run            },
    { "jpp_sdk_module_unload",         (void *)jpp_sdk_module_unload         },

    /* Device / RTC */
    { "jpp_sdk_device_status",         (void *)jpp_sdk_device_status         },
    { "jpp_sdk_get_time",              (void *)jpp_sdk_get_time              },
    { "jpp_sdk_is_dummy_mode",         (void *)jpp_sdk_is_dummy_mode         },

    /* File I/O — scoped */
    { "jpp_sdk_file_read",             (void *)jpp_sdk_file_read             },
    { "jpp_sdk_file_write",            (void *)jpp_sdk_file_write            },
    { "jpp_sdk_file_list",             (void *)jpp_sdk_file_list             },

    /* File I/O — shared */
    { "jpp_sdk_shared_read",           (void *)jpp_sdk_shared_read           },
    { "jpp_sdk_shared_write",          (void *)jpp_sdk_shared_write          },
    { "jpp_sdk_shared_list",           (void *)jpp_sdk_shared_list           },

    /* File I/O — full path (requires capability) */
    { "jpp_sdk_file_open",             (void *)jpp_sdk_file_open             },
    { "jpp_sdk_handle_read",           (void *)jpp_sdk_handle_read           },
    { "jpp_sdk_handle_write",          (void *)jpp_sdk_handle_write          },
    { "jpp_sdk_handle_list",           (void *)jpp_sdk_handle_list           },
    { "jpp_sdk_handle_close",          (void *)jpp_sdk_handle_close          },

    /* BLE — scanning / advertising */
    { "jpp_sdk_ble_scan",              (void *)jpp_sdk_ble_scan              },
    { "jpp_sdk_ble_advertise_start",   (void *)jpp_sdk_ble_advertise_start   },
    { "jpp_sdk_ble_advertise_stop",    (void *)jpp_sdk_ble_advertise_stop    },
    { "jpp_sdk_ble_set_connectable",   (void *)jpp_sdk_ble_set_connectable   },

    /* BLE — GATT client */
    { "jpp_sdk_ble_connect",           (void *)jpp_sdk_ble_connect           },
    { "jpp_sdk_ble_read_char",         (void *)jpp_sdk_ble_read_char         },
    { "jpp_sdk_ble_write_char",        (void *)jpp_sdk_ble_write_char        },
    { "jpp_sdk_ble_disconnect",        (void *)jpp_sdk_ble_disconnect        },

    /* BLE — GATT host */
    { "jpp_sdk_ble_service_register",  (void *)jpp_sdk_ble_service_register  },
    { "jpp_sdk_ble_service_unregister",(void *)jpp_sdk_ble_service_unregister},
    { "jpp_sdk_ble_host_set_value",    (void *)jpp_sdk_ble_host_set_value    },
    { "jpp_sdk_ble_host_wait_write",   (void *)jpp_sdk_ble_host_wait_write   },
    { "jpp_sdk_ble_host_clear",        (void *)jpp_sdk_ble_host_clear        },

    /* ESP-NOW */
    { "jpp_sdk_espnow_send",           (void *)jpp_sdk_espnow_send           },
    { "jpp_sdk_espnow_recv",           (void *)jpp_sdk_espnow_recv           },

    /* Wakelock */
    { "jpp_sdk_wakelock_acquire",      (void *)jpp_sdk_wakelock_acquire      },
    { "jpp_sdk_wakelock_release",      (void *)jpp_sdk_wakelock_release      },

    /* Buzzer */
    { "jpp_sdk_buzzer_play",           (void *)jpp_sdk_buzzer_play           },
    { "jpp_sdk_buzzer_tone",           (void *)jpp_sdk_buzzer_tone           },
    { "jpp_sdk_buzzer_play_sequence",  (void *)jpp_sdk_buzzer_play_sequence  },
    { "jpp_sdk_buzzer_play_sequence_async", (void *)jpp_sdk_buzzer_play_sequence_async },
    { "jpp_sdk_buzzer_stop",           (void *)jpp_sdk_buzzer_stop           },

    /* LED */
    { "jpp_sdk_led_set_color",         (void *)jpp_sdk_led_set_color         },
    { "jpp_sdk_led_off",               (void *)jpp_sdk_led_off               },

    /* HTTP */
    { "jpp_sdk_http_request",          (void *)jpp_sdk_http_request          },
    { "jpp_sdk_net_bind",              (void *)jpp_sdk_net_bind              },
    { "jpp_sdk_net_accept",            (void *)jpp_sdk_net_accept            },
    { "jpp_sdk_net_recv",              (void *)jpp_sdk_net_recv              },
    { "jpp_sdk_net_send",              (void *)jpp_sdk_net_send              },
    { "jpp_sdk_net_close",             (void *)jpp_sdk_net_close             },
    { "jpp_sdk_net_close_all",         (void *)jpp_sdk_net_close_all         },

    /* Background tasks */
    { "jpp_sdk_background_register",   (void *)jpp_sdk_background_register   },

    /* IPC */
    { "jpp_sdk_ipc_send",              (void *)jpp_sdk_ipc_send              },
    { "jpp_sdk_ipc_recv",              (void *)jpp_sdk_ipc_recv              },

    /* KV store */
    { "jpp_sdk_kv_get",                (void *)jpp_sdk_kv_get                },
    { "jpp_sdk_kv_set",                (void *)jpp_sdk_kv_set                },
    { "jpp_sdk_kv_delete",             (void *)jpp_sdk_kv_delete             },

    /* ---- Broker core (apps may call result helpers directly) ------------ */
    { "jpp_broker_result_get",         (void *)jpp_broker_result_get         },
    { "jpp_broker_result_put",         (void *)jpp_broker_result_put         },
    { "jpp_broker_ok_result",          (void *)jpp_broker_ok_result          },
    { "jpp_broker_error_result",       (void *)jpp_broker_error_result       },

    /* ---- Cryptography --------------------------------------------------- */
    { "jpp_crypto_keygen",             (void *)jpp_crypto_keygen             },
    { "jpp_crypto_sign",               (void *)jpp_crypto_sign               },
    { "jpp_crypto_verify",             (void *)jpp_crypto_verify             },
    { "jpp_crypto_musig2_nonce_gen",   (void *)jpp_crypto_musig2_nonce_gen   },
    { "jpp_crypto_musig2_partial_sign",(void *)jpp_crypto_musig2_partial_sign},
    { "jpp_crypto_musig2_aggregate_sigs",
                                       (void *)jpp_crypto_musig2_aggregate_sigs   },
    { "jpp_crypto_musig2_aggregate_nonces",
                                       (void *)jpp_crypto_musig2_aggregate_nonces },
    { "jpp_crypto_musig2_aggregate_pubkeys",
                                       (void *)jpp_crypto_musig2_aggregate_pubkeys},
    { "jpp_crypto_musig2_verify",      (void *)jpp_crypto_musig2_verify      },

    /* libsodium */
    { "randombytes_buf",               (void *)randombytes_buf               },

    /* ---- libsodium (low-level helpers called directly by some apps) ------- */
    { "crypto_hash_sha512",            (void *)crypto_hash_sha512            },

    /* ---- libgcc soft-arithmetic helpers (needed for 64-bit ops on rv32) -- */
    { "__udivdi3",   (void *)__udivdi3   },
    { "__divdi3",    (void *)__divdi3    },
    { "__umoddi3",   (void *)__umoddi3   },
    { "__moddi3",    (void *)__moddi3    },
    { "__muldi3",    (void *)__muldi3    },
    { "__ashldi3",   (void *)__ashldi3   },
    { "__ashrdi3",   (void *)__ashrdi3   },
    { "__lshrdi3",   (void *)__lshrdi3   },
    { "__clzsi2",    (void *)__clzsi2    },
    { "__clzdi2",    (void *)__clzdi2    },
    { "__ctzsi2",    (void *)__ctzsi2    },
    { "__popcountsi2", (void *)__popcountsi2 },

    /* ---- ESP-IDF / FreeRTOS --------------------------------------------- */
    { "esp_log",                       (void *)esp_log                       },
    { "esp_log_write",                 (void *)esp_log_write                 },
    { "esp_log_timestamp",             (void *)esp_log_timestamp             },
    { "eTaskGetState",                 (void *)eTaskGetState                 },
    { "vTaskDelay",                    (void *)vTaskDelay                    },
    { "vTaskDelete",                   (void *)vTaskDelete                   },
    { "xTaskCreatePinnedToCore",       (void *)xTaskCreatePinnedToCore       },
    { "xTaskGetTickCount",             (void *)xTaskGetTickCount             },

    /* ---- C standard library (newlib, linked into firmware) --------------- */
    { "memcpy",    (void *)memcpy    },
    { "memset",    (void *)memset    },
    { "memcmp",    (void *)memcmp    },
    { "memmove",   (void *)memmove   },
    { "strlen",    (void *)strlen    },
    { "strnlen",   (void *)strnlen   },
    { "strcmp",    (void *)strcmp    },
    { "strncmp",   (void *)strncmp   },
    { "strcpy",    (void *)strcpy    },
    { "strncpy",   (void *)strncpy   },
    { "strcat",    (void *)strcat    },
    { "strncat",   (void *)strncat   },
    { "strchr",    (void *)strchr    },
    { "strstr",    (void *)strstr    },
    { "snprintf",  (void *)snprintf  },
    { "sprintf",   (void *)sprintf   },
    { "sscanf",    (void *)sscanf    },
    { "atoi",      (void *)atoi      },
    { "malloc",    (void *)malloc    },
    { "calloc",    (void *)calloc    },
    { "realloc",   (void *)realloc   },
    { "free",      (void *)free      },
    { "fopen",     (void *)fopen     },
    { "fclose",    (void *)fclose    },
    { "fread",     (void *)fread     },
    { "fwrite",    (void *)fwrite    },
    { "fseek",     (void *)fseek     },
    { "ftell",     (void *)ftell     },
    { "rewind",    (void *)rewind    },
    { "fflush",    (void *)fflush    },
    { "stat",      (void *)stat      },
    { "mkdir",     (void *)mkdir     },
    { "remove",    (void *)remove    },
    { "rename",    (void *)rename    },
    { "abort",     (void *)abort     },
};

const jpp_native_sym_t *const jpp_native_symtab  = s_symtab;
const size_t                  jpp_native_symtab_count =
    sizeof(s_symtab) / sizeof(s_symtab[0]);
