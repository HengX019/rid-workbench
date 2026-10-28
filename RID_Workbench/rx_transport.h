#pragma once
#include <initializer_list>
#include "rid_decoder.h"
namespace rid {
// Emits complete protocol payloads only. Unknown CN carrier identifiers are
// accepted inside typed vendor/service containers after mandatory-field checks.
template<class Emit> void cnCandidates(const uint8_t *p,size_t n,Emit emit) {
  for(size_t i=0;i+6<=n;++i) { size_t len=cnPacketSize(p+i,n-i); if(len) {emit(p+i,len,uint8_t(1)); i+=len-1;} }
}
template<class Emit> void advertising(const uint8_t *p,size_t n,Emit emit) {
  for(size_t pos=0;pos<n;) {
    size_t len=p[pos]; if(!len || len>n-pos-1) return;
    const uint8_t *ad=p+pos+1;
    // Equivalent Bluetooth service UUID encodings. Device models are never
    // inferred from a MAC/OUI or an unverified serial prefix.
    static const uint8_t uuid128[16]={0xFB,0x34,0x9B,0x5F,0x80,0,0,0x80,0,0x10,0,0,0xFA,0xFF,0,0};
    size_t servicePrefix=0;
    if(len>=3 && ad[0]==0x16 && ad[1]==0xFA && ad[2]==0xFF)servicePrefix=3;
    else if(len>=5 && ad[0]==0x20 && ad[1]==0xFA && ad[2]==0xFF && !ad[3] && !ad[4])servicePrefix=5;
    else if(len>=17 && ad[0]==0x21 && !memcmp(ad+1,uuid128,16))servicePrefix=17;
    if(servicePrefix && len>=servicePrefix+2 && ad[servicePrefix]==0x0D && validPayload(ad+servicePrefix+2,len-servicePrefix-2))emit(ad+servicePrefix+2,len-servicePrefix-2,uint8_t(0));
    // GB defines its own 0xFF type byte. Accept a complete GB packet that
    // uses the AD manufacturer type as that byte, or is nested after a company
    // identifier. Mandatory flags, exact field lengths and ASCII still apply.
    if(ad[0]==0xFF)cnCandidates(ad,len,emit);
    size_t prefix=ad[0]==0x16?3:ad[0]==0x20?5:ad[0]==0x21?17:0;
    if(prefix && len>prefix) cnCandidates(ad+prefix,len-prefix,emit);
    pos+=len+1;
  }
}
template<class Emit> void nanDescriptor(const uint8_t *p,size_t n,Emit emit) {
  static const uint8_t service[6]={0x88,0x69,0x19,0x9D,0x92,0x09};
  if(n<9) return;
  bool odid=!memcmp(p,service,6);
  uint8_t control=p[8]; size_t pos=9;
  if(control&0x40) {if(n-pos<2)return;pos+=2;}
  for(uint8_t bit: {uint8_t(4),uint8_t(8)}) if(control&bit) {if(pos>=n)return;size_t len=p[pos++];if(len>n-pos)return;pos+=len;}
  if(!(control&0x10) || pos>=n) return;
  size_t len=p[pos++]; if(!len || len>n-pos) return;
  // ASTM service info starts with one-byte message counter.
  if(odid && validPayload(p+pos+1,len-1)) emit(p+pos+1,len-1,uint8_t(0));
  cnCandidates(p+pos,len,emit);
}
template<class Emit> void wifiFrame(const uint8_t *p,size_t n,Emit emit) {
  if(!p || n<24 || (p[0]&0x0C)!=0 || (p[1]&0x40))return;
  uint8_t subtype=p[0]&0xFC;
  if(subtype==0x80 || subtype==0x50) {
    if(n<36)return;
    for(size_t pos=36;pos+2<=n;) {
      size_t len=p[pos+1];if(len>n-pos-2)return;const uint8_t *ie=p+pos+2;
      if(p[pos]==221 && len>=4) {
        if(len>=6 && ie[0]==0xFA && ie[1]==0x0B && ie[2]==0xBC && ie[3]==0x0D && validPayload(ie+5,len-5))emit(ie+5,len-5,uint8_t(0),uint8_t(2));
        cnCandidates(ie,len,[&](const uint8_t *d,size_t z,uint8_t proto){emit(d,z,proto,uint8_t(2));});
      }
      // A GB type byte can be carried as an extension element's ID. Only
      // emit a complete, validated GB packet; unrelated HE extension IEs fail.
      if(p[pos]==255 && len>=5){
        uint8_t gb[MaxPayload];if(len+1<=sizeof(gb)){
          gb[0]=255;memcpy(gb+1,ie,len);
          const size_t size=cnPacketSize(gb,len+1);
          if(size==len+1)emit(gb,size,uint8_t(1),uint8_t(2));
        }
      }
      pos+=2+len;
    }
  } else if(subtype==0xD0 && n>=30) {
    static const uint8_t nan[6]={4,9,0x50,0x6F,0x9A,0x13};
    if(!memcmp(p+24,nan,6)) {
      for(size_t pos=30;pos+3<=n;) {
        size_t len=u16(p+pos+1);if(len>n-pos-3)return;
        if(p[pos]==3)nanDescriptor(p+pos+3,len,[&](const uint8_t *d,size_t z,uint8_t proto){emit(d,z,proto,uint8_t(3));});
        pos+=3+len;
      }
    } else if(p[24]==4 && p[25]==9 && n>=30) {
      cnCandidates(p+26,n-26,[&](const uint8_t *d,size_t z,uint8_t proto){emit(d,z,proto,uint8_t(2));});
    } else if(p[24]==127 && n>=28) {
      cnCandidates(p+28,n-28,[&](const uint8_t *d,size_t z,uint8_t proto){emit(d,z,proto,uint8_t(2));});
    }
  }
}
// NimBLE can deliver extended advertising as multiple HCI reports. No heap use.
struct Fragments {
  static constexpr size_t Capacity=1650;
  struct Slot {uint8_t key[8]{};uint32_t time=0;size_t size=0;bool active=false;uint8_t data[Capacity];} slots[4]{};
  uint32_t discarded=0,truncated=0;
  const uint8_t *accept(const uint8_t key[8],uint8_t status,const uint8_t *p,size_t n,uint32_t now,size_t &length) {
    Slot *slot=nullptr,*free=nullptr;
    for(auto &s:slots){if(s.active && uint32_t(now-s.time)>1500)s.active=false;if(s.active&&!memcmp(s.key,key,8))slot=&s;if(!s.active)free=&s;}
    if(status>2 || n>Capacity) {if(slot)slot->active=false;++discarded;return nullptr;}
    // A truncated report can still contain complete AD elements before the
    // truncated final element. The AD iterator emits complete elements only.
    if(status==2)++truncated;
    if(!slot && status!=1){length=n;return p;}
    if(!slot){slot=free;if(!slot){++discarded;return nullptr;}slot->active=true;slot->size=0;memcpy(slot->key,key,8);}
    if(n>Capacity-slot->size){slot->active=false;++discarded;return nullptr;}
    memcpy(slot->data+slot->size,p,n);slot->size+=n;slot->time=now;
    if(status==1)return nullptr;
    length=slot->size;slot->active=false;return slot->data;
  }
};
}
