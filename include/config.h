#pragma once

#include <cstdint>
#include <cstddef>
#include <driver/gpio.h>

namespace config {

// --- Wi-Fi portal ---
constexpr char kPortalApName[] = "PlaneRadar-Setup";
constexpr char kPortalIp[] = "192.168.4.1";
/** mDNS host (no ".local" suffix); browser: http://plane-radar.local */
constexpr char kPortalHostname[] = "plane-radar";
constexpr char kPortalHostUrl[] = "plane-radar.local";

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiPortalTimeoutSec = 0;  // 0 = no timeout while configuring
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Wait after disconnect before reconnecting (avoids portal on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 4000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;

// --- Buttons & Hardware Controls ---
constexpr gpio_num_t kBootPin = GPIO_NUM_9;            // System BOOT pin (Flashing / Factory Reset)
constexpr gpio_num_t kLocationBtnPin = GPIO_NUM_2;     // Dedicated Location Button (Active LOW)
constexpr gpio_num_t kRadiusBtnPin   = GPIO_NUM_5;     // Dedicated Radius Button (Active LOW)

constexpr unsigned long kBootResetHoldMs = 3000UL;
/** Ignore button taps shorter than this (debounce). */
constexpr unsigned long kBootTapMinMs = 40UL;

enum class ButtonMode : uint8_t {
    CycleLocation = 0,
    CycleRadius   = 1
};

// --- Display: GC9A01 1.28" round 240×240 (SPI) ---
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_0;
constexpr gpio_num_t kDisplayPinCs  = GPIO_NUM_1;
constexpr gpio_num_t kDisplayPinDc  = GPIO_NUM_10;
constexpr gpio_num_t kDisplayPinMosi = GPIO_NUM_3;  // Display SDA
constexpr gpio_num_t kDisplayPinSclk = GPIO_NUM_4;  // Display SCL

constexpr int kDisplayWidth  = 240;
constexpr int kDisplayHeight = 240;

constexpr uint32_t kDisplaySpiWriteHz = 40000000;
// GC9A01 modules often need invert + BGR for correct black/green output
constexpr bool kDisplayInvert = true;
constexpr bool kDisplayRgbOrder = true;

// --- Multi-Location Configuration ---
constexpr size_t kMaxLocations = 5;

struct RadarLocation {
    char name[20];
    double lat;
    double lon;
};

// Default preset locations (Melbourne, Essendon, etc.)
constexpr RadarLocation kDefaultLocations[kMaxLocations] = {
    {"Melbourne", -37.81878, 144.95153},
    {"Essendon Airport", -37.7281, 144.9021},
    {"Tullamarine Airport", -37.6637, 144.8448},
    {"Avalon Airport", -38.0370, 144.4683},
    {"Location 5", 0.0000, 0.0000}
};

// --- Multi-SSID Profile Limits ---
constexpr size_t kMaxLocationProfiles = 5;

/** Poll adsb.fi (API public limit: 1 req/s). */
constexpr unsigned long kAdsbFetchIntervalMs = 3000;
/** Legacy scale unused — fetch uses radar::fetchRadiusKm() to screen edge. */
constexpr float kAdsbFetchRadiusScale = 1.0f;
/** false = hide aircraft with alt_baro "ground"; true = show them too. */
constexpr bool kAdsbShowGroundAircraft = false;

// --- UI colors (RGB565) — status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kTextOnYellow = kColorBlack;
constexpr uint16_t kTextOnBlack = 0xFFFF;

}  // namespace config