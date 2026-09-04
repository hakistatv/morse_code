#pragma once

/*
 * Compile-time defaults, used the first time the device boots (or after an
 * NVS erase). SSID/password/hostname become runtime-editable from the
 * /settings web page after that -- see device_settings.h.
 */

/* --- Wi-Fi access point --- */
/* Same SSID/password as the sibling projects ../qr_code_wallet and
 * ../photo_album -- one board runs at a time, so they share the network
 * name; only the mDNS hostname below is per-project. */
#define WIFI_AP_SSID     "Hakista"
#define WIFI_AP_PASS     "hak1sta!" /* >= 8 chars, WPA2-PSK */
#define WIFI_AP_CHANNEL  1
#define WIFI_AP_MAX_CONN 4

/* --- mDNS --- */
/* Resolves as http://<MDNS_HOSTNAME>.local -- plain "hakista" (no .local)
 * isn't a valid browser hostname. Matches the AP SSID above; override
 * from the /settings page if two Hakista boards ever run at once. */
#define MDNS_HOSTNAME    "hakista"
