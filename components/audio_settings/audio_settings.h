#pragma once

#include "esp_err.h"

/*
 * Runtime-configurable audio settings -- Morse speed (WPM), sidetone
 * frequency (Hz) and output volume (%) -- editable from the /audio web
 * page and persisted in NVS so they survive reboots. audio_config.h
 * supplies the first-boot defaults and the accepted ranges.
 *
 * Same shape as device_settings.h. NVS must already be up, so call
 * message_store_init() before audio_settings_load() / audio_settings_save().
 */

/* Accepted / clamp ranges, also used by the /audio web form. WPM tops out
 * at 40 (a saner ceiling than morse_player's hard 8..60 guard); the tone
 * range sits inside its 100..4000 guard. */
#define AUDIO_WPM_MIN      8
#define AUDIO_WPM_MAX      40
#define AUDIO_TONE_MIN     300
#define AUDIO_TONE_MAX     1200
#define AUDIO_VOLUME_MIN   0
#define AUDIO_VOLUME_MAX   100

extern int current_wpm;
extern int current_tone_hz;
extern int current_volume;
extern int current_loop; /* 0 = play once, 1 = repeat playback until BOOT */

/* Loads current_wpm/current_tone_hz/current_volume/current_loop from NVS,
 * falling back to the audio_config.h defaults for any value not yet saved.
 * Numeric values are clamped to their AUDIO_*_MIN..MAX range; loop is
 * normalized to 0/1. */
void audio_settings_load(void);

/* Clamps/normalizes each argument, persists all four to NVS, and updates
 * the current_* globals. Does not push the volume to the codec -- the
 * caller applies it via morse_player_set_volume(). */
esp_err_t audio_settings_save(int wpm, int tone_hz, int volume, int loop);
