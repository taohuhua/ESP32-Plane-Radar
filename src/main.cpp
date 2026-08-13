/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "button_handler.h"
#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/profile_manager.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

// Definition of the global ProfileManager instance
ProfileManager g_profileManager;

namespace {

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_adsb_fetch_ms = 0;

void syncLocationFromActiveProfile() {
  // wifiSetupConnect() may have opened the config portal and changed the
  // active profile in between services::location::init() (step 2) and
  // here — re-sync the cached index so it matches ProfileManager. This
  // must NOT call services::location::set(), which WRITES the given
  // coordinates into whatever slot the cache currently points at: with a
  // stale cache that overwrites the wrong slot (see triggerLocationCycle()
  // in button_handler.cpp for the same bug, previously present here too).
  services::location::init();

  LocationProfile* prof = g_profileManager.getActiveProfile();
  if (prof) {
    Serial.printf("[Setup] Active Profile: %s (Lat: %.4f, Lon: %.4f)\n", 
                  prof->name, prof->lat, prof->lon);
  } else {
    Serial.println("[Setup] No profile found, using location defaults.");
  }
}

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  ui::radarDisplayDraw();
  g_radar_visible = true;
}

void fetchAndDrawAircraft() {
  const float fetch_km = ui::radar::fetchRadiusKm();
  if (!services::adsb::fetchUpdate(services::location::lat(),
                                   services::location::lon(), fetch_km)) {
    buttonHandlerPoll();
    return;
  }
  ui::radarDisplayRefreshAircraft();
  buttonHandlerPoll();
}

void handleBootButtonTap() {
  if (!bootButtonConsumeTap()) {
    return;
  }

  const uint8_t mode = getBootButtonMode();
  Serial.printf("[Button] BOOT tap. Mode: %d (%s)\n", mode,
                mode == 0 ? "range" : "location");

  // triggerRangeCycle()/triggerLocationCycle() already animate the sweep,
  // skip zero-coordinate location slots, redraw, and refetch aircraft —
  // same shared logic used by the dedicated GPIO2/GPIO5 buttons.
  if (mode == 0) {
    triggerRangeCycle();
  } else {
    triggerLocationCycle();
  }
}

}  // namespace

#include <Preferences.h>

void testNvsPersistence() {
  Preferences prefs;
  
  // 1. Open the "wifi" or "profile" namespace in read/write mode (false)
  if (!prefs.begin("wifi", false)) {
    Serial.println("[NVS TEST] FAILED: Could not open Preferences namespace!");
    return;
  }

  // 2. Read the previous boot count (defaulting to 0 if key doesn't exist)
  uint32_t bootCount = prefs.getUInt("boot_count", 0);
  bootCount++;

  // 3. Write updated count back to NVS
  prefs.putUInt("boot_count", bootCount);
  
  // 4. Always close the namespace to commit writes to flash
  prefs.end();

  Serial.printf("[NVS TEST] Success! Boot Count persistent value: %u\n", bootCount);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nPlane Radar Starting...");

  // 1. Initialize hardware peripherals and interrupt handlers first
  bootButtonInit();
  buttonHandlerInit();
  displayInit();
  ui::radar::rangeInit();

  // 2. Initialize persistent storage & location subsystem
  g_profileManager.begin();
  services::location::init();

  // 3. Register background WiFi processing loop for network calls
  services::adsb::setPollFn(wifiLoop);

  // 4. Connect to WiFi or launch setup portal
  // Note: wifiSetupConnect() manages its own status screens (connecting, portal, success/fail)
  if (wifiSetupConnect()) {
    // Disable modem sleep to prevent AP/hotspot disconnections
    WiFi.setSleep(WIFI_PS_NONE);
    WiFi.setAutoReconnect(true);

    // Sync radar range units & Active Profile location after connection stabilizes
    syncLocationFromActiveProfile();

    // 5. Render primary radar UI
    showRadarIfConnected();
  } else {
    Serial.println("[BOOT] WiFi setup failed or timed out.");
    // Handles failure state display (e.g., statusScreenConnectFailed)
  }
}

void loop() {
  buttonHandlerPoll();
  handleBootButtonTap();
  wifiLoop();

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    if (!g_radar_visible) {
      showRadarIfConnected();
    } else if (millis() - g_last_adsb_fetch_ms >= config::kAdsbFetchIntervalMs) {
      g_last_adsb_fetch_ms = millis();
      fetchAndDrawAircraft();
    }
  }

  delay(10);
}