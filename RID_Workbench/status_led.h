#pragma once
#include <stdint.h>
namespace lab {
enum class LedMode : uint8_t { Off, Detect, Transmit };
inline LedMode indicatorMode(bool monitoring,bool transmitting) {
  return transmitting?LedMode::Transmit:monitoring?LedMode::Detect:LedMode::Off;
}
inline const char *ledModeName(LedMode mode) {
  return mode==LedMode::Detect?"detect":mode==LedMode::Transmit?"transmit":"off";
}
struct LedColor {
  uint8_t r=0,g=0,b=0;
  bool operator!=(const LedColor &other) const {return r!=other.r || g!=other.g || b!=other.b;}
};
struct StatusLed {
  static constexpr uint32_t PeriodMs=2000, OnMs=1000;
  static constexpr uint8_t Brightness=16;
  LedMode mode=LedMode::Off;
  uint32_t changedAt=0;
  LedColor color{};
  LedColor update(LedMode next,uint32_t now) {
    if(next!=mode){mode=next;changedAt=now;}
    const bool on=mode!=LedMode::Off && uint32_t(now-changedAt)%PeriodMs<OnMs;
    color={uint8_t(on && mode==LedMode::Transmit?Brightness:0),uint8_t(on && mode==LedMode::Detect?Brightness:0),0};
    return color;
  }
};
} // namespace lab
