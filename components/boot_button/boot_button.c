#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "boot_button.h"

static const char *TAG = "boot_button";

/* BOOT button, per
 * ../photo_album/docs/waveshare-esp32-s3-epaper-1.54-reference.md --
 * active-low (pulled up on-board; reads 0 when pressed). It's also the
 * ESP32-S3's download-mode strapping pin, but that only matters during
 * reset/power-on -- safe to read as a plain GPIO input once the app is
 * running. */
#define BOOT_BUTTON_PIN GPIO_NUM_0

#define POLL_INTERVAL_MS 20
#define DEBOUNCE_POLLS   3   /* ~60ms of a stable level before a transition counts */
#define DOUBLE_GAP_POLLS 18  /* ~350ms window for the 2nd press of a double */
#define LONG_PRESS_MS    2000
#define LONG_PRESS_POLLS (LONG_PRESS_MS / POLL_INTERVAL_MS) /* held this long -> long-press */

static boot_button_cb_t s_on_single;
static boot_button_cb_t s_on_double;
static boot_button_cb_t s_on_long;

/* Poll-and-debounce loop (no interrupt/ISR) -- the button is pressed at
 * most a few times a minute, so polling every 20ms costs nothing and
 * keeps this in line with the rest of the project's hand-rolled drivers
 * (see epd_1in54). Debounces the level, then classifies each interaction:
 *
 *  - held LONG_PRESS_POLLS without release   -> `s_on_long`, fired once
 *    while still down; the release is then swallowed.
 *  - a lone tap                              -> `s_on_single`, fired
 *    DOUBLE_GAP_POLLS after release with no 2nd press.
 *  - a 2nd press within that window          -> `s_on_double`, immediately.
 *
 * The single-press decision is deferred to *release* (not the press
 * edge) so a long hold doesn't also register as a tap on its way past
 * the ~350ms mark. */
static void boot_button_task(void *arg) {
    (void)arg;
    bool pressed = false;
    int stable_count = 0;
    int held_polls = 0;        /* consecutive polls the button has been down */
    bool consumed = false;     /* this hold already fired long/double -- no tap on release */
    bool long_done = false;    /* long-press already fired for the current hold */
    int click_pending = 0;     /* >0: polls left in the double-press window after a release */

    while (1) {
        bool is_low = gpio_get_level(BOOT_BUTTON_PIN) == 0;
        bool press_event = false;
        bool release_event = false;

        if (is_low == pressed) {
            stable_count = 0; /* matches what we already believe -- nothing changing */
        } else if (++stable_count >= DEBOUNCE_POLLS) {
            pressed = is_low;
            stable_count = 0;
            press_event = pressed;
            release_event = !pressed;
        }

        if (press_event) {
            held_polls = 0;
            long_done = false;
            if (click_pending > 0) { /* 2nd press of a pair */
                click_pending = 0;
                consumed = true; /* neither this press nor a long hold of it counts as a tap */
                long_done = true;
                ESP_LOGI(TAG, "BOOT double-press");
                if (s_on_double) {
                    s_on_double();
                }
            } else {
                consumed = false;
            }
        } else if (release_event) {
            if (!consumed) {
                click_pending = DOUBLE_GAP_POLLS;
            }
            consumed = false;
            held_polls = 0;
        } else if (pressed) {
            if (!long_done && ++held_polls >= LONG_PRESS_POLLS) {
                long_done = true;
                consumed = true; /* swallow the release */
                ESP_LOGI(TAG, "BOOT long-press (%dms hold)", LONG_PRESS_MS);
                if (s_on_long) {
                    s_on_long();
                }
            }
        } else if (click_pending > 0 && --click_pending == 0) {
            ESP_LOGI(TAG, "BOOT press");
            if (s_on_single) {
                s_on_single();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

void boot_button_init(boot_button_cb_t on_single, boot_button_cb_t on_double,
                      boot_button_cb_t on_long) {
    s_on_single = on_single;
    s_on_double = on_double;
    s_on_long = on_long;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&io));

    /* 6144 bytes: the single-press handler hands off to morse_player (PCM
     * scratch is on the heap), and the double-press / long-press handlers
     * re-render the e-paper via text_display + a full SPI refresh. */
    xTaskCreate(boot_button_task, "boot_button", 6144, NULL, 5, NULL);
    ESP_LOGI(TAG, "Watching BOOT button (GPIO%d): press plays, double-press cycles messages, "
                  "%dms hold toggles infinite loop", BOOT_BUTTON_PIN, LONG_PRESS_MS);
}
