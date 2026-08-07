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

// Draw a full-screen radar sweep with a 2-stage dimming phosphorescent tail
void showRadarSweepLoading(const char* labelStr) {
  // Center coordinates and radius scaled for full 240x240 GC9A01 panel
  const int16_t cx = 120;
  const int16_t cy = 120;
  const int16_t r  = 120; 

  // Define custom 16-bit RGB565 colors for the multi-stage trailing tail
  const uint16_t COLOR_SWEEP_BEAM = TFT_GREEN;       // Bright active beam
  const uint16_t COLOR_TRAIL_MID  = TFT_DARKGREEN;   // Medium fade (step - 1)
  const uint16_t COLOR_TRAIL_DIM  = 0x0280;          // Very dark green for faint tail (step - 2)

  // Clear full display background
  tft.fillScreen(TFT_BLACK);

  // Outer edge and inner range rings
  tft.drawCircle(cx, cy, r - 1, TFT_DARKGREEN);
  tft.drawCircle(cx, cy, (r * 2) / 3, TFT_DARKGREEN);
  tft.drawCircle(cx, cy, r / 3, TFT_DARKGREEN);
  tft.drawFastHLine(0, cy, 240, TFT_DARKGREEN);
  tft.drawFastVLine(cx, 0, 240, TFT_DARKGREEN);

  // Text banner setup
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setFont(&fonts::Font2);
  
  const int16_t bannerY = 40;
  tft.fillRect(cx - 70, bannerY - 2, 140, 20, TFT_BLACK);
  tft.drawString(labelStr, cx, bannerY);

  constexpr int kTotalSteps = 24;
  constexpr int kStepDelayMs = 60; 
  constexpr float kStepAngleRad = (360.0f / kTotalSteps) * (3.14159f / 180.0f);

  for (int rotation = 0; rotation < 1; ++rotation) {
    for (int step = 0; step < kTotalSteps; ++step) {
      // 1. Primary active sweep beam (brightest green)
      float angleRad = step * kStepAngleRad;
      int16_t xLine = cx + static_cast<int16_t>(r * cos(angleRad));
      int16_t yLine = cy + static_cast<int16_t>(r * sin(angleRad));

      // 2. Primary tail line (medium green, 1 step behind)
      float prevRad1 = (step - 1) * kStepAngleRad;
      int16_t xPrev1 = cx + static_cast<int16_t>(r * cos(prevRad1));
      int16_t yPrev1 = cy + static_cast<int16_t>(r * sin(prevRad1));

      // 3. Secondary tail line (faint/dark green, 2 steps behind)
      float prevRad2 = (step - 2) * kStepAngleRad;
      int16_t xPrev2 = cx + static_cast<int16_t>(r * cos(prevRad2));
      int16_t yPrev2 = cy + static_cast<int16_t>(r * sin(prevRad2));

      // Draw active beam & multi-stage phosphor trails
      tft.drawLine(cx, cy, xLine, yLine, COLOR_SWEEP_BEAM);
      tft.drawLine(cx, cy, xPrev1, yPrev1, COLOR_TRAIL_MID);
      tft.drawLine(cx, cy, xPrev2, yPrev2, COLOR_TRAIL_DIM);

      delay(kStepDelayMs);

      // Clear the trailing tip (step - 2 line) back to black before redrawing grid
      tft.drawLine(cx, cy, xPrev2, yPrev2, TFT_BLACK);

      // Redraw grid elements cut by line clearing
      tft.drawCircle(cx, cy, r - 1, TFT_DARKGREEN);
      tft.drawCircle(cx, cy, (r * 2) / 3, TFT_DARKGREEN);
      tft.drawCircle(cx, cy, r / 3, TFT_DARKGREEN);
      tft.drawFastHLine(0, cy, 240, TFT_DARKGREEN);
      tft.drawFastVLine(cx, 0, 240, TFT_DARKGREEN);

      // Refresh label text banner
      tft.fillRect(cx - 70, bannerY - 2, 140, 20, TFT_BLACK);
      tft.drawString(labelStr, cx, bannerY);
    }
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

  // Trigger the sweep animation
  showRadarSweepLoading("SWITCHING...");

  uint8_t mode = getBootButtonMode();
  Serial.printf("[Button] Tap detected. Boot Button Mode: %d\n", mode);

  if (mode == 0) {
    // Checkbox UNCHECKED: Cycle Distance / Range
    ui::radar::rangeNext();
    Serial.println("[Button] Switched Radar Range");

    if (g_radar_visible) {
      ui::radarDisplayDraw();
      fetchAndDrawAircraft();
    }
  } else {
    // Checkbox CHECKED: Cycle Location Profile
    int totalProfiles = g_profileManager.getProfileCount();
    Serial.printf("[Button] Cycling location... Total profiles available: %d\n", totalProfiles);

    if (totalProfiles > 0) {
      for (int i = 0; i < totalProfiles; ++i) {
        g_profileManager.nextProfile();
        LocationProfile* prof = g_profileManager.getActiveProfile();

        // Stop if valid non-zero coordinates are found
        if (prof && (prof->lat != 0.0f || prof->lon != 0.0f)) {
          break;
        }
      }

      syncLocationFromActiveProfile();

      // Refresh radar aircraft view for new location
      if (g_radar_visible) {
        ui::radarDisplayDraw();
        fetchAndDrawAircraft();
      }
      Serial.println("[Button] Switched Location Profile");
    } else {
      Serial.println("[Button] Warning: No profiles available to switch.");
    }
  }
}

}  // namespace

#include <Preferences.h>

void testNvsPersistence() {
  Preferences prefs;
  
  // 1. Open the "wifi" or "profile" namespace in read/write mode (false)
  if (!prefs.begin("wifi", false)) {
    Serial.println("[NVS TEST] ❌ FAILED: Could not open Preferences namespace!");
    return;
  }

  // 2. Read the previous boot count (defaulting to 0 if key doesn't exist)
  uint32_t bootCount = prefs.getUInt("boot_count", 0);
  bootCount++;

  // 3. Write updated count back to NVS
  prefs.putUInt("boot_count", bootCount);
  
  // 4. Always close the namespace to commit writes to flash
  prefs.end();

  Serial.printf("[NVS TEST] ✅ Success! Boot Count persistent value: %u\n", bootCount);
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