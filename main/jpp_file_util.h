#pragma once

#include <stddef.h>

#define JPP_READ_ERR_OPEN (-1L)  /* file cannot be opened */
#define JPP_READ_ERR_SIZE (-2L)  /* file does not fit in buf_len - 1 */

/* Read the whole file at path into a malloc'd NUL-terminated buffer.
   Returns NULL if the file cannot be opened or memory is unavailable.
   out_size (optional) receives the byte count excluding the NUL.
   The caller frees the buffer. */
char *jpp_read_file(const char *path, size_t *out_size);

/* Read the whole file at path into buf, NUL-terminating it. Returns the
   byte count on success, JPP_READ_ERR_OPEN or JPP_READ_ERR_SIZE on failure. */
long jpp_read_file_into(const char *path, char *buf, size_t buf_len);

/* Create every missing parent directory of path ("mkdir -p" for the dirname). */
void jpp_make_parent_dirs(const char *path);
