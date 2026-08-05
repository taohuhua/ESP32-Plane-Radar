#include "services/radar_location.h"
#include <Arduino.h>
#include <Preferences.h>
#include <cstdlib>
#include "config.h"

namespace services {
namespace location {

namespace {
const char* kPrefsNamespace = "radar_loc";
const char* kKeyActiveIdx = "active_idx";

size_t s_active_index = 0;
config::RadarLocation s_locations[config::kMaxLocations];

void loadDefaults() {
  for (size_t i = 0; i < config::kMaxLocations; ++i) {
    s_locations[i] = config::kDefaultLocations[i];
  }
}

void loadFromPreferences() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    loadDefaults();
    return;
  }

  s_active_index = prefs.getUInt(kKeyActiveIdx, 0);
  if (s_active_index >= config::kMaxLocations) {
    s_active_index = 0;
  }

  for (size_t i = 0; i < config::kMaxLocations; ++i) {
    char key_lat[16], key_lon[16], key_name[16];
    snprintf(key_lat, sizeof(key_lat), "lat_%zu", i);
    snprintf(key_lon, sizeof(key_lon), "lon_%zu", i);
    snprintf(key_name, sizeof(key_name), "name_%zu", i);

    s_locations[i].lat = prefs.getDouble(key_lat, config::kDefaultLocations[i].lat);
    s_locations[i].lon = prefs.getDouble(key_lon, config::kDefaultLocations[i].lon);
    
    String stored_name = prefs.getString(key_name, config::kDefaultLocations[i].name);
    snprintf(s_locations[i].name, sizeof(s_locations[i].name), "%s", stored_name.c_str());
  }

  prefs.end();
}

void saveActiveIndex() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putUInt(kKeyActiveIdx, s_active_index);
    prefs.end();
  }
}

}  // namespace

void init() {
  loadDefaults();
  loadFromPreferences();
}

double lat() { return s_locations[s_active_index].lat; }
double lon() { return s_locations[s_active_index].lon; }
const char* name() { return s_locations[s_active_index].name; }
size_t currentIndex() { return s_active_index; }
size_t count() { return config::kMaxLocations; }

void setIndex(size_t index) {
  if (index >= config::kMaxLocations) {
    index = 0;
  }
  s_active_index = index;
  saveActiveIndex();
}

void next() {
  s_active_index = (s_active_index + 1) % config::kMaxLocations;
  saveActiveIndex();
}

void set(double latitude, double longitude, const char* location_name) {
  s_locations[s_active_index].lat = latitude;
  s_locations[s_active_index].lon = longitude;
  if (location_name && location_name[0] != '\0') {
    snprintf(s_locations[s_active_index].name, sizeof(s_locations[s_active_index].name), "%s", location_name);
  } else {
    snprintf(s_locations[s_active_index].name, sizeof(s_locations[s_active_index].name), "Custom");
  }

  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    char key_lat[16], key_lon[16], key_name[16];
    snprintf(key_lat, sizeof(key_lat), "lat_%zu", s_active_index);
    snprintf(key_lon, sizeof(key_lon), "lon_%zu", s_active_index);
    snprintf(key_name, sizeof(key_name), "name_%zu", s_active_index);

    prefs.putDouble(key_lat, s_locations[s_active_index].lat);
    prefs.putDouble(key_lon, s_locations[s_active_index].lon);
    prefs.putString(key_name, s_locations[s_active_index].name);
    prefs.end();
  }
}

bool saveFromStrings(const char* lat_str, const char* lon_str) {
  if (!lat_str || !lon_str || lat_str[0] == '\0' || lon_str[0] == '\0') {
    return false;
  }
  char* end_lat;
  char* end_lon;
  double new_lat = strtod(lat_str, &end_lat);
  double new_lon = strtod(lon_str, &end_lon);

  if (end_lat == lat_str || end_lon == lon_str) {
    return false;
  }

  set(new_lat, new_lon, "Web Config");
  return true;
}

void clear() {
  loadDefaults();
  s_active_index = 0;

  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.clear();
    prefs.end();
  }
}

}  // namespace location
}  // namespace services