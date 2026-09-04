#pragma once

/*
 * Compile-time defaults and accepted ranges for the runtime audio
 * settings (Morse speed, tone pitch, output volume). The defaults are
 * used the first time the device boots (or after an NVS erase); after
 * that the values are runtime-editable from the /audio web page -- see
 * audio_settings.h.
 */

#define AUDIO_DEFAULT_WPM      15
#define AUDIO_DEFAULT_TONE_HZ  600
#define AUDIO_DEFAULT_VOLUME   100
#define AUDIO_DEFAULT_LOOP     0   /* 0 = play once, 1 = repeat until BOOT */

/* Accepted ranges live in audio_settings.h (they are part of the API --
 * the web form validates against them). */
