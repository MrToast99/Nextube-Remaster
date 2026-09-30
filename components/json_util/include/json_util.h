#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lightweight strstr-based extraction of a JSON string value (first match
 * of "key": "value" in buf). Works on a truncated/partial JSON buffer as
 * long as the target field itself is intact — no full-document parse, so
 * it tolerates a response cut off past the field of interest. Not a real
 * JSON parser: doesn't handle escaped quotes within the value, nested keys
 * of the same name, or non-string values. Returns false (dst untouched
 * beyond dst[0]) if the key isn't found or its value isn't a quoted string.
 * Shared by weather.c (Met.no/OpenWeather/wttr responses) and
 * update_check.c (GitHub releases' "tag_name"). */
bool json_extract_string(const char *buf, const char *key,
                          char *dst, size_t dstsz);

#ifdef __cplusplus
}
#endif
