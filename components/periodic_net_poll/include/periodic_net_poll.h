/**
 * @file periodic_net_poll.h
 * @brief Shared task for periodic, non-latency-sensitive network polling.
 *
 * weather_task, subscribers_task, and update_check_task used to be three
 * separate permanent FreeRTOS tasks, each doing the exact same shape of
 * work: sleep until due, do one HTTPS fetch, sleep again. All three
 * already serialise their real network work through the same tls_sem
 * mutex (see config_mgr.h), so they were never actually concurrent in the
 * way three separate stacks implies — they were just paying for three
 * separate stacks to run work that was already effectively sequential.
 * One shared task does the same job on a fraction of the permanent stack
 * cost, because it never runs more than one subsystem's tick_fn at a time
 * — see periodic_net_poll.c's NET_POLL_STACK_SIZE comment for the actual
 * numbers.
 *
 * Each subsystem keeps its own component, its own state, its own fetch/
 * parse logic, and its own public API (weather_get(), subscribers_get(),
 * update_check_get_status(), ...) completely unchanged — only the "own a
 * dedicated FreeRTOS task" part moved here. A subsystem's existing
 * _start() (still called the same way, still gated by the same
 * boot_X_enabled config flag in main.c) now registers a tick function
 * instead of calling xTaskCreate() directly.
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
