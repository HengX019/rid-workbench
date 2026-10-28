#pragma once
#include "opendroneid.h"
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

namespace lab {
struct Config {
  uint32_t version = 2;
  char uasId[21] = "TEST-0001";
  char operatorId[21] = "TEST-OPERATOR";
  double lat = 0, lon = 0, opLat = 0, opLon = 0;
  float altitude = 30, opAltitude = 0;
  uint16_t duration = 60;
};
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
  return c.version == 2 && a > 0 && a <= 20 && b <= 20 &&
    validText(c.uasId, a) && validText(c.operatorId, b) &&
    isfinite(c.lat) && c.lat >= -90 && c.lat <= 90 &&
    isfinite(c.lon) && c.lon >= -180 && c.lon <= 180 &&
    isfinite(c.opLat) && c.opLat >= -90 && c.opLat <= 90 &&
    isfinite(c.opLon) && c.opLon >= -180 && c.opLon <= 180 &&
    isfinite(c.altitude) && c.altitude >= -999.5f && c.altitude <= 31767.5f &&
    isfinite(c.opAltitude) && c.opAltitude >= -999.5f && c.opAltitude <= 31767.5f &&
    c.duration >= 5 && c.duration <= 600;
}
struct Messages {
  uint8_t data[4][25]{};
  bool build(const Config &c) {
    if (!valid(c)) return false;
    ODID_BasicID_data basic; odid_initBasicIDData(&basic);
    basic.UAType = ODID_UATYPE_HELICOPTER_OR_MULTIROTOR;
    basic.IDType = ODID_IDTYPE_SERIAL_NUMBER;
    memcpy(basic.UASID, c.uasId, sizeof(basic.UASID));
    ODID_Location_data loc; odid_initLocationData(&loc);
    loc.Status = ODID_STATUS_UNDECLARED;
    loc.Latitude = c.lat; loc.Longitude = c.lon; loc.AltitudeGeo = c.altitude;
    loc.TimeStamp = INV_TIMESTAMP;
    ODID_System_data system; odid_initSystemData(&system);
    system.OperatorLocationType = ODID_OPERATOR_LOCATION_TYPE_FIXED;
    system.OperatorLatitude = c.opLat; system.OperatorLongitude = c.opLon;
    system.OperatorAltitudeGeo = c.opAltitude;
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
