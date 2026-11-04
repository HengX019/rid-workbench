#pragma once
#include "tx_codec.h"
#include "model_codes.h"
#include <stdio.h>

namespace lab {
constexpr uint16_t SimCount = 100;
constexpr double SimRadius = 10000.0, SimPi = 3.14159265358979323846, SimEarth = 6378137.0;
constexpr uint32_t SimWifiSlotMs = 10, SimBleSlotMs = 80;
// Wire labels fit Self-ID's 23-byte field including the explicit SIM| prefix.
constexpr const char *SimModels[] = {"mini5pro","air3s","mavic4pro","antigravitya1",
  "evolite","mini4pro","air3","mavic3pro","evoiipro","avata2",
  "avata360","mavic3","litox1","lito1","neo2",
  "neo","mini3pro","mavic3classic","air2s","avata"};
constexpr uint8_t SimModelCount = sizeof(SimModels)/sizeof(SimModels[0]);
inline bool knownSimModel(const char *s) { for(auto name:SimModels)if(!strcmp(name,s))return true;return false; }
inline const char *simModelPrefix(const char *model) {
  for(const auto &entry:rid::SerialModels)
    if(entry.model[0] && !strcmp(entry.model,model))return entry.prefix;
  return nullptr;
}
struct SimConfig {
  uint32_t seed = 1;
  uint16_t duration = 60, count = 100;
  double centerLat = 31.2300, centerLon = 121.4700;
  int8_t model = -1; // -1 = balanced, seed-shuffled model labels
  bool wifi = true, ble = true;
};
inline bool valid(const SimConfig &c) {
  return c.seed && c.duration >= 30 && c.duration <= 600 && (c.wifi || c.ble) &&
    (c.count==1 || c.count==100) && isfinite(c.centerLat) && isfinite(c.centerLon) &&
    fabs(c.centerLat)<=85 && fabs(c.centerLon)<=180 && c.model>=-1 && c.model<SimModelCount;
}
inline uint32_t simSlotMs(const SimConfig &c,bool ble) {
  const uint32_t minimum=ble?SimBleSlotMs:SimWifiSlotMs, rate=1000/(2*c.count);
  return rate>minimum?rate:minimum;
}
struct SimDevice {
  Config flight;
  char model[21]{};
  double takeoffLat = 0, takeoffLon = 0;
  float takeoffAltitude = 0;
  float speed = 0, verticalSpeed = 0, heading = 0;
  uint8_t wifiMac[6]{}, bleMac[6]{};
};
// Spherical exponential map: offsets are distances from the selected WGS-84
// coordinate, with no longitude discontinuity in the motion or its derivative.
inline void simProject(double lat,double lon,double x,double y,double vx,double vy,
                       double &outLat,double &outLon,double &outVe,double &outVn) {
  double p=lat*SimPi/180,l=lon*SimPi/180,r=hypot(x,y);
  double c[3]={cos(p)*cos(l),cos(p)*sin(l),sin(p)};
  double e[3]={-sin(l),cos(l),0},n[3]={-sin(p)*cos(l),-sin(p)*sin(l),cos(p)};
  double pos[3],vel[3];
  for(int k=0;k<3;++k){
    if(r<1e-8){pos[k]=c[k];vel[k]=vx*e[k]+vy*n[k];continue;}
    double u=(x*e[k]+y*n[k])/r,rdot=(x*vx+y*vy)/r;
    double udot=(vx*e[k]+vy*n[k]-u*rdot)/r;
    pos[k]=cos(r/SimEarth)*c[k]+sin(r/SimEarth)*u;
    vel[k]=-sin(r/SimEarth)*rdot*c[k]+cos(r/SimEarth)*rdot*u+SimEarth*sin(r/SimEarth)*udot;
  }
  p=atan2(pos[2],hypot(pos[0],pos[1]));l=atan2(pos[1],pos[0]);
  outLat=p*180/SimPi;outLon=l*180/SimPi;
  outVe=-vel[0]*sin(l)+vel[1]*cos(l);
  outVn=-vel[0]*sin(p)*cos(l)-vel[1]*sin(p)*sin(l)+vel[2]*cos(p);
}
inline bool simulatedDevice(const SimConfig &cfg, uint16_t index, uint32_t elapsed, SimDevice &d) {
  if (!valid(cfg) || index >= cfg.count) return false;
  uint32_t seed=cfg.seed;
  d = SimDevice{};
  snprintf(d.flight.operatorId,sizeof(d.flight.operatorId),"SIM-OP-%03u",unsigned(index+1));
  // Coprime permutation of 0..99: twenty models each occur five times in mixed fleets.
  unsigned model=cfg.model<0?(((index*37+seed%100)%100)%SimModelCount):unsigned(cfg.model);
  snprintf(d.model,sizeof(d.model),"%s",SimModels[model]);
  if(const char *prefix=simModelPrefix(d.model))
    snprintf(d.flight.uasId,sizeof(d.flight.uasId),"%sI%08lX%03u",prefix,(unsigned long)seed,unsigned(index+1));
  else
    snprintf(d.flight.uasId,sizeof(d.flight.uasId),"SIM-%08lX-%03u",(unsigned long)seed,unsigned(index+1));
  const double rotation=double(seed&65535)/65536.0*2*SimPi;
  const double radius=cfg.count==1?0:9700*sqrt((index+0.5)/cfg.count);
  const double angle=index*2.399963229728653+rotation;
  const double anchorE=radius*cos(angle),anchorN=radius*sin(angle);
  double unusedE,unusedN;
  simProject(cfg.centerLat,cfg.centerLon,anchorE,anchorN,0,0,d.takeoffLat,d.takeoffLon,unusedE,unusedN);
  d.takeoffAltitude = 10 + (index%10)*0.5f;
  // Bounded smooth paths with analytic derivatives. Max displacement < 140 m;
  // the outer anchors are < 9700 m, so even the outer tracks turn inside 10 km.
  const double t=double(elapsed)/1000.0, omega=2*SimPi/(70+(index%9)*7);
  const double phase=index*2.399963229728653+rotation;
  const double theta=omega*t+phase+0.28*sin(omega*t*0.55+phase*0.3);
  const double rate=omega*(1+0.154*cos(omega*t*0.55+phase*0.3));
  const double eastRadius=45+(index%6)*9, northRadius=35+(index%8)*7;
  const double east=eastRadius*cos(theta)+8*sin(2*theta+phase);
  const double north=northRadius*sin(theta)+5*cos(3*theta+phase*0.5);
  const double eastSpeed=(-eastRadius*sin(theta)+16*cos(2*theta+phase))*rate;
  const double northSpeed=(northRadius*cos(theta)-15*sin(3*theta+phase*0.5))*rate;
  double ve,vn;
  simProject(cfg.centerLat,cfg.centerLon,anchorE+east,anchorN+north,eastSpeed,northSpeed,
    d.flight.lat,d.flight.lon,ve,vn);
  const double climbPhase=omega*0.45*t+phase, bobPhase=omega*1.3*t+phase*0.7;
  d.flight.altitude=d.takeoffAltitude+50+(index%12)*3+float(10*sin(climbPhase)+3*sin(bobPhase));
  d.speed=float(hypot(ve,vn));
  d.verticalSpeed=float(10*omega*0.45*cos(climbPhase)+3*omega*1.3*cos(bobPhase));
  d.heading=float(fmod(atan2(ve,vn)*180/SimPi+360.0,360.0));
  d.flight.height=d.flight.altitude-d.takeoffAltitude;
  d.flight.speed=d.speed;d.flight.verticalSpeed=d.verticalSpeed;d.flight.heading=d.heading;
  simProject(cfg.centerLat,cfg.centerLon,anchorE+10,anchorN-20,0,0,
    d.flight.opLat,d.flight.opLon,unusedE,unusedN);
  d.flight.opAltitude=d.takeoffAltitude+1.5f;
  const uint8_t mac[6]={0x02,uint8_t(seed>>16),uint8_t(seed>>8),uint8_t(seed),0x53,uint8_t(index)};
  memcpy(d.wifiMac,mac,6);memcpy(d.bleMac,mac,6);d.bleMac[0]=0xC2;
  return valid(d.flight);
}
// Compatibility for packet fixtures; production uses the complete configuration.
inline bool simulatedDevice(uint32_t seed,uint16_t index,uint32_t elapsed,SimDevice &d) {
  SimConfig c;c.seed=seed;return simulatedDevice(c,index,elapsed,d);
}
struct SimMessages {
  static constexpr size_t Count=5, PackSize=3+Count*25, AdSize=6+PackSize, IeSize=7+PackSize;
  uint8_t data[Count][25]{};
  bool build(const SimDevice &d, bool takeoff, const TelemetryTime &time = {}) {
    Messages base;if (!base.build(d.flight,time)) return false;
    memcpy(data,base.data,sizeof(base.data));
    ODID_System_data system;odid_initSystemData(&system);
    system.OperatorLocationType=takeoff?ODID_OPERATOR_LOCATION_TYPE_TAKEOFF:ODID_OPERATOR_LOCATION_TYPE_FIXED;
    system.OperatorLatitude=takeoff?d.takeoffLat:d.flight.opLat;
    system.OperatorLongitude=takeoff?d.takeoffLon:d.flight.opLon;
    system.OperatorAltitudeGeo=takeoff?d.takeoffAltitude:d.flight.opAltitude;
    system.Timestamp=time.valid()?time.since2019:0;
    ODID_SelfID_data self;odid_initSelfIDData(&self);
    snprintf(self.Desc,sizeof(self.Desc),"SIM|%s",d.model);
    return encodeSystemMessage(reinterpret_cast<ODID_System_encoded *>(data[2]),&system)==ODID_SUCCESS &&
      encodeSelfIDMessage(reinterpret_cast<ODID_SelfID_encoded *>(data[4]),&self)==ODID_SUCCESS;
  }
  void pack(uint8_t *out) const {out[0]=0xF2;out[1]=25;out[2]=Count;memcpy(out+3,data,sizeof(data));}
  void ble(uint8_t *out,uint8_t counter) const {
    const uint8_t prefix[]={uint8_t(AdSize-1),0x16,0xFA,0xFF,0x0D};
    memcpy(out,prefix,5);out[5]=counter;pack(out+6);
  }
  void wifi(uint8_t *out,uint8_t counter) const {
    const uint8_t prefix[]={0xDD,uint8_t(IeSize-2),0xFA,0x0B,0xBC,0x0D};
    memcpy(out,prefix,6);out[6]=counter;pack(out+7);
  }
};
// Complete beacon without FCS. No authentication, data, or disconnection frames.
inline size_t simulatedBeacon(uint8_t *out,size_t capacity,const SimDevice &d,const SimMessages &m,
                              uint8_t counter,uint32_t elapsed,uint8_t channel=6) {
  constexpr size_t size=36+2+13+3+3+6+SimMessages::IeSize;
  if(!out || capacity<size || channel<1 || channel>13)return 0;
  memset(out,0,size);out[0]=0x80;memset(out+4,0xFF,6);
  memcpy(out+10,d.wifiMac,6);memcpy(out+16,d.wifiMac,6);
  uint64_t timestamp=uint64_t(elapsed)*1000;
  for(int i=0;i<8;++i)out[24+i]=uint8_t(timestamp>>(8*i));
  out[32]=0xE8;out[33]=0x03; // nominal beacon interval, 1000 TU
  out[34]=0x01; // ESS capability, no privacy bit; synthetic beacon only
  size_t p=36;out[p++]=0;out[p++]=13;memcpy(out+p,"RID-SIMULATED",13);p+=13;
  out[p++]=1;out[p++]=1;out[p++]=0x82; // 1 Mbps basic rate
  out[p++]=3;out[p++]=1;out[p++]=channel;
  const uint8_t tim[]={5,4,0,1,0,0};memcpy(out+p,tim,sizeof(tim));p+=sizeof(tim);
  m.wifi(out+p,counter);return p+SimMessages::IeSize;
}
struct SimCursor {
  uint16_t index=0;
  bool takeoff=false;
  uint32_t slots=0,cycles=0,cycleStarted=0,lastCycleMs=0;
  uint16_t unique=0;
  void sent(uint32_t now,uint16_t count=SimCount) {
    ++slots;
    if(!takeoff) { if(unique<count)++unique;takeoff=true; }
    else { takeoff=false;if(++index==count){index=0;++cycles;lastCycleMs=uint32_t(now-cycleStarted);cycleStarted=now;} }
  }
};
} // namespace lab
