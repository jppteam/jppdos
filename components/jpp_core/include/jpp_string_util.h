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

/* True iff name is a dot-separated identifier: [A-Za-z0-9_] segments joined
   by single dots, with no leading or trailing dot. */
static inline bool jpp_str_name_valid(const char *name)
{
    bool previous_was_dot = false;

    if (!jpp_str_nonempty(name) || name[0] == '.') {
        return false;
    }
    for (size_t i = 0u; name[i] != '\0'; i++) {
        char c = name[i];
        if (c == '.') {
            if (previous_was_dot || name[i + 1u] == '\0') {
                return false;
            }
            previous_was_dot = true;
            continue;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            previous_was_dot = false;
            continue;
        }
        return false;
    }
    return true;
}

/* True iff path contains a ".." segment ("..", "../x", "x/../y", "x/.."). */
static inline bool jpp_str_has_parent_segment(const char *path)
{
    if (path == NULL) {
        return false;
    }
    for (size_t i = 0u; path[i] != '\0'; i++) {
        if (path[i] == '.' && path[i + 1u] == '.' &&
            (i == 0u || path[i - 1u] == '/') &&
            (path[i + 2u] == '\0' || path[i + 2u] == '/')) {
            return true;
        }
    }
    return false;
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
