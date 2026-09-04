#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Invoked (from a dedicated FreeRTOS task, not an ISR -- safe to do real
 * work like SPI transactions or blocking I2S writes in it) for a debounced
 * press of the BOOT button. */
typedef void (*boot_button_cb_t)(void);

/* Starts watching the BOOT button (GPIO0, per
 * ../photo_album/docs/waveshare-esp32-s3-epaper-1.54-reference.md) and
 * classifies each interaction as a single press, a double press, or a
 * long (~2s) hold:
 *
 *  - `on_single` fires ~350ms after a lone tap is released (the wait is
 *    how a second press is ruled out).
 *  - `on_double` fires immediately on the second press of a pair.
 *  - `on_long` fires once the button has been held ~2s, while it is still
 *    down; the eventual release is then swallowed (no single/double).
 *
 * Any may be NULL. The callback runs to completion before the next
 * press is looked at, so a long-running handler (e.g. playing Morse)
 * simply drops presses that land while it runs. Call once at startup. */
void boot_button_init(boot_button_cb_t on_single, boot_button_cb_t on_double,
                      boot_button_cb_t on_long);

#ifdef __cplusplus
}
#endif
