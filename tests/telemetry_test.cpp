#include "tx_codec.h"
#include "simulation.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <set>
#include <string>
#include <type_traits>

static ODID_Location_data location(const uint8_t *wire) {
  ODID_Location_data out;
  assert(decodeLocationMessage(&out, reinterpret_cast<const ODID_Location_encoded *>(wire))==ODID_SUCCESS);
  return out;
}
static void near(double actual, double expected, double tolerance) { assert(fabs(actual-expected)<=tolerance); }
static void accuracy(const ODID_Location_data &p, bool synced=true) {
  assert(p.HorizAccuracy==ODID_HOR_ACC_10_METER);
  assert(p.VertAccuracy==ODID_VER_ACC_3_METER);
  assert(p.SpeedAccuracy==ODID_SPEED_ACC_1_METERS_PER_SECOND);
  assert(p.TSAccuracy==(synced?ODID_TIME_ACC_0_1_SECOND:ODID_TIME_ACC_UNKNOWN));
  assert(p.HeightType==ODID_HEIGHT_REF_OVER_TAKEOFF);
}
static void timeTests() {
  lab::BroadcastClock clock;
  assert(!clock.at(100).valid());
  assert(!clock.sync(lab::BroadcastClock::MinUnixMs-1,10));
  assert(!clock.sync(lab::BroadcastClock::MaxUnixMs,10));
  constexpr uint64_t hour=1793725200000ULL;
  static_assert(hour%3600000==0,"fixture on UTC hour boundary");
  assert(clock.sync(hour+3599999,0xfffffff0u));
  near(clock.at(0xfffffff0u).seconds,3599.9,0.001);
  auto t=clock.at(0x54u); // 100 ms across millis() wrap
  assert(t.valid());near(t.seconds,0,0.001);
  assert(t.since2019==(hour+3600000)/1000-1546300800ULL);
  assert(!clock.at(uint32_t(0xfffffff0u+86400001u)).valid());
  assert(clock.sync(hour+123456,400));
  assert(!clock.sync(0,500)); // rejected sync preserves last valid anchor
  near(clock.at(500).seconds,123.5,0.001);
  std::puts("PASS clock: UTC hour rollover, millis rollover, expiry, invalid sync");
}
static void configTests() {
  static_assert(std::is_trivially_copyable<lab::Config>::value,"NVS payload must be trivially copyable");
  lab::ConfigV2 old;strcpy(old.uasId,"TEST-SAVED-ID");strcpy(old.operatorId,"TEST-SAVED-OP");
  old.lat=37.25;old.lon=112.5;old.altitude=3586.5;old.opAltitude=3500;old.duration=120;
  lab::Config next;assert(lab::restoreConfig(&old,sizeof(old),next));
  assert(next.version==3 && !strcmp(next.uasId,old.uasId) && !strcmp(next.operatorId,old.operatorId));
  assert(next.lat==old.lat && next.lon==old.lon && next.altitude==old.altitude && next.opAltitude==old.opAltitude && next.duration==old.duration);
  assert(next.height==30 && next.speed==0 && next.verticalSpeed==0 && next.heading==0);
  next.height=120;next.speed=15.25;next.heading=270;next.verticalSpeed=-1.5;
  lab::Config restored;assert(lab::restoreConfig(&next,sizeof(next),restored));
  assert(restored.height==120 && restored.speed==15.25 && restored.heading==270 && restored.verticalSpeed==-1.5);
  auto unchanged=restored;next.speed=INFINITY;
  assert(!lab::restoreConfig(&next,sizeof(next),restored));assert(restored.speed==unchanged.speed);
  assert(!lab::restoreConfig(&old,sizeof(old)-1,restored));old.version=99;assert(!lab::restoreConfig(&old,sizeof(old),restored));
  for(auto field:{&lab::Config::height,&lab::Config::speed,&lab::Config::verticalSpeed,&lab::Config::heading}){
    lab::Config c;c.*field=NAN;assert(!lab::valid(c));c.*field=INFINITY;assert(!lab::valid(c));
  }
  lab::Config c;c.height=-1000;assert(!lab::valid(c));c={};c.speed=-0.25;assert(!lab::valid(c));c={};c.verticalSpeed=62.5;assert(!lab::valid(c));c={};c.heading=361;assert(!lab::valid(c));
  std::printf("PASS config: v2 migration and v3 round trip, invalid data; sizes %zu/%zu\n",sizeof(old),sizeof(next));
}
static void singleTests() {
  lab::Config c;c.lat=37.865271;c.lon=112.579663;c.altitude=3586.5;c.height=86.5;c.speed=15.25;c.verticalSpeed=-1.5;c.heading=270;
  const lab::TelemetryTime time{123.4f,247000123};lab::Messages m;assert(m.build(c,time));
  auto p=location(m.data[1]);accuracy(p);near(p.Latitude,c.lat,1e-7);near(p.Longitude,c.lon,1e-7);
  near(p.Height,86.5,0.001);near(p.AltitudeGeo,3586.5,0.001);near(p.Direction,270,0.001);near(p.SpeedHorizontal,15.25,0.001);near(p.SpeedVertical,-1.5,0.001);near(p.TimeStamp,123.4,0.001);
  assert(p.Status==ODID_STATUS_AIRBORNE);
  // Check the actual Location wire fields, including accuracy nibbles, independently.
  assert(m.data[1][19]==0x5A && m.data[1][20]==0x03 && m.data[1][23]==0x01);
  assert(m.data[1][21]==0xD2 && m.data[1][22]==0x04);
  uint8_t ble[31],wifi[110];m.ble(ble,1,17);m.wifi(wifi,18);
  assert(ble[0]==30 && ble[5]==17 && memcmp(ble+6,m.data[1],25)==0);
  assert(wifi[1]+2==sizeof(wifi) && wifi[7]==0xF2 && wifi[9]==4);
  ODID_UAS_Data decoded;odid_initUasData(&decoded);
  assert(decodeMessagePack(&decoded,reinterpret_cast<const ODID_MessagePack_encoded *>(wifi+7))==ODID_SUCCESS);
  assert(decoded.LocationValid && decoded.SystemValid && decoded.BasicIDValid[0] && decoded.OperatorIDValid);
  accuracy(decoded.Location);assert(decoded.System.Timestamp==time.since2019);
  assert(m.build(c));p=location(m.data[1]);accuracy(p,false);assert(p.TimeStamp==INV_TIMESTAMP);
  c.height=0;c.speed=0;c.verticalSpeed=0;c.heading=360;assert(m.build(c,time));p=location(m.data[1]);assert(p.Status==ODID_STATUS_GROUND && p.Direction==0 && p.SpeedHorizontal==0 && p.SpeedVertical==0 && p.Height==0);
  for(float speed:{0.f,63.75f,64.5f,254.25f}){c.speed=speed;assert(m.build(c,time));near(location(m.data[1]).SpeedHorizontal,speed,0.001);}
  for(float height:{-999.5f,31767.5f}){c.height=height;assert(m.build(c,time));near(location(m.data[1]).Height,height,0.001);}
  for(float vertical:{-62.f,62.f}){c.verticalSpeed=vertical;assert(m.build(c,time));near(location(m.data[1]).SpeedVertical,vertical,0.001);}
  c.lat=c.lon=0;assert(m.build(c,time));assert(location(m.data[1]).HorizAccuracy==ODID_HOR_ACC_UNKNOWN);
  std::puts("PASS single: eight requested telemetry fields encoded over BLE and Wi-Fi, stationary zeroes, boundary values");
}
static void fleetTests() {
  lab::BroadcastClock clock;assert(clock.sync(1793728799900ULL,0));
  unsigned packets=0;
  for(uint16_t count:{1,100})for(double centerLat:{31.23,85.0,-85.0}) {
    lab::SimConfig c;c.count=count;c.centerLat=centerLat;c.centerLon=179.99;c.seed=12345;
    for(uint32_t elapsed:{0u,100u,17000u,599999u}) {
      std::set<std::string> ids;
      for(uint16_t i=0;i<count;++i){
        lab::SimDevice d;assert(lab::simulatedDevice(c,i,elapsed,d));assert(ids.insert(d.flight.uasId).second);
        for(bool takeoff:{false,true}){
          lab::SimMessages m;assert(m.build(d,takeoff,clock.at(elapsed)));
          uint8_t ble[lab::SimMessages::AdSize],wifi[lab::SimMessages::IeSize];m.ble(ble,5);m.wifi(wifi,6);
          for(const uint8_t *pack:{ble+6,wifi+7}){
            ODID_UAS_Data decoded;odid_initUasData(&decoded);assert(decodeMessagePack(&decoded,reinterpret_cast<const ODID_MessagePack_encoded *>(pack))==ODID_SUCCESS);
            assert(decoded.LocationValid && decoded.SystemValid && decoded.SelfIDValid);
            const auto &p=decoded.Location;accuracy(p);assert(p.Status==ODID_STATUS_AIRBORNE);
            near(p.Height,d.flight.altitude-d.takeoffAltitude,0.251);near(p.AltitudeGeo,d.flight.altitude,0.251);
            near(p.SpeedHorizontal,d.speed,0.126);near(p.SpeedVertical,d.verticalSpeed,0.251);
            double angle=fabs(p.Direction-d.heading);assert(fmin(angle,360-angle)<=0.501);
            near(p.TimeStamp,clock.at(elapsed).seconds,0.001);
            assert(decoded.System.Timestamp==clock.at(elapsed).since2019);
            assert(decoded.System.OperatorLocationType==(takeoff?ODID_OPERATOR_LOCATION_TYPE_TAKEOFF:ODID_OPERATOR_LOCATION_TYPE_FIXED));
            near(decoded.System.OperatorLatitude,takeoff?d.takeoffLat:d.flight.opLat,1e-7);
            assert(strncmp(decoded.SelfID.Desc,"SIM|",4)==0);++packets;
          }
        }
      }
    }
  }
  std::printf("PASS fleet: %u decoded BLE/Wi-Fi packets, 1/100 devices, poles/dateline, takeoff/operator, continuous motion\n",packets);
}
int main(){timeTests();configTests();singleTests();fleetTests();}
