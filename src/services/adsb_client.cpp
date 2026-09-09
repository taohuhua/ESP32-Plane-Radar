#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cstring>

#include "config.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
constexpr int kConnectAttemptMs = 200;
constexpr unsigned long kRequestTimeoutMs = 10000;

// A TLS 1.2 handshake on ESP32's mbedTLS build typically wants somewhere
// in the 40-50KB range of *contiguous* heap. On an ESP32-C3 Super Mini
// (no PSRAM) that's a meaningful fraction of total RAM once WiFi, the
// display driver, and WiFiManager are resident. Skip the attempt (and
// the multi-line mbedTLS error burst that comes with it) when there's
// clearly not enough room, rather than hammering a doomed connect() —
// this only avoids wasted churn/log noise, it doesn't fix a genuine
// shortage; watch the free/max_alloc numbers already logged below to see
// whether that's what's actually going on.
constexpr size_t kMinContiguousHeapForTls = 45000;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
PollFn s_poll_fn = nullptr;

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

int performGetWithPoll(HTTPClient& http) {
  http.setConnectTimeout(kConnectAttemptMs);
  pollNetwork();
  return http.GET();
}

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

/** Filter content is identical on every call, so build it once and reuse —
 *  no reason to alloc/free the same 14 boolean flags every 5 seconds. */
JsonDocument& sharedFilterDoc() {
  static JsonDocument filter;
  static bool initialized = false;
  if (!initialized) {
    filter["ac"][0]["lat"] = true;
    filter["ac"][0]["lon"] = true;
    filter["ac"][0]["true_heading"] = true;
    filter["ac"][0]["mag_heading"] = true;
    filter["ac"][0]["track"] = true;
    filter["ac"][0]["dir"] = true;
    filter["ac"][0]["gs"] = true;
    filter["ac"][0]["tas"] = true;
    filter["ac"][0]["ias"] = true;
    filter["ac"][0]["alt_baro"] = true;
    filter["ac"][0]["alt_geom"] = true;
    filter["ac"][0]["flight"] = true;
    filter["ac"][0]["hex"] = true;
    filter["ac"][0]["t"] = true;
    initialized = true;
  }
  return filter;
}

/** Reused across calls (cleared, not reconstructed) so its backing pool
 *  settles at the largest response seen instead of a fresh malloc/free of a
 *  differently-sized buffer every fetch — the main heap-fragmentation risk
 *  on a device that's meant to run for days between reboots. */
JsonDocument& sharedResponseDoc() {
  static JsonDocument doc;
  return doc;
}

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) return v;
  if (readJsonFloat(plane, "mag_heading", &v)) return v;
  if (readJsonFloat(plane, "track", &v)) return v;
  if (readJsonFloat(plane, "dir", &v)) return v;
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) return v;
  if (readJsonFloat(plane, "true_heading", &v)) return v;
  if (readJsonFloat(plane, "mag_heading", &v)) return v;
  if (readJsonFloat(plane, "dir", &v)) return v;
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) return v;
  if (readJsonFloat(plane, "tas", &v)) return v;
  if (readJsonFloat(plane, "ias", &v)) return v;
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) return;

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
  }
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  String url = kApiBase;
  url += String(center_lat, 6);
  url += "/lon/";
  url += String(center_lon, 6);
  url += "/dist/";
  url += String(dist_nm, 1);

  WiFiClientSecure client;
  client.setInsecure();

  const size_t max_alloc = ESP.getMaxAllocHeap();
  if (max_alloc < kMinContiguousHeapForTls) {
    LOG_ERROR(
        "adsb: skipping fetch, largest free block too small for TLS "
        "(max_alloc=%u free=%u, want>=%u)",
        static_cast<unsigned>(max_alloc), ESP.getFreeHeap(),
        static_cast<unsigned>(kMinContiguousHeapForTls));
    return false;
  }

  HTTPClient http;
  // deserializeJson() below reads directly from http.getStream() to avoid
  // buffering the whole response into a String first — but per ArduinoJson's
  // own docs, doing that bypasses HTTPClient's chunked-transfer-encoding
  // handling entirely, so any chunk-size framing bytes leak straight into
  // the JSON parser (this is exactly what "JSON parse error: InvalidInput"
  // with a body preview starting in a stray hex digit + \r\n turned out to
  // be). Requesting HTTP/1.0 makes compliant servers respond with
  // Content-Length or a close-terminated body instead of chunked encoding,
  // sidestepping the bypassed code path rather than working around it.
  http.useHTTP10(true);
  if (!http.begin(client, url)) {
    LOG_ERROR("adsb: http.begin failed (heap free=%u max_alloc=%u)",
              ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return false;
  }

  http.setTimeout(kRequestTimeoutMs);
  const int code = performGetWithPoll(http);
  if (code != HTTP_CODE_OK) {
    LOG_ERROR("adsb: HTTP %d (heap free=%u max_alloc=%u)", code,
              ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    http.end();
    return false;
  }

  // Define JSON Filter to save >70% heap RAM during parsing
  JsonDocument& filter = sharedFilterDoc();

  // Stream directly from socket without constructing a large String payload
  JsonDocument& doc = sharedResponseDoc();
  doc.clear();
  const DeserializationError err = deserializeJson(
      doc, http.getStream(), DeserializationOption::Filter(filter));

  if (err) {
    // InvalidInput almost always means the very first byte wasn't valid
    // JSON — which means the parser consumed at most a byte or two before
    // giving up, so the rest of whatever the server actually sent is very
    // likely still sitting unread in the stream right here. Log a preview
    // of it (printable characters kept as-is, everything else as [xx] hex)
    // so we can see whether this is a genuine payload issue (truncated/
    // malformed JSON), an HTML error/rate-limit page from a proxy in front
    // of adsb.fi, or something else entirely — the error code alone can't
    // tell us that.
    char preview[161];
    size_t n = 0;
    Stream& body = http.getStream();
    while (n < sizeof(preview) - 1 && body.available()) {
      const int c = body.read();
      if (c < 0) break;
      preview[n++] = static_cast<char>(c);
    }
    preview[n] = '\0';
    String preview_escaped;
    preview_escaped.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
      const unsigned char c = static_cast<unsigned char>(preview[i]);
      if (c >= 0x20 && c < 0x7f) {
        preview_escaped += static_cast<char>(c);
      } else {
        char hex[6];
        snprintf(hex, sizeof(hex), "[%02x]", c);
        preview_escaped += hex;
      }
    }
    LOG_ERROR("adsb: JSON parse error: %s | body preview (%u bytes): %s",
              err.c_str(), static_cast<unsigned>(n), preview_escaped.c_str());
    http.end();
    return false;
  }

  http.end();

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull()) {
    s_aircraft_count = 0;
    // See the shrinkToFit() call below — release doc's capacity even when
    // there's nothing to parse, so it isn't sitting on memory it doesn't
    // need between fetches either.
    doc.shrinkToFit();
    return true;
  }

  size_t n = 0;
  for (JsonObject plane : ac) {
    if (n >= kMaxAircraft) {
      break;
    }
    if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
      continue;
    }
    if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
      continue;
    }

    s_aircraft[n].lat = plane["lat"].as<float>();
    s_aircraft[n].lon = plane["lon"].as<float>();
    s_aircraft[n].nose_deg = pickNoseHeading(plane);
    s_aircraft[n].track_deg = pickTrackHeading(plane);
    s_aircraft[n].gs_knots = pickGroundSpeed(plane);
    fillTagFields(&s_aircraft[n], plane);
    ++n;
  }

  s_aircraft_count = n;

  // doc is static/reused (see sharedResponseDoc()) so it doesn't churn
  // malloc/free every 5s — but left at its grown capacity, it permanently
  // competes with other big allocations (like radar_display.cpp's 240x240
  // frame sprite) for the same contiguous heap, even between fetches when
  // it's not actually holding anything useful. shrinkToFit() releases the
  // spare capacity now that the data's been copied out into s_aircraft[];
  // next call, deserializeJson() just regrows it, which costs a bit of
  // realloc time but not a bigger footprint than before, and gives whatever
  // draws next (the frame sprite, right after this returns) first crack at
  // that memory instead of it sitting reserved and idle.
  doc.shrinkToFit();

  Serial.printf("adsb: %u aircraft, heap free=%u max_alloc=%u\n",
                static_cast<unsigned>(n), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  return true;
}

}  // namespace services::adsb
