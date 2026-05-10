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

/* AP is disabled 60 seconds after STA gets an IP.  60 s gives the browser
 * enough time to finish loading the web UI on the AP-side IP before the
 * AP disappears (client then reconnects via the LAN IP / mDNS). */
#define AP_DISABLE_DELAY_US      (60LL * 1000 * 1000)   /* 60 seconds  */

/* AP fallback timeout.  When credentials ARE saved, the device starts in
 * STA-only mode (no AP broadcast).  If STA fails to associate AND obtain
 * an IP within this window, the AP comes up as a recovery path so the
 * user can re-configure.  The same timer is re-armed on any later
 * disconnect — a brief WiFi blip that recovers within the window stays
 * silent; an extended outage triggers the AP fallback automatically. */
#define AP_FALLBACK_TIMEOUT_US   (90LL * 1000 * 1000)   /* 90 seconds  */

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
static esp_timer_handle_t s_ap_fallback_timer  = NULL;

/* Set by wifi_manager_reconnect_sta() before it calls esp_wifi_connect()
 * directly.  Tells WIFI_EVENT_STA_DISCONNECTED NOT to fire a second
 * esp_wifi_connect() for that same reconnect attempt, which would create
 * two concurrent association requests and leave the TCP/IP stack in an
 * indeterminate state. */
static bool s_manual_reconnect = false;
static bool s_mdns_inited  = false; /* mdns_init() called (pre-WiFi-start) */
static bool s_mdns_started = false; /* netif registered — guard against double task spawn */

static void start_mdns(void);             /* defined after wifi_event_handler */
static void mdns_start_task(void *arg);   /* spawned from IP_EVENT_STA_GOT_IP */
static void mdns_unregister_ap_task(void *arg); /* spawned from WIFI_EVENT_AP_START */

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

/* AP fallback timer fired — STA hasn't obtained an IP within the window.
 * Bring the setup AP up so the user has a recovery path.  Stays up
 * indefinitely; closes only when STA eventually does get an IP (then
 * ap_disable_timer fires 60 s later). */
static void ap_fallback_cb(void *arg)
{
    if (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) return; /* made it in time */
    if (s_ap_active) return; /* already up */
    ESP_LOGW(TAG, "STA failed to obtain IP within %.0f s – enabling fallback AP",
             AP_FALLBACK_TIMEOUT_US / 1e6);
    s_ap_active = true;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
}

static void init_ap_timers(void)
{
    esp_timer_create_args_t a = { .callback = ap_disable_cb,  .name = "ap_disable"  };
    esp_timer_create(&a, &s_ap_disable_timer);
    esp_timer_create_args_t b = { .callback = ap_fallback_cb, .name = "ap_fallback" };
    esp_timer_create(&b, &s_ap_fallback_timer);
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
            /* (Re-)arm the fallback timer.  A brief blip that recovers
             * within the window stays silent (timer cancelled by IP_GOT
             * before it fires).  An extended outage triggers the AP
             * fallback automatically.  No-op if AP is already up. */
            if (!s_ap_active && s_ap_fallback_timer) {
                esp_timer_stop(s_ap_fallback_timer);
                esp_timer_start_once(s_ap_fallback_timer,
                                     AP_FALLBACK_TIMEOUT_US);
            }
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
        case WIFI_EVENT_AP_START:
            /* If mDNS is already running, the managed espressif/mdns component
             * may have auto-registered the AP netif on this event (its default
             * behaviour when CONFIG_MDNS_PREDEF_NETIF_AP is not honoured for
             * managed-component builds).  Spawn a task to unregister it so the
             * AP interface never leaks its hostname ("espressif") to LAN-side
             * mDNS resolvers.  The task is cheap — it runs, unregisters, then
             * deletes itself. */
            if (s_mdns_inited && s_ap_netif) {
                xTaskCreate(mdns_unregister_ap_task, "mdns_unreg_ap",
                            2048, NULL, 5, NULL);
            }
            break;
        default: break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        /* STA made it — cancel the AP-fallback timer if still running. */
        if (s_ap_fallback_timer) esp_timer_stop(s_ap_fallback_timer);
        /* If the AP is currently up (either because we never had STA
         * credentials and started in APSTA, or because the fallback
         * fired earlier), schedule a graceful 60 s shutdown so the
         * browser session on 192.168.4.1 has time to migrate to the
         * LAN-side IP / mDNS hostname.  When STA succeeded directly
         * (no AP up) there's nothing to close — skip. */
        if (s_ap_active) {
            ESP_LOGI(TAG, "STA got IP: %s – AP will stop in 60 s", s_ip_str);
            if (s_ap_disable_timer) {
                esp_timer_stop(s_ap_disable_timer);
                esp_timer_start_once(s_ap_disable_timer, AP_DISABLE_DELAY_US);
            }
        } else {
            ESP_LOGI(TAG, "STA got IP: %s", s_ip_str);
        }
        /* Register the STA netif with mDNS once we have a routable IP.
         * mdns_init() and mdns_hostname_set() were already called in
         * wifi_manager_start() while all netifs were DOWN; this deferred
         * task only does the register + service-add step.  It runs in a
         * task (not here) so mdns_unregister_netif(s_ap_netif) can be
         * called without deadlocking the default event loop. */
        if (!s_mdns_started && s_mdns_inited) {
            s_mdns_started = true;
            xTaskCreate(mdns_start_task, "mdns_reg", 3072, NULL, 5, NULL);
        }
    }
}

static void start_mdns(void)
{
    /* mdns_init(), mdns_hostname_set(), and mdns_instance_name_set() were
     * already called in wifi_manager_start() before esp_wifi_start(), while
     * all netifs were still DOWN.  This function only handles the registration
     * step, which requires the STA to have a valid IP first.
     *
     * Refresh s_hostname in case the user changed the hostname via the web UI
     * after the initial pre-init (requires a reboot to take effect in mDNS,
     * but the netif hostname is updated here for DHCP accuracy). */
    config_lock();
    const nextube_config_t *cfg = config_get();
    if (cfg->hostname[0] != '\0') {
        strncpy(s_hostname, cfg->hostname, sizeof(s_hostname) - 1);
        s_hostname[sizeof(s_hostname) - 1] = '\0';
    }
    config_unlock();

    esp_netif_set_hostname(s_sta_netif, s_hostname);

    /* Register only the STA netif.  ESP_ERR_INVALID_STATE = already registered. */
    if (s_sta_netif) {
        esp_err_t err = mdns_register_netif(s_sta_netif);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "mdns_register_netif STA: %s", esp_err_to_name(err));
        }
    }

    /* Unregister the AP netif.  The managed espressif/mdns component may have
     * auto-registered it via the predefined-netif mechanism if
     * CONFIG_MDNS_PREDEF_NETIF_AP was not honoured.  Safe to call here because
     * we are in mdns_start_task, not the default event loop task. */
    if (s_ap_netif) {
        esp_err_t err = mdns_unregister_netif(s_ap_netif);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND
                          && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "mdns_unregister_netif AP: %s", esp_err_to_name(err));
        }
    }

    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://%s.local  (STA netif registered)", s_hostname);
}

/* Deferred AP-netif de-registration.  Spawned from WIFI_EVENT_AP_START after
 * mDNS has been initialised so the AP interface never leaks its hostname to
 * the LAN.  Self-deletes when done. */
static void mdns_unregister_ap_task(void *arg)
{
    if (s_ap_netif) {
        esp_err_t err = mdns_unregister_netif(s_ap_netif);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "mDNS: AP netif unregistered");
        } else if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "mdns_unregister_netif AP: %s", esp_err_to_name(err));
        }
    }
    vTaskDelete(NULL);
}

/* Deferred mDNS initialisation.  Runs start_mdns() in a task context so
 * mdns_unregister_netif() inside start_mdns() is safe (no deadlock with the
 * default event loop task).  Self-deletes when done. */
static void mdns_start_task(void *arg)
{
    start_mdns();
    vTaskDelete(NULL);
}

void wifi_manager_start(void)
{
    char ssid[64], password[64];
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
    config_unlock();

    s_wifi_events = xEventGroupCreate();

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    /* Set the correct hostname on both netifs BEFORE esp_wifi_start().
     * The DHCP client sends the hostname in DISCOVER/REQUEST packets which
     * go out before IP_EVENT_STA_GOT_IP fires (and before start_mdns() is
     * called).  Without this, all early DHCP packets use the LWIP compile-
     * time default "espressif" — Unifi (and other controllers) log that as
     * the device hostname, creating a conflict with the mDNS announcement
     * of "nextube-remaster" and causing the name to oscillate every DHCP
     * retry cycle (~15 s) until the lease is fully established.
     * Setting it here ensures the very first DHCP DISCOVER already carries
     * the correct name. */
    esp_netif_set_hostname(s_sta_netif, s_hostname);
    esp_netif_set_hostname(s_ap_netif,  s_hostname);
    ESP_LOGI(TAG, "Netif hostname set to \"%s\" (STA + AP)", s_hostname);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    init_ap_timers();

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

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
     *   No SSID configured  → APSTA from boot.  AP stays up indefinitely
     *                          (no fallback timer needed) so the user
     *                          can configure WiFi via the captive web UI.
     *   SSID configured     → demote to STA only.  AP fallback timer
     *                          armed for AP_FALLBACK_TIMEOUT_US — if STA
     *                          doesn't obtain an IP in that window, AP
     *                          comes up as a recovery path (ap_fallback_cb).
     *
     * Avoids unnecessarily broadcasting "Nextube-Setup" every boot on a
     * working device — the setup AP only appears when actually needed
     * (first boot, or STA recovery after creds stop working). */
    if (have_creds) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        s_ap_active = false;
        ESP_LOGI(TAG, "STA: connecting to \"%s\" (AP fallback in %.0f s if no IP)",
                 ssid, AP_FALLBACK_TIMEOUT_US / 1e6);
        esp_timer_start_once(s_ap_fallback_timer, AP_FALLBACK_TIMEOUT_US);
    } else {
        s_ap_active = true;
        ESP_LOGI(TAG, "No STA credentials — AP-only mode for first-boot setup");
    }

    /* Pre-initialise mDNS while ALL netifs are still DOWN (before
     * esp_wifi_start fires any WIFI_EVENT_* that could trigger auto-
     * registration).  The managed espressif/mdns v1.11.x component probes
     * with its current internal hostname the moment a netif is registered;
     * if mdns_init() is called later (after the STA gets an IP) there is a
     * narrow window where it may probe with the compile-time default
     * "espressif" before mdns_hostname_set() takes effect.  Initialising
     * here and queuing hostname_set immediately afterwards guarantees the
     * mDNS task's very first probe — for any netif, via any code path —
     * already uses the correct name. */
    {
        bool mdns_on;
        config_lock();
        mdns_on = config_get()->mdns_enabled;
        config_unlock();
        if (mdns_on) {
            mdns_init();
            mdns_hostname_set(s_hostname);
            mdns_instance_name_set("Nextube Remaster");
            s_mdns_inited = true;
            ESP_LOGI(TAG, "mDNS pre-init: hostname \"%s\" (netif registration deferred to first IP)",
                     s_hostname);
        }
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi started.  AP SSID: Nextube-Setup (WPA2) %s",
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
     * AP shutdown.  If STA fails, the AP stays up (no fallback timer
     * needed — we're already broadcasting). */
    if (s_ap_fallback_timer) esp_timer_stop(s_ap_fallback_timer);
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
