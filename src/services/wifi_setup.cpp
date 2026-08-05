#include "services/wifi_setup.h"

#include <WiFi.h>
#include <WiFiManager.h>
#include <cstdio>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_wifi.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "services/profile_manager.h"
#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

// Boot button action state: 0 = Cycle Distance, 1 = Cycle Location
uint8_t g_bootButtonMode = 0; 

// --- Boot Button ISR and Debounce State ---
portMUX_TYPE s_boot_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_boot_tap_pending = false;
volatile bool s_boot_is_down = false;
volatile unsigned long s_boot_down_ms = 0;
bool s_long_press_handled = false;
bool s_boot_interrupt_attached = false;

void IRAM_ATTR onBootButtonIsr() {
  const bool down = digitalRead(config::kBootPin) == LOW;
  const unsigned long now = millis();
  portENTER_CRITICAL_ISR(&s_boot_mux);
  if (down) {
    s_boot_is_down = true;
    s_boot_down_ms = now;
  } else if (s_boot_is_down) {
    const unsigned long held = now - s_boot_down_ms;
    if (held >= config::kBootTapMinMs && held < config::kBootResetHoldMs) {
      s_boot_tap_pending = true;
    }
    s_boot_is_down = false;
  }
  portEXIT_CRITICAL_ISR(&s_boot_mux);
}

void initBootButton() {
  pinMode(config::kBootPin, INPUT_PULLUP);
  if (s_boot_interrupt_attached) {
    return;
  }
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kBootPin)),
                  onBootButtonIsr, CHANGE);
  s_boot_interrupt_attached = true;
}

namespace {

constexpr char kWifiPrefsNamespace[] = "wifi";
constexpr char kPrefsForcePortalKey[] = "portal";
constexpr char kPrefsButtonModeKey[]  = "btn_mode";

bool s_force_config_portal = false;
WiFiManager s_wm;
bool s_wm_configured = false;

void ensureWifiManager();
void startLanWebPortal();
void stopLanWebPortal();
bool wifiLinkUp();

constexpr int kCoordParamLen = 20;
constexpr int kNameParamLen = 32;
constexpr char kCoordInputAttrs[] = " type=\"number\" step=\"0.000001\"";

// Storage Buffers
char s_loc_name_bufs[config::kMaxLocations][kNameParamLen + 1];
char s_loc_lat_bufs[config::kMaxLocations][kCoordParamLen + 1];
char s_loc_lon_bufs[config::kMaxLocations][kCoordParamLen + 1];

// Parameter Pointers
WiFiManagerParameter* s_param_loc_names[config::kMaxLocations];
WiFiManagerParameter* s_param_loc_lats[config::kMaxLocations];
WiFiManagerParameter* s_param_loc_lons[config::kMaxLocations];
WiFiManagerParameter* s_loc_headers[config::kMaxLocations];

// Dynamic IDs and Headers
char s_param_ids[config::kMaxLocations][3][16];
char s_header_html[config::kMaxLocations][64];

// Unit & Display Options
char s_miles_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_miles("use_miles", "Display distances in miles", "T", 2,
                                   s_miles_checkbox_attrs, WFM_LABEL_AFTER);

char s_runways_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_runways("show_runways", "Show airport runways", "T", 2,
                                     s_runways_checkbox_attrs, WFM_LABEL_AFTER);

// BOOT Button Mode Option Pointer
char s_btn_mode_html[256];
WiFiManagerParameter* s_param_btn_mode = nullptr;

void initLocationParameters() {
  static bool parametersInitialized = false;
  if (parametersInitialized) return;

  for (int i = 0; i < config::kMaxLocations; ++i) {
    snprintf(s_param_ids[i][0], sizeof(s_param_ids[i][0]), "loc_name_%d", i);
    snprintf(s_param_ids[i][1], sizeof(s_param_ids[i][1]), "loc_lat_%d", i);
    snprintf(s_param_ids[i][2], sizeof(s_param_ids[i][2]), "loc_lon_%d", i);

    snprintf(s_header_html[i], sizeof(s_header_html[i]), "<hr><h3>Location Preset %d</h3>", i + 1);

    s_loc_headers[i]     = new WiFiManagerParameter(s_header_html[i]);
    s_param_loc_names[i] = new WiFiManagerParameter(s_param_ids[i][0], "SSID / Network Name", "", kNameParamLen);
    s_param_loc_lats[i]  = new WiFiManagerParameter(s_param_ids[i][1], "Latitude", "0.000000", kCoordParamLen, kCoordInputAttrs);
    s_param_loc_lons[i]  = new WiFiManagerParameter(s_param_ids[i][2], "Longitude", "0.000000", kCoordParamLen, kCoordInputAttrs);
  }
  parametersInitialized = true;
}

void loadButtonModePreference() {
  Preferences prefs;
  if (prefs.begin(kWifiPrefsNamespace, true)) {
    g_bootButtonMode = prefs.getUChar(kPrefsButtonModeKey, 0);
    prefs.end();
  }
}

void refreshPortalParamDefaults() {
  initLocationParameters();
  loadButtonModePreference();

  for (int i = 0; i < config::kMaxLocations; ++i) {
    LocationProfile* prof = g_profileManager.getProfile(i);

    // 1. Check if profile exists and has a non-empty name and non-zero lat/lon
    if (prof && strlen(prof->name) > 0 && (prof->lat != 0.0f || prof->lon != 0.0f)) {
      snprintf(s_loc_name_bufs[i], sizeof(s_loc_name_bufs[i]), "%s", prof->name);
      snprintf(s_loc_lat_bufs[i], sizeof(s_loc_lat_bufs[i]), "%.6f", prof->lat);
      snprintf(s_loc_lon_bufs[i], sizeof(s_loc_lon_bufs[i]), "%.6f", prof->lon);
    } 
    // 2. Fall back to config.h defaults if valid index exists
    else if (i < (int)(sizeof(config::kDefaultLocations) / sizeof(config::kDefaultLocations[0]))) {
      snprintf(s_loc_name_bufs[i], sizeof(s_loc_name_bufs[i]), "%s", config::kDefaultLocations[i].name);
      snprintf(s_loc_lat_bufs[i], sizeof(s_loc_lat_bufs[i]), "%.6f", config::kDefaultLocations[i].lat);
      snprintf(s_loc_lon_bufs[i], sizeof(s_loc_lon_bufs[i]), "%.6f", config::kDefaultLocations[i].lon);
    } 
    // 3. Blank fields for unused slots
    else {
      snprintf(s_loc_name_bufs[i], sizeof(s_loc_name_bufs[i]), "");
      snprintf(s_loc_lat_bufs[i], sizeof(s_loc_lat_bufs[i]), "0.000000");
      snprintf(s_loc_lon_bufs[i], sizeof(s_loc_lon_bufs[i]), "0.000000");
    }

    s_param_loc_names[i]->setValue(s_loc_name_bufs[i], kNameParamLen);
    s_param_loc_lats[i]->setValue(s_loc_lat_bufs[i], kCoordParamLen);
    s_param_loc_lons[i]->setValue(s_loc_lon_bufs[i], kCoordParamLen);
  }

  snprintf(s_miles_checkbox_attrs, sizeof(s_miles_checkbox_attrs), "type=\"checkbox\"%s",
           ui::radar::useMiles() ? " checked" : "");
  s_param_miles.setValue("T", 2);
  snprintf(s_runways_checkbox_attrs, sizeof(s_runways_checkbox_attrs),
           "type=\"checkbox\"%s", ui::radar::showRunways() ? " checked" : "");
  s_param_runways.setValue("T", 2);

  snprintf(s_btn_mode_html, sizeof(s_btn_mode_html),
           "<hr><h3>Button Settings</h3>"
           "<label for=\"btn_mode\">BOOT Button Tap Action</label>"
           "<select name=\"btn_mode\" id=\"btn_mode\">"
           "<option value=\"0\" %s>Cycle Distance Range</option>"
           "<option value=\"1\" %s>Cycle Location Profile</option>"
           "</select>",
           (g_bootButtonMode == 0) ? "selected" : "",
           (g_bootButtonMode == 1) ? "selected" : "");

  if (s_param_btn_mode != nullptr) {
    delete s_param_btn_mode;
  }
  s_param_btn_mode = new WiFiManagerParameter(s_btn_mode_html);
}

void onPortalParamsSaved() {
  String activeSSID = s_wm.getWiFiSSID();
  String activePass = s_wm.getWiFiPass();

  for (int i = 0; i < config::kMaxLocations; ++i) {
    const char* name = s_param_loc_names[i]->getValue();
    float lat = atof(s_param_loc_lats[i]->getValue());
    float lon = atof(s_param_loc_lons[i]->getValue());

    if (strlen(name) > 0) {
      g_profileManager.addOrUpdateProfile(name, name, activePass.c_str(), lat, lon);
    }
  }

  int activeIndex = g_profileManager.getActiveIndex();
  LocationProfile* currentProf = g_profileManager.getProfile(activeIndex);
  if (currentProf) {
    services::location::set(currentProf->lat, currentProf->lon, currentProf->name);
  }

  ui::radar::saveMilesFromPortal(s_param_miles.getValue());
  ui::radar::saveRunwaysFromPortal(s_param_runways.getValue());

  if (s_param_btn_mode != nullptr) {
    const char* btnVal = s_param_btn_mode->getValue();
    if (btnVal != nullptr) {
      g_bootButtonMode = static_cast<uint8_t>(atoi(btnVal));
      Preferences prefs;
      if (prefs.begin(kWifiPrefsNamespace, false)) {
        prefs.putUChar(kPrefsButtonModeKey, g_bootButtonMode);
        prefs.end();
      }
    }
  }
}

void attachPortalParams(WiFiManager& wm) {
  refreshPortalParamDefaults();

  for (int i = 0; i < config::kMaxLocations; ++i) {
    wm.addParameter(s_loc_headers[i]);
    wm.addParameter(s_param_loc_names[i]);
    wm.addParameter(s_param_loc_lats[i]);
    wm.addParameter(s_param_loc_lons[i]);
  }

  wm.addParameter(&s_param_miles);
  wm.addParameter(&s_param_runways);
  if (s_param_btn_mode != nullptr) {
    wm.addParameter(s_param_btn_mode);
  }
  wm.setSaveParamsCallback(onPortalParamsSaved);
}

void markForceConfigPortal() {
  s_force_config_portal = true;
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kPrefsForcePortalKey, true);
  prefs.end();
}

bool consumeForceConfigPortal() {
  if (s_force_config_portal) {
    s_force_config_portal = false;
    Preferences prefs;
    if (prefs.begin(kWifiPrefsNamespace, false)) {
      prefs.remove(kPrefsForcePortalKey);
      prefs.end();
    }
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  if (!pending) {
    return false;
  }

  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.remove(kPrefsForcePortalKey);
    prefs.end();
  }
  return true;
}

void eraseWifiCredentials() {
  stopLanWebPortal();
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_OFF);
  delay(100);

  ensureWifiManager();
  WiFi.persistent(true);
  s_wm.resetSettings();
  s_wm.erase();
  WiFi.disconnect(true, true);
  WiFi.persistent(false);

  WiFi.mode(WIFI_OFF);
  delay(100);
}

void resetWifiCredentials() {
  markForceConfigPortal();
  eraseWifiCredentials();
  services::location::clear();
  ui::radar::unitsReset();
  Serial.println("WiFi credentials, location, and units cleared");
}

void onConfigPortalApStarted(WiFiManager*) {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  statusScreenPortal();
#ifdef WM_MDNS
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Setup portal: http://%s.local (or http://%s)\n",
                  config::kPortalHostname, config::kPortalIp);
  } else {
    Serial.printf("Setup portal: http://%s (mDNS unavailable)\n", config::kPortalIp);
  }
#else
  Serial.printf("Setup portal: http://%s\n", config::kPortalIp);
#endif
}

bool wifiLinkUp() {
  return WiFi.status() == WL_CONNECTED &&
         WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void ensureWifiManager() {
  if (s_wm_configured) {
    return;
  }
  s_wm.setConfigPortalTimeout(config::kWifiPortalTimeoutSec);
  s_wm.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                           IPAddress(255, 255, 255, 0));
  s_wm.setHostname(config::kPortalHostname);
  s_wm.setAPCallback(onConfigPortalApStarted);
  attachPortalParams(s_wm);
  s_wm_configured = true;
}

void startLanWebPortal() {
  if (!wifiLinkUp() || s_wm.getWebPortalActive() ||
      s_wm.getConfigPortalActive()) {
    return;
  }
  refreshPortalParamDefaults();
  WiFi.mode(WIFI_STA);
  s_wm.setConfigPortalBlocking(false);
#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
  }
#endif
  s_wm.startWebPortal();
  Serial.printf("LAN config: http://%s.local or http://%s\n",
                config::kPortalHostname, WiFi.localIP().toString().c_str());
}

void stopLanWebPortal() {
  if (!s_wm.getWebPortalActive()) {
    return;
  }
  s_wm.stopWebPortal();
#ifdef WM_MDNS
  MDNS.end();
#endif
}

void prepareSta() {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(true);
}

bool waitForLinkWithUi(const char* ssid_for_ui, unsigned long attempt_ms) {
  const unsigned long deadline = millis() + attempt_ms;
  while (millis() < deadline) {
    if (wifiLinkUp()) {
      return true;
    }
    bootButtonPollLongPress();
    statusScreenConnectingTick();
    delay(config::kWifiConnectingFrameMs);
  }
  return wifiLinkUp();
}

bool tryConnectWithUi(const String& ssid, const String& pass, bool show_ui) {
  if (wifiLinkUp()) {
    return true;
  }

  const char* ui_ssid = ssid.length() > 0 ? ssid.c_str() : "network";
  if (show_ui) {
    statusScreenConnectingBegin(ui_ssid);
  }

  for (uint8_t attempt = 1; attempt <= config::kWifiConnectAttempts; ++attempt) {
    if (attempt > 1) {
      Serial.printf("WiFi connect retry %u/%u\n", attempt,
                    config::kWifiConnectAttempts);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(400);
    }

    prepareSta();
    if (ssid.length() > 0) {
      WiFi.begin(ssid.c_str(), pass.c_str());
    } else {
      WiFi.begin();
    }

    if (waitForLinkWithUi(ui_ssid, config::kWifiConnectAttemptMs)) {
      return true;
    }
  }

  return false;
}

bool scanAndConnectSavedNetworks(bool show_ui) {
  int profileCount = g_profileManager.getProfileCount();
  if (profileCount == 0) {
    String ssid = s_wm.getWiFiSSID();
    String pass = s_wm.getWiFiPass();
    if (ssid.length() > 0) {
      return tryConnectWithUi(ssid, pass, show_ui);
    }
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.println("[WIFI] Scanning networks...");
  int n = WiFi.scanNetworks();

  for (int i = 0; i < n; ++i) {
    String scannedSSID = WiFi.SSID(i);
    for (int p = 0; p < profileCount; p++) {
      LocationProfile* prof = g_profileManager.getProfile(p);
      if (scannedSSID.equalsIgnoreCase(prof->ssid)) {
        Serial.printf("[WIFI] Found saved network: %s. Connecting...\n", prof->ssid);
        if (tryConnectWithUi(prof->ssid, prof->pass, show_ui)) {
          g_profileManager.setActiveIndex(p);

          services::location::set(prof->lat, prof->lon, prof->name);
          Serial.printf("[LOCATION] Loaded Profile Coordinates: (%.6f, %.6f)\n", prof->lat, prof->lon);
          return true;
        }
      }
    }
  }

  return false;
}

bool openConfigPortal() {
  stopLanWebPortal();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  statusScreenPortal();
  s_wm.setConfigPortalBlocking(false);
  s_wm.startConfigPortal(config::kPortalApName);
  while (s_wm.getConfigPortalActive()) {
    bootButtonPollLongPress();
    if (s_wm.process()) {
      return true;
    }
    delay(10);
  }
  return wifiLinkUp();
}

}  // namespace

bool wifiShowsSetupScreenOnBoot() {
  if (s_force_config_portal) {
    return true;
  }
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  return pending;
}

bool wifiBootButtonPressed() {
  return digitalRead(config::kBootPin) == LOW;
}

void bootButtonInit() { 
  initBootButton(); 
  loadButtonModePreference();
}

bool bootButtonConsumeTap() {
  portENTER_CRITICAL(&s_boot_mux);
  const bool tap = s_boot_tap_pending;
  if (tap) {
    s_boot_tap_pending = false;
  }
  portEXIT_CRITICAL(&s_boot_mux);
  return tap;
}

void bootButtonPollLongPress() {
  if (wifiBootButtonPressed()) {
    portENTER_CRITICAL(&s_boot_mux);
    if (!s_boot_is_down) {
      s_boot_is_down = true;
      s_boot_down_ms = millis();
    }
    const unsigned long down_ms = s_boot_down_ms;
    portEXIT_CRITICAL(&s_boot_mux);

    if (!s_long_press_handled &&
        millis() - down_ms >= config::kBootResetHoldMs) {
      s_long_press_handled = true;
      Serial.println("BOOT held — resetting WiFi");
      wifiResetCredentialsAndReboot();
    }
  } else {
    portENTER_CRITICAL(&s_boot_mux);
    s_boot_is_down = false;
    portEXIT_CRITICAL(&s_boot_mux);
    s_long_press_handled = false;
  }
}

void wifiResetCredentialsAndReboot() {
  resetWifiCredentials();
  statusScreenWifiReset();
  delay(800);
  esp_restart();
}

bool wifiReconnect() {
  bootButtonInit();
  Serial.println("WiFi reconnecting...");
  return scanAndConnectSavedNetworks(true);
}

void wifiLoop() {
  ensureWifiManager();
  if (wifiLinkUp()) {
    if (!s_wm.getWebPortalActive() && !s_wm.getConfigPortalActive()) {
      startLanWebPortal();
    }
    if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
      bootButtonPollLongPress();
      s_wm.process();
    }
  } else {
    stopLanWebPortal();
  }
}

bool wifiSetupConnect() {
  bootButtonInit();
  ensureWifiManager();

  const bool force_portal = consumeForceConfigPortal();
  WiFi.setAutoReconnect(false);

  if (force_portal) {
    eraseWifiCredentials();
    WiFi.mode(WIFI_OFF);
    delay(100);
  }

  if (force_portal) {
    Serial.println("Opening WiFi setup portal (after reset)");
    if (openConfigPortal() && wifiLinkUp()) {
      WiFi.setAutoReconnect(true);
      Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      return true;
    }
    Serial.println("WiFi connection failed");
    statusScreenConnectFailed();
    return false;
  }

  Serial.println("Connecting to WiFi (portal opens if needed)...");

  if (wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (scanAndConnectSavedNetworks(true)) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("No saved network available — opening setup portal");

  if (openConfigPortal() && wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("WiFi connection failed");
  statusScreenConnectFailed();
  return false;
}