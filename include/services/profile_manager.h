#ifndef SERVICES_PROFILE_MANAGER_H
#define SERVICES_PROFILE_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>

#define MAX_LOCATION_PROFILES 5

struct LocationProfile {
    char name[24];
    float lat;
    float lon;
};

class ProfileManager {
private:
    Preferences prefs;
    LocationProfile profiles[MAX_LOCATION_PROFILES];
    int profileCount = 0;
    int activeIndex = -1;

public:
    void begin() {
        prefs.begin("plane_radar", false);
        loadProfiles();
    }

    void loadProfiles() {
        String json = prefs.getString("profiles_json", "");
        profileCount = 0;

        if (json.length() > 0) {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, json);

            if (!err && doc.is<JsonArray>()) {
                JsonArray arr = doc.as<JsonArray>();
                for (JsonObject obj : arr) {
                    if (profileCount >= MAX_LOCATION_PROFILES) break;
                    strlcpy(profiles[profileCount].name, obj["name"] | "Preset", sizeof(profiles[profileCount].name));
                    profiles[profileCount].lat = obj["lat"] | -37.7281f;
                    profiles[profileCount].lon = obj["lon"] | 144.9021f;
                    profileCount++;
                }
            }
        }

        // Initialize default location preset if NVS is fresh/empty
        if (profileCount == 0) {
            strlcpy(profiles[0].name, "Default Location", sizeof(profiles[0].name));
            profiles[0].lat = -37.8136f; // Default Melbourne / Essendon region
            profiles[0].lon = 144.9631f;
            profileCount = 1;
            saveProfiles();
        }

        activeIndex = prefs.getInt("active_idx", 0);
        if (activeIndex < 0 || activeIndex >= profileCount) {
            activeIndex = 0;
        }
    }

    void saveProfiles() {
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();

        for (int i = 0; i < profileCount; i++) {
            JsonObject obj = arr.add<JsonObject>();
            obj["name"] = profiles[i].name;
            obj["lat"] = profiles[i].lat;
            obj["lon"] = profiles[i].lon;
        }

        String json;
        serializeJson(doc, json);
        prefs.putString("profiles_json", json);
        prefs.putInt("active_idx", activeIndex);
    }

    // Cycle to next available location preset
    LocationProfile* nextProfile() {
        if (profileCount <= 0) return nullptr;
        
        if (profileCount > 1) {
            activeIndex = (activeIndex + 1) % profileCount;
            saveProfiles();
        }
        
        return &profiles[activeIndex];
    }

    // Set or update a location preset by index slot (0 to 4)
    bool setProfileAt(int index, const char* name, float lat, float lon) {
        if (index < 0 || index >= MAX_LOCATION_PROFILES) return false;

        strlcpy(profiles[index].name, name, sizeof(profiles[index].name));
        profiles[index].lat = lat;
        profiles[index].lon = lon;

        if (index >= profileCount) {
            profileCount = index + 1;
        }

        saveProfiles();
        return true;
    }

    int getProfileCount() const { return profileCount; }
    int getActiveIndex() const { return activeIndex; }
    
    void setActiveIndex(int idx) { 
        if (idx >= 0 && idx < profileCount) {
            activeIndex = idx; 
            saveProfiles();
        }
    }

    LocationProfile* getActiveProfile() {
        if (activeIndex >= 0 && activeIndex < profileCount) {
            return &profiles[activeIndex];
        }
        return nullptr;
    }

    LocationProfile* getProfile(int idx) {
        if (idx >= 0 && idx < profileCount) return &profiles[idx];
        return nullptr;
    }
};

extern ProfileManager g_profileManager;

#endif // SERVICES_PROFILE_MANAGER_H