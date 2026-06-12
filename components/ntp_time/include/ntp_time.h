#pragma once
#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void ntp_time_start(void);

/**
 * Sync-stats listener: called after each steady-state NTP sync (boot and
 * boot-window syncs excluded) with the values the "NTP sync:" log line shows:
 *   xtal_drift_ms  - what the free-running ESP crystal's error WOULD have
 *                    been since the last sync without discipline (signed).
 *   pcf_max_err_ms - worst single-minute clock error the PCF8563 slave
 *                    discipline allowed since the last sync; -1 when not
 *                    available (discipline mode != 2 or no ticks recorded).
 *   discipline_mode- 0 off, 1 ESP rate discipline, 2 PCF slave.
 * Runs in SNTP callback context — i.e. lwIP's tcpip thread (tiT).  The
 * handler MUST NOT block or take any mutex that a network-using task can
 * hold (config_lock, esp_mqtt_client_publish, sockets): blocking tiT
 * deadlocks the entire network stack.  Stash the values and defer all work
 * to your own task.  One listener slot.
 */
typedef void (*ntp_sync_listener_t)(int32_t xtal_drift_ms,
                                    float pcf_max_err_ms,
                                    int discipline_mode);
void ntp_register_sync_listener(ntp_sync_listener_t cb);
bool ntp_time_synced(void);      /* true once SNTP has completed at least one sync */
bool ntp_has_valid_time(void);   /* true once any valid time source is available
                                  * (RTC seed that passed the epoch sanity check, or
                                  *  a successful NTP sync).  Use this instead of
                                  *  ntp_time_synced() wherever the display needs a
                                  *  valid wall-clock time but doesn't require NTP
                                  *  specifically — e.g. the night-mode brightness check. */
bool ntp_rtc_battery_ok(void);  /* true if the RTC seed at boot was >= 2025-01-01.
                                  * Never becomes true from an NTP sync alone — reflects
                                  * the physical battery/RTC state at power-on.  When
                                  * false the web UI shows a "replace CR1220" warning. */
void ntp_get_local(struct tm *t);
void ntp_apply_timezone(void);  /* re-apply TZ from current config (call after settings change) */
void ntp_apply_servers(void);   /* update SNTP server list from current config (call after settings change) */
void ntp_seed_rtc_early(void);  /* call from app_main BEFORE display_task_start() — reads RTC once and
                                  * seeds the system clock synchronously so the first display render
                                  * never shows the stopwatch.  No-op if the RTC value fails the
                                  * epoch sanity check (dead battery / never set). */
#ifdef __cplusplus
}
#endif
