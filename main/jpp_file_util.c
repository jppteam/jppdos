#include "jpp_file_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

char *jpp_read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)size + 1u);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1u, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';
    if (out_size != NULL) {
        *out_size = n;
    }
    return buf;
}

long jpp_read_file_into(const char *path, char *buf, size_t buf_len)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return JPP_READ_ERR_OPEN;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size < 0 || (size_t)size >= buf_len) {
        fclose(f);
        return JPP_READ_ERR_SIZE;
    }
    size_t n = fread(buf, 1u, (size_t)size, f);
    fclose(f);
    buf[n] = '\0';
    return (long)n;
}

void jpp_make_parent_dirs(const char *path)
{
    char tmp[256];
    strncpy(tmp, path, sizeof(tmp) - 1u);
    tmp[sizeof(tmp) - 1u] = '\0';
    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}
