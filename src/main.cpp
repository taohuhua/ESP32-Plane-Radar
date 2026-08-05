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

// External parameter defined in wifi_setup.cpp
// 0 = Cycle Distance Range, 1 = Cycle Location Profile
extern uint8_t g_bootButtonMode;

// Definition of the global ProfileManager instance
ProfileManager g_profileManager;

namespace {

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_adsb_fetch_ms = 0;

void syncLocationFromActiveProfile() {
  LocationProfile* prof = g_profileManager.getActiveProfile();
  if (prof) {
    services::location::set(prof->lat, prof->lon, prof->name);
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

  if (g_bootButtonMode == 0) {
    // Mode 0: Cycle Distance / Range
    ui::radar::rangeNext();
    Serial.println("[Button] Switched Radar Range");
  } else {
    // Mode 1: Cycle Location Profile (Skips profiles with Lat/Lon at 0.0)
    int totalProfiles = g_profileManager.getProfileCount();
    if (totalProfiles > 0) {
      int startIndex = g_profileManager.getActiveIndex();
      int nextIdx = startIndex;

      for (int i = 0; i < totalProfiles; ++i) {
        g_profileManager.nextProfile();
        LocationProfile* prof = g_profileManager.getActiveProfile();
        
        // Stop if we find valid non-zero coordinates
        if (prof && (prof->lat != 0.0f || prof->lon != 0.0f)) {
          break;
        }
      }

      syncLocationFromActiveProfile();

      // Refresh aircraft data immediately for new location coordinates
      if (g_radar_visible) {
        fetchAndDrawAircraft();
      }
      Serial.println("[Button] Switched Location Profile");
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Plane Radar");

  bootButtonInit();
  buttonHandlerInit();
  displayInit();
  
  // 1. Initialize Profile Manager before attempting Wi-Fi or portal screens
  g_profileManager.begin();

  // 2. Sync stored location coordinates into the location service
  services::location::init();
  syncLocationFromActiveProfile();

  // 3. Launch portal screen if configured/requested
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  
  ui::radar::rangeInit();
  services::adsb::setPollFn(wifiLoop);

  // 4. Connect using profile credentials
  if (wifiSetupConnect()) {
    showRadarIfConnected();
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