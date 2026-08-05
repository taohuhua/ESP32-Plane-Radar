#ifndef SERVICES_PROFILE_MANAGER_H
#define SERVICES_PROFILE_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>

#define MAX_LOCATION_PROFILES 5

struct LocationProfile {
    char name[24];
    char ssid[33];
    char pass[64];
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
        String json = prefs.getString("profiles_json", "[]");
        JsonDocument doc; // ArduinoJson v7 syntax
        DeserializationError err = deserializeJson(doc, json);

        profileCount = 0;
        if (!err && doc.is<JsonArray>()) {
            JsonArray arr = doc.as<JsonArray>();
            for (JsonObject obj : arr) {
                if (profileCount >= MAX_LOCATION_PROFILES) break;
                strlcpy(profiles[profileCount].name, obj["name"] | "Preset", sizeof(profiles[profileCount].name));
                strlcpy(profiles[profileCount].ssid, obj["ssid"] | "", sizeof(profiles[profileCount].ssid));
                strlcpy(profiles[profileCount].pass, obj["pass"] | "", sizeof(profiles[profileCount].pass));
                profiles[profileCount].lat = obj["lat"] | -37.7281f; // Default Essendon area fallback
                profiles[profileCount].lon = obj["lon"] | 144.9021f;
                profileCount++;
            }
        }
        activeIndex = prefs.getInt("active_idx", profileCount > 0 ? 0 : -1);
    }

    void saveProfiles() {
        JsonDocument doc; // ArduinoJson v7 syntax
        JsonArray arr = doc.to<JsonArray>();

        for (int i = 0; i < profileCount; i++) {
            JsonObject obj = arr.add<JsonObject>();
            obj["name"] = profiles[i].name;
            obj["ssid"] = profiles[i].ssid;
            obj["pass"] = profiles[i].pass;
            obj["lat"] = profiles[i].lat;
            obj["lon"] = profiles[i].lon;
        }

        String json;
        serializeJson(doc, json);
        prefs.putString("profiles_json", json);
        prefs.putInt("active_idx", activeIndex);
    }

    // Cycles to the next available profile in the array
    LocationProfile* nextProfile() {
        if (profileCount <= 0) return nullptr;
        
        if (profileCount > 1) {
            activeIndex = (activeIndex + 1) % profileCount;
            saveProfiles();
        }
        
        return &profiles[activeIndex];
    }

    bool addOrUpdateProfile(const char* name, const char* ssid, const char* pass, float lat, float lon) {
        for (int i = 0; i < profileCount; i++) {
            if (strcmp(profiles[i].ssid, ssid) == 0) {
                strlcpy(profiles[i].name, name, sizeof(profiles[i].name));
                strlcpy(profiles[i].pass, pass, sizeof(profiles[i].pass));
                profiles[i].lat = lat;
                profiles[i].lon = lon;
                activeIndex = i;
                saveProfiles();
                return true;
            }
        }
        if (profileCount < MAX_LOCATION_PROFILES) {
            strlcpy(profiles[profileCount].name, name, sizeof(profiles[profileCount].name));
            strlcpy(profiles[profileCount].ssid, ssid, sizeof(profiles[profileCount].ssid));
            strlcpy(profiles[profileCount].pass, pass, sizeof(profiles[profileCount].pass));
            profiles[profileCount].lat = lat;
            profiles[profileCount].lon = lon;
            activeIndex = profileCount;
            profileCount++;
            saveProfiles();
            return true;
        }
        return false;
    }

    void deleteProfile(int index) {
        if (index < 0 || index >= profileCount) return;
        for (int i = index; i < profileCount - 1; i++) {
            profiles[i] = profiles[i + 1];
        }
        profileCount--;
        if (activeIndex >= profileCount) activeIndex = profileCount - 1;
        saveProfiles();
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