#pragma once
#include "opendroneid.h"
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

namespace lab {
// Keep the v2 layout intact so upgrading does not discard saved identities or coordinates.
struct ConfigV2 {
  uint32_t version = 2;
  char uasId[21] = "TEST-0001";
  char operatorId[21] = "TEST-OPERATOR";
  double lat = 0, lon = 0, opLat = 0, opLon = 0;
  float altitude = 30, opAltitude = 0;
  uint16_t duration = 60;
};
struct Config : ConfigV2 {
  float height = 30, speed = 0, verticalSpeed = 0, heading = 0;
  Config() { version = 3; }
};
struct TelemetryTime {
  float seconds = INV_TIMESTAMP;
  uint32_t since2019 = 0;
  bool valid() const { return isfinite(seconds) && seconds >= 0 && seconds < 3600 && since2019; }
};
// The browser supplies UTC even when offline. Never persist or substitute uptime for UTC.
struct BroadcastClock {
  uint64_t unixMs = 0;
  uint32_t syncedAt = 0;
  static constexpr uint64_t MinUnixMs = 1577836800000ULL, MaxUnixMs = 4102444800000ULL;
  bool sync(uint64_t value, uint32_t now) {
    if (value < MinUnixMs || value >= MaxUnixMs) return false;
    unixMs = value; syncedAt = now; return true;
  }
  TelemetryTime at(uint32_t now) const {
    const uint32_t age = now - syncedAt;
    if (!unixMs || age > 86400000UL) return {};
    const uint64_t utc = unixMs + age;
    TelemetryTime result;
    // Quantize down before encoding so 3599.999 cannot round into the next hour.
    result.seconds = float((utc % 3600000ULL) / 100) / 10.0f;
    result.since2019 = uint32_t(utc / 1000 - 1546300800ULL);
    return result;
  }
};
// Nominal simulation accuracy classes, not measurements of this ESP32 or GNSS hardware.
constexpr float SimHorizontalAccuracyM = 10, SimVerticalAccuracyM = 3,
                SimSpeedAccuracyMps = 1, SimTimeAccuracyS = 0.1f;
// Validate UTF-8 without accepting controls, truncated sequences or surrogate codepoints.
inline bool validText(const char *s, size_t n) {
  for (size_t i = 0; i < n;) {
    uint8_t c = uint8_t(s[i]);
    if (c >= 32 && c <= 126) { ++i; continue; }
    size_t count; uint32_t code;
    if (c >= 0xC2 && c <= 0xDF) { count = 2; code = c & 31; }
    else if (c >= 0xE0 && c <= 0xEF) { count = 3; code = c & 15; }
    else if (c >= 0xF0 && c <= 0xF4) { count = 4; code = c & 7; }
    else return false;
    if (i + count > n) return false;
    for (size_t j = 1; j < count; ++j) {
      uint8_t v = uint8_t(s[i+j]); if ((v & 0xC0) != 0x80) return false;
      code = (code << 6) | (v & 63);
    }
    if ((count == 3 && code < 0x800) || (count == 4 && code < 0x10000) ||
        code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
    i += count;
  }
  return true;
}
inline bool number(const char *s, double low, double high, double &out) {
  if (!s || !*s || strlen(s) > 40) return false;
  char *end;
  double d = strtod(s, &end);
  if (*end || !isfinite(d) || d < low || d > high) return false;
  out = d; return true;
}
inline bool valid(const Config &c) {
  size_t a = strnlen(c.uasId, 21), b = strnlen(c.operatorId, 21);
  return c.version == 3 && a > 0 && a <= 20 && b <= 20 &&
    validText(c.uasId, a) && validText(c.operatorId, b) &&
    isfinite(c.lat) && c.lat >= -90 && c.lat <= 90 &&
    isfinite(c.lon) && c.lon >= -180 && c.lon <= 180 &&
    isfinite(c.opLat) && c.opLat >= -90 && c.opLat <= 90 &&
    isfinite(c.opLon) && c.opLon >= -180 && c.opLon <= 180 &&
    isfinite(c.altitude) && c.altitude >= -999.5f && c.altitude <= 31767.5f &&
    isfinite(c.opAltitude) && c.opAltitude >= -999.5f && c.opAltitude <= 31767.5f &&
    isfinite(c.height) && c.height >= -999.5f && c.height <= MAX_ALT &&
    isfinite(c.speed) && c.speed >= 0 && c.speed <= MAX_SPEED_H &&
    isfinite(c.verticalSpeed) && c.verticalSpeed >= MIN_SPEED_V && c.verticalSpeed <= MAX_SPEED_V &&
    isfinite(c.heading) && c.heading >= 0 && c.heading <= MAX_DIR &&
    c.duration >= 5 && c.duration <= 600;
}
inline bool restoreConfig(const void *bytes, size_t size, Config &out) {
  if (!bytes) return false;
  Config next;
  if (size == sizeof(Config)) memcpy(&next, bytes, size);
  else if (size == sizeof(ConfigV2)) {
    ConfigV2 old; memcpy(&old, bytes, size);
    if (old.version != 2) return false;
    static_cast<ConfigV2 &>(next) = old; next.version = 3;
  } else return false;
  if (!valid(next)) return false;
  out = next; return true;
}
inline void simulatedLocation(ODID_Location_data &loc, const Config &c, const TelemetryTime &time) {
  odid_initLocationData(&loc);
  loc.Status = c.height == 0 && c.verticalSpeed == 0 ? ODID_STATUS_GROUND : ODID_STATUS_AIRBORNE;
  loc.Latitude = c.lat; loc.Longitude = c.lon; loc.AltitudeGeo = c.altitude;
  loc.HeightType = ODID_HEIGHT_REF_OVER_TAKEOFF; loc.Height = c.height;
  loc.Direction = c.heading == 360 ? 0 : c.heading;
  loc.SpeedHorizontal = c.speed; loc.SpeedVertical = c.verticalSpeed;
  loc.HorizAccuracy = (c.lat || c.lon) ? ODID_HOR_ACC_10_METER : ODID_HOR_ACC_UNKNOWN;
  loc.VertAccuracy = ODID_VER_ACC_3_METER;
  loc.SpeedAccuracy = ODID_SPEED_ACC_1_METERS_PER_SECOND;
  loc.TimeStamp = time.valid() ? time.seconds : INV_TIMESTAMP;
  loc.TSAccuracy = time.valid() ? ODID_TIME_ACC_0_1_SECOND : ODID_TIME_ACC_UNKNOWN;
}
struct Messages {
  uint8_t data[4][25]{};
  bool build(const Config &c, const TelemetryTime &time = {}) {
    if (!valid(c)) return false;
    ODID_BasicID_data basic; odid_initBasicIDData(&basic);
    basic.UAType = ODID_UATYPE_HELICOPTER_OR_MULTIROTOR;
    basic.IDType = ODID_IDTYPE_SERIAL_NUMBER;
    memcpy(basic.UASID, c.uasId, sizeof(basic.UASID));
    ODID_Location_data loc; simulatedLocation(loc, c, time);
    ODID_System_data system; odid_initSystemData(&system);
    system.OperatorLocationType = ODID_OPERATOR_LOCATION_TYPE_FIXED;
    system.OperatorLatitude = c.opLat; system.OperatorLongitude = c.opLon;
    system.OperatorAltitudeGeo = c.opAltitude;
    system.Timestamp = time.valid() ? time.since2019 : 0;
    ODID_OperatorID_data op; odid_initOperatorIDData(&op);
    memcpy(op.OperatorId, c.operatorId, sizeof(op.OperatorId));
    return encodeBasicIDMessage(reinterpret_cast<ODID_BasicID_encoded *>(data[0]), &basic) == ODID_SUCCESS &&
      encodeLocationMessage(reinterpret_cast<ODID_Location_encoded *>(data[1]), &loc) == ODID_SUCCESS &&
      encodeSystemMessage(reinterpret_cast<ODID_System_encoded *>(data[2]), &system) == ODID_SUCCESS &&
      encodeOperatorIDMessage(reinterpret_cast<ODID_OperatorID_encoded *>(data[3]), &op) == ODID_SUCCESS;
  }
  void ble(uint8_t *out, uint8_t phase, uint8_t counter) const {
    const uint8_t prefix[] = {30, 0x16, 0xFA, 0xFF, 0x0D};
    memcpy(out, prefix, 5); out[5] = counter;
    memcpy(out + 6, data[phase % 4], 25);
  }
  void wifi(uint8_t *out, uint8_t counter) const {
    const uint8_t prefix[] = {0xDD, 108, 0xFA, 0x0B, 0xBC, 0x0D};
    memcpy(out, prefix, 6); out[6] = counter;
    out[7] = 0xF2; out[8] = 25; out[9] = 4;
    memcpy(out + 10, data, 100);
  }
};
} // namespace lab
