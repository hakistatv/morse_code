/*
 * Config web server for morse_code. Same page shape as ../qr_code_wallet's:
 *
 *   GET  /               Home -- links to Messages and Settings
 *   GET  /style.css      shared stylesheet
 *   GET  /messages       the 10 message slots, each with an edit field
 *   POST /messages       save one slot ("slot" + "message"); redraw if it's
 *                        the selected slot
 *   POST /messages/clear clear one slot ("slot")
 *   GET  /audio          WPM / tone frequency / volume / loop form
 *   POST /audio          persist to NVS (audio_settings); "test" plays PARIS
 *   GET  /settings       Wi-Fi SSID / password / device-name form
 *   POST /settings       persist to NVS (device_settings) and restart
 *
 * url_decode() and html_escape() are carried over verbatim from
 * ../qr_code_wallet's web_server.c.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "epd.h"
#include "message_store.h"
#include "text_display.h"
#include "device_settings.h"
#include "audio_settings.h"
#include "morse_player.h"
#include "web_server.h"

static const char *TAG = "web_server";

/* Pages under pages/, embedded NUL-terminated at build time
 * (EMBED_TXTFILES in CMakeLists.txt). home.html, restart.html and
 * style.css are sent as-is; settings.html is a printf-style template;
 * messages.html has a "{{SLOTS}}" marker the per-slot rows are streamed
 * into (chunked, so its CSS-free body still needs no format-string care). */
extern const char home_html_start[] asm("_binary_home_html_start");
extern const char messages_html_start[] asm("_binary_messages_html_start");
extern const char audio_html_start[] asm("_binary_audio_html_start");
extern const char settings_html_start[] asm("_binary_settings_html_start");
extern const char restart_html_start[] asm("_binary_restart_html_start");
extern const char style_css_start[] asm("_binary_style_css_start");

/* application/x-www-form-urlencoded body. 120 raw chars can triple under
 * %XX encoding -> ~370; 512 covers a "slot=NN&message=..." POST and also
 * the /settings body (ssid+password+hostname). */
#define MAX_BODY_LEN 512

#define SLOTS_MARKER     "{{SLOTS}}"
#define SLOTS_MARKER_LEN (sizeof(SLOTS_MARKER) - 1)

/* Decodes an x-www-form-urlencoded value in place: '+' -> space,
 * '%XX' -> byte. httpd_query_key_value() only splits the raw substring. */
static void url_decode(char *str) {
    char *src = str;
    char *dst = str;
    while (*src) {
        if (src[0] == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], 0 };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* Escapes a string for safe use inside an HTML attribute value.
 * Truncates rather than overflows if dst is too small. */
static void html_escape(const char *src, char *dst, size_t dst_size) {
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0'; si++) {
        const char *rep = NULL;
        switch (src[si]) {
            case '&': rep = "&amp;";  break;
            case '<': rep = "&lt;";   break;
            case '>': rep = "&gt;";   break;
            case '"': rep = "&quot;"; break;
            default: break;
        }
        size_t add_len = rep ? strlen(rep) : 1;
        if (di + add_len >= dst_size) {
            break;
        }
        if (rep) {
            memcpy(dst + di, rep, add_len);
        } else {
            dst[di] = src[si];
        }
        di += add_len;
    }
    dst[di] = '\0';
}

/* Reads the whole request body into buf (NUL-terminated). Returns false
 * (and sends a 400) if it's missing or larger than buf_size-1. */
static bool recv_body(httpd_req_t *req, char *buf, size_t buf_size) {
    if (req->content_len <= 0 || (size_t)req->content_len >= buf_size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Form data missing or too large");
        return false;
    }
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return false;
        }
        received += ret;
    }
    buf[received] = '\0';
    return true;
}

/* Locked render of `text` onto the panel -- the BOOT-button task also
 * drives the framebuffer/panel, and the two must not interleave. */
static void panel_show_text(const char *text) {
    epd_lock();
    text_display_show_message(text);
    epd_display();
    epd_unlock();
}

/* Keeps the panel in step after slot `changed_slot` was saved or cleared:
 * redraw if it's the selected slot; if the selected slot is now empty,
 * adopt the first non-empty slot (or show the default). Leaves the panel
 * alone when an unrelated slot changed. */
static void resync_panel(int changed_slot) {
    int sel = message_store_get_selected();
    char buf[MESSAGE_MAX_LEN + 1];

    if (message_store_load(sel, buf, sizeof(buf))) {
        if (changed_slot == sel) {
            panel_show_text(buf);
        }
        return;
    }
    for (int slot = 1; slot <= MESSAGE_STORE_NUM_SLOTS; slot++) {
        if (message_store_load(slot, buf, sizeof(buf))) {
            message_store_set_selected(slot);
            panel_show_text(buf);
            return;
        }
    }
    panel_show_text(MESSAGE_DEFAULT);
}

/* Pulls one integer query param (e.g. "?saved=3") out of the request.
 * Returns 0 if absent/invalid. */
static int query_int(httpd_req_t *req, const char *key) {
    char qs[64];
    size_t qs_len = httpd_req_get_url_query_len(req);
    if (qs_len == 0 || qs_len >= sizeof(qs)) {
        return 0;
    }
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK) {
        return 0;
    }
    char val[8];
    if (httpd_query_key_value(qs, key, val, sizeof(val)) != ESP_OK) {
        return 0;
    }
    return atoi(val);
}

/* Shared stylesheet -- one static file, so the browser caches it after
 * the first page load. */
static esp_err_t style_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/css");
    return httpd_resp_send(req, style_css_start, HTTPD_RESP_USE_STRLEN);
}

/* GET / -- Home: links to Messages and Settings. No substitution. */
static esp_err_t home_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, home_html_start, HTTPD_RESP_USE_STRLEN);
}

/* GET /messages -- streams the page: template up to {{SLOTS}}, an optional
 * status line, one row per slot, then the rest of the template. */
static esp_err_t messages_get_handler(httpd_req_t *req) {
    const char *tpl = messages_html_start;
    const char *marker = strstr(tpl, SLOTS_MARKER);
    int selected = message_store_get_selected();
    int saved = query_int(req, "saved");
    int cleared = query_int(req, "cleared");

    httpd_resp_set_type(req, "text/html");
    if (!marker) {
        return httpd_resp_send(req, tpl, HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_send_chunk(req, tpl, marker - tpl);

    char row[MESSAGE_MAX_LEN * 6 + 800];
    if (saved >= 1 && saved <= MESSAGE_STORE_NUM_SLOTS) {
        snprintf(row, sizeof(row), "<p class=\"status\">Slot %d saved.</p>", saved);
        httpd_resp_send_chunk(req, row, HTTPD_RESP_USE_STRLEN);
    } else if (cleared >= 1 && cleared <= MESSAGE_STORE_NUM_SLOTS) {
        snprintf(row, sizeof(row), "<p class=\"status\">Slot %d cleared.</p>", cleared);
        httpd_resp_send_chunk(req, row, HTTPD_RESP_USE_STRLEN);
    }

    for (int slot = 1; slot <= MESSAGE_STORE_NUM_SLOTS; slot++) {
        char msg[MESSAGE_MAX_LEN + 1];
        message_store_load(slot, msg, sizeof(msg));
        char msg_esc[MESSAGE_MAX_LEN * 6 + 1];
        html_escape(msg, msg_esc, sizeof(msg_esc));

        snprintf(row, sizeof(row),
                 "<form class=\"slot\" method=\"POST\" action=\"/messages\">"
                 "<input type=\"hidden\" name=\"slot\" value=\"%d\">"
                 "<label>Slot %d%s</label>"
                 "<input type=\"text\" name=\"message\" value=\"%s\" maxlength=\"%d\" autocomplete=\"off\">"
                 "<div class=\"slot-actions\">"
                 "<button type=\"submit\">Save</button>"
                 "<button type=\"submit\" formaction=\"/messages/clear\" class=\"clear-btn\">Clear</button>"
                 "</div></form>",
                 slot, slot,
                 slot == selected ? " <span class=\"cur\">&middot; playing now</span>" : "",
                 msg_esc, MESSAGE_MAX_LEN);
        httpd_resp_send_chunk(req, row, HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_send_chunk(req, marker + SLOTS_MARKER_LEN, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Shared by POST /messages and POST /messages/clear: pulls "slot" out of
 * the body and validates it. Returns 0 (after sending a 400) on failure. */
static int parse_slot(httpd_req_t *req, const char *body) {
    char slot_val[4] = {0};
    if (httpd_query_key_value(body, "slot", slot_val, sizeof(slot_val)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing slot field");
        return 0;
    }
    int slot = atoi(slot_val);
    if (slot < 1 || slot > MESSAGE_STORE_NUM_SLOTS) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid slot");
        return 0;
    }
    return slot;
}

/* POST /messages -- save one slot, then keep the panel in step. */
static esp_err_t messages_post_handler(httpd_req_t *req) {
    char body[MAX_BODY_LEN];
    if (!recv_body(req, body, sizeof(body))) {
        return ESP_FAIL;
    }

    int slot = parse_slot(req, body);
    if (slot == 0) {
        return ESP_FAIL;
    }

    /* Full-body sized so an over-long / heavily %XX-encoded value from a
     * non-browser client is still captured; message_store_save() clamps
     * the decoded string to MESSAGE_MAX_LEN. Absent field -> "" -> clears
     * the slot. */
    char msg[MAX_BODY_LEN] = {0};
    httpd_query_key_value(body, "message", msg, sizeof(msg));
    url_decode(msg);

    esp_err_t err = message_store_save(slot, msg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save message");
        return ESP_FAIL;
    }
    resync_panel(slot);
    ESP_LOGI(TAG, "Slot %d updated via web: \"%s\"", slot, msg);

    char loc[32];
    snprintf(loc, sizeof(loc), "/messages?saved=%d", slot);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", loc);
    return httpd_resp_send(req, NULL, 0);
}

/* POST /messages/clear -- empty one slot, then keep the panel in step. */
static esp_err_t messages_clear_handler(httpd_req_t *req) {
    char body[MAX_BODY_LEN];
    if (!recv_body(req, body, sizeof(body))) {
        return ESP_FAIL;
    }

    int slot = parse_slot(req, body);
    if (slot == 0) {
        return ESP_FAIL;
    }

    esp_err_t err = message_store_clear(slot);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to clear slot");
        return ESP_FAIL;
    }
    resync_panel(slot);
    ESP_LOGI(TAG, "Slot %d cleared via web", slot);

    char loc[32];
    snprintf(loc, sizeof(loc), "/messages?cleared=%d", slot);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", loc);
    return httpd_resp_send(req, NULL, 0);
}

/* GET /audio -- the WPM / tone / volume / loop form, prefilled with the
 * current values, plus a one-shot "Saved." banner after a redirect. */
static esp_err_t audio_get_handler(httpd_req_t *req) {
    char page[1536];
    int len = snprintf(page, sizeof(page), audio_html_start,
                       query_int(req, "saved") ? "<p class=\"status\">Saved.</p>" : "",
                       current_wpm, current_tone_hz, current_volume,
                       current_loop ? " checked" : "");
    if (len < 0 || (size_t)len >= sizeof(page)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Page too large");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, len);
}

/* POST /audio -- persist WPM / tone / volume / loop to NVS
 * (audio_settings), apply the volume, and if the "Save & test" button was
 * used, play PARIS once at the new settings. Redirects back to GET /audio. */
static esp_err_t audio_post_handler(httpd_req_t *req) {
    char body[MAX_BODY_LEN];
    if (!recv_body(req, body, sizeof(body))) {
        return ESP_FAIL;
    }

    char wpm_val[8] = {0}, tone_val[8] = {0}, vol_val[8] = {0};
    if (httpd_query_key_value(body, "wpm", wpm_val, sizeof(wpm_val)) != ESP_OK ||
        httpd_query_key_value(body, "tone", tone_val, sizeof(tone_val)) != ESP_OK ||
        httpd_query_key_value(body, "volume", vol_val, sizeof(vol_val)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing wpm/tone/volume field");
        return ESP_FAIL;
    }

    int wpm = atoi(wpm_val), tone = atoi(tone_val), volume = atoi(vol_val);
    if (wpm < AUDIO_WPM_MIN || wpm > AUDIO_WPM_MAX ||
        tone < AUDIO_TONE_MIN || tone > AUDIO_TONE_MAX ||
        volume < AUDIO_VOLUME_MIN || volume > AUDIO_VOLUME_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "wpm/tone/volume out of range");
        return ESP_FAIL;
    }

    /* Unchecked checkboxes are simply absent from the body. */
    char loop_val[4];
    int loop = httpd_query_key_value(body, "loop", loop_val, sizeof(loop_val)) == ESP_OK;

    if (audio_settings_save(wpm, tone, volume, loop) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save audio settings");
        return ESP_FAIL;
    }
    morse_player_set_volume(current_volume);
    /* Keep the panel's corner loop mark in step with the setting, the same
     * way the BOOT long-press does. */
    text_display_set_loop_indicator(current_loop != 0);
    resync_panel(message_store_get_selected());
    ESP_LOGI(TAG, "Audio settings updated via web: %d wpm, %d Hz, volume %d%%, loop %s",
             current_wpm, current_tone_hz, current_volume, current_loop ? "on" : "off");

    char test_val[4];
    if (httpd_query_key_value(body, "test", test_val, sizeof(test_val)) == ESP_OK) {
        /* Always a single play -- never loop the test tone. */
        if (morse_player_play("PARIS", current_wpm, current_tone_hz, false) != ESP_OK) {
            ESP_LOGW(TAG, "Test tone skipped -- player busy or unavailable");
        }
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/audio?saved=1");
    return httpd_resp_send(req, NULL, 0);
}

/* GET /settings -- current Wi-Fi SSID/hostname (the password is never
 * echoed back into the page source). */
static esp_err_t settings_get_handler(httpd_req_t *req) {
    char ssid_esc[SSID_MAX_LEN * 6 + 1];
    char host_esc[HOST_MAX_LEN * 6 + 1];
    html_escape(current_ssid, ssid_esc, sizeof(ssid_esc));
    html_escape(current_hostname, host_esc, sizeof(host_esc));

    char page[1536];
    int len = snprintf(page, sizeof(page), settings_html_start, ssid_esc, host_esc);
    if (len < 0 || (size_t)len >= sizeof(page)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Page too large");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, len);
}

/* Restarting from inside the handler that sent the response would race the
 * socket close, so hand it to a task that waits for the flush first. A
 * restart (rather than a live Wi-Fi reconfigure) is the simplest reliable
 * way to apply a new SSID/password -- same as ../qr_code_wallet. */
static void restart_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

/* POST /settings -- save a new Wi-Fi SSID/password/hostname to NVS (via
 * device_settings.h) and restart so they take effect. */
static esp_err_t settings_post_handler(httpd_req_t *req) {
    char body[MAX_BODY_LEN];
    if (!recv_body(req, body, sizeof(body))) {
        return ESP_FAIL;
    }

    char ssid_val[SSID_MAX_LEN + 1] = {0};
    char pass_val[PASS_MAX_LEN + 1] = {0};
    char host_val[HOST_MAX_LEN + 1] = {0};

    bool has_ssid = httpd_query_key_value(body, "ssid", ssid_val, sizeof(ssid_val)) == ESP_OK;
    bool has_pass = httpd_query_key_value(body, "password", pass_val, sizeof(pass_val)) == ESP_OK;
    bool has_host = httpd_query_key_value(body, "hostname", host_val, sizeof(host_val)) == ESP_OK;
    url_decode(ssid_val);
    url_decode(pass_val);
    url_decode(host_val);

    if (!has_ssid || strlen(ssid_val) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Wi-Fi network name is required");
        return ESP_FAIL;
    }
    if (!has_host || strlen(host_val) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Device name is required");
        return ESP_FAIL;
    }
    if (has_pass && strlen(pass_val) > 0 && strlen(pass_val) < 8) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Password must be at least 8 characters, or left blank to keep the current one");
        return ESP_FAIL;
    }

    const char *new_pass = (has_pass && strlen(pass_val) > 0) ? pass_val : current_pass;
    esp_err_t err = save_runtime_config(ssid_val, new_pass, host_val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save settings: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save settings");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, restart_html_start, HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "Settings saved (SSID=\"%s\" hostname=\"%s\") -- restarting", ssid_val, host_val);
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static const httpd_uri_t home_get_uri = {
    .uri = "/", .method = HTTP_GET, .handler = home_get_handler,
};
static const httpd_uri_t style_uri = {
    .uri = "/style.css", .method = HTTP_GET, .handler = style_get_handler,
};
static const httpd_uri_t messages_get_uri = {
    .uri = "/messages", .method = HTTP_GET, .handler = messages_get_handler,
};
static const httpd_uri_t messages_post_uri = {
    .uri = "/messages", .method = HTTP_POST, .handler = messages_post_handler,
};
static const httpd_uri_t messages_clear_uri = {
    .uri = "/messages/clear", .method = HTTP_POST, .handler = messages_clear_handler,
};
static const httpd_uri_t audio_get_uri = {
    .uri = "/audio", .method = HTTP_GET, .handler = audio_get_handler,
};
static const httpd_uri_t audio_post_uri = {
    .uri = "/audio", .method = HTTP_POST, .handler = audio_post_handler,
};
static const httpd_uri_t settings_get_uri = {
    .uri = "/settings", .method = HTTP_GET, .handler = settings_get_handler,
};
static const httpd_uri_t settings_post_uri = {
    .uri = "/settings", .method = HTTP_POST, .handler = settings_post_handler,
};

httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192; /* html_escape scratch + per-slot row buffer; modest */
    config.max_uri_handlers = 10;

    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return NULL;
    }

    httpd_register_uri_handler(server, &home_get_uri);
    httpd_register_uri_handler(server, &style_uri);
    httpd_register_uri_handler(server, &messages_get_uri);
    httpd_register_uri_handler(server, &messages_post_uri);
    httpd_register_uri_handler(server, &messages_clear_uri);
    httpd_register_uri_handler(server, &audio_get_uri);
    httpd_register_uri_handler(server, &audio_post_uri);
    httpd_register_uri_handler(server, &settings_get_uri);
    httpd_register_uri_handler(server, &settings_post_uri);

    ESP_LOGI(TAG, "Web server started -- browse to http://192.168.4.1/");
    return server;
}
