#include "wifi_manager.h"
#include "config_mgr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT BIT0
static EventGroupHandle_t s_wifi_events;
static char s_ip_str[20] = "0.0.0.0";
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;

/* mDNS state — set once in wifi_manager_start() based on config.
 *
 * s_mdns_on:       true when mDNS was enabled at boot.  Avoids touching the
 *                  mDNS API in the event handler if the feature is disabled.
 *
 * s_last_mdns_ip:  The IP address seen at the last IP_EVENT_STA_GOT_IP.
 *                  Starts at {0} (all-zeros).  NEVER cleared on disconnect.
 *
 * On GOT_IP we pick one of two mDNS actions:
 *   • New / changed IP  → ENABLE_IP4: full probe + announce so .local
 *                         resolvers learn (or update) the address.
 *   • Same IP (a reconnect on the same DHCP lease) → ANNOUNCE_IP4: re-announce
 *                         without re-probing.  The hostname claim hasn't
 *                         changed, so a fresh probe cycle would just be wasted
 *                         mDNS multicast traffic.
 *
 * Why we track s_last_mdns_ip ourselves instead of using ev->ip_changed:
 * esp_netif_action_disconnected() clears ip_info_old to 0.0.0.0 on every
 * disconnect, so ev->ip_changed is always true after a reconnect — even when
 * the lease IP is unchanged.  Keeping our own last-IP and never clearing it on
 * disconnect lets us tell a genuine address change from a same-IP reconnect.
 * Because s_last_mdns_ip starts at {0}, the very first GOT_IP is always a
 * "changed" IP → ENABLE_IP4 runs first and initialises the interface.
 * (ANNOUNCE_IP4 is a no-op until the interface has been enabled, so it can
 * never fire before that first ENABLE_IP4.)
 *
 * mdns_register_netif() IS called once in wifi_manager_start(), immediately
 * after mdns_init().  With CONFIG_MDNS_PREDEF_NETIF_STA=n the daemon does not
 * auto-populate its s_esp_netifs[] table, so without this call
 * get_if_from_netif() cannot find s_sta_netif and every mdns_netif_action()
 * returns ESP_ERR_INVALID_STATE (mDNS never announces).  In the refactored
 * (2025) espressif/mdns component the call ONLY stores the esp_netif_t pointer
 * in that table — it installs no LWIP netif-ext callback and no esp-event
 * handler. */
static bool         s_mdns_on      = false;
static esp_ip4_addr_t s_last_mdns_ip = {0};

/* AP is disabled 60 seconds after STA gets an IP.  60 s gives the browser
 * enough time to finish loading the web UI on the AP-side IP before the
 * AP disappears (client then reconnects via the LAN IP / mDNS). */
#define AP_DISABLE_DELAY_US      (60LL * 1000 * 1000)   /* 60 seconds  */

/* The setup AP is NOT brought up automatically when STA fails.  Instead the
 * user summons it on demand by holding the LEFT + RIGHT touch pads together
 * for 15 s (touch_input combo → wifi_manager_force_ap()).  This avoids the AP
 * popping up unexpectedly during transient WiFi outages, and is reliable even
 * during a connect-retry storm (which previously kept resetting the old timer
 * so it never elapsed). */

/* ────— per-device WPA2 PIN for the setup AP ─────────────────────────
 * Generated on first boot (from esp_random) and stored in the NVS namespace
 * "nextube_sec" so it persists across reboots and across normal flash
 * re-imaging (NVS is at offset 0x9000, outside the merged-image regions).
 * Only an explicit erase_flash or a "Full factory reset" UI action wipes it.
 *
 * The PIN is rendered on the LCD tubes whenever the AP is broadcasting and
 * no client is associated — see display.c::render_ap_pin and the
 * wifi_manager_ap_pin_visible() flag below.  This eliminates any need for
 * an external sticker / label on the device. */
#define AP_PIN_NVS_NS         "nextube_sec"
#define AP_PIN_NVS_KEY        "ap_pin"

#define AP_PIN_LEN            8     /* must be ≥ 8 — WPA2_PSK minimum length */

static char s_ap_pin[AP_PIN_LEN + 1] = {0};

/* Module-level hostname buffer.  LWIP's netif_set_hostname() stores the raw
 * pointer — it does NOT copy the string.  Passing a stack-local char array
 * leaves netif->hostname dangling after the caller returns, causing DHCP
 * to read freed stack memory (and emit whatever was there — often the
 * compile-time "espressif" default) on every subsequent DHCP transaction.
 * Keeping the string here guarantees it lives as long as the netif. */
static char s_hostname[32] = "nextube-remaster";
static volatile int s_ap_client_count = 0;

/* Explicit "AP is broadcasting" flag.  Tracked locally rather than queried
 * via esp_wifi_get_mode() because (a) the driver's mode field can hold
 * transient values during init and reconfig, and (b) we may stage the AP
 * config in APSTA mode and then demote to STA before esp_wifi_start —
 * during that window the driver thinks mode == APSTA but no AP frames
 * have actually been emitted.  The display task uses this flag to decide
 * whether to render the PIN on the tubes; getting it wrong shows the PIN
 * on a deployed device that doesn't have its AP up. */
static volatile bool s_ap_active = false;

static esp_timer_handle_t s_ap_disable_timer   = NULL;

/* Set by wifi_manager_reconnect_sta() before it calls esp_wifi_connect()
 * directly.  Tells WIFI_EVENT_STA_DISCONNECTED NOT to fire a second
 * esp_wifi_connect() for that same reconnect attempt, which would create
 * two concurrent association requests and leave the TCP/IP stack in an
 * indeterminate state. */
static bool s_manual_reconnect = false;

/* ──────— AP PIN helpers ──────────────────────────────────────────── */

/* Generate a random 8-digit numeric PIN into out (must be AP_PIN_LEN+1).
 * Uses esp_random() so each boot of an unprovisioned device gets a fresh
 * value.  Writes only ASCII digits ('0'..'9'). */
static void generate_pin(char out[AP_PIN_LEN + 1])
{
    /* Two 32-bit randoms give ~64 bits of entropy, more than enough for 8
     * decimal digits (~26.6 bits).  We could just use one but burning two
     * is free and lets us combine them for digits 6-7 to avoid a small
     * modulo bias on a single uint32. */
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    uint64_t r  = ((uint64_t)r1 << 32) | r2;
    snprintf(out, AP_PIN_LEN + 1, "%08llu", (unsigned long long)(r % 100000000ULL));
}

/* Load PIN from NVS, or generate-and-store on first call. */
static void ensure_ap_pin(void)
{
    if (s_ap_pin[0] != '\0') return;   /* already loaded this boot */

    nvs_handle_t h;
    if (nvs_open(AP_PIN_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        /* NVS open failed — fall back to a fresh in-RAM PIN so the AP can
         * still come up.  It won't persist, but the device is functional. */
        generate_pin(s_ap_pin);
        ESP_LOGW(TAG, "NVS unavailable — using transient AP PIN");
        return;
    }
    size_t len = sizeof(s_ap_pin);
    esp_err_t err = nvs_get_str(h, AP_PIN_NVS_KEY, s_ap_pin, &len);
    if (err != ESP_OK || strlen(s_ap_pin) < AP_PIN_LEN) {
        generate_pin(s_ap_pin);
        nvs_set_str(h, AP_PIN_NVS_KEY, s_ap_pin);
        nvs_commit(h);
        ESP_LOGI(TAG, "Generated new AP PIN (first boot)");
    }
    nvs_close(h);
}

const char *wifi_manager_get_ap_pin(void)
{
    return s_ap_pin;
}

bool wifi_manager_ap_active(void)
{
    return s_ap_active;
}

bool wifi_manager_ap_pin_visible(void)
{
    return wifi_manager_ap_active() && (s_ap_client_count == 0);
}

esp_err_t wifi_manager_regenerate_ap_pin(void)
{
    char fresh[AP_PIN_LEN + 1];
    generate_pin(fresh);

    nvs_handle_t h;
    esp_err_t err = nvs_open(AP_PIN_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, AP_PIN_NVS_KEY, fresh);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    memcpy(s_ap_pin, fresh, sizeof(fresh));

    /* Push the new password into the live AP config.  Existing associated
     * clients are not kicked — only new associations need the new PIN. */
    wifi_config_t cfg;
    if (esp_wifi_get_config(WIFI_IF_AP, &cfg) == ESP_OK) {
        memset(cfg.ap.password, 0, sizeof(cfg.ap.password));
        strncpy((char *)cfg.ap.password, s_ap_pin, sizeof(cfg.ap.password) - 1);
        esp_wifi_set_config(WIFI_IF_AP, &cfg);
    }
    ESP_LOGI(TAG, "AP PIN regenerated");
    return ESP_OK;
}

void wifi_manager_factory_reset_ap_pin(void)
{
    nvs_handle_t h;
    if (nvs_open(AP_PIN_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, AP_PIN_NVS_KEY);
        nvs_commit(h);
        nvs_close(h);
    }
    s_ap_pin[0] = '\0';
    ESP_LOGW(TAG, "AP PIN factory-reset — fresh PIN will be generated on next boot");
}


static void ap_disable_cb(void *arg)
{
    ESP_LOGI(TAG, "STA connected – disabling setup AP");
    s_ap_active = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
}

/* Bring the setup AP up on demand (manual trigger — the LEFT+RIGHT touch-pad
 * hotkey, see touch_input combo handling).  Idempotent: a no-op if the AP is
 * already broadcasting.  The AP stays up until STA obtains an IP, at which
 * point IP_EVENT_STA_GOT_IP schedules the usual 60 s graceful shutdown so the
 * browser session can migrate to the LAN IP.  Safe to call from a task
 * context (esp_wifi_set_mode is thread-safe). */
void wifi_manager_force_ap(void)
{
    if (s_ap_active) return;            /* already broadcasting */
    ESP_LOGW(TAG, "Manual AP trigger — enabling setup AP (SSID: Nextube-Setup)");
    s_ap_active = true;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
}

static void init_ap_timers(void)
{
    esp_timer_create_args_t a = { .callback = ap_disable_cb,  .name = "ap_disable"  };
    esp_timer_create(&a, &s_ap_disable_timer);
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
            /* Cancel any pending AP-disable — STA is no longer "up". */
            if (s_ap_disable_timer) esp_timer_stop(s_ap_disable_timer);
            if (s_manual_reconnect) {
                /* wifi_manager_reconnect_sta() already called esp_wifi_connect()
                 * directly — skip the auto-reconnect here to avoid a second
                 * concurrent association attempt. */
                ESP_LOGI(TAG, "STA disconnected (manual reconnect in progress)");
                s_manual_reconnect = false;
            } else {
                ESP_LOGW(TAG, "STA disconnected, retrying...");
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev = data;
            s_ap_client_count++;
            ESP_LOGI(TAG, "AP: client connected (AID=%d, total=%d)",
                     ev->aid, s_ap_client_count);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *ev = data;
            if (s_ap_client_count > 0) s_ap_client_count--;
            ESP_LOGI(TAG, "AP: client disconnected (AID=%d, total=%d)",
                     ev->aid, s_ap_client_count);
            break;
        }
        default: break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "STA IP: %s", s_ip_str);
        /* If the AP is currently up (either because we never had STA
         * credentials and started in APSTA, or because the user summoned it
         * via the LEFT+RIGHT touch hotkey), schedule a graceful 60 s shutdown so the
         * browser session on 192.168.4.1 has time to migrate to the
         * LAN-side IP / mDNS hostname.  When STA succeeded directly
         * (no AP up) there's nothing to close — skip. */
        if (s_ap_active) {
            ESP_LOGI(TAG, "AP will stop in 60 s (browser session can migrate to %s)", s_ip_str);
            if (s_ap_disable_timer) {
                esp_timer_stop(s_ap_disable_timer);
                esp_timer_start_once(s_ap_disable_timer, AP_DISABLE_DELAY_US);
            }
        }
        /* mDNS probe or re-announce on GOT_IP.
         *
         * ip_actually_changed = true  → new IP (boot, DHCP reassign, new
         *   network): call ENABLE_IP4 so the daemon probes for uniqueness and
         *   then announces the address.  .local resolvers update their caches.
         *
         * ip_actually_changed = false → reconnect on the same DHCP lease:
         *   call ANNOUNCE_IP4 to re-announce without re-probing.  The hostname
         *   claim hasn't changed, so a fresh probe cycle would be wasted
         *   multicast traffic.
         *
         * ANNOUNCE_IP4 is guarded by mdns_priv_if_ready() inside the daemon
         * and is a silent no-op when the interface has not been enabled yet.
         * Because s_last_mdns_ip starts at 0, the very first GOT_IP always
         * has ip_actually_changed = true → ENABLE_IP4 runs first, so the
         * interface is always enabled before ANNOUNCE_IP4 can fire.
         *
         * ev->ip_changed is NOT used: esp_netif_action_disconnected() clears
         * ip_info_old to 0.0.0.0 on every disconnect, making ev->ip_changed
         * always true — even for a same-IP reconnect.
         *
         * mdns_register_netif() is called once at startup (see wifi_manager_start)
         * so that get_if_from_netif() can locate s_sta_netif and these
         * mdns_netif_action() calls succeed. */
        if (s_mdns_on && s_sta_netif) {
            bool ip_actually_changed = (ev->ip_info.ip.addr != s_last_mdns_ip.addr);
            s_last_mdns_ip = ev->ip_info.ip;   /* update before action so re-entrant GOT_IP is safe */
            if (ip_actually_changed) {
                ESP_LOGI(TAG, "mDNS: new IP " IPSTR " → probe (ENABLE_IP4)",
                         IP2STR(&ev->ip_info.ip));
                esp_err_t e = mdns_netif_action(s_sta_netif, MDNS_EVENT_ENABLE_IP4);
                if (e != ESP_OK) ESP_LOGW(TAG, "mDNS ENABLE_IP4: %s", esp_err_to_name(e));
                else ESP_LOGI(TAG, "mDNS: probing → http://%s.local", s_hostname);
            } else {
                ESP_LOGI(TAG, "mDNS: same IP " IPSTR " → re-announce (ANNOUNCE_IP4)",
                         IP2STR(&ev->ip_info.ip));
                esp_err_t e = mdns_netif_action(s_sta_netif, MDNS_EVENT_ANNOUNCE_IP4);
                if (e != ESP_OK) ESP_LOGW(TAG, "mDNS ANNOUNCE_IP4: %s", esp_err_to_name(e));
                else ESP_LOGI(TAG, "mDNS: re-announced → http://%s.local", s_hostname);
            }
        }
    }
}

void wifi_manager_start(void)
{
    char ssid[64], password[64];
    bool mdns_on;
    config_lock();
    const nextube_config_t *cfg = config_get();
    strncpy(ssid,     cfg->ssid,     sizeof(ssid)     - 1); ssid[sizeof(ssid)         - 1] = '\0';
    strncpy(password, cfg->password, sizeof(password) - 1); password[sizeof(password) - 1] = '\0';
    /* Write into the module-level s_hostname buffer so netif->hostname
     * remains valid after this function returns (LWIP stores the pointer). */
    if (cfg->hostname[0] != '\0') {
        strncpy(s_hostname, cfg->hostname, sizeof(s_hostname) - 1);
    } else {
        strncpy(s_hostname, "nextube-remaster", sizeof(s_hostname) - 1);
    }
    s_hostname[sizeof(s_hostname) - 1] = '\0';
    mdns_on = cfg->mdns_enabled;
    config_unlock();

    s_wifi_events = xEventGroupCreate();

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    /* Set the correct hostname on both netifs BEFORE esp_wifi_start().
     * The DHCP client sends the hostname in DISCOVER/REQUEST packets which
     * go out before IP_EVENT_STA_GOT_IP fires.  Without this, all early
     * DHCP packets use the LWIP compile-time default "espressif" — Unifi
     * (and other controllers) log that as the device hostname, creating a
     * conflict with the mDNS announcement of "nextube-remaster" and causing
     * the name to oscillate every DHCP retry cycle (~15 s) until the lease
     * is fully established.  Setting it here ensures the very first DHCP
     * DISCOVER already carries the correct name. */
    esp_netif_set_hostname(s_sta_netif, s_hostname);
    esp_netif_set_hostname(s_ap_netif,  s_hostname);
    ESP_LOGI(TAG, "Netif hostname set to \"%s\" (STA + AP)", s_hostname);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    init_ap_timers();

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    s_mdns_on = mdns_on;   /* save for IP-event handler */
    if (mdns_on) {
        /* Initialise the mDNS daemon and register hostname + service BEFORE
         * WiFi starts so the records are ready when the first probe fires.
         * mdns_hostname_set() stores the hostname in the daemon without
         * probing (no active PCBs yet); probing begins later when
         * mdns_netif_action(ENABLE_IP4) is called from the IP handler.
         * s_last_mdns_ip deliberately starts at {0} so the first GOT_IP
         * always has ip_actually_changed=true → ENABLE_IP4 → mDNS interface
         * properly initialised on every boot.  Without this initial ENABLE_IP4
         * the daemon's PCB is never activated and mDNS would never announce. */
        mdns_init();
        mdns_hostname_set(s_hostname);
        mdns_instance_name_set("Nextube Remaster");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        /* Register the STA netif so mdns_netif_action() can find it.
         * With PREDEF_STA=n the daemon's s_esp_netifs[] table has no predefined
         * STA entry; without this call get_if_from_netif() returns
         * MDNS_MAX_INTERFACES (= invalid) and every mdns_netif_action()
         * returns ESP_ERR_INVALID_STATE.
         * In the 2025 refactored component mdns_register_netif() is a pure
         * registry operation — it stores the netif pointer in a free slot and
         * installs NO LWIP callbacks and NO esp-event handlers. */
        {
            esp_err_t reg_err = mdns_register_netif(s_sta_netif);
            if (reg_err != ESP_OK) {
                ESP_LOGE(TAG, "mDNS: mdns_register_netif failed (%s) — "
                         "mdns_netif_action will return INVALID_STATE",
                         esp_err_to_name(reg_err));
            } else {
                ESP_LOGI(TAG, "mDNS: STA netif registered (no LWIP callbacks)");
            }
        }
        ESP_LOGI(TAG, "mDNS initialised: http://%s.local (probe deferred until first IP)",
                 s_hostname);
    } else {
        ESP_LOGI(TAG, "mDNS disabled in config — not advertising");
    }

    /* load (or generate on first boot) the per-device WPA2 PIN. */
    ensure_ap_pin();

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = "Nextube-Setup",
            .max_connection = 4,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .channel        = 1,
        },
    };
    strncpy((char *)ap_cfg.ap.password, s_ap_pin,
            sizeof(ap_cfg.ap.password) - 1);

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid,     ssid,     sizeof(sta_cfg.sta.ssid)     - 1);
    strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1);

    bool have_creds = (strlen(ssid) > 0);

    /* Stage both configs in APSTA mode — this is purely config-time;
     * esp_wifi_start hasn't been called so no AP frames are emitted yet.
     * ESP-IDF requires the WiFi mode to include AP before
     * esp_wifi_set_config(WIFI_IF_AP, ...) will accept the call, so we
     * use APSTA as the staging mode regardless of final intent.  The
     * config persists across the subsequent set_mode(STA) demotion. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,  &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    /* Mode policy:
     *   No SSID configured  → APSTA from boot.  AP stays up indefinitely so
     *                          the user can configure WiFi via the web UI.
     *   SSID configured     → demote to STA only.  The setup AP is NOT
     *                          brought up automatically on failure; the user
     *                          summons it on demand with the LEFT+RIGHT touch
     *                          hotkey (→ wifi_manager_force_ap()).
     *
     * Avoids unnecessarily broadcasting "Nextube-Setup" — the setup AP only
     * appears on first boot or when the user explicitly requests it. */
    if (have_creds) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        s_ap_active = false;
        ESP_LOGI(TAG, "STA: connecting to \"%s\" (hold LEFT+RIGHT touch 15 s for setup AP)", ssid);
    } else {
        s_ap_active = true;
        ESP_LOGI(TAG, "No STA credentials — AP-only mode for first-boot setup");
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    /* WiFi power-save: modem-sleep (IDF default, matches stock firmware).
     *
     * We previously forced WIFI_PS_NONE to remove the periodic DTIM wake/sleep
     * tick.  But PS_NONE keeps the radio PA powered continuously (~120 mA
     * steady) instead of sleeping between DTIM beacons (~20–40 mA average) —
     * that continuous draw on the shared 3.3 V rail is itself a continuous
     * noise floor coupled into the always-on amplifier via its PSRR.  Ghidra
     * decompilation confirmed the stock firmware runs default modem-sleep and
     * is silent at idle, so we revert to WIFI_PS_MIN_MODEM to drop the
     * continuous rail current.  (Trade-off: a faint periodic DTIM tick may
     * return; A/B against the PS_NONE build to compare.) */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    ESP_LOGI(TAG, "WiFi started (PS=MIN_MODEM).  AP SSID: Nextube-Setup (WPA2) %s",
             have_creds ? "(staged, not broadcasting)" : "(broadcasting)");
}

void wifi_manager_reconnect_sta(void)
{
    char ssid[64], password[64];
    config_lock();
    const nextube_config_t *cfg = config_get();
    strncpy(ssid,     cfg->ssid,     sizeof(ssid)     - 1); ssid[sizeof(ssid)         - 1] = '\0';
    strncpy(password, cfg->password, sizeof(password) - 1); password[sizeof(password) - 1] = '\0';
    config_unlock();

    if (strlen(ssid) == 0) return;

    /* User just saved credentials and is most likely connected via the AP.
     * Force APSTA so they remain on the AP during the connection attempt;
     * if STA succeeds, IP_EVENT_STA_GOT_IP schedules the 60 s grace-period
     * AP shutdown.  If STA fails, the AP stays up — we're already
     * broadcasting. */
    s_ap_active = true;
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid,     ssid,     sizeof(sta_cfg.sta.ssid)     - 1);
    strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    /* Set the flag BEFORE calling esp_wifi_disconnect() so that if the
     * WIFI_EVENT_STA_DISCONNECTED event fires synchronously (or very quickly)
     * it sees s_manual_reconnect=true and skips its own esp_wifi_connect(). */
    s_manual_reconnect = true;

    /* esp_wifi_disconnect() stops any in-progress connection attempt.
     * On first-time credential save the STA was never connected and this
     * call returns ESP_ERR_WIFI_NOT_CONNECT without firing a disconnect
     * event — which is exactly why we call esp_wifi_connect() ourselves
     * rather than relying on the event handler to do it. */
    esp_wifi_disconnect();
    esp_wifi_connect();
    ESP_LOGI(TAG, "STA: connecting to \"%s\"", ssid);
}

bool wifi_manager_is_connected(void)
{
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

const char *wifi_manager_get_ip(void) { return s_ip_str; }

void wifi_manager_apply_sta_credentials(void)
{
    char ssid[64], password[64];
    config_lock();
    const nextube_config_t *cfg = config_get();
    strncpy(ssid,     cfg->ssid,     sizeof(ssid)     - 1); ssid[sizeof(ssid)         - 1] = '\0';
    strncpy(password, cfg->password, sizeof(password) - 1); password[sizeof(password) - 1] = '\0';
    config_unlock();

    if (strlen(ssid) == 0) return;
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid,     ssid,     sizeof(sta_cfg.sta.ssid)     - 1);
    strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    ESP_LOGI(TAG, "STA credentials updated (no reconnect)");
}

void wifi_manager_scan_start(void)
{
    wifi_scan_config_t scan = { .show_hidden = true };
    esp_wifi_scan_start(&scan, false);
}
