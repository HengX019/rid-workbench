#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "tx_codec.h"
#include "opendroneidcn.h"

// ASTM F3411/OpenDroneID receive-side subset. Wire offsets cross-checked against
// https://github.com/opendroneid/opendroneid-core-c (see README for revision).
namespace rid {
constexpr size_t MessageSize = 25;
constexpr size_t MaxPayload = 256;
// Protocol: 0 ASTM, 1 GB 46750. Transport: 0 legacy BLE, 1 BLE 5, 2 Beacon, 3 NAN.
constexpr size_t MaxDevices = 50;
struct Frame {
  uint8_t mac[6];
  uint8_t source; // 0 BLE, 1 Wi-Fi
  uint8_t protocol, transport, channel, phy;
  int8_t rssi;
  uint16_t length;
  uint32_t received;
  uint8_t payload[MaxPayload];
};
// Keep each explicitly identified ground position and its own receive time/datum.
// A takeoff point is never inferred from an operator or aircraft coordinate.
struct GroundPosition {
  double lat = 0, lon = 0;
  uint32_t seen = 0;
  uint8_t coordSystem = 255;
  bool received = false, valid = false;
};
struct Device {
  uint8_t mac[6], source, type, idType, protocol, transport, channel, phy, operatorLocationType;
  DroneRIDData_t cn{};
  bool hasGcsAltitude;
  bool hasSpeed=false,hasHeading=false,hasVerticalSpeed=false,hasRelativeAltitude=false,hasPressureAltitude=false;
  float speed=0,heading=0,verticalSpeed=0,relativeAltitude=0,pressureAltitude=0;
  uint8_t flightStatus=0,cnVersion=0;
  uint32_t cnPackets=0;
  int8_t rssi;
  uint32_t lastSeen, packets, locationSeen, systemSeen;
  char uasId[41], operatorId[21];
  char selfDescription[24]{};
  uint8_t uaType=0,selfDescriptionType=0;
  uint8_t messageMask=0;uint32_t messageCounts[6]{};
  bool uaTypeReceived=false,selfIdReceived=false;
  double lat, lon, opLat, opLon;
  GroundPosition operatorPosition{}, takeoffPosition{};
  float altitude;
  bool hasLocation, hasAltitude, hasOperatorLocation, locationReceived, systemReceived;
};
inline void groundPosition(Device &d, uint8_t type, double lat, double lon,
                           bool valid, uint32_t now, uint8_t crs) {
  if (type > 2) return;
  GroundPosition &p = type == 0 ? d.takeoffPosition : d.operatorPosition;
  p.lat = lat; p.lon = lon; p.valid = valid; p.received = true;
  p.seen = now; p.coordSystem = crs;
}
inline uint16_t u16(const uint8_t *p) {
  return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
inline int32_t i32(const uint8_t *p) {
  uint32_t raw = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                 (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  int32_t out;
  memcpy(&out, &raw, sizeof(out));
  return out;
}
inline bool position(int32_t lat, int32_t lon) {
  return lat >= -900000000 && lat <= 900000000 &&
         lon >= -1800000000 && lon <= 1800000000 && (lat != 0 || lon != 0);
}
inline void textId(char *out, const uint8_t *in) {
  size_t n = 0; while (n < 20 && in[n]) ++n;
  if (lab::validText(reinterpret_cast<const char *>(in), n)) memcpy(out, in, n);
  else for (size_t i = 0; i < n; ++i) out[i] = in[i] >= 32 && in[i] < 127 ? char(in[i]) : '?';
  out[n] = 0;
}

inline bool validSingle(const uint8_t *p, size_t n) {
  return n == MessageSize && (p[0] >> 4) <= 5;
}
inline bool validPayload(const uint8_t *p, size_t n) {
  if (!p || !n) return false;
  if ((p[0] >> 4) != 15) return validSingle(p, n);
  if (n < 3 || p[1] != MessageSize || p[2] == 0 || p[2] > 9 ||
      n != 3 + size_t(p[2]) * MessageSize) return false;
  for (uint8_t i = 0; i < p[2]; ++i)
    if (!validSingle(p + 3 + size_t(i) * MessageSize, MessageSize)) return false;
  return true;
}
// Bounded header/flag validation before looking inside vendor-specific containers.
inline size_t cnPacketSize(const uint8_t *p, size_t n) {
  if (!p || n < 6 || p[0] != 0xFF || (p[1] & 0xE0) != 0x20 || !p[2] || p[2] > 200) return 0;
  size_t flags = 0;
  do { if (flags >= 16 || 3 + flags >= n) return 0; } while (p[3 + flags++] & 1);
  if (flags < 3 || (p[3] & 0xDE) != 0xDE || (p[4] & 0xE4) != 0xE4 || (p[5] & 0xFE) != 0xFE) return 0;
  static const uint8_t lengths[21] = {20,8,1,1,1,8,2,8,2,2,2,1,2,2,1,1,1,1,1,6,1};
  size_t known = 0;
  for (size_t i=0; i<21; ++i) if (p[3+i/7] & (0x80 >> (i%7))) known += lengths[i];
  if (known > p[2] || (flags == 3 && known != p[2])) return 0;
  size_t total = 3 + flags + p[2];
  return total <= n && total <= MaxPayload ? total : 0;
}
// GB table 3 permits NULL registration and high-side NULL padding of legacy
// identifiers. Normalize padding without accepting embedded-NUL suffix data.
inline bool normalizeCnText(char *text,size_t capacity,bool required) {
  size_t first=0;while(first<capacity-1 && !text[first])++first;
  size_t end=first;while(end<capacity-1 && text[end])++end;
  for(size_t i=end;i<capacity-1;++i)if(text[i])return false;
  if(first==end){text[0]=0;return !required;}
  memmove(text,text+first,end-first);text[end-first]=0;return true;
}
inline bool validCn(const uint8_t *p, size_t n, DroneRIDData_t &data) {
  return cnPacketSize(p,n)==n && n && CN46750_RID_Decode(p,n,&data,nullptr)>=RID_OK &&
    normalizeCnText(data.sn,sizeof(data.sn),true) && normalizeCnText(data.uin,sizeof(data.uin),false);
}
inline bool cnPosition(const uint8_t *p) {
  int32_t lon=i32(p), lat=i32(p+4);
  return lon != -1 && lat != -1 && lat >= -900000000 && lat <= 900000000 && lon >= -1800000000 && lon <= 1800000000;
}
inline void decodeCn(Device &d, const Frame &f, const DroneRIDData_t &cn) {
  d.cn=cn; d.type=255; d.idType=1;
  memcpy(d.uasId,cn.sn,21);
  d.lat=cn.drone_lat; d.lon=cn.drone_lon; d.opLat=cn.gcs_lat; d.opLon=cn.gcs_lon;
  size_t flags=0; while (f.payload[3+flags++] & 1) {}
  const uint8_t *gcs=f.payload+3+flags+30+((f.payload[3]&0x20)?1:0);
  const uint8_t *uav=gcs+10;
  d.hasOperatorLocation=cnPosition(gcs); d.hasLocation=cnPosition(uav);
  groundPosition(d, uint8_t(cn.gcs_pos_type) <= 1 ? uint8_t(cn.gcs_pos_type) : 255,
                 d.opLat, d.opLon, d.hasOperatorLocation, f.received, uint8_t(cn.coord_system));
  d.hasGcsAltitude=u16(gcs+8)!=0;
  const uint8_t *alt=uav+12+((f.payload[4]&0x10)?2:0)+((f.payload[4]&0x08)?1:0);
  d.hasAltitude=u16(alt)!=0; d.altitude=cn.drone_alt;
  d.hasSpeed=u16(uav+10)!=0xFFFF;d.speed=cn.ground_speed;
  d.hasHeading=u16(uav+8)!=0xFFFF;d.heading=cn.track_angle;
  const uint8_t *optional=uav+12;
  d.hasRelativeAltitude=cn.has_rel_alt && u16(optional)!=0;d.relativeAltitude=cn.rel_alt;
  if(cn.has_rel_alt)optional+=2;
  d.hasVerticalSpeed=cn.has_v_speed && *optional!=0xFF;d.verticalSpeed=cn.vertical_speed;
  if(cn.has_v_speed)++optional;
  d.hasPressureAltitude=cn.has_pressure_alt && u16(optional+2)!=0;d.pressureAltitude=cn.pressure_alt;
  d.flightStatus=uint8_t(cn.op_status);d.cnVersion=f.payload[1]&31;++d.cnPackets;
  d.locationReceived=d.systemReceived=true; d.locationSeen=d.systemSeen=f.received;
}
inline void decode(Device &d, const uint8_t *m, uint32_t now) {
  d.type = m[0] >> 4;
  if(d.type<=5){d.messageMask|=uint8_t(1u<<d.type);++d.messageCounts[d.type];}
  if (d.type == 0) {
    d.uaType=m[1]&15;d.uaTypeReceived=true;
    const uint8_t idType = m[1] >> 4;
    // Preserve serial number when a second Basic ID carries a registration ID.
    if (idType == 0 || (d.idType == 1 && idType != 1)) return;
    if(idType==1 && d.uasId[0]) {
      char incoming[21]{};textId(incoming,m+2);
      if(strcmp(incoming,d.uasId)){d.selfDescription[0]=0;d.selfIdReceived=false;}
    }
    d.idType = idType;
    if (idType == 1 || idType == 2) textId(d.uasId, m + 2);
    else {
      static const char hex[] = "0123456789ABCDEF";
      for (size_t i = 0; i < 20; ++i) {
        d.uasId[2*i] = hex[m[i+2] >> 4];
        d.uasId[2*i+1] = hex[m[i+2] & 15];
      }
      d.uasId[40] = 0;
    }
  } else if (d.type == 1) {
    int32_t lat = i32(m + 5), lon = i32(m + 9);
    // Keep wire telemetry from compatible domestic firmware, independently of
    // GNSS coordinate validity. Do not replace invalid sentinels with zero.
    ODID_Location_encoded encoded{};memcpy(&encoded,m,MessageSize);
    ODID_Location_data location{};
    if(decodeLocationMessage(&location,&encoded)==ODID_SUCCESS){
      d.hasHeading=location.Direction>=0 && location.Direction<=MAX_DIR;d.heading=location.Direction;
      d.hasSpeed=location.SpeedHorizontal>=MIN_SPEED_H && location.SpeedHorizontal<=MAX_SPEED_H;d.speed=location.SpeedHorizontal;
      d.hasVerticalSpeed=location.SpeedVertical>=MIN_SPEED_V && location.SpeedVertical<=MAX_SPEED_V;d.verticalSpeed=location.SpeedVertical;
      // Height relative to ground is not a takeoff-relative altitude.
      d.hasRelativeAltitude=u16(m+17)!=0 && location.HeightType==ODID_HEIGHT_REF_OVER_TAKEOFF;d.relativeAltitude=location.Height;
      d.hasPressureAltitude=u16(m+13)!=0;d.pressureAltitude=location.AltitudeBaro;
      d.flightStatus=uint8_t(location.Status);
    }
    d.locationReceived = true;
    d.locationSeen = now;
    d.hasLocation = position(lat, lon);
    d.lat = double(lat) / 1e7;
    d.lon = double(lon) / 1e7;
    d.hasAltitude = u16(m + 15) != 0;
    d.altitude = float(u16(m + 15)) * 0.5f - 1000.0f;
  } else if (d.type == 3) {
    d.selfIdReceived=true;d.selfDescriptionType=m[1];
    size_t n=0;while(n<23 && m[n+2])++n;
    if(lab::validText(reinterpret_cast<const char *>(m+2),n))memcpy(d.selfDescription,m+2,n);
    else for(size_t i=0;i<n;++i)d.selfDescription[i]=m[i+2]>=32 && m[i+2]<127?char(m[i+2]):'?';
    d.selfDescription[n]=0;
  } else if (d.type == 4) {
    int32_t lat = i32(m + 2), lon = i32(m + 6);
    d.operatorLocationType = m[1] & 3;
    d.systemReceived = true;
    d.systemSeen = now;
    d.hasOperatorLocation = position(lat, lon);
    d.opLat = double(lat) / 1e7;
    d.opLon = double(lon) / 1e7;
    groundPosition(d, d.operatorLocationType, d.opLat, d.opLon,
                   d.hasOperatorLocation, now, 0);
  } else if (d.type == 5) textId(d.operatorId, m + 2);
}
struct Table {
  Device devices[MaxDevices]{};
  size_t count = 0;
  uint32_t packets = 0;
  void clear() { count = 0; packets = 0; }
  void expire(uint32_t now) {
    for (size_t i = 0; i < count;)
      if (uint32_t(now - devices[i].lastSeen) >= 60000) devices[i] = devices[--count];
      else ++i;
  }
  bool accept(const Frame &f) {
    DroneRIDData_t cn{};
    if (f.length > MaxPayload || f.protocol > 1 ||
        (f.protocol ? !validCn(f.payload,f.length,cn) : !validPayload(f.payload,f.length))) return false;
    size_t index = count;
    for (size_t i = 0; i < count; ++i)
      if (devices[i].source == f.source && devices[i].protocol == f.protocol && memcmp(devices[i].mac, f.mac, 6) == 0) { index = i; break; }
    if (index == count) {
      if (count < MaxDevices) ++count;
      else {
        index = 0;
        for (size_t i = 1; i < count; ++i)
          if (uint32_t(f.received - devices[i].lastSeen) >
              uint32_t(f.received - devices[index].lastSeen)) index = i;
      }
      devices[index] = Device{};
      memcpy(devices[index].mac, f.mac, 6);
      devices[index].source = f.source;
      devices[index].protocol = f.protocol;
    }
    Device &d = devices[index];
    d.transport=f.transport; d.channel=f.channel; d.phy=f.phy;
    d.rssi = f.rssi;
    d.lastSeen = f.received;
    ++d.packets; ++packets; // count received transport packets, not submessages
    if (f.protocol) decodeCn(d,f,cn);
    else if ((f.payload[0] >> 4) == 15) {
      for (uint8_t i = 0; i < f.payload[2]; ++i) decode(d, f.payload + 3 + size_t(i) * MessageSize, f.received);
      d.type = 15;
    } else decode(d, f.payload, f.received);
    return true;
  }
};
// Inputs exclude the four-byte 802.11 FCS. Returns matching complete ASTM IE.
inline bool wifiPayload(const uint8_t *frame, size_t n, const uint8_t *&out, size_t &length) {
  if (!frame || n < 36 || ((frame[0] & 0xFC) != 0x80 && (frame[0] & 0xFC) != 0x50)) return false;
  for (size_t pos = 36; pos + 2 <= n;) {
    size_t len = frame[pos + 1];
    if (pos + 2 + len > n) return false;
    const uint8_t *ie = frame + pos + 2;
    if (frame[pos] == 221 && len >= 6 && ie[0] == 0xFA && ie[1] == 0x0B &&
        ie[2] == 0xBC && ie[3] == 0x0D) {
      out = ie + 5; length = len - 5; // OUI, type, counter
      return length <= MaxPayload && validPayload(out, length);
    }
    pos += 2 + len;
  }
  return false;
}
} // namespace rid
