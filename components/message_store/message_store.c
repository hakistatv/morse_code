/*
 * NVS-backed store for the Morse messages this firmware plays.
 *
 * Same idea as qr_code_wallet's qr_store: persist short text strings and
 * regenerate everything downstream from them (here: the keyed audio and
 * the on-panel text), rather than storing rendered output. Ten numbered
 * slots ("msg1".."msg10"), plus "sel" -- the slot the BOOT button plays.
 */
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "message_store.h"

static const char *TAG = "message_store";

#define MSG_NVS_NAMESPACE "morse"
#define MSG_KEY_PREFIX    "msg"      /* "msg1".."msg10" */
#define SEL_NVS_KEY       "sel"
#define LEGACY_NVS_KEY    "message"  /* pre-slots single-message key */

/* "msg" + up to two digits + NUL. */
static void slot_key(int slot, char *buf, size_t buf_size) {
    snprintf(buf, buf_size, MSG_KEY_PREFIX "%d", slot);
}

static bool slot_valid(int slot) {
    return slot >= 1 && slot <= MESSAGE_STORE_NUM_SLOTS;
}

/* If the pre-slots "message" key is still around and slot 1 is empty, move
 * it into slot 1 so an already-configured device keeps its message. */
static void migrate_legacy_key(void) {
    nvs_handle_t nvs;
    if (nvs_open(MSG_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }

    /* Sized to the old (pre-cap) 120-char limit, not MESSAGE_MAX_LEN, so a
     * longer legacy value still reads back -- it's then truncated to the
     * current cap before being stored into slot 1. */
    char buf[128];
    size_t len = sizeof(buf);
    if (nvs_get_str(nvs, LEGACY_NVS_KEY, buf, &len) == ESP_OK) {
        buf[MESSAGE_MAX_LEN] = '\0'; /* clamp to the current per-slot cap */
        size_t slot1_len = 0;
        if (nvs_get_str(nvs, MSG_KEY_PREFIX "1", NULL, &slot1_len) == ESP_ERR_NVS_NOT_FOUND) {
            nvs_set_str(nvs, MSG_KEY_PREFIX "1", buf);
            ESP_LOGI(TAG, "Migrated legacy message into slot 1: \"%s\"", buf);
        }
        nvs_erase_key(nvs, LEGACY_NVS_KEY);
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}

void message_store_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    migrate_legacy_key();
}

bool message_store_load(int slot, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (!slot_valid(slot)) {
        return false;
    }

    char key[8];
    slot_key(slot, key, sizeof(key));

    nvs_handle_t nvs;
    if (nvs_open(MSG_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t len = out_size;
    esp_err_t err = nvs_get_str(nvs, key, out, &len);
    nvs_close(nvs);

    if (err != ESP_OK || out[0] == '\0') {
        out[0] = '\0';
        return false;
    }
    return true;
}

esp_err_t message_store_save(int slot, const char *text) {
    if (!slot_valid(slot) || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char buf[MESSAGE_MAX_LEN + 1];
    strlcpy(buf, text, sizeof(buf)); /* clamp to MESSAGE_MAX_LEN */

    char key[8];
    slot_key(slot, key, sizeof(key));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MSG_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    if (buf[0] == '\0') {
        err = nvs_erase_key(nvs, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_str(nvs, key, buf);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Slot %d %s", slot, buf[0] ? "saved" : "cleared");
    } else {
        ESP_LOGE(TAG, "Slot %d save failed: %s", slot, esp_err_to_name(err));
    }
    return err;
}

esp_err_t message_store_clear(int slot) {
    return message_store_save(slot, "");
}

int message_store_get_selected(void) {
    nvs_handle_t nvs;
    int32_t sel = 1;
    if (nvs_open(MSG_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_i32(nvs, SEL_NVS_KEY, &sel);
        nvs_close(nvs);
    }
    if (sel < 1 || sel > MESSAGE_STORE_NUM_SLOTS) {
        sel = 1;
    }
    return (int)sel;
}

void message_store_set_selected(int slot) {
    if (!slot_valid(slot)) {
        return;
    }
    nvs_handle_t nvs;
    if (nvs_open(MSG_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    if (nvs_set_i32(nvs, SEL_NVS_KEY, slot) == ESP_OK) {
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}
