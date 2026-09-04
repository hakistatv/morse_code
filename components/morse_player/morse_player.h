#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up the onboard ES8311 codec + I2S TX path for tone playback on
 * the Waveshare ESP32-S3-ePaper-1.54 (V1). Call once at startup. Returns
 * ESP_OK on success; on failure the module stays disabled and
 * morse_player_play() becomes a logged no-op (check the boot log for the
 * step that failed -- usually a wiring/pin issue). Independent of the
 * e-paper: different pins, different bus. */
esp_err_t morse_player_init(void);

/* Queues `text` to be played through the speaker as International Morse --
 * an on/off-keyed `tone_hz` sine at `wpm` words per minute (PARIS timing)
 * -- on the internal player task, and returns immediately (does NOT block
 * for the ~seconds the pattern takes).
 *
 * With `loop` set, the message repeats (separated by a word gap) until
 * morse_player_stop() is called; otherwise it plays through once.
 *
 * Only A-Z, 0-9 and space are understood (case-insensitive); any other
 * character is skipped. Audio is synthesized and written one element at a
 * time, so message length is bounded only by MAX_TEXT_LEN, not by heap.
 * Only one message plays at a time: a call made while one is still playing
 * is rejected (not queued) with ESP_ERR_INVALID_STATE. `wpm` must be 8..60.
 *
 * Returns ESP_OK when the message was accepted for playback,
 * ESP_ERR_INVALID_STATE if the module isn't initialized or is already
 * playing, or ESP_ERR_INVALID_ARG for a nonsensical wpm/tone_hz or a
 * `text` with nothing playable in it. */
esp_err_t morse_player_play(const char *text, int wpm, int tone_hz, bool loop);

/* True from the moment morse_player_play() accepts a message until its
 * last element (and tail silence) has been written. */
bool morse_player_is_playing(void);

/* Asks the in-progress playback (if any) to stop. It ends at the next
 * element boundary -- within roughly one element time -- with the PA
 * dropped cleanly. A no-op if nothing is playing. */
void morse_player_stop(void);

/* Sets the ES8311 master output volume, 0..100 (clamped). The codec is
 * closed between plays, so this is not pushed to hardware immediately --
 * it takes effect on the next morse_player_play(). */
void morse_player_set_volume(int pct);

#ifdef __cplusplus
}
#endif
