#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Number of message slots, editable from the /messages web page. The BOOT
 * button plays the currently-selected slot; a double-press advances the
 * selection to the next non-empty slot. */
#define MESSAGE_STORE_NUM_SLOTS 10

/* Hard per-slot cap. Enforced on save (message_store_save() clamps to it)
 * and mirrored into the /messages web form's maxlength. 25 chars fills two
 * centred lines of the 2x 5x7 panel font and plays in ~22-25 s of Morse. */
#define MESSAGE_MAX_LEN 25

/* Played/shown when the selected slot is empty (first boot / NVS erase). */
#define MESSAGE_DEFAULT "X"

/* nvs_flash_init() with erase-and-retry, plus a one-time migration of the
 * pre-slots single "message" key into slot 1. Call once at startup before
 * anything else touches NVS or Wi-Fi (esp_wifi_init() needs NVS ready). */
void message_store_init(void);

/* Copies slot `slot` (1..MESSAGE_STORE_NUM_SLOTS) into `out` (always
 * NUL-terminated, truncated to out_size-1). Returns true if that slot
 * holds a non-empty saved message; false otherwise (and `out` is set to
 * an empty string). */
bool message_store_load(int slot, char *out, size_t out_size);

/* Persists `text` (clamped to MESSAGE_MAX_LEN) to slot `slot`. An empty
 * string clears the slot. Content is not validated -- morse_player skips
 * characters it has no code for and text_display renders any printable
 * ASCII. Returns ESP_OK on success. */
esp_err_t message_store_save(int slot, const char *text);

/* Removes whatever is stored in slot `slot`. Returns ESP_OK on success
 * (also if the slot was already empty). */
esp_err_t message_store_clear(int slot);

/* The slot the BOOT button plays, 1..MESSAGE_STORE_NUM_SLOTS. Persisted
 * across reboots. Defaults to 1. */
int message_store_get_selected(void);

/* Sets and persists the selected slot (clamped to a valid range). */
void message_store_set_selected(int slot);

#ifdef __cplusplus
}
#endif
