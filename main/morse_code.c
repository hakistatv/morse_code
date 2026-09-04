/*
 * Shows a text message on the Waveshare ESP32-S3-ePaper-1.54 (V1) board's
 * e-paper panel and plays it in Morse code through the speaker: a keyed
 * sine tone whose speed, pitch, volume and infinite-loop mode are
 * configurable from the /audio web page (defaults 15 wpm / 600 Hz /
 * 100% / loop off, persisted in NVS via audio_settings).
 *
 *  - Single BOOT press: play the currently-selected message slot (or stop
 *    playback if it is already running). With "Infinite loop" set, it
 *    repeats until the next press.
 *  - Double BOOT press: advance the selection to the next non-empty slot
 *    and redraw the panel with it (no playback).
 *  - Long BOOT press (~2s hold): toggle "Infinite loop" and redraw -- the
 *    top-left corner mark shows when it is on.
 *
 * Ten message slots are stored in NVS (message_store) and editable from a
 * phone over a Wi-Fi access point + web form (wifi_ap + web_server) -- the
 * same pattern as ../qr_code_wallet, down to its Home / Settings pages:
 * the AP SSID/password and the mDNS device name are runtime-configurable
 * too (device_settings + mdns_service). Slot 1 defaults to "X".
 *
 * Same board and driver style as the sibling projects ../qr_code_wallet
 * and ../photo_album; see
 * ../photo_album/docs/waveshare-esp32-s3-epaper-1.54-reference.md for the
 * board's pin map and onboard peripherals.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"

#include "epd.h"
#include "text_display.h"
#include "morse_player.h"
#include "boot_button.h"
#include "message_store.h"
#include "device_settings.h"
#include "audio_settings.h"
#include "wifi_ap.h"
#include "mdns_service.h"
#include "web_server.h"

static const char *TAG = "morse_code";

/* Copies the selected slot's message into `out`, or MESSAGE_DEFAULT if
 * that slot is empty. */
static void load_selected(char *out, size_t out_size) {
    int sel = message_store_get_selected();
    if (!message_store_load(sel, out, out_size)) {
        strlcpy(out, MESSAGE_DEFAULT, out_size);
    }
}

/* Locked render of `text` onto the panel -- the web server's save/clear
 * handlers also drive the framebuffer/panel from their own task, and the
 * two must not interleave. */
static void show_on_panel(const char *text) {
    epd_lock();
    text_display_show_message(text);
    epd_display();
    epd_unlock();
}

/* Point text_display's corner "infinite loop" mark at the current setting.
 * Call after every change to current_loop so the next redraw agrees with
 * NVS. */
static void sync_loop_indicator(void) {
    text_display_set_loop_indicator(current_loop != 0);
}

/* Single press: if Morse is playing, stop it; otherwise play the selected
 * slot -- looping until the next press if "Infinite loop" is set in
 * /audio. Re-reads NVS each press (cheap, and the web server can change
 * the selection/contents between presses). morse_player_play() is
 * non-blocking, so the boot_button task stays responsive and can catch
 * the stop press. */
static void on_boot_single(void) {
    if (morse_player_is_playing()) {
        ESP_LOGI(TAG, "BOOT press -- stopping playback");
        morse_player_stop();
        return;
    }
    char msg[MESSAGE_MAX_LEN + 1];
    load_selected(msg, sizeof(msg));
    ESP_LOGI(TAG, "BOOT press -- playing slot %d: \"%s\" (%d wpm, %d Hz%s)",
             message_store_get_selected(), msg, current_wpm, current_tone_hz,
             current_loop ? ", loop" : "");
    morse_player_play(msg, current_wpm, current_tone_hz, current_loop != 0);
}

/* Double press: advance to the next non-empty slot after the current one
 * (wrapping), persist the new selection, and redraw the panel. Also stops
 * any playback first, so cycling never leaves the old slot's tone playing
 * under a new panel. No-op if no slot holds a message; redraws in place if
 * only the current one does. */
static void on_boot_double(void) {
    morse_player_stop(); /* no-op if idle */

    int sel = message_store_get_selected();
    for (int i = 1; i <= MESSAGE_STORE_NUM_SLOTS; i++) {
        int slot = (sel + i - 1) % MESSAGE_STORE_NUM_SLOTS + 1;
        char msg[MESSAGE_MAX_LEN + 1];
        if (!message_store_load(slot, msg, sizeof(msg))) {
            continue;
        }
        message_store_set_selected(slot);
        show_on_panel(msg);
        ESP_LOGI(TAG, "BOOT double-press -- selected slot %d: \"%s\"", slot, msg);
        return;
    }
    ESP_LOGI(TAG, "BOOT double-press -- no stored messages to cycle to");
}

/* Long press (~2s hold): flip the "Infinite loop" audio setting, persist
 * it (the other three audio knobs keep their saved values), and redraw
 * the selected message so the corner loop mark appears or clears. Does
 * not disturb playback that is already running -- it takes effect on the
 * next BOOT press. */
static void on_boot_long(void) {
    int next = current_loop ? 0 : 1;
    esp_err_t err = audio_settings_save(current_wpm, current_tone_hz, current_volume, next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BOOT long-press -- could not toggle Infinite loop: %s", esp_err_to_name(err));
        return;
    }
    sync_loop_indicator();
    ESP_LOGI(TAG, "BOOT long-press -- Infinite loop %s", current_loop ? "ON" : "OFF");

    char msg[MESSAGE_MAX_LEN + 1];
    load_selected(msg, sizeof(msg));
    show_on_panel(msg);
}

void app_main(void) {
    ESP_LOGI(TAG, "morse_code starting up");

    message_store_init(); /* also does nvs_flash_init(), needed before Wi-Fi */
    load_runtime_config(); /* Wi-Fi SSID/pass + mDNS hostname from NVS */
    audio_settings_load(); /* WPM / tone / volume / infinite-loop from NVS */
    sync_loop_indicator(); /* so the very first render shows the loop mark if it's on */

    char msg[MESSAGE_MAX_LEN + 1];
    load_selected(msg, sizeof(msg));

    epd_power_init();
    epd_power_on();
    vTaskDelay(pdMS_TO_TICKS(10)); /* let the panel rail settle */
    epd_init();
    epd_clear();
    show_on_panel(msg);
    ESP_LOGI(TAG, "Displayed slot %d: \"%s\"", message_store_get_selected(), msg);

    if (morse_player_init() != ESP_OK) {
        ESP_LOGE(TAG, "Audio init failed -- BOOT button will be inert. Check wiring/pin map.");
    }
    morse_player_set_volume(current_volume);

    wifi_ap_init();
    start_mdns();
    start_webserver();
    boot_button_init(on_boot_single, on_boot_double, on_boot_long);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
