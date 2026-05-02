#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void audio_init(void);
void audio_play_file(const char *path);
void audio_set_volume(int vol);
void audio_set_enabled(bool enabled);   /* false = DAC off (Hi-Z), zero noise floor */
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
