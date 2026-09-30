#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Percent-encode a value for embedding in a URL query/path segment (RFC 3986
 * unreserved set kept literal; everything else — including '&', '?', '"',
 * spaces — escaped as %XX). Used to safely embed usernames/instance names/
 * city names sourced from user-editable config into request URLs.
 * Shared by weather.c (city name) and subscribers.c (usernames/instance
 * names for Instagram/TikTok/Mastodon lookups). */
void url_encode_query(const char *in, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif
