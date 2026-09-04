#include "esp_log.h"
#include "nvs.h"
#include "audio_config.h"
#include "audio_settings.h"

static const char *TAG = "audio_settings";

#define CFG_NVS_NAMESPACE "audio_cfg"
#define CFG_KEY_WPM       "wpm"
#define CFG_KEY_TONE      "tone_hz"
#define CFG_KEY_VOL       "vol"
#define CFG_KEY_LOOP      "loop"

int current_wpm;
int current_tone_hz;
int current_volume;
int current_loop;

static int clamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void audio_settings_load(void) {
    current_wpm = AUDIO_DEFAULT_WPM;
    current_tone_hz = AUDIO_DEFAULT_TONE_HZ;
    current_volume = AUDIO_DEFAULT_VOLUME;
    current_loop = AUDIO_DEFAULT_LOOP;

    nvs_handle_t nvs;
    if (nvs_open(CFG_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(nvs, CFG_KEY_WPM, &v) == ESP_OK)  current_wpm = v;
        if (nvs_get_i32(nvs, CFG_KEY_TONE, &v) == ESP_OK) current_tone_hz = v;
        if (nvs_get_i32(nvs, CFG_KEY_VOL, &v) == ESP_OK)  current_volume = v;
        if (nvs_get_i32(nvs, CFG_KEY_LOOP, &v) == ESP_OK) current_loop = v;
        nvs_close(nvs);
    } else {
        ESP_LOGI(TAG, "No saved audio settings in NVS yet -- using defaults");
    }

    current_wpm = clamp(current_wpm, AUDIO_WPM_MIN, AUDIO_WPM_MAX);
    current_tone_hz = clamp(current_tone_hz, AUDIO_TONE_MIN, AUDIO_TONE_MAX);
    current_volume = clamp(current_volume, AUDIO_VOLUME_MIN, AUDIO_VOLUME_MAX);
    current_loop = current_loop ? 1 : 0;

    ESP_LOGI(TAG, "Audio settings: %d wpm, %d Hz, volume %d%%, loop %s",
             current_wpm, current_tone_hz, current_volume, current_loop ? "on" : "off");
}

esp_err_t audio_settings_save(int wpm, int tone_hz, int volume, int loop) {
    wpm = clamp(wpm, AUDIO_WPM_MIN, AUDIO_WPM_MAX);
    tone_hz = clamp(tone_hz, AUDIO_TONE_MIN, AUDIO_TONE_MAX);
    volume = clamp(volume, AUDIO_VOLUME_MIN, AUDIO_VOLUME_MAX);
    loop = loop ? 1 : 0;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CFG_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_i32(nvs, CFG_KEY_WPM, wpm);
    if (err == ESP_OK) err = nvs_set_i32(nvs, CFG_KEY_TONE, tone_hz);
    if (err == ESP_OK) err = nvs_set_i32(nvs, CFG_KEY_VOL, volume);
    if (err == ESP_OK) err = nvs_set_i32(nvs, CFG_KEY_LOOP, loop);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        current_wpm = wpm;
        current_tone_hz = tone_hz;
        current_volume = volume;
        current_loop = loop;
        ESP_LOGI(TAG, "Saved audio settings: %d wpm, %d Hz, volume %d%%, loop %s",
                 wpm, tone_hz, volume, loop ? "on" : "off");
    } else {
        ESP_LOGE(TAG, "Failed to save audio settings: %s", esp_err_to_name(err));
    }
    return err;
}
