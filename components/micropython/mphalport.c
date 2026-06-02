#include "mphalport.h"
#include "py/mphal.h"
#include "py/runtime.h"
#include "py/gc.h"
#include "py/lexer.h"
#include "py/builtin.h"
#include "py/mperrno.h"

#include "esp_log.h"

#include <sys/stat.h>
#include <string.h>

static const char *TAG = "mp_hal";

/* stdout_helpers.c provides mp_hal_stdout_tx_str built on top of this.
   Python print() output goes to the serial monitor as a DEBUG message. */
mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len)
{
    /* ESP_LOGD needs a null-terminated string; copy if needed. */
    char buf[256];
    size_t n = len < sizeof(buf) - 1u ? len : sizeof(buf) - 1u;
    memcpy(buf, str, n);
    buf[n] = '\0';
    ESP_LOGD(TAG, "%s", buf);
    return (mp_uint_t)len;
}

/* Interactive stdin is not supported — apps use jppsdk.wait_key() instead. */
int mp_hal_stdin_rx_chr(void)
{
    return -1;
}

void mp_hal_set_interrupt_char(int c)
{
    (void)c;
}

void nlr_jump_fail(void *val)
{
    (void)val;
    ESP_LOGE(TAG, "nlr_jump_fail: unhandled exception escape");
    abort();
}

/* Required by MicroPython's garbage collector — mark roots on the C stack. */
void gc_collect(void)
{
    gc_collect_start();
    void *sp = (void *)__builtin_frame_address(0);
    void *stack_bottom = pxTaskGetStackStart(NULL);
    if (stack_bottom && sp > stack_bottom) {
        gc_collect_root((void **)stack_bottom,
                        ((void *)sp - stack_bottom) / sizeof(void *));
    }
    gc_collect_end();
}

/* Port-required: check whether a filesystem path exists and what it is.
 *
 * MicroPython v1.28 calls stat_file_py_or_mpy() which checks the .py path
 * first; if it reports FILE the import succeeds and mp_lexer_new_from_file()
 * is called (which is a stub that always raises OSError on this port).  To
 * make the import machinery skip .py and fall through to .mpy, report every
 * .py path as absent.  This is correct: JPPDOS apps ship as compiled .mpy
 * bytecode only and .py source import is intentionally unsupported. */
mp_import_stat_t mp_import_stat(const char *path)
{
    size_t len = strlen(path);
    if (len >= 3 && path[len - 3] == '.' &&
        path[len - 2] == 'p' && path[len - 1] == 'y') {
        return MP_IMPORT_STAT_NO_EXIST;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        return MP_IMPORT_STAT_NO_EXIST;
    }
    if (S_ISDIR(st.st_mode)) {
        return MP_IMPORT_STAT_DIR;
    }
    return MP_IMPORT_STAT_FILE;
}

/* Port-required: the built-in open() function.
   Apps must use jppsdk file APIs; raw open() is not permitted. */
static mp_obj_t mp_builtin_open_impl(size_t n_args, const mp_obj_t *args,
                                     mp_map_t *kwargs)
{
    (void)n_args; (void)args; (void)kwargs;
    mp_raise_OSError(MP_EACCES);
    __builtin_unreachable();
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, mp_builtin_open_impl);
