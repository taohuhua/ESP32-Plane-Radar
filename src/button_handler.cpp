#include "button_handler.h"
#include <Arduino.h>
#include <WiFi.h>

#include "services/adsb_client.h"
#include "services/profile_manager.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"

namespace {

config::ButtonMode g_boot_button_mode = config::ButtonMode::CycleLocation;

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
    ui::radarDisplayDraw();
  }
}

}  // namespace

void buttonHandlerInit() {
  pinMode(config::kBootPin, INPUT_PULLUP);
  pinMode(config::kLocationBtnPin, INPUT_PULLUP);
  pinMode(config::kRadiusBtnPin, INPUT_PULLUP);
}

void buttonHandlerPoll() {
  // --- 1. BOOT Pin Tap (GPIO 9) ---
  if (digitalRead(config::kBootPin) == LOW) {
    delay(config::kBootTapMinMs);
    if (digitalRead(config::kBootPin) == LOW) {
      if (g_boot_button_mode == config::ButtonMode::CycleLocation) {
        triggerLocationCycle();
      } else {
        triggerRangeCycle();
      }
      while (digitalRead(config::kBootPin) == LOW) {
        delay(10);
      }
    }
  }

  // --- 2. Dedicated External Location Button (GPIO 2) ---
  if (digitalRead(config::kLocationBtnPin) == LOW) {
    delay(config::kBootTapMinMs);
    if (digitalRead(config::kLocationBtnPin) == LOW) {
      triggerLocationCycle();
      while (digitalRead(config::kLocationBtnPin) == LOW) {
        delay(10);
      }
    }
  }

  // --- 3. Dedicated External Radius Button (GPIO 5) ---
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