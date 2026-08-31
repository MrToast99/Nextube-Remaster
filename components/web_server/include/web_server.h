#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void web_server_start(void);
void web_server_stop(void);
/* Number of currently active httpd client sockets (0..max_open_sockets), or
 * -1 if the server hasn't started yet. For periodic telemetry (main.c) --
 * lru_purge_enable stops connections from exhausting the pool permanently,
 * but doesn't by itself show whether they're still slowly accumulating over
 * long uptime; this makes that visible. */
int web_server_socket_count(void);
/* "Per-task stack log" checkbox (POST /api/debug/stacklog).
 * Runtime only, off by default. Polled once per cycle by main.c's
 * heap_telemetry_task() to gate the verbose per-task stack dump. */
bool web_server_debug_stacklog_enabled(void);
#ifdef __cplusplus
}
#endif
