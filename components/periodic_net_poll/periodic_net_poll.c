#include "periodic_net_poll.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "net_poll";

/* See periodic_net_poll.h's file doc for why these three merged into one
 * task. Sizing: was 3 separate 6144 B stacks (18432 B total; measured
 * peaks weather 4384 B / subscribers 4112 B / update_check ~3820 B); this
 * task only ever runs one subsystem's tick_fn at a time, so its own peak
 * is bounded by whichever ONE is deepest, not their sum. 7168 leaves ~39%
 * margin over that single deepest peak — inherited reasoning from the
 * pre-merge tasks, not yet a real measurement of this task itself.
 * Revisit with this task's own high-water-mark log (logged at the bottom
 * of periodic_net_poll_task()) once it's run a full cycle of all three. */
#define NET_POLL_STACK_SIZE 7168

#define NET_POLL_MAX_ENTRIES 4

typedef struct {
    uint32_t (*tick_fn)(void);
    const char *name;
    uint32_t first_delay_ms;
    int64_t  next_due_us;   /* 0 = boot gate hasn't cleared yet; task or a
                              * late registration fills this in for real —
                              * see periodic_net_poll_register() below. */
} poll_entry_t;

static poll_entry_t      s_entries[NET_POLL_MAX_ENTRIES];
static int               s_entry_count   = 0;
static SemaphoreHandle_t s_wake_sem      = NULL;
static bool              s_task_started  = false;
/* 0 until the shared WiFi-connected + DNS-settle gate clears; read by
 * periodic_net_poll_register() to decide whether a (hypothetical) late
 * registration can compute its own first deadline immediately, or has to
 * wait for periodic_net_poll_task() to do it once the gate clears. In the
 * expected case — weather_start()/subscribers_start()/update_check_start()
 * all called synchronously within app_main(), which returns in well under a
 * second — every registration happens long before this is ever non-zero. */
static volatile int64_t  s_boot_gate_us  = 0;

static void periodic_net_poll_task(void *arg)
{
    ESP_LOGI(TAG, "waiting for WiFi...");
    while (!wifi_manager_is_connected())
        vTaskDelay(pdMS_TO_TICKS(1000));

    /* 8 s DNS-settle: weather_task and update_check_task each carried this
     * same delay independently before the merge — 3 s proved insufficient
     * on some routers (first geocoding/API lookup still hit EAI_AGAIN,
     * getaddrinfo code 202, because the lwIP resolver wasn't ready yet),
     * 8 s eliminated it in practice. (subscribers_task didn't explicitly
     * wait on WiFi at all before the merge — a flat 20 s from task-start
     * instead, relying on its own fetch functions' error handling if WiFi
     * wasn't up yet. All three now get this same, stricter guarantee.) */
    vTaskDelay(pdMS_TO_TICKS(8000));

    s_boot_gate_us = esp_timer_get_time();
    for (int i = 0; i < s_entry_count; i++)
        if (s_entries[i].next_due_us == 0)
            s_entries[i].next_due_us = s_boot_gate_us + (int64_t)s_entries[i].first_delay_ms * 1000;

    for (;;) {
        int64_t now     = esp_timer_get_time();
        int64_t soonest = now + 60LL * 1000000;   /* re-evaluate at least once a minute */
        for (int i = 0; i < s_entry_count; i++)
            if (s_entries[i].next_due_us < soonest) soonest = s_entries[i].next_due_us;

        int64_t wait_us = soonest - now;
        if (wait_us < 0) wait_us = 0;
        /* Times out at `soonest`, OR returns early via
         * periodic_net_poll_force()'s xSemaphoreGive() — either way we just
         * fall through and re-check every entry's own next_due_us below, so
         * a forced wake for one subsystem can't accidentally tick the
         * others early. */
        xSemaphoreTake(s_wake_sem, pdMS_TO_TICKS(wait_us / 1000));

        now = esp_timer_get_time();
        for (int i = 0; i < s_entry_count; i++) {
            if (now < s_entries[i].next_due_us) continue;
            uint32_t next_ms = s_entries[i].tick_fn();
            s_entries[i].next_due_us = esp_timer_get_time() + (int64_t)next_ms * 1000;
        }

        ESP_LOGD(TAG, "stack high-water mark: %u B unused (of %u allocated)",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)NET_POLL_STACK_SIZE);
    }
}

void periodic_net_poll_register(uint32_t (*tick_fn)(void), uint32_t first_delay_ms,
                                 const char *name)
{
    if (s_entry_count >= NET_POLL_MAX_ENTRIES) {
        ESP_LOGE(TAG, "%s: no room to register (max %d)", name, NET_POLL_MAX_ENTRIES);
        return;
    }
    int idx = s_entry_count++;
    s_entries[idx].tick_fn        = tick_fn;
    s_entries[idx].name           = name;
    s_entries[idx].first_delay_ms = first_delay_ms;
    /* Gate already cleared (a late registration, after boot) — compute our
     * own deadline right now instead of waiting for a task init loop that
     * already ran. Otherwise leave it 0; periodic_net_poll_task() fills
     * every still-0 entry in once the gate clears (the expected path). */
    s_entries[idx].next_due_us = s_boot_gate_us
        ? s_boot_gate_us + (int64_t)first_delay_ms * 1000
        : 0;

    if (!s_task_started) {
        s_task_started = true;   /* set before create: a second registration
                                   * arriving before xTaskCreate() returns
                                   * can't double-create the task */
        s_wake_sem = xSemaphoreCreateBinary();
        if (xTaskCreate(periodic_net_poll_task, "net_poll", NET_POLL_STACK_SIZE,
                        NULL, 3, NULL) != pdPASS)
            ESP_LOGE(TAG, "periodic_net_poll_task creation failed");
    }
    ESP_LOGI(TAG, "%s registered (first tick %lu ms after WiFi/DNS ready)",
             name, (unsigned long)first_delay_ms);
}

void periodic_net_poll_force(const char *name)
{
    for (int i = 0; i < s_entry_count; i++) {
        if (strcmp(s_entries[i].name, name) == 0) {
            s_entries[i].next_due_us = 0;
            break;
        }
    }
    if (s_wake_sem) xSemaphoreGive(s_wake_sem);
}
