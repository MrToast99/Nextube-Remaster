#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
void wifi_manager_start(void);
/* Re-read credentials, disconnect + reconnect.  Returns true when a reboot
 * is needed for the current config to take full effect (this path re-applies
 * SSID/password only — it does not restart DHCP or reapply static-IP fields,
 * so if static IP is enabled the caller should surface that a reboot is
 * required).  No-op (returns false) if no SSID is configured. */
bool wifi_manager_reconnect_sta(void);
void wifi_manager_apply_sta_credentials(void); /* update driver config without disconnecting */
bool wifi_manager_is_connected(void);
const char *wifi_manager_get_ip(void);
void wifi_manager_scan_start(void);
/* True from wifi_manager_scan_start() until WIFI_EVENT_SCAN_DONE fires. */
bool wifi_manager_scan_in_progress(void);

/* ──────— disconnect/reconnect diagnostics ──────────────────────────
 * Session-scoped counters (reset on reboot), fed by the WIFI_EVENT_STA_
 * DISCONNECTED / IP_EVENT_STA_GOT_IP handlers. */

/* Number of STA disconnect events observed since boot. */
uint32_t wifi_manager_get_disconnect_count(void);

/* wifi_err_reason_t of the most recent disconnect, or 0 if none yet this
 * session.  Raw code — map to a human string in the caller (web UI). */
uint8_t wifi_manager_get_last_disconnect_reason(void);

/* esp_timer_get_time() timestamp of the current connection's GOT_IP event,
 * or 0 if not currently connected. */
int64_t wifi_manager_get_connected_since_us(void);

/* Snapshot of the current STA network configuration/link details.
 * Returns false (out left unmodified) when not connected. */
typedef struct {
    char    mac[18];
    char    bssid[18];
    uint8_t channel;
    int8_t  rssi;    /* dBm, typically -30 excellent to -90 unusable; 0 if unavailable */
    char    netmask[16];
    char    gateway[16];
    char    dns1[16];
    char    dns2[16];
    bool    phy_11b, phy_11g, phy_11n, phy_lr;
} wifi_manager_net_info_t;

bool wifi_manager_get_net_info(wifi_manager_net_info_t *out);

/* Bring the setup AP ("Nextube-Setup") up on demand, regardless of STA state.
 * Triggered by the LEFT+RIGHT touch-pad hotkey (held 15 s).  Idempotent —
 * a no-op if the AP is already broadcasting.  The AP closes automatically
 * 60 s after STA next obtains an IP (so a browser session can migrate). */
void wifi_manager_force_ap(void);

/* ──────— setup AP PIN ──────────────────────────────────────────────
 * The setup AP ("Nextube-Setup") is WPA2-secured with a per-device
 * 8-digit PIN.  PIN is generated on first boot, persisted in the NVS
 * namespace "nextube_sec", and displayed on the LCD tubes whenever
 * the AP is broadcasting and no client is associated. */

/* Returns the current 8-digit PIN string (NUL-terminated).  Pointer is
 * stable for the lifetime of the process. */
const char *wifi_manager_get_ap_pin(void);

/* True when the AP is currently broadcasting (WIFI_MODE_APSTA). */
bool wifi_manager_ap_active(void);

/* True when AP is broadcasting AND no client is associated.  The display
 * task uses this to decide whether to render the PIN on the tubes. */
bool wifi_manager_ap_pin_visible(void);

/* Generate a new random PIN, persist it, and apply to the live AP.
 * Existing associated clients are NOT kicked.  Returns ESP_OK on success. */
esp_err_t wifi_manager_regenerate_ap_pin(void);

/* Wipe the PIN from NVS.  Intended for the "Full factory reset" flow.
 * A fresh PIN is auto-generated on the next call to wifi_manager_start
 * (i.e. next boot). */
void wifi_manager_factory_reset_ap_pin(void);

/* Call right before rebooting a settings save that changed the SSID but
 * skipped the live-reconnect path (i.e. some OTHER field in the same save
 * also required a reboot — hostname, weather/youtube/mdns/mic/audio_enabled).
 * Without this, wifi_manager_start()'s normal policy (any saved SSID → STA
 * only, no AP) would boot straight into brand-new, never-tried credentials
 * with no fallback: if they're wrong, a client still on the setup AP loses
 * the device outright. Makes the very next boot bring the setup AP up
 * alongside STA instead, so it's still reachable either way; one-shot,
 * consumed on that next boot regardless of whether STA succeeds. */
void wifi_manager_mark_untested_sta(void);

/* Defer a needed reboot until STA actually confirms the just-applied
 * credentials (IP_EVENT_STA_GOT_IP), instead of rebooting immediately with
 * wifi_manager_mark_untested_sta()'s AP-fallback safety net. Call this
 * INSTEAD of mark_untested_sta()+reboot when a settings save changed the
 * SSID and also needs a reboot for something else (hostname/weather/etc.):
 * it starts a live reconnect on the new credentials (same path as a plain
 * SSID-only change — the setup AP, if any, stays up and reachable
 * throughout) and only reboots once that reconnect succeeds, so the reboot
 * never has to gamble on untested WiFi credentials. Falls back to the old
 * mark-untested-sta-and-reboot-now behavior if STA doesn't confirm within
 * timeout_ms, so genuinely wrong restored credentials still can't wedge
 * the device — caller must still have triggered the live reconnect itself
 * before calling this. */
void wifi_manager_reboot_once_connected(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
