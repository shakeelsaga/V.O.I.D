// INSTRUCTIONS: Rename this file to 'secrets.h' and generate your own secure keys.
#pragma once

#define SECRET_WIFI_SSID "USERNAME"
#define SECRET_WIFI_PASS "PASSWORD"
#define SECRET_MQTT_BROKER_IP "XXX.XXX.XXX.XXX"

// V2.0 END-TO-END ENCRYPTION KEYS (Dummy Data - Do not use in production)
// PMK (Primary Master Key) - Used to encrypt the LMK
static const uint8_t PMK_KEY[16] = {0x4F, 0x92, 0x1A, 0x3B, 0xE7, 0xD4, 0x5C, 0x88, 0x21, 0xA6, 0xB9, 0xF0, 0x33, 0xCC, 0x18, 0x5E};

// LMK (Local Master Key) - Used to encrypt the actual payload data
static const uint8_t LMK_KEY[16] = {0x72, 0xC4, 0x8A, 0x19, 0x55, 0xFE, 0x3D, 0x0B, 0x66, 0x91, 0xEA, 0x47, 0xD2, 0x8C, 0xF3, 0x05};