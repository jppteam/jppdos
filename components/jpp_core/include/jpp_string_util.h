#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true iff s is non-NULL and non-empty. */
static inline bool jpp_str_nonempty(const char *s)
{
    return s != NULL && s[0] != '\0';
}

/* Returns true iff both a and b are non-NULL and compare equal. */
static inline bool jpp_str_eq(const char *a, const char *b)
{
    return a != NULL && b != NULL && strcmp(a, b) == 0;
}

/* Copy src into dst[0..n-1], always NUL-terminating. Treats NULL src as "". */
static inline void jpp_str_copy(char *dst, size_t n, const char *src)
{
    if (dst == NULL || n == 0u) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    (void)snprintf(dst, n, "%s", src);
}

#ifdef __cplusplus
}
#endif
