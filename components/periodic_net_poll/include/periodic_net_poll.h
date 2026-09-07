/**
 * @file periodic_net_poll.h
 * @brief Shared task for periodic, non-latency-sensitive network polling.
 *
 * weather, subscribers, and update_check all do the same shape of work
 * (sleep until due, do one HTTPS fetch, sleep again) and already serialise
 * their real network work through the same tls_sem mutex, so three
 * separate task stacks were paying for concurrency that never existed.
 * One shared task does the job on a fraction of the stack cost.
 *
 * Each subsystem keeps its own component, state, fetch/parse logic, and
 * public API unchanged — only task ownership moved here. A subsystem's
 * _start() now registers a tick function instead of calling xTaskCreate()
 * directly.
 *
 * Also shares WiFi-outage handling: the main loop checks
 * wifi_manager_is_connected() before every dispatch round and skips
 * calling any tick_fn while it's down, instead of each subsystem hitting
 * the same connect failure and logging it separately. A tick_fn doesn't
 * need to know about this — it just wasn't called this round.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Registers a subsystem with the shared poll task, creating that task on
 * the very first registration (order across weather/subscribers/
 * update_check doesn't matter — each one's first tick is scheduled from a
 * shared WiFi-connected + DNS-settle gate, not from registration time).
 *
 * @param tick_fn        Does this subsystem's own "check if due, fetch if
 *                        so" work and returns how many milliseconds until
 *                        it wants to be ticked again — its own steady
 *                        interval, or a shorter value while backing off
 *                        after a failure. Entirely the subsystem's own
 *                        decision; the shared task just tracks the
 *                        deadline this returns and calls back at or after
 *                        it, same as each subsystem's own vTaskDelay() loop
 *                        used to.
 * @param first_delay_ms How long after the shared gate clears before this
 *                        subsystem's very first tick (0 = as soon as the
 *                        gate clears, matching weather's and
 *                        update_check's original "fetch immediately once
 *                        WiFi is up" behavior).
 * @param name           Used for logging and by periodic_net_poll_force().
 *                        Must outlive the registration (pass a string
 *                        literal) — not copied.
 */
void periodic_net_poll_register(uint32_t (*tick_fn)(void), uint32_t first_delay_ms,
                                 const char *name);

/**
 * Forces the named subsystem's next tick to run now instead of waiting out
 * its current interval, and wakes the shared task so it notices right
 * away. Used by subscribers_refresh_now(); a name that isn't currently
 * registered (that subsystem disabled this boot) is a harmless no-op.
 */
void periodic_net_poll_force(const char *name);

#ifdef __cplusplus
}
#endif
