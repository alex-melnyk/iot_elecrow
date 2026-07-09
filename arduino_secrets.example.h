// arduino_secrets.example.h — TEMPLATE.
//
// Copy this file to `arduino_secrets.h` and fill in your own values:
//     cp arduino_secrets.example.h arduino_secrets.h
//
// arduino_secrets.h is git-ignored, so your real credentials never get committed.
#pragma once

#define SECRET_WIFI_SSID       "YOUR_WIFI_SSID"
#define SECRET_WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

#define SECRET_VICTRON_IP      "192.168.1.100"   // your Victron GX IP address
#define SECRET_VRM_ID          "xxxxxxxxxxxx"    // VRM Portal ID (Settings -> VRM online portal)
#define SECRET_VEBUS_INSTANCE  "275"             // vebus instance; verify with:
                                                 //   mosquitto_sub -h <GX_IP> -t 'N/<VRM_ID>/vebus/#' -v
// POSIX TZ for the on-screen clock. Examples:
//   Kyiv:   "EET-2EEST,M3.5.0/3,M10.5.0/4"   London: "GMT0BST,M3.5.0/1,M10.5.0"
//   New York: "EST5EDT,M3.2.0,M11.1.0"       LA:     "PST8PDT,M3.2.0,M11.1.0"
#define SECRET_TZ              "EET-2EEST,M3.5.0/3,M10.5.0/4"
