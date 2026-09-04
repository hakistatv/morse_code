#pragma once

/* Brings up Wi-Fi as a WPA2 access point using current_ssid/current_pass
 * (see device_settings.h) so a phone can reach the config pages directly,
 * no router needed. Call message_store_init() (which does nvs_flash_init(),
 * required by esp_wifi_init()) and load_runtime_config() before this, and
 * this before start_mdns() / start_webserver(). */
void wifi_ap_init(void);
