#pragma once
#include <cstddef>
#include <cstdint>

namespace lab {
struct RadioConfig {
  uint32_t version=1;
  int16_t wifiQuarterDbm=8;
  int8_t bleDbm=-12;
  uint8_t wifiScan=1,bleScan=1;
  int8_t rssiFloor=-127; // -127 means no software RSSI rejection.
};
constexpr int WifiPowerRequests[]={8,20,28,34,44,52,56,60,66,72,78,80,84};
constexpr int BlePowerRequests[]={-24,-21,-18,-15,-12,-9,-6,-3,0,3,6,9,12,15,18,20};
struct PowerOption {int requested=0,actual=0;};
template<size_t N> struct PowerOptions {
  PowerOption items[N]{};size_t count=0;
  void add(int requested,int actual){
    for(size_t i=0;i<count;++i)if(items[i].actual==actual)return;
    if(count<N)items[count++]={requested,actual};
  }
  bool accepts(int requested)const{for(size_t i=0;i<count;++i)if(items[i].requested==requested)return true;return false;}
};
inline bool valid(const RadioConfig &c){
  bool wifi=false,ble=false;
  for(int n:WifiPowerRequests)wifi|=c.wifiQuarterDbm==n;
  for(int n:BlePowerRequests)ble|=c.bleDbm==n;
  return c.version==1 && wifi && ble && c.wifiScan<=2 && c.bleScan<=2 &&
    (c.rssiFloor==-127 || c.rssiFloor==-100 || c.rssiFloor==-90 || c.rssiFloor==-80 || c.rssiFloor==-70 || c.rssiFloor==-60);
}
inline uint16_t wifiDwellMs(uint8_t mode){return mode==0?25:mode==2?60:40;}
inline uint16_t bleWindowUnits(uint8_t mode){return mode==0?32:mode==2?96:64;}
constexpr uint16_t BleIntervalUnits=320; // 0.625 ms units; per PHY, subject to radio coexistence.
constexpr uint16_t WifiHomeMs=300;
inline bool acceptsRssi(int rssi,int floor){return floor==-127 || rssi>=floor;}
} // namespace lab
