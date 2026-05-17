#pragma once
#include <time.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void ntp_time_start(void);
bool ntp_time_synced(void);      /* true once SNTP has completed at least one sync */
bool ntp_has_valid_time(void);   /* true once any valid time source is available
                                  * (RTC seed that passed the epoch sanity check, or
                                  *  a successful NTP sync).  Use this instead of
                                  *  ntp_time_synced() wherever the display needs a
                                  *  valid wall-clock time but doesn't require NTP
                                  *  specifically — e.g. the night-mode brightness check. */
void ntp_get_local(struct tm *t);
void ntp_apply_timezone(void);  /* re-apply TZ from current config (call after settings change) */
void ntp_apply_servers(void);   /* update SNTP server list from current config (call after settings change) */
#ifdef __cplusplus
}
#endif
