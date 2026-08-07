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

// Boot button action state: 0 = Cycle Distance Range, 1 = Cycle Location Profile
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

// Change static object arrays to pointer arrays
WiFiManagerParameter* s_param_loc_names[config::kMaxLocations] = {nullptr};
WiFiManagerParameter* s_param_loc_lats[config::kMaxLocations]  = {nullptr};
WiFiManagerParameter* s_param_loc_lons[config::kMaxLocations]  = {nullptr};
WiFiManagerParameter* s_loc_headers[config::kMaxLocations]      = {nullptr};

// Dynamic IDs and Headers
char s_param_ids[config::kMaxLocations][3][16];
char s_header_html[config::kMaxLocations][64];

// Checkbox Custom HTML Attributes
char s_miles_checkbox_attrs[64] = "type=\"checkbox\"";
char s_runways_checkbox_attrs[64] = "type=\"checkbox\"";
char s_btn_mode_checkbox_attrs[64] = "type=\"checkbox\"";

WiFiManagerParameter s_param_miles("use_miles", "Display distances in miles", "T", 2,
                                   s_miles_checkbox_attrs, WFM_LABEL_AFTER);

WiFiManagerParameter s_param_runways("show_runways", "Show airport runways", "T", 2,
                                     s_runways_checkbox_attrs, WFM_LABEL_AFTER);

WiFiManagerParameter s_param_btn_location("btn_cycle_location", "BOOT button cycles locations (instead of range)", "T", 2,
                                          s_btn_mode_checkbox_attrs, WFM_LABEL_AFTER);

void initLocationParameters() {
  static bool parametersInitialized = false;
  if (parametersInitialized) return;

  for (int i = 0; i < config::kMaxLocations; ++i) {
    snprintf(s_param_ids[i][0], sizeof(s_param_ids[i][0]), "loc_name_%d", i);
    snprintf(s_param_ids[i][1], sizeof(s_param_ids[i][1]), "loc_lat_%d", i);
    snprintf(s_param_ids[i][2], sizeof(s_param_ids[i][2]), "loc_lon_%d", i);

    snprintf(s_header_html[i], sizeof(s_header_html[i]), "<hr><h3>Location Preset %d</h3>", i + 1);

    s_loc_headers[i]     = new WiFiManagerParameter(s_header_html[i]);
    // Changed label from "SSID" to "Location Name"
    s_param_loc_names[i] = new WiFiManagerParameter(s_param_ids[i][0], "Location Name", "", kNameParamLen);
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

void saveButtonModePreference(uint8_t mode) {
  g_bootButtonMode = mode;
  Preferences prefs;
  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.putUChar(kPrefsButtonModeKey, g_bootButtonMode);
    prefs.end();
  }
}

bool portalCheckboxChecked(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  if (value[0] == 'T' || value[0] == 't' || value[0] == '1') {
    return true;
  }
  return (strcmp(value, "on") == 0 || strcmp(value, "true") == 0);
}

void refreshPortalParamDefaults() {
  initLocationParameters();
  loadButtonModePreference();

  for (int i = 0; i < config::kMaxLocations; ++i) {
    LocationProfile* prof = g_profileManager.getProfile(i);

    // 1. Try reading from ProfileManager JSON store
    if (prof && strlen(prof->name) > 0 && (prof->lat != 0.0f || prof->lon != 0.0f)) {
      snprintf(s_loc_name_bufs[i], sizeof(s_loc_name_bufs[i]), "%s", prof->name);
      snprintf(s_loc_lat_bufs[i], sizeof(s_loc_lat_bufs[i]), "%.6f", prof->lat);
      snprintf(s_loc_lon_bufs[i], sizeof(s_loc_lon_bufs[i]), "%.6f", prof->lon);
    } 
    // 2. Fall back to hardcoded config defaults
    else if (i < (int)(sizeof(config::kDefaultLocations) / sizeof(config::kDefaultLocations[0]))) {
      snprintf(s_loc_name_bufs[i], sizeof(s_loc_name_bufs[i]), "%s", config::kDefaultLocations[i].name);
      snprintf(s_loc_lat_bufs[i], sizeof(s_loc_lat_bufs[i]), "%.6f", config::kDefaultLocations[i].lat);
      snprintf(s_loc_lon_bufs[i], sizeof(s_loc_lon_bufs[i]), "%.6f", config::kDefaultLocations[i].lon);
    } 
    // 3. Blank slot fallback
    else {
      s_loc_name_bufs[i][0] = '\0';
      snprintf(s_loc_lat_bufs[i], sizeof(s_loc_lat_bufs[i]), "0.000000");
      snprintf(s_loc_lon_bufs[i], sizeof(s_loc_lon_bufs[i]), "0.000000");
    }

    // Assign buffer pointers to WiFiManager Custom HTML Parameters
    if (s_param_loc_names[i]) s_param_loc_names[i]->setValue(s_loc_name_bufs[i], kNameParamLen);
    if (s_param_loc_lats[i])  s_param_loc_lats[i]->setValue(s_loc_lat_bufs[i], kCoordParamLen);
    if (s_param_loc_lons[i])  s_param_loc_lons[i]->setValue(s_loc_lon_bufs[i], kCoordParamLen);
  }

  // Set up Portal Checkboxes
  snprintf(s_miles_checkbox_attrs, sizeof(s_miles_checkbox_attrs), "type=\"checkbox\"%s",
           ui::radar::useMiles() ? " checked" : "");

  snprintf(s_runways_checkbox_attrs, sizeof(s_runways_checkbox_attrs), "type=\"checkbox\"%s",
           ui::radar::showRunways() ? " checked" : "");

  snprintf(s_btn_mode_checkbox_attrs, sizeof(s_btn_mode_checkbox_attrs), "type=\"checkbox\"%s",
           (g_bootButtonMode == 1) ? " checked" : "");
}

void onPortalParamsSaved() {
  int previousActiveIndex = g_profileManager.getActiveIndex();

  // Save the 5 pure location presets by index slot
  for (int i = 0; i < config::kMaxLocations; ++i) {
    if (s_param_loc_names[i] && s_param_loc_lats[i] && s_param_loc_lons[i]) {
      const char* name = s_param_loc_names[i]->getValue();
      const char* latStr = s_param_loc_lats[i]->getValue();
      const char* lonStr = s_param_loc_lons[i]->getValue();

      if (name != nullptr && strlen(name) > 0) {
        float lat = atof(latStr);
        float lon = atof(lonStr);
        g_profileManager.setProfileAt(i, name, lat, lon);
      }
    }
  }

  // Restore active index or fallback to index 0
  if (previousActiveIndex >= 0 && previousActiveIndex < g_profileManager.getProfileCount()) {
    g_profileManager.setActiveIndex(previousActiveIndex);
  } else {
    g_profileManager.setActiveIndex(0);
  }

  // Sync active location coordinates to the radar service
  LocationProfile* currentProf = g_profileManager.getActiveProfile();
  if (currentProf) {
    services::location::set(currentProf->lat, currentProf->lon, currentProf->name);
  }

  // Save custom radar portal parameters
  ui::radar::saveMilesFromPortal(s_param_miles.getValue());
  ui::radar::saveRunwaysFromPortal(s_param_runways.getValue());

  // Save button mode preference
  bool cycleLocations = portalCheckboxChecked(s_param_btn_location.getValue());
  saveButtonModePreference(cycleLocations ? 1 : 0);
  
  Serial.printf("[WiFiManager] Saved Button Mode: %u (%s)\n", 
                g_bootButtonMode, cycleLocations ? "Cycle Locations" : "Cycle Range");
}

void attachPortalParams(WiFiManager& wm) {
  refreshPortalParamDefaults();

// Pass pointers directly to WiFiManager
for (int i = 0; i < config::kMaxLocations; ++i) {
  wm.addParameter(s_loc_headers[i]);
  wm.addParameter(s_param_loc_names[i]);
  wm.addParameter(s_param_loc_lats[i]);
  wm.addParameter(s_param_loc_lons[i]);
}

  wm.addParameter(&s_param_miles);
  wm.addParameter(&s_param_runways);
  wm.addParameter(&s_param_btn_location);

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

bool checkForceConfigPortal() {
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

void clearForceConfigPortalFlag() {
  s_force_config_portal = false;
  Preferences prefs;
  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.remove(kPrefsForcePortalKey);
    prefs.end();
  }
}

void eraseWifiCredentials() {
  stopLanWebPortal();
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true, true);
  
  ensureWifiManager();
  s_wm.resetSettings();
  
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
  // 1. Enable persistent storage so NVS credentials load correctly
  WiFi.persistent(true);
  
  // 2. Set clean STA mode
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE); 
  delay(100);

  // 3. Check saved SSID directly from core ESP32 NVS
  String savedSSID = WiFi.SSID();

  Serial.printf("[WIFI] NVS SSID: '%s'\n", savedSSID.c_str());

  if (savedSSID.length() == 0) {
    Serial.println("[WIFI] No saved Wi-Fi credentials found in NVS.");
    return false;
  }

  if (show_ui) {
    statusScreenConnectingBegin(savedSSID.c_str());
  }

  // 4. Disconnect soft state without wiping NVS memory
  WiFi.disconnect(false, false); 
  delay(100);

  // 5. Calling WiFi.begin() without parameters tells the ESP32 driver
  // to fetch the encrypted SSID/PSK directly from its hardware NVS partition
  WiFi.begin();

  // 6. Connection wait loop
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 20000) {
    bootButtonPollLongPress();
    statusScreenConnectingTick();
    delay(100);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("[WIFI] Connection attempt timed out.");
  return false;
}

bool openConfigPortal() {
  stopLanWebPortal();
  WiFi.disconnect();
  delay(100);
  
  WiFi.mode(WIFI_AP_STA); // Enable AP + STA for captive portal
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

  const bool force_portal = checkForceConfigPortal();

  if (force_portal) {
    Serial.println("[WIFI] Reset flag detected. Opening setup portal...");
    clearForceConfigPortalFlag();
    if (openConfigPortal() && wifiLinkUp()) {
      WiFi.setAutoReconnect(true);
      return true;
    }
    return false;
  }

  Serial.println("[WIFI] Booting normally — connecting to Wi-Fi...");

  // 1. Connect to standard saved Wi-Fi network
  if (scanAndConnectSavedNetworks(true)) {
    WiFi.setAutoReconnect(true);
    Serial.printf("[WIFI] Connected: %s | IP: %s\n", 
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    return true;
  }

  // 2. Launch portal if Wi-Fi connection fails
  Serial.println("[WIFI] Could not connect to saved Wi-Fi — opening setup portal");
  if (openConfigPortal() && wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    return true;
  }

  Serial.println("[WIFI] Connection failed");
  statusScreenConnectFailed();
  return false;
}