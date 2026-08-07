#include "services/radar_location.h"

#include <Arduino.h>
#include "config.h"
#include "services/profile_manager.h"

extern ProfileManager g_profileManager;

namespace services {
namespace location {

namespace {
size_t s_active_index = 0;
}  // namespace

void init() {
  s_active_index = g_profileManager.getActiveIndex();
}

double lat() { 
  LocationProfile* prof = g_profileManager.getProfile(s_active_index);
  return prof ? prof->lat : config::kDefaultLocations[0].lat; 
}

double lon() { 
  LocationProfile* prof = g_profileManager.getProfile(s_active_index);
  return prof ? prof->lon : config::kDefaultLocations[0].lon; 
}

const char* name() { 
  LocationProfile* prof = g_profileManager.getProfile(s_active_index);
  return prof ? prof->name : config::kDefaultLocations[0].name; 
}

size_t currentIndex() { return g_profileManager.getActiveIndex(); }
size_t count() { return g_profileManager.getProfileCount(); }

void setIndex(size_t index) {
  g_profileManager.setActiveIndex(index);
  s_active_index = g_profileManager.getActiveIndex();
}

void next() {
  g_profileManager.nextProfile();
  s_active_index = g_profileManager.getActiveIndex();
}

void set(double latitude, double longitude, const char* location_name) {
  g_profileManager.setProfileAt(s_active_index, location_name, latitude, longitude);
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
  // Reset profiles to default configurations
  for (size_t i = 0; i < config::kMaxLocations; ++i) {
    if (i < (sizeof(config::kDefaultLocations) / sizeof(config::kDefaultLocations[0]))) {
      g_profileManager.setProfileAt(i, config::kDefaultLocations[i].name, 
                                    config::kDefaultLocations[i].lat, 
                                    config::kDefaultLocations[i].lon);
    } else {
      g_profileManager.setProfileAt(i, "", 0.0f, 0.0f);
    }
  }
  g_profileManager.setActiveIndex(0);
  s_active_index = 0;
}

}  // namespace location
}  // namespace services