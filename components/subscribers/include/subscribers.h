#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t subscriber_count;
    bool     valid;
} sub_count_t;
void subscribers_start(void);
/** Immediately wake the subscribers task to start a fresh poll cycle,
 *  bypassing the remaining sleep interval. No-op if task is mid-fetch. */
void subscribers_refresh_now(void);
const sub_count_t *subscribers_get(void);
const sub_count_t *instagram_get(void);
const sub_count_t *tiktok_get(void);
const sub_count_t *mastodon_get(void);
#ifdef __cplusplus
}
#endif
