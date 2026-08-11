#include "button_handler.h"
#include <Arduino.h>
#include <WiFi.h>

#include "services/adsb_client.h"
#include "services/profile_manager.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

// NOTE: GPIO 9 (BOOT) is intentionally NOT handled here. It has its own
// interrupt-driven tap/long-press logic in wifi_setup.cpp (bootButtonInit /
// bootButtonConsumeTap / bootButtonPollLongPress), consumed in main.cpp's
// handleBootButtonTap(). This file only owns the two dedicated external
// buttons (GPIO 2 / GPIO 5) so BOOT taps aren't handled twice.

void triggerLocationCycle() {
  LocationProfile* prof = g_profileManager.nextProfile();

  if (!prof) {
    Serial.println("[Location] No profiles available to cycle.");
    return;
  }

  // Directly update internal location service parameters
  services::location::set(prof->lat, prof->lon, prof->name);

  Serial.printf("[Location] Switched to %s (Index: %d, Lat: %.4f, Lon: %.4f)\n",
                services::location::name(),
                g_profileManager.getActiveIndex(),
                services::location::lat(),
                services::location::lon());

  if (WiFi.status() == WL_CONNECTED) {
    statusScreenRadarSweep(services::location::name());
    ui::radarDisplayDraw();
    const float fetch_km = ui::radar::fetchRadiusKm();
    services::adsb::fetchUpdate(services::location::lat(),
                                services::location::lon(),
                                fetch_km);
    ui::radarDisplayRefreshAircraft();
  }
}

void triggerRangeCycle() {
  ui::radar::rangeNext();
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  if (WiFi.status() == WL_CONNECTED) {
    statusScreenRadarSweep(range_label);
    ui::radarDisplayDraw();
  }
}

void buttonHandlerInit() {
  pinMode(config::kLocationBtnPin, INPUT_PULLUP);
  pinMode(config::kRadiusBtnPin, INPUT_PULLUP);
}

void buttonHandlerPoll() {
  // --- Dedicated External Location Button (GPIO 2) ---
  if (digitalRead(config::kLocationBtnPin) == LOW) {
    delay(config::kBootTapMinMs);
    if (digitalRead(config::kLocationBtnPin) == LOW) {
      triggerLocationCycle();
      while (digitalRead(config::kLocationBtnPin) == LOW) {
        delay(10);
      }
    }
  }

  // --- Dedicated External Radius Button (GPIO 5) ---
  if (digitalRead(config::kRadiusBtnPin) == LOW) {
    delay(config::kBootTapMinMs);
    if (digitalRead(config::kRadiusBtnPin) == LOW) {
      triggerRangeCycle();
      while (digitalRead(config::kRadiusBtnPin) == LOW) {
        delay(10);
      }
    }
  }
}
