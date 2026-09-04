#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Renders `text` into the e-paper framebuffer with a 3x-scaled, faux-bold
 * 5x7 bitmap font, word-wrapped to the 200px panel width (also breaks on
 * '\n', and hard-wraps a word longer than one line) and centred on both
 * axes. Anything past the bottom of the panel is clipped (and logged).
 * Unsupported bytes render as '?'.
 *
 * Does not touch the panel -- the caller wraps this in
 * epd_lock()/epd_display()/epd_unlock() the same way qr_code_wallet's
 * render path does. Calls epd_clear() itself, so it always starts from a
 * blank page.
 *
 * If the loop indicator is enabled (see below), a small lemniscate
 * ("infinity") mark is stamped into the top-left corner as well. */
void text_display_show_message(const char *text);

/* Enables/disables the top-left "infinite loop" mark drawn by every
 * subsequent text_display_show_message(). Off until set. The caller flips
 * this whenever the audio "loop" setting changes so the panel and the
 * setting stay in agreement. */
void text_display_set_loop_indicator(bool on);

#ifdef __cplusplus
}
#endif
