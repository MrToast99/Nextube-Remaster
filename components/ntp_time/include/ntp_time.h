#pragma once
#include <time.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void ntp_time_start(void);
bool ntp_time_synced(void);
void ntp_get_local(struct tm *t);
void ntp_apply_timezone(void);  /* re-apply TZ from current config (call after settings change) */
#ifdef __cplusplus
}
#endif
