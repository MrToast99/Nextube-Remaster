#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/**
 * Initialise the audio subsystem.
 *
 * @param enabled  Pass the value of cfg->audio_enabled read from config at
 *                 boot.  When false the DAC is never started — GPIO25 is
 *                 driven OUTPUT LOW instead, which clamps the amplifier's
 *                 AC-coupled input at 0 V (same low-impedance effect as the
 *                 DAC continuous driver outputting level 0) without touching
 *                 the I²S0 peripheral, APLL, or any DMA heap.  The mutex is
 *                 still created so audio_set_enabled(true) can start the DAC
 *                 later from the web UI without a reboot.
 *                 When true the full DAC / DMA ring is brought up immediately.
 */
void audio_init(bool enabled);
void audio_play_file(const char *path);
void audio_set_volume(int vol);
void audio_set_enabled(bool enabled);   /* false = DAC ring at 0 V (clamped); true = bring DAC up */
void audio_stop(void);

/**
 * Inject a test signal on the DAC output for hardware diagnostics.
 * Stops any active playback and blocks audio_play_file() until mode "normal"
 * is set.  Thread-safe; intended to be called from the HTTP handler task.
 *
 *   mode "hiz"     – tear down DAC; GPIO25 = Hi-Z input (maximum isolation)
 *   mode "silence" – constant 128 (mid-rail, amplifier bias point)
 *   mode "dc"      – constant param_a (0–255); probe DC offsets / bias
 *   mode "tone"    – sine wave; param_a = freq Hz (1–4000),
 *                               param_b = amplitude (0–127)
 *   mode "normal"  – restore idle (silence DMA loop); clears test mode
 */
void audio_dac_test_set(const char *mode, int param_a, int param_b);

/** Restore normal idle operation — alias for audio_dac_test_set("normal",0,0). */
void audio_dac_test_stop(void);

#ifdef __cplusplus
}
#endif
