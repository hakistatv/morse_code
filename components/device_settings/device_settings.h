#pragma once

#include "esp_err.h"

/*
 * Runtime-configurable settings (Wi-Fi AP SSID/password, mDNS hostname),
 * editable from the /settings web page (see web_server.h) and persisted in
 * NVS so they survive reboots. device_config.h supplies the defaults used
 * the first time the device boots (or after an NVS erase).
 *
 * Carried over from ../qr_code_wallet's device_settings, minus its
 * init_nvs() -- in this firmware message_store_init() already does
 * nvs_flash_init(). Call message_store_init() before load_runtime_config()
 * or save_runtime_config().
 */

#define SSID_MAX_LEN 32
#define PASS_MAX_LEN 64
#define HOST_MAX_LEN 32

extern char current_ssid[SSID_MAX_LEN + 1];
extern char current_pass[PASS_MAX_LEN + 1];
extern char current_hostname[HOST_MAX_LEN + 1];

/* Loads current_ssid/current_pass/current_hostname from NVS, falling back
 * to the device_config.h defaults for any value not yet saved. */
void load_runtime_config(void);

/* Persists new settings to NVS. Does not apply them or restart the device
 * -- callers (the /settings POST handler) are responsible for that. */
esp_err_t save_runtime_config(const char *ssid, const char *pass, const char *hostname);
