#pragma once

#include "esp_http_server.h"

/* Starts the HTTP server on port 80. Pages: GET / (Home), GET /messages +
 * POST /messages + POST /messages/clear (edit the 10 Morse message slots,
 * persisted to NVS via message_store, panel redrawn when the selected
 * slot changes), GET /settings + POST /settings (Wi-Fi
 * SSID/password/device-name, persisted via device_settings, device
 * restarts), GET /style.css. Call wifi_ap_init() first. Returns the httpd
 * handle, or NULL on failure. */
httpd_handle_t start_webserver(void);
