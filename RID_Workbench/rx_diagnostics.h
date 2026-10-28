#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
namespace rid {
// Host-testable, bounded capture store. Only the loop task writes this store;
// radio callbacks use a separate non-blocking FreeRTOS queue.
constexpr size_t CaptureBytes=1650,CaptureSlots=128,CapturePerAddress=16;
struct RawReport {
  uint32_t at=0;
  uint8_t mac[6]{};
  uint8_t source=0,kind=0,channel=0,phy=0,status=0;
  uint8_t addressType=255,sid=255,primaryPhy=0,secondaryPhy=0;uint16_t properties=0;
  int8_t rssi=0;
  uint16_t original=0,length=0;
  uint8_t data[CaptureBytes]{};
};
inline uint32_t reportHash(const RawReport &r) {
  uint32_t h=2166136261u;
  for(size_t i=0;i<r.length;++i){h^=r.data[i];h*=16777619u;}
  return h;
}
struct CaptureEntry {RawReport report;uint32_t hash=0,repeats=0,firstAt=0;};
struct CaptureStore {
  CaptureEntry *entries=nullptr;size_t count=0;uint32_t replaced=0;
  void clear(){count=0;replaced=0;}
  void accept(const RawReport &r) {
    if(!entries || r.length>CaptureBytes)return;
    const uint32_t hash=reportHash(r);size_t chosen=count,sameMac=0,oldestMac=0;
    for(size_t i=0;i<count;++i){
      auto &e=entries[i];
      if(e.report.source==r.source && !memcmp(e.report.mac,r.mac,6)){
        if(!sameMac || uint32_t(r.at-e.report.at)>uint32_t(r.at-entries[oldestMac].report.at))oldestMac=i;
        ++sameMac;
        if(e.report.kind==r.kind && e.report.status==r.status && e.report.sid==r.sid && e.report.addressType==r.addressType && e.report.properties==r.properties && e.report.primaryPhy==r.primaryPhy && e.report.secondaryPhy==r.secondaryPhy && e.report.original==r.original && e.report.length==r.length && e.hash==hash && !memcmp(e.report.data,r.data,r.length)){
          e.report.at=r.at;e.report.rssi=r.rssi;++e.repeats;return;
        }
      }
    }
    // Avoid a single busy advertiser filling the entire capture.
    if(sameMac>=CapturePerAddress){chosen=oldestMac;++replaced;}
    else if(count<CaptureSlots)++count;
    else {
      // Reserve up to half the store for BLE so busy access points cannot
      // evict every Bluetooth sample during an A1 capture.
      size_t ble=0;for(size_t i=0;i<count;++i)if(!entries[i].report.source)++ble;
      chosen=count;
      for(size_t i=0;i<count;++i){
        if(entries[i].report.source==0 && ble<=CaptureSlots/2 && r.source)continue;
        if(!r.source && ble<CaptureSlots/2 && !entries[i].report.source)continue;
        if(chosen==count || uint32_t(r.at-entries[i].report.at)>uint32_t(r.at-entries[chosen].report.at))chosen=i;
      }
      if(chosen==count){++replaced;return;}
      ++replaced;
    }
    entries[chosen]={r,hash,1,r.at};
  }
};
}
