#include "json_util.h"
#include <string.h>
#include <stdio.h>

bool json_extract_string(const char *buf, const char *key,
                          char *dst, size_t dstsz)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(buf, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;  /* skip opening quote */
    size_t i = 0;
    while (*p && *p != '"' && i < dstsz - 1) dst[i++] = *p++;
    dst[i] = '\0';
    return i > 0;
}
