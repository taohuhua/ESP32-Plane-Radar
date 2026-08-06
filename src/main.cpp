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

// Draw a cool 360-degree radar sweep loading animation
void showRadarSweepLoading(const char* labelStr) {
  // Center coordinates and radius tuned for the round GC9A01 (240x240)
  const int16_t cx = 120;
  const int16_t cy = 120;
  const int16_t r  = 45;

  // Background overlay box and outer radar grid lines
  tft.fillRect(cx - r - 15, cy - r - 20, (r * 2) + 30, (r * 2) + 40, TFT_BLACK);
  tft.drawCircle(cx, cy, r, TFT_DARKGREEN);
  tft.drawCircle(cx, cy, r / 2, TFT_DARKGREEN);
  tft.drawFastHLine(cx - r, cy, r * 2, TFT_DARKGREEN);
  tft.drawFastVLine(cx, cy - r, r * 2, TFT_DARKGREEN);

  // Label text above the radar circle (LovyanGFX modern font API)
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setFont(&fonts::Font2);
  tft.drawString(labelStr, cx, cy - r - 16);

  // Animate two full sweep rotations
  for (int step = 0; step < 24; ++step) {
    float angleRad = (step * 30.0f) * (3.14159f / 180.0f);
    int16_t xLine = cx + static_cast<int16_t>(r * cos(angleRad));
    int16_t yLine = cy + static_cast<int16_t>(r * sin(angleRad));

    // Draw active sweep line
    tft.drawLine(cx, cy, xLine, yLine, TFT_GREEN);

    // Draw trailing dim line
    float prevRad = ((step - 1) * 30.0f) * (3.14159f / 180.0f);
    int16_t xPrev = cx + static_cast<int16_t>(r * cos(prevRad));
    int16_t yPrev = cy + static_cast<int16_t>(r * sin(prevRad));
    tft.drawLine(cx, cy, xPrev, yPrev, TFT_DARKGREEN);

    delay(15);

    // Clear active line back to dark grid format
    tft.drawLine(cx, cy, xLine, yLine, TFT_BLACK);
    tft.drawCircle(cx, cy, r, TFT_DARKGREEN);
    tft.drawCircle(cx, cy, r / 2, TFT_DARKGREEN);
    tft.drawFastHLine(cx - r, cy, r * 2, TFT_DARKGREEN);
    tft.drawFastVLine(cx, cy - r, r * 2, TFT_DARKGREEN);
  }
}

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
    // Checkbox UNCHECKED: Cycle Distance / Range
    showRadarSweepLoading("CHANGING RANGE");

    ui::radar::rangeNext();
    Serial.println("[Button] Switched Radar Range");

    if (g_radar_visible) {
      ui::radarDisplayDraw();
      fetchAndDrawAircraft();
    }
  } else {
    // Checkbox CHECKED: Cycle Location Profile (Skips profiles with Lat/Lon at 0.0)
    int totalProfiles = g_profileManager.getProfileCount();
    if (totalProfiles > 0) {
      for (int i = 0; i < totalProfiles; ++i) {
        g_profileManager.nextProfile();
        LocationProfile* prof = g_profileManager.getActiveProfile();

        // Stop if valid non-zero coordinates are found
        if (prof && (prof->lat != 0.0f || prof->lon != 0.0f)) {
          break;
        }
      }

      LocationProfile* newProf = g_profileManager.getActiveProfile();
      char msgBuf[32];
      snprintf(msgBuf, sizeof(msgBuf), "LOC: %s", newProf ? newProf->name : "NEW");

      // Play sweep animation with target location name
      showRadarSweepLoading(msgBuf);

      syncLocationFromActiveProfile();

      // Refresh radar aircraft view for new location
      if (g_radar_visible) {
        ui::radarDisplayDraw();
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