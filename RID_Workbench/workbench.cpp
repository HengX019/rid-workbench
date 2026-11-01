#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <esp_netif.h>
#include <esp_heap_caps.h>
#include <NimBLEDevice.h>
#if !CONFIG_BT_NIMBLE_EXT_ADV
#error Use the bundled NimBLE-Arduino library with extended advertising enabled.
#endif
#include <Preferences.h>
#include <esp_wifi.h>
#include <atomic>
#include "tx_codec.h"
#include "rid_decoder.h"
#include "rx_transport.h"
#include "rx_diagnostics.h"
#include "broadcast_identity.h"
#include <esp_event.h>
#include "page.h"
#include "navigation.h"
#include "simulation.h"
#include "map_picker.h"
#include "status_led.h"
#include "radio_config.h"

namespace {
constexpr char DefaultSsid[] = "RID-Test-ESP32";
constexpr char DefaultPass[] = "";
constexpr uint8_t Channel = 6;
WebServer server(80);
bool localOnlyDhcp=false;
uint32_t apDisconnects=0;
uint32_t lastHttpAt=0;
std::atomic<bool> scanning{false};
std::atomic<bool> bleReady{false};
QueueHandle_t rxQueue = nullptr;
QueueHandle_t captureQueue=nullptr;
rid::CaptureStore captureStore;
std::atomic<bool> captureActive{false};
std::atomic<bool> captureBleOnly{false};bool captureWifiPaused=false;
enum RxStage {Ble1M,BleCoded,BleMore,BleTruncated,BleAssembled,BadAd,AstPayload,CnPayload,QueuedPayload,DecodedPayload,RejectedPayload,StageCount};
std::atomic<uint32_t> rxStages[StageCount]{};
uint32_t captureBaseline[StageCount]{},captureFinal[StageCount]{};
bool captureHasRun=false;
void endCapture(){
 if(captureActive.exchange(false))for(size_t i=0;i<StageCount;++i)captureFinal[i]=rxStages[i].load();
}
String receptionProgress(){
 static const char *keys[]={"ble1M","bleCoded","bleMore","bleTruncated","bleAssembled","malformedAd","astmPayloads","cnPayloads","queuedPayloads","decodedPayloads","rejectedPayloads"};
 String out="{";for(size_t i=0;i<StageCount;++i){if(i)out+=',';out+='"';out+=keys[i];out+="\":";out+=String((captureHasRun && !captureActive.load()?captureFinal[i]:rxStages[i].load())-captureBaseline[i]);}return out+"}";
}
void restoreCaptureWifi();
void pauseCaptureWifi();
std::atomic<uint32_t> captureOffered{0},captureDrops{0};
uint32_t captureStarted=0,captureEnded=0;
void captureRaw(const uint8_t *mac,uint8_t source,uint8_t kind,int rssi,const uint8_t *p,size_t n,uint8_t channel=0,uint8_t phy=0,uint8_t status=0,uint16_t properties=0,uint8_t sid=255,uint8_t addressType=255,uint8_t primaryPhy=0,uint8_t secondaryPhy=0) {
  if(!captureActive.load() || !captureQueue || !p || !n || (captureBleOnly && source))return;
  rid::RawReport report;report.at=millis();memcpy(report.mac,mac,6);
  report.source=source;report.kind=kind;report.rssi=rssi;report.channel=channel;report.phy=phy;report.status=status;
  report.properties=properties;report.sid=sid;report.addressType=addressType;report.primaryPhy=primaryPhy;report.secondaryPhy=secondaryPhy;
  report.original=n>65535?65535:n;report.length=n>rid::CaptureBytes?rid::CaptureBytes:n;
  memcpy(report.data,p,report.length);++captureOffered;
  if(xQueueSend(captureQueue,&report,0)!=pdTRUE)++captureDrops;
}
void tickCapture() {
  if(captureActive.load() && uint32_t(millis()-captureStarted)>=60000){endCapture();captureEnded=millis();}
  if(!captureActive.load())restoreCaptureWifi();
  rid::RawReport report;
  for(int i=0;captureQueue && i<4 && xQueueReceive(captureQueue,&report,0)==pdTRUE;++i)captureStore.accept(report);
}
bool startCapture(bool bleOnly=false) {
  endCapture();restoreCaptureWifi();captureBleOnly=bleOnly;
  if(!captureQueue)captureQueue=xQueueCreate(16,sizeof(rid::RawReport));
  if(!captureStore.entries)captureStore.entries=static_cast<rid::CaptureEntry *>(heap_caps_calloc(rid::CaptureSlots,sizeof(rid::CaptureEntry),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  if(!captureQueue || !captureStore.entries)return false;
  endCapture();xQueueReset(captureQueue);captureStore.clear();captureOffered.store(0);captureDrops.store(0);
  for(size_t i=0;i<StageCount;++i)captureBaseline[i]=rxStages[i].load();captureHasRun=true;
  captureStarted=millis();captureEnded=0;captureActive.store(true);return true;
}
String captureSummary() {
  const uint32_t elapsed=millis()-captureStarted;
  return String("{\"active\":")+(captureActive.load()?"true":"false")+",\"bleOnly\":"+(captureBleOnly?"true":"false")+",\"samples\":"+String(captureStore.count)+
    ",\"reports\":"+String(captureOffered.load())+",\"queueDrops\":"+String(captureDrops.load())+
    ",\"replaced\":"+String(captureStore.replaced)+",\"remaining\":"+String(captureActive.load() && elapsed<60000?(60000-elapsed)/1000:0)+",\"progress\":"+receptionProgress()+"}";
}
rid::Table table;

std::atomic<uint32_t> dropped{0};
std::atomic<bool> monitoring{false};
bool wifiListening = false;
String ssid, password;
struct ApConfig { uint32_t version = 1; char ssid[33]{}; char pass[64]{}; };
uint8_t apMac[6];
uint32_t rebootAt = 0;
TaskHandle_t probeTask = nullptr;
lab::Config txConfig;
lab::Messages txMessages;
bool txEnabled = false, resumeMonitoring = false, txWifi = false;
uint32_t txStarted = 0, nextBle = 0, lastWifi = 0, txCycles = 0;
uint8_t blePhase = 0, bleCounters[4]{}, wifiCounter = 0;
String txError;
lab::SimConfig simConfig;
lab::SimCursor simWifiCursor, simBleCursor;
bool txSimulation=false;
uint32_t nextSimWifi=0,nextSimBle=0,simFailures=0;
uint8_t statusLedPin=48;
lab::StatusLed statusLed;
lab::LedColor appliedLed{};
std::atomic<uint32_t> bleReports{0}, wifiReports{0};
std::atomic<uint32_t> bleExtendedReports{0}, bleCodedReports{0}, wifiChannelMask{0};
std::atomic<uint32_t> wifiChannels[14]{};
std::atomic<bool> sweepDone{false};
std::atomic<int> sweepStatus{0};
rid::Fragments fragments;
std::atomic<uint32_t> scanGeneration{0};
int bleScanError=0, wifiScanError=0;
uint8_t scanChannel=1;
uint32_t roundStarted=0;
bool sweepActive=false;
uint32_t sweepStarted=0, nextSweep=0, sweeps=0, sweepFailures=0;
lab::RadioConfig radioConfig;
lab::PowerOptions<16> wifiPowerOptions,blePowerOptions;
int wifiActualQuarter=0,bleActualDbm=0;
bool wifiPowerValid=false,blePowerValid=false;
int radioWifiError=0,radioBleError=0;
uint16_t appliedWifiDwell=0,appliedBleWindow=0;
uint32_t lastSweepMs=0;
std::atomic<int> rssiFloor{-127};
std::atomic<uint32_t> filteredReports{0};
std::atomic<int> lastRxChannel{0};
bool bleTxConfigured=false;
bool bleTxActive(){return bleTxConfigured && ble_gap_ext_adv_active(0);}
int stopBle(){return bleTxActive()?ble_gap_ext_adv_stop(0):0;}
void stopTx(const String &reason = "");
void updateStatusLed(bool force=false) {
  auto next=lab::indicatorMode(monitoring.load(),txEnabled || bleTxActive() || txWifi);
  auto color=statusLed.update(next,millis());
  if(force || color!=appliedLed){rgbLedWrite(statusLedPin,color.r,color.g,color.b);appliedLed=color;}
}
void initializeStatusLed() {
  Preferences prefs;
  if(prefs.begin("ridlab2",true)){uint8_t saved=prefs.getUChar("ledpin",48);if(saved==38 || saved==48)statusLedPin=saved;prefs.end();}
  updateStatusLed(true);
}
String statusLedJson() {
  return String("{\"pin\":")+String(statusLedPin)+",\"mode\":\""+lab::ledModeName(statusLed.mode)+
    "\",\"on\":"+((appliedLed.r || appliedLed.g)?"true":"false")+",\"periodMs\":2000}";
}

// Discovery configures stopped advertising sets only; no RID is sent at boot.
int configureBlePower(bool legacy,int requested,int8_t &selected){
  ble_gap_ext_adv_params params{};
  params.legacy_pdu=legacy;params.own_addr_type=BLE_OWN_ADDR_PUBLIC;
  params.primary_phy=BLE_HCI_LE_PHY_1M;params.secondary_phy=BLE_HCI_LE_PHY_1M;
  params.itvl_min=params.itvl_max=160;params.channel_map=7;params.tx_power=int8_t(requested);
  return ble_gap_ext_adv_configure(0,&params,&selected,nullptr,nullptr);
}
bool applyRadioPower(const lab::RadioConfig &next){
  int8_t wifi=0,selected=127;
  radioWifiError=esp_wifi_set_max_tx_power(next.wifiQuarterDbm);
  if(!radioWifiError)radioWifiError=esp_wifi_get_max_tx_power(&wifi);
  wifiPowerValid=radioWifiError==0;if(wifiPowerValid)wifiActualQuarter=wifi;
  radioBleError=configureBlePower(false,next.bleDbm,selected);
  blePowerValid=radioBleError==0 && selected!=127;
  if(!radioBleError){int removed=ble_gap_ext_adv_remove(0);if(removed){radioBleError=removed;blePowerValid=false;}}
  if(blePowerValid)bleActualDbm=selected;
  return wifiPowerValid && blePowerValid;
}
void initializeRadio(){
  int8_t actual=0;
  for(int requested:lab::WifiPowerRequests){
    if(esp_wifi_set_max_tx_power(requested)==ESP_OK && esp_wifi_get_max_tx_power(&actual)==ESP_OK)
      wifiPowerOptions.add(requested,actual);
  }
  for(int requested:lab::BlePowerRequests){
    int8_t extended=127,legacy=127;
    int a=configureBlePower(false,requested,extended);
    int b=a? a:configureBlePower(true,requested,legacy);
    if(!a && !b && extended==legacy && extended!=127)blePowerOptions.add(requested,extended);
    if(!a)ble_gap_ext_adv_remove(0);
  }
  if(!wifiPowerOptions.accepts(radioConfig.wifiQuarterDbm))radioConfig.wifiQuarterDbm=wifiPowerOptions.count?wifiPowerOptions.items[0].requested:8;
  if(!blePowerOptions.accepts(radioConfig.bleDbm))radioConfig.bleDbm=blePowerOptions.count?blePowerOptions.items[0].requested:-12;
  applyRadioPower(radioConfig);rssiFloor.store(radioConfig.rssiFloor);
  Serial.printf("RADIO capabilities wifi=%u ble=%u appliedWifi=%.2f appliedBle=%d errors=%d/%d\n",unsigned(wifiPowerOptions.count),unsigned(blePowerOptions.count),wifiActualQuarter/4.0,bleActualDbm,radioWifiError,radioBleError);
}
String powerOptionsJson(const lab::PowerOptions<16> &options,double scale){
  String out="[";for(size_t i=0;i<options.count;++i){if(i)out+=',';out+="{\"value\":"+String(options.items[i].requested)+",\"dbm\":"+String(options.items[i].actual/scale,2)+"}";}return out+"]";
}
String radioSettingsJson(){
  // Wi-Fi getter reports the driver ceiling, not measured instantaneous RF output.
  int8_t actual=0;int err=esp_wifi_get_max_tx_power(&actual);
  wifiPowerValid=!err;if(!err)wifiActualQuarter=actual;
  return String("{\"wifiQuarterDbm\":")+radioConfig.wifiQuarterDbm+",\"bleDbm\":"+String(radioConfig.bleDbm)+
    ",\"wifiActualDbm\":"+(wifiPowerValid?String(wifiActualQuarter/4.0,2):String("null"))+
    ",\"bleActualDbm\":"+(blePowerValid?String(bleActualDbm):String("null"))+
    ",\"wifiOptions\":"+powerOptionsJson(wifiPowerOptions,4)+",\"bleOptions\":"+powerOptionsJson(blePowerOptions,1)+
    ",\"wifiError\":"+String(err?err:radioWifiError)+",\"bleError\":"+String(radioBleError)+
    ",\"bleAdvertising\":"+(bleTxActive()?"true":"false")+
    ",\"wifiScan\":"+String(radioConfig.wifiScan)+",\"bleScan\":"+String(radioConfig.bleScan)+
    ",\"wifiDwellMs\":"+String(lab::wifiDwellMs(radioConfig.wifiScan))+
    ",\"bleIntervalMs\":200,\"bleWindowMs\":"+String(lab::bleWindowUnits(radioConfig.bleScan)*0.625,1)+
    ",\"appliedWifiDwellMs\":"+(monitoring.load() && wifiListening && appliedWifiDwell?String(appliedWifiDwell):String("null"))+
    ",\"appliedBleWindowMs\":"+(scanning.load() && appliedBleWindow?String(appliedBleWindow*0.625,1):String("null"))+
    ",\"rssiFloor\":"+String(radioConfig.rssiFloor)+",\"filtered\":"+String(filteredReports.load())+
    ",\"lastSweepMs\":"+String(lastSweepMs)+",\"lastChannel\":"+String(lastRxChannel.load())+",\"queueCapacity\":48}";
}

void enqueue(const uint8_t *mac, uint8_t source, int rssi, const uint8_t *data, size_t n,
             uint8_t protocol, uint8_t transport, uint8_t channel=0, uint8_t phy=0) {
  if (!monitoring.load() || !rxQueue || n > rid::MaxPayload ||
      (protocol ? rid::cnPacketSize(data,n)!=n : !rid::validPayload(data,n))) return;
  ++rxStages[protocol?CnPayload:AstPayload];
  if(!lab::acceptsRssi(rssi,rssiFloor.load())){++filteredReports;return;}
  rid::Frame f{};
  memcpy(f.mac, mac, 6);
  f.source=source; f.rssi=rssi; f.length=n; f.received=millis();
  f.protocol=protocol; f.transport=transport; f.channel=channel; f.phy=phy;
  memcpy(f.payload,data,n);
  if (xQueueSend(rxQueue,&f,0)!=pdTRUE) ++dropped;else ++rxStages[QueuedPayload];
}
int bleCallback(ble_gap_event *event, void *) {
  if (event->type==BLE_GAP_EVENT_DISC_COMPLETE) {scanning.store(false);return 0;}
  if (!monitoring.load()) return 0;
  static uint32_t generation=0;
  if(generation!=scanGeneration.load()){for(auto &slot:fragments.slots)slot.active=false;generation=scanGeneration.load();}
  const uint8_t *data=nullptr; size_t length=0; uint8_t mac[6],transport=0,phy=1; int rssi=0;
  if(event->type==BLE_GAP_EVENT_EXT_DISC) {
    const auto &r=event->ext_disc;
    ++bleReports; transport=(r.props & BLE_HCI_ADV_LEGACY_MASK)?0:1;
    if(transport)++bleExtendedReports;
    if(r.prim_phy==3 || r.sec_phy==3)++bleCodedReports;
    phy=r.sec_phy?r.sec_phy:r.prim_phy; rssi=r.rssi;
    ++rxStages[r.prim_phy==3?BleCoded:Ble1M];
    if(r.data_status==1)++rxStages[BleMore];else if(r.data_status==2)++rxStages[BleTruncated];
    for(size_t i=0;i<6;++i)mac[i]=r.addr.val[5-i];
    captureRaw(mac,0,0,rssi,r.data,r.length_data,0,phy,r.data_status,r.props,r.sid,r.addr.type,r.prim_phy,r.sec_phy);
    // Advertising data and scan-response fragments are distinct streams.
    uint8_t key[8];memcpy(key,r.addr.val,6);key[6]=r.addr.type | ((r.props & BLE_HCI_ADV_SCAN_RSP_MASK)?0x80:0);key[7]=r.sid;
    data=fragments.accept(key,r.data_status,r.data,r.length_data,millis(),length);
  } else if(event->type==BLE_GAP_EVENT_DISC) {
    const auto &r=event->disc; ++bleReports;++rxStages[Ble1M]; data=r.data;length=r.length_data;rssi=r.rssi;
    for(size_t i=0;i<6;++i)mac[i]=r.addr.val[5-i];
    captureRaw(mac,0,0,rssi,r.data,r.length_data,0,1,0,0,255,r.addr.type,1,0);
  }
  if(data && transport){++rxStages[BleAssembled];captureRaw(mac,0,1,rssi,data,length,0,phy,event->ext_disc.data_status,event->ext_disc.props,event->ext_disc.sid,event->ext_disc.addr.type,event->ext_disc.prim_phy,event->ext_disc.sec_phy);}
  if(data){for(size_t pos=0;pos<length;){size_t len=data[pos];if(!len)break;if(len>length-pos-1){++rxStages[BadAd];break;}pos+=len+1;}}
  if(data)rid::advertising(data,length,[&](const uint8_t *p,size_t n,uint8_t protocol){enqueue(mac,0,rssi,p,n,protocol,transport,0,phy);});
  return 0;
}
void startBleScan() {
  ble_gap_ext_disc_params uncoded{},coded{};
  uncoded.passive=1;uncoded.itvl=lab::BleIntervalUnits;uncoded.window=captureBleOnly && captureActive.load()?160:lab::bleWindowUnits(radioConfig.bleScan);
  coded.passive=1;coded.itvl=uncoded.itvl;coded.window=uncoded.window;
  bleScanError=ble_gap_ext_disc(BLE_OWN_ADDR_PUBLIC,0,0,0,0,0,&uncoded,&coded,bleCallback,nullptr);
  scanning.store(bleScanError==0 || bleScanError==BLE_HS_EALREADY);
  if(bleScanError==0)appliedBleWindow=uncoded.window;
  Serial.printf("BLE5 scan 1M+Coded result=%d active=%d\n",bleScanError,scanning.load());
}
void wifiCallback(void *buffer,wifi_promiscuous_pkt_type_t type) {
  if(type!=WIFI_PKT_MGMT || !monitoring.load())return;
  ++wifiReports;
  const auto *packet=static_cast<const wifi_promiscuous_pkt_t *>(buffer);
  uint8_t channel=packet->rx_ctrl.channel;lastRxChannel.store(channel);
  if(channel>=1 && channel<=13){++wifiChannels[channel];wifiChannelMask.fetch_or(1u<<channel);}
  size_t n=packet->rx_ctrl.sig_len;if(n<28)return;
  const uint8_t *frame=packet->payload;if(!memcmp(frame+10,apMac,6))return;
  const uint8_t subtype=frame[0]&0xFC;
  if(subtype==0x80 || subtype==0x50 || subtype==0xD0)captureRaw(frame+10,1,2,packet->rx_ctrl.rssi,frame,n-4,channel);
  if(subtype==0x80 || subtype==0x50){
    const size_t length=n-4;
    for(size_t pos=36;pos+2<=length;){
      const size_t size=frame[pos+1];if(size>length-pos-2)break;
      if(length>rid::CaptureBytes && frame[pos]==221)captureRaw(frame+10,1,3,packet->rx_ctrl.rssi,frame+pos,size+2,channel);
      pos+=size+2;
    }
  }
  rid::wifiFrame(frame,n-4,[&](const uint8_t *p,size_t len,uint8_t proto,uint8_t transport){enqueue(frame+10,1,packet->rx_ctrl.rssi,p,len,proto,transport,channel);});
}
void scanEvent(void *,esp_event_base_t,int32_t,void *data) {
  auto *event=static_cast<wifi_event_sta_scan_done_t *>(data);
  sweepStatus.store(event->status);sweepDone.store(true);
}
// One short off-channel dwell, then at least 300 ms back at the AP home
// channel. AP+STA without an upstream STA can otherwise run foreground scans.
void tickWifiScan() {
  const uint32_t now=millis();
  if(sweepDone.exchange(false)) {
    esp_wifi_clear_ap_list();
    if(sweepActive) {
      if(sweepStatus.load()==0) {
        if(++scanChannel>13){scanChannel=1;++sweeps;lastSweepMs=now-roundStarted;roundStarted=now;}
      } else ++sweepFailures;
    }
    sweepActive=false;nextSweep=now+lab::WifiHomeMs;
  }
  if(sweepActive && uint32_t(now-sweepStarted)>1500) {
    esp_wifi_scan_stop();esp_wifi_clear_ap_list();sweepActive=false;
    sweepDone.store(false);++sweepFailures;nextSweep=now+1000;
  }
  if(!monitoring.load() || !wifiListening || sweepActive || int32_t(now-nextSweep)<0)return;
  // A stalled HTTP writer must not trigger a new off-channel scan immediately.
  if(uint32_t(now-lastHttpAt)<150)return;
  wifi_scan_config_t config{};
  config.show_hidden=true;config.scan_type=WIFI_SCAN_TYPE_PASSIVE;
  config.channel=scanChannel;config.scan_time.passive=lab::wifiDwellMs(radioConfig.wifiScan);
  config.home_chan_dwell_time=120;config.coex_background_scan=true;
  wifiScanError=esp_wifi_scan_start(&config,false);
  if(wifiScanError==ESP_OK){sweepActive=true;sweepStarted=now;appliedWifiDwell=config.scan_time.passive;}
  else {++sweepFailures;nextSweep=now+1000;}
}
void setMonitoring(bool enabled) {
  monitoring.store(false);
  ++scanGeneration;
  if (!enabled) {
    if(bleReady.load())ble_gap_disc_cancel();scanning.store(false);
    esp_wifi_scan_stop();sweepActive=false;sweepDone.store(false);esp_wifi_clear_ap_list();
    esp_wifi_set_promiscuous(false);wifiListening=false;appliedWifiDwell=0;appliedBleWindow=0;xQueueReset(rxQueue);
  } else {
    monitoring.store(true);
    if(bleReady.load() && !scanning.load())startBleScan();
    wifiListening=esp_wifi_set_promiscuous(true)==ESP_OK;scanChannel=1;roundStarted=millis();nextSweep=millis()+600;
  }
  updateStatusLed();
}
void pauseCaptureWifi() {
  if(!captureBleOnly || !monitoring.load())return;
  esp_wifi_scan_stop();sweepActive=false;sweepDone.store(false);esp_wifi_clear_ap_list();
  esp_wifi_set_promiscuous(false);wifiListening=false;captureWifiPaused=true;
  if(bleReady.load()){ble_gap_disc_cancel();scanning.store(false);++scanGeneration;startBleScan();}
}
void restoreCaptureWifi() {
  if(!captureWifiPaused)return;
  captureWifiPaused=false;
  if(monitoring.load() && !txEnabled){
    wifiListening=esp_wifi_set_promiscuous(true)==ESP_OK;nextSweep=millis()+500;
    if(bleReady.load()){ble_gap_disc_cancel();scanning.store(false);++scanGeneration;startBleScan();}
  }
}
bool clearWifiTx() {
  esp_err_t a = esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, nullptr);
  esp_err_t b = esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, nullptr);
  bool ok = a == ESP_OK && b == ESP_OK;
  if (ok) txWifi = false;
  return ok;
}
bool sendWifiTx() {
  uint8_t ie[110]; txMessages.wifi(ie, wifiCounter++);
  if (!clearWifiTx()) return false; // This ESP-IDF requires removal before replacing an IE.
  esp_err_t a = esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, ie);
  esp_err_t b = esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_0, ie);
  txWifi = a == ESP_OK || b == ESP_OK;
  return a == ESP_OK && b == ESP_OK;
}
bool sendBleTx() {
  int stopped = stopBle();
  if (stopped && stopped != BLE_HS_EALREADY) return false;
  uint8_t ad[31]; txMessages.ble(ad, blePhase, bleCounters[blePhase]++);
  if(!bleTxConfigured){
    ble_gap_ext_adv_params params{};
    params.legacy_pdu=1;params.own_addr_type=BLE_OWN_ADDR_PUBLIC;
    params.primary_phy=BLE_HCI_LE_PHY_1M;params.secondary_phy=BLE_HCI_LE_PHY_1M;
    params.itvl_min=160;params.itvl_max=160;params.channel_map=7;params.tx_power=radioConfig.bleDbm;
    int8_t selected=127;radioBleError=ble_gap_ext_adv_configure(0,&params,&selected,nullptr,nullptr);
    blePowerValid=!radioBleError && selected!=127;
    if(!blePowerValid)return false;bleActualDbm=selected;
    bleTxConfigured=true;
  }
  os_mbuf *data=ble_hs_mbuf_from_flat(ad,sizeof(ad));if(!data)return false;
  if(ble_gap_ext_adv_set_data(0,data))return false; // API consumes the mbuf.
  if(ble_gap_ext_adv_start(0,0,0))return false;
  blePhase = (blePhase + 1) % 4;
  if (!blePhase) ++txCycles;
  return true;
}
bool sendSimWifi(uint32_t now) {
  lab::SimDevice d;lab::SimMessages messages;uint8_t beacon[256];
  if(!lab::simulatedDevice(simConfig,simWifiCursor.index,uint32_t(now-txStarted),d) ||
     !messages.build(d,simWifiCursor.takeoff,uint32_t(now-txStarted)))return false;
  size_t n=lab::simulatedBeacon(beacon,sizeof(beacon),d,messages,uint8_t(simWifiCursor.cycles*2+simWifiCursor.takeoff),uint32_t(now-txStarted),Channel);
  if(!n || esp_wifi_80211_tx(WIFI_IF_AP,beacon,n,true)!=ESP_OK)return false;
  txWifi=true;simWifiCursor.sent(now,simConfig.count);return true;
}
bool sendSimBle(uint32_t now) {
  int stopped=stopBle();if(stopped && stopped!=BLE_HS_EALREADY)return false;
  if(!bleTxConfigured){
    ble_gap_ext_adv_params params{};params.own_addr_type=BLE_OWN_ADDR_RANDOM;
    params.primary_phy=BLE_HCI_LE_PHY_1M;params.secondary_phy=BLE_HCI_LE_PHY_1M;
    params.itvl_min=32;params.itvl_max=32;params.channel_map=7;params.tx_power=radioConfig.bleDbm;
    int8_t selected=127;radioBleError=ble_gap_ext_adv_configure(0,&params,&selected,nullptr,nullptr);
    blePowerValid=!radioBleError && selected!=127;
    if(!blePowerValid)return false;bleActualDbm=selected;
    bleTxConfigured=true;
  }
  lab::SimDevice d;lab::SimMessages messages;uint8_t ad[lab::SimMessages::AdSize];
  if(!lab::simulatedDevice(simConfig,simBleCursor.index,uint32_t(now-txStarted),d) ||
     !messages.build(d,simBleCursor.takeoff,uint32_t(now-txStarted)))return false;
  ble_addr_t addr{};addr.type=BLE_ADDR_RANDOM;for(size_t i=0;i<6;++i)addr.val[i]=d.bleMac[5-i];
  if(ble_gap_ext_adv_set_addr(0,&addr))return false;
  messages.ble(ad,uint8_t(simBleCursor.cycles*2+simBleCursor.takeoff));
  os_mbuf *data=ble_hs_mbuf_from_flat(ad,sizeof(ad));if(!data)return false;
  if(ble_gap_ext_adv_set_data(0,data) || ble_gap_ext_adv_start(0,0,0))return false;
  simBleCursor.sent(now,simConfig.count);return true;
}
void stopTx(const String &reason) {
  bool wasActive = txEnabled || bleTxActive() || txWifi;
  int stopped = stopBle();
  if((!stopped || stopped==BLE_HS_EALREADY) && bleTxConfigured){
    int removed=ble_gap_ext_adv_remove(0);
    if(!removed)bleTxConfigured=false;else stopped=removed;
  }
  bool wifiStopped = clearWifiTx();
  txEnabled = false;
  if ((stopped && stopped != BLE_HS_EALREADY) || !wifiStopped) {
    txError = "停止发射失败，请断电重启";
    // Keep the radios out of scanning mode until the failure is acknowledged.
    resumeMonitoring = false;
  } else if (reason.length()) txError = reason;
  else txError = "";
  if (wasActive) setMonitoring(resumeMonitoring);
  updateStatusLed();
  Serial.printf("TX stopped BLE=%d WiFi=%d monitor=%d reason=%s\n", bleTxActive(), txWifi, monitoring.load(), txError.c_str());
}
bool prepareTx() {
  bool resume=txEnabled?resumeMonitoring:monitoring.load();
  if(txEnabled || bleTxActive() || txWifi){resumeMonitoring=false;stopTx();if(txError.length())return false;}
  setMonitoring(false);resumeMonitoring=resume;return true;
}
bool startSimulation(const lab::SimConfig &config) {
  if(!lab::valid(config)){txError="模拟参数无效";return false;}
  if(!prepareTx())return false;
  simConfig=config;simWifiCursor={};simBleCursor={};simFailures=0;txCycles=0;
  txSimulation=true;txEnabled=true;txError="";txStarted=millis();
  simWifiCursor.cycleStarted=simBleCursor.cycleStarted=txStarted;
  if((config.wifi && !sendSimWifi(txStarted)) || (config.ble && !sendSimBle(txStarted))){
    ++simFailures;stopTx("模拟发射启动失败，已停止");return false;
  }
  nextSimWifi=txStarted+lab::simSlotMs(simConfig,false);nextSimBle=txStarted+lab::simSlotMs(simConfig,true);
  updateStatusLed();
  Serial.printf("SIM started count=%u seed=%08lx WiFi=%d BLE5=%d duration=%u\n",config.count,(unsigned long)config.seed,config.wifi,config.ble,config.duration);
  return true;
}
bool startTx(const lab::Config &config) {
  lab::Messages built;
  if (!built.build(config)) { txError = "无法编码发射参数"; return false; }
  if(!prepareTx())return false;
  txSimulation=false;
  txConfig = config; txMessages = built; txError = "";
  blePhase = 0; memset(bleCounters, 0, sizeof(bleCounters)); wifiCounter = 0; txCycles = 0;
  txEnabled = true;
  if (!sendWifiTx() || !sendBleTx()) { stopTx("发射启动失败，请重试"); return false; }
  txEnabled = true; txStarted = millis(); nextBle = txStarted + 240; lastWifi = txStarted;
  updateStatusLed();
  Serial.printf("TX started single target BLE=%d WiFi=%d duration=%u\n", bleTxActive(), txWifi, txConfig.duration);
  return true;
}
void tickTx() {
  if (!txEnabled) return;
  uint32_t now = millis();
  if (uint32_t(now - txStarted) >= uint32_t(txSimulation?simConfig.duration:txConfig.duration)*1000) { stopTx(); return; }
  if(txSimulation){
    if(simConfig.wifi && int32_t(now-nextSimWifi)>=0){
      nextSimWifi=now+lab::simSlotMs(simConfig,false);
      if(!sendSimWifi(now)){++simFailures;stopTx("模拟 Wi-Fi 发射失败，已结束测试");return;}
    }
    if(simConfig.ble && int32_t(now-nextSimBle)>=0){
      nextSimBle=now+lab::simSlotMs(simConfig,true);
      if(!sendSimBle(now)){++simFailures;stopTx("模拟 BLE 5 发射失败，已结束测试");return;}
    }
    txCycles=simConfig.wifi?simWifiCursor.cycles:simBleCursor.cycles;
    return;
  }
  if (int32_t(now-nextBle) >= 0) {
    nextBle = now + 240;
    if (!sendBleTx()) { stopTx("BLE 发射异常，已结束测试"); return; }
  }
  if (uint32_t(now-lastWifi) >= 1000) {
    lastWifi = now;
    if (!sendWifiTx()) stopTx("Wi-Fi 发射异常，已结束测试");
  }
}
String escape(const String &s) {
  String out;
  for (size_t i = 0; i < s.length(); ++i) {
    uint8_t c = s[i];
    if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
    else if (c < 32) { char b[7]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
    else out += char(c);
  }
  return out;
}
String simulationGeometry();
String txJson() {
  bool active = txEnabled || bleTxActive() || txWifi;
  uint32_t elapsed = millis()-txStarted, total = uint32_t(txSimulation?simConfig.duration:txConfig.duration)*1000;
  uint32_t remaining = txEnabled && elapsed < total ? (total-elapsed+999)/1000 : 0;
  return String("{\"enabled\":") + (active ? "true" : "false") +
    ",\"ble\":" + (bleTxActive() ? "true" : "false") +
    ",\"wifi\":" + (txWifi ? "true" : "false") +
    ",\"remaining\":" + String(remaining) + ",\"cycles\":" + String(txCycles) +
    ",\"mode\":\""+(txSimulation?"simulation":"single")+"\",\"simulation\":{\"count\":"+String(simConfig.count)+",\"seed\":"+String(simConfig.seed)+
    ",\"duration\":"+String(simConfig.duration)+",\"wifi\":"+(simConfig.wifi?"true":"false")+",\"ble\":"+(simConfig.ble?"true":"false")+
    simulationGeometry()+",\"wifiDevices\":"+String(simWifiCursor.unique)+",\"bleDevices\":"+String(simBleCursor.unique)+
    ",\"wifiFrames\":"+String(simWifiCursor.slots)+",\"bleSlots\":"+String(simBleCursor.slots)+
    ",\"wifiCycles\":"+String(simWifiCursor.cycles)+",\"bleCycles\":"+String(simBleCursor.cycles)+
    ",\"wifiCycleMs\":"+String(simWifiCursor.lastCycleMs)+",\"bleCycleMs\":"+String(simBleCursor.lastCycleMs)+
    ",\"failures\":"+String(simFailures)+"}"+
    ",\"error\":\"" + escape(txError) + "\",\"uasId\":\"" + escape(txConfig.uasId) +
    "\",\"opId\":\"" + escape(txConfig.operatorId) + "\",\"lat\":" + String(txConfig.lat,7) +
    ",\"lon\":" + String(txConfig.lon,7) + ",\"alt\":" + String(txConfig.altitude,1) +
    ",\"opLat\":" + String(txConfig.opLat,7) + ",\"opLon\":" + String(txConfig.opLon,7) +
    ",\"opAlt\":" + String(txConfig.opAltitude,1) + ",\"duration\":" + String(txConfig.duration) + "}";
}
bool persistTx(const lab::Config &config) {
  Preferences prefs; if (!prefs.begin("ridlab2", false)) return false;
  bool ok = prefs.putBytes("tx", &config, sizeof(config)) == sizeof(config);
  prefs.end(); return ok;
}
bool readTx(lab::Config &out, String &error) {
  if (!server.hasArg("uasId") || !server.hasArg("opId")) { error = "请填写身份参数"; return false; }
  String uas = server.arg("uasId"), op = server.arg("opId");
  uas.trim(); op.trim();
  if (!uas.length() || uas.length() > 20 || op.length() > 20 ||
      !lab::validText(uas.c_str(), uas.length()) || !lab::validText(op.c_str(), op.length())) {
    error = "身份须为有效文字，UAS ID 为 1–20 字节，操作员 ID 最多 20 字节"; return false;
  }
  memcpy(out.uasId, uas.c_str(), uas.length()+1); memcpy(out.operatorId, op.c_str(), op.length()+1);
  const char *fields[] = {"lat","lon","alt","opLat","opLon","opAlt","duration"};
  const double low[] = {-90,-180,-999.5,-90,-180,-999.5,5};
  const double high[] = {90,180,31767.5,90,180,31767.5,600};
  double values[7];
  for (size_t i = 0; i < 7; ++i) {
    if (!server.hasArg(fields[i]) || !lab::number(server.arg(fields[i]).c_str(), low[i], high[i], values[i])) {
      error = String("参数无效或超出范围：") + fields[i]; return false;
    }
  }
  if (values[6] != floor(values[6])) { error = "测试时长须为整数秒"; return false; }
  out.lat=values[0]; out.lon=values[1]; out.altitude=values[2];
  out.opLat=values[3]; out.opLon=values[4]; out.opAltitude=values[5]; out.duration=values[6];
  return lab::valid(out);
}
void sendJson(int status, const String &body) {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Connection","close");
  server.send(status, "application/json; charset=utf-8", body);
}
String simulationGeometry() {
  String out=",\"centerLat\":"+String(simConfig.centerLat,7)+",\"centerLon\":"+String(simConfig.centerLon,7)+
    ",\"radiusM\":10000,\"modelIndex\":"+String(simConfig.model)+",\"models\":[";
  for(unsigned i=0;i<lab::SimModelCount;++i){if(i)out+=',';out+='"';out+=lab::SimModels[i];out+='"';}
  return out+"]";
}
String simulationRoster() {
  uint32_t elapsed=txEnabled && txSimulation?uint32_t(millis()-txStarted):0;
  String body;body.reserve(36000);
  body="{\"count\":"+String(simConfig.count)+",\"seed\":"+String(simConfig.seed)+simulationGeometry()+",\"elapsedMs\":"+String(elapsed)+",\"items\":[";
  for(uint16_t i=0;i<simConfig.count;++i){
    lab::SimDevice d;if(!lab::simulatedDevice(simConfig,i,elapsed,d))return "{\"error\":\"模拟生成失败\"}";
    if(i)body+=',';
    body+="{\"index\":"+String(i+1)+",\"sn\":\""+d.flight.uasId+"\",\"model\":\""+d.model+"\",\"operatorId\":\""+d.flight.operatorId+
      "\",\"lat\":"+String(d.flight.lat,7)+",\"lon\":"+String(d.flight.lon,7)+",\"alt\":"+String(d.flight.altitude,1)+
      ",\"speed\":"+String(d.speed,2)+",\"verticalSpeed\":"+String(d.verticalSpeed,2)+",\"heading\":"+String(d.heading,1)+
      ",\"opLat\":"+String(d.flight.opLat,7)+",\"opLon\":"+String(d.flight.opLon,7)+",\"opAlt\":"+String(d.flight.opAltitude,1)+
      ",\"takeoffLat\":"+String(d.takeoffLat,7)+",\"takeoffLon\":"+String(d.takeoffLon,7)+",\"takeoffAlt\":"+String(d.takeoffAltitude,1)+"}";
  }
  return body+"]}";
}
String radioJson() {
  String out=",\"ble5\":"+String(scanning.load()?"true":"false")+",\"bleScanError\":"+String(bleScanError)+
    ",\"bleExtendedReports\":"+String(bleExtendedReports.load())+",\"bleCodedReports\":"+String(bleCodedReports.load())+
    ",\"wifiScanError\":"+String(wifiScanError)+",\"scanChannels\":\"1–13\",\"sweeps\":"+String(sweeps)+
    ",\"sweepFailures\":"+String(sweepFailures)+",\"sweepActive\":"+(sweepActive?"true":"false")+
    ",\"wifiChannelMask\":"+String(wifiChannelMask.load())+",\"channelReports\":[";
  for(int ch=1;ch<=13;++ch){if(ch>1)out+=',';out+=String(wifiChannels[ch].load());}
  return out+"]";
}
void sendObservations() {
  lastHttpAt=millis();
  server.sendHeader("Cache-Control","no-store");
  server.sendHeader("Connection","close");
  server.client().setNoDelay(true);
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200,"application/json; charset=utf-8","");
  table.expire(millis());
  const uint32_t now = millis();
  String body;
  body.reserve(6144);
  body = "{\"devices\":" + String(table.count) + ",\"packets\":" + String(table.packets) +
    ",\"monitoring\":" + (monitoring.load() ? "true" : "false") +
    ",\"scanning\":" + (scanning.load() ? "true" : "false") +
    ",\"wifiListening\":" + (wifiListening ? "true" : "false") +
    ",\"channel\":" + String(Channel) + ",\"dropped\":" + String(dropped.load()) +
    ",\"bleReports\":" + String(bleReports.load()) + ",\"wifiReports\":" + String(wifiReports.load()) +
    ",\"heap\":" + String(ESP.getFreeHeap()) + radioJson() + ",\"radio\":"+radioSettingsJson()+",\"led\":"+statusLedJson()+",\"items\":[";
  server.sendContent(body);body="";
  for (size_t i = 0; i < table.count; ++i) {
    if(!server.client().connected())return;
    const rid::Device &d = table.devices[i];
    const auto identity=rid::broadcastIdentity(d);
    const auto product=rid::productCode(d);
    const char *model=identity.model;
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", d.mac[0],d.mac[1],d.mac[2],d.mac[3],d.mac[4],d.mac[5]);
    if (i) body += ',';
    body += "{\"address\":\"" + String(mac) + "\",\"source\":\"" + (d.source ? "Wi-Fi" : "BLE") +
      "\",\"protocol\":\"" + (d.protocol?"GB 46750-2025":"ASTM") + "\",\"transport\":\"" +
      (d.transport==0?"BLE Legacy":d.transport==1?"BLE 5":d.transport==2?"Wi-Fi Beacon":"Wi-Fi NAN") +
      "\",\"rxChannel\":"+String(d.channel)+",\"phy\":"+String(d.phy)+
      ",\"coordSystem\":\""+(d.protocol?(d.cn.coord_system==0?"WGS-84":d.cn.coord_system==1?"CGCS2000":"未知"):"WGS-84")+
      "\",\"registration\":\""+escape(d.protocol?d.cn.uin:"")+"\",\"uasId\":\"" + escape(d.uasId) + "\",\"idType\":" + String(d.idType) +
      ",\"model\":\"" + escape(model) + "\",\"modelSource\":\"" + identity.source +
      "\",\"modelEditable\":" + "false" +
      ",\"modelCatalogVersion\":\""+rid::ModelCatalogVersion+"\",\"modelCatalogReference\":\""+(identity.catalog?identity.catalog->reference:"")+"\",\"modelCatalogPrefix\":\""+(identity.catalog?identity.catalog->prefix:"")+"\",\"modelVariant\":\""+(identity.catalog?identity.catalog->variant:"")+"\",\"modelCatalogName\":\""+(identity.catalog?identity.catalog->model:"")+"\",\"modelCandidates\":\""+(identity.catalog?identity.catalog->candidates:"")+"\",\"modelConflict\":"+(identity.conflict?"true":"false")+
      ",\"manufacturerCode\":\""+String(product.valid?product.manufacturer:"")+"\",\"productModelCode\":\""+String(product.valid?product.model:"")+"\",\"productCodeSource\":\""+(product.valid?"GB/T 41300":"unavailable")+"\""+
      ",\"uaType\":"+(d.uaTypeReceived?String(d.uaType):String("null"))+",\"selfDescriptionType\":"+(d.selfIdReceived?String(d.selfDescriptionType):String("null"))+
      ",\"selfDescription\":\""+escape(d.selfDescription)+"\",\"operatorId\":\"" + escape(d.operatorId) + "\",\"rssi\":" + String(d.rssi) +
      ",\"messageMask\":"+String(d.messageMask)+",\"messageCounts\":["+String(d.messageCounts[0])+","+String(d.messageCounts[1])+","+String(d.messageCounts[2])+","+String(d.messageCounts[3])+","+String(d.messageCounts[4])+","+String(d.messageCounts[5])+"],\"locationReceived\":"+(d.locationReceived?"true":"false")+",\"systemReceived\":"+(d.systemReceived?"true":"false")+
      ",\"type\":" + String(d.type) + ",\"packets\":" + String(d.packets) +
      ",\"age\":" + String(uint32_t(now-d.lastSeen)/1000) +
      ",\"locationAge\":" + (d.locationReceived ? String(uint32_t(now-d.locationSeen)/1000) : String(-1)) +
      ",\"systemAge\":" + (d.systemReceived ? String(uint32_t(now-d.systemSeen)/1000) : String(-1)) +
      ",\"lat\":" + (d.hasLocation ? String(d.lat,7) : "null") +
      ",\"lon\":" + (d.hasLocation ? String(d.lon,7) : "null") +
      ",\"alt\":" + (d.hasAltitude ? String(d.altitude,1) : "null") +
      ",\"opLat\":" + (d.operatorPosition.valid ? String(d.operatorPosition.lat,7) : "null") +
      ",\"opLon\":" + (d.operatorPosition.valid ? String(d.operatorPosition.lon,7) : "null") +
      ",\"operatorAge\":" + (d.operatorPosition.received ? String(uint32_t(now-d.operatorPosition.seen)/1000) : String(-1)) +
      ",\"operatorCoordSystem\":\"" + (d.operatorPosition.coordSystem==0?"WGS-84":d.operatorPosition.coordSystem==1?"CGCS2000":"未知") + "\"" +
      ",\"takeoffLat\":" + (d.takeoffPosition.valid ? String(d.takeoffPosition.lat,7) : "null") +
      ",\"takeoffLon\":" + (d.takeoffPosition.valid ? String(d.takeoffPosition.lon,7) : "null") +
      ",\"takeoffAge\":" + (d.takeoffPosition.received ? String(uint32_t(now-d.takeoffPosition.seen)/1000) : String(-1)) +
      ",\"takeoffCoordSystem\":\"" + (d.takeoffPosition.coordSystem==0?"WGS-84":d.takeoffPosition.coordSystem==1?"CGCS2000":"未知") + "\"" +
      ",\"operatorLocationType\":"+(d.systemReceived?(d.protocol?(int(d.cn.gcs_pos_type)<=1?String(int(d.cn.gcs_pos_type)):String("null")):String(d.operatorLocationType)):"null")+
      ",\"gcsAltitude\":"+(d.protocol && d.hasGcsAltitude?String(d.cn.gcs_alt,1):"null")+
      ",\"speed\":"+(d.hasSpeed?String(d.speed,2):"null")+
      ",\"heading\":"+(d.hasHeading?String(d.heading,1):"null")+
      ",\"verticalSpeed\":"+(d.hasVerticalSpeed?String(d.verticalSpeed,2):"null")+
      ",\"relativeAltitude\":"+(d.hasRelativeAltitude?String(d.relativeAltitude,1):"null")+
      ",\"pressureAltitude\":"+(d.hasPressureAltitude?String(d.pressureAltitude,1):"null")+
      ",\"cnPackets\":"+String(d.cnPackets)+",\"cnVersion\":"+(d.protocol?String(d.cnVersion):"null")+
      ",\"flightStatus\":"+String(d.flightStatus)+
      ",\"droneClass\":"+(d.protocol?String(int(d.cn.drone_class)):"null")+
      ",\"timestampMs\":"+(d.protocol?String((unsigned long long)d.cn.timestamp_ms):"null")+"}";
    if(body.length()>=4096){server.sendContent(body);body="";delay(1);}
  }
  body += "],\"receiver\":{\"region\":\"CN\",\"standard\":\"GB 46750-2025\",\"wifi24\":true,\"wifi58\":false,\"ble5\":true,\"brandAllowlist\":false},\"diagnostics\":";body+=captureSummary();body+=",\"tx\":"; body += txJson(); body += '}';
  server.sendContent(body);server.sendContent("");lastHttpAt=millis();
}
void sendDiagnostics() {
  lastHttpAt=millis();server.client().setNoDelay(true);server.sendHeader("Cache-Control","no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);server.send(200,"application/json; charset=utf-8","");
  server.sendContent(String("{\"version\":\"2.21\",\"capture\":")+captureSummary()+
    ",\"bleReports\":"+String(bleReports.load())+",\"bleExtendedReports\":"+String(bleExtendedReports.load())+
    ",\"wifiReports\":"+String(wifiReports.load())+",\"bleScanError\":"+String(bleScanError)+
    ",\"wifiScanError\":"+String(wifiScanError)+",\"fragmentDiscards\":"+String(fragments.discarded)+",\"fragmentTruncations\":"+String(fragments.truncated)+
    ",\"maxBytesPerSample\":1650,\"maxSamplesPerAddress\":16,\"samples\":[");
  for(size_t i=0;i<captureStore.count;++i){
    if(!server.client().connected())return;
    const auto &e=captureStore.entries[i];const auto &v=e.report;char mac[18];
    snprintf(mac,sizeof(mac),"%02X:%02X:%02X:%02X:%02X:%02X",v.mac[0],v.mac[1],v.mac[2],v.mac[3],v.mac[4],v.mac[5]);
    String row;row.reserve(3600);if(i)row=",";
    row+="{\"address\":\""+String(mac)+"\",\"source\":\""+(v.source?"Wi-Fi":"BLE")+"\",\"kind\":"+String(v.kind)+
      ",\"rssi\":"+String(v.rssi)+",\"channel\":"+String(v.channel)+",\"phy\":"+String(v.phy)+",\"dataStatus\":"+String(v.status)+",\"addressType\":"+String(v.addressType)+",\"sid\":"+String(v.sid)+",\"properties\":"+String(v.properties)+",\"primaryPhy\":"+String(v.primaryPhy)+",\"secondaryPhy\":"+String(v.secondaryPhy)+
      ",\"firstMs\":"+String(e.firstAt)+",\"lastMs\":"+String(v.at)+",\"repeats\":"+String(e.repeats)+
      ",\"originalLength\":"+String(v.original)+",\"capturedLength\":"+String(v.length)+",\"truncated\":"+(v.length<v.original?"true":"false")+",\"hex\":\"";
    static const char digits[]="0123456789ABCDEF";
    for(size_t j=0;j<v.length;++j){row+=digits[v.data[j]>>4];row+=digits[v.data[j]&15];}
    row+="\"}";server.sendContent(row);delay(1);
  }
  server.sendContent("]}");server.sendContent("");lastHttpAt=millis();
}
void dashboard() {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Connection","close");
  server.send_P(200, "text/html; charset=utf-8", kPage);
}
void resetConfig() {
  Preferences prefs;
  prefs.begin("ridlab2", false); prefs.remove("ap"); prefs.end();
}
void onPost(const char *path, WebServer::THandlerFunction handler) {
  server.on(path, HTTP_POST, [handler]() {
    const String origin = server.header("Origin");
    if (origin.length() && origin != "http://" + server.hostHeader()) {
      sendJson(403, "{\"error\":\"请从设备本地页面操作\"}"); return;
    }
    handler();
  });
}
void configureWeb() {
  server.on("/api/health",HTTP_GET,[](){
    sendJson(200,String("{\"version\":\"2.21\",\"uptimeMs\":")+String(millis())+
      ",\"heap\":"+String(ESP.getFreeHeap())+",\"minHeap\":"+String(ESP.getMinFreeHeap())+
      ",\"largestBlock\":"+String(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT))+
      ",\"localOnlyDhcp\":"+(localOnlyDhcp?"true":"false")+",\"apDisconnects\":"+String(apDisconnects)+"}");
  });
  server.on("/api/diagnostics",HTTP_GET,sendDiagnostics);
  onPost("/api/diagnostics/start",[](){
    if(txEnabled || bleTxActive() || txWifi){sendJson(409,"{\"error\":\"请先停止发射，再采集接收诊断\"}");return;}
    const String mode=server.hasArg("mode")?server.arg("mode"):String("both");
    if(mode!="ble" && mode!="both"){sendJson(400,"{\"error\":\"请选择蓝牙或双载体诊断\"}");return;}
    if(!startCapture(mode=="ble")){sendJson(503,"{\"error\":\"诊断缓冲区不足，未开始采集\"}");return;}
    if(!monitoring.load())setMonitoring(true);pauseCaptureWifi();
    sendJson(200,captureSummary());
  });
  onPost("/api/diagnostics/stop",[](){endCapture();captureEnded=millis();tickCapture();sendJson(200,captureSummary());});
  const char *headers[] = {"Origin"}; server.collectHeaders(headers, 1);
  server.on("/", HTTP_GET, dashboard);
  server.on("/navigation.js",HTTP_GET,[](){server.sendHeader("Cache-Control","no-store");server.send_P(200,"application/javascript; charset=utf-8",kNavigationScript);});
  server.on("/generate_204", HTTP_GET, dashboard);
  server.on("/hotspot-detect.html", HTTP_GET, dashboard);
  server.on("/connecttest.txt", HTTP_GET, dashboard);
  server.on("/ncsi.txt", HTTP_GET, dashboard);
  server.on("/redirect", HTTP_GET, dashboard);
  server.on("/api/status", HTTP_GET, []() { sendObservations(); });
  server.on("/api/observations", HTTP_GET, []() { sendObservations(); });
  server.on("/api/radio",HTTP_GET,[](){sendJson(200,radioSettingsJson());});
  onPost("/api/radio",[](){
    if(txEnabled || bleTxActive() || txWifi){sendJson(409,"{\"error\":\"请先停止发射，再修改功率和侦测设置\"}");return;}
    const char *keys[]={"wifiQuarterDbm","bleDbm","wifiScan","bleScan","rssiFloor"};
    double values[5];const double low[]={8,-24,0,0,-127},high[]={84,20,2,2,-60};
    for(size_t i=0;i<5;++i)if(!server.hasArg(keys[i]) || !lab::number(server.arg(keys[i]).c_str(),low[i],high[i],values[i]) || values[i]!=floor(values[i])){
      sendJson(400,"{\"error\":\"功率或侦测参数无效，请选择页面提供的档位\"}");return;
    }
    lab::RadioConfig next=radioConfig;
    next.wifiQuarterDbm=values[0];next.bleDbm=values[1];next.wifiScan=values[2];next.bleScan=values[3];next.rssiFloor=values[4];
    if(!lab::valid(next) || !wifiPowerOptions.accepts(next.wifiQuarterDbm) || !blePowerOptions.accepts(next.bleDbm)){
      sendJson(400,"{\"error\":\"该档位未通过本机驱动／控制器验证\"}");return;
    }
    bool resume=monitoring.load();setMonitoring(false);
    if(!applyRadioPower(next)){
      applyRadioPower(radioConfig);if(resume)setMonitoring(true);
      sendJson(503,"{\"error\":\"硬件未接受新功率，已尝试恢复原设置，请刷新检查\"}");return;
    }
    Preferences prefs;bool saved=false;
    if(prefs.begin("ridlab2",false)){saved=prefs.putBytes("radio",&next,sizeof(next))==sizeof(next);prefs.end();}
    if(!saved){applyRadioPower(radioConfig);if(resume)setMonitoring(true);sendJson(500,"{\"error\":\"保存失败，已恢复原设置\"}");return;}
    radioConfig=next;rssiFloor.store(next.rssiFloor);if(resume)setMonitoring(true);
    sendObservations();
  });
  onPost("/api/clear", []() {
    xQueueReset(rxQueue); table.clear(); dropped.store(0); sendJson(200, "{\"ok\":true}");
  });
  onPost("/api/monitor", []() {
    if (txEnabled || bleTxActive() || txWifi) {
      sendJson(409, "{\"error\":\"请先停止发射，再开始监测\"}"); return;
    }
    if (!server.hasArg("enabled") || (server.arg("enabled") != "0" && server.arg("enabled") != "1")) {
      sendJson(400, "{\"error\":\"enabled 必须为 0 或 1\"}"); return;
    }
    setMonitoring(server.arg("enabled") == "1");
    sendObservations();
  });
  server.on("/api/tx", HTTP_GET, []() { sendJson(200, txJson()); });
  server.on("/api/simulation",HTTP_GET,[](){sendJson(200,simulationRoster());});
  server.on("/map-picker.js",HTTP_GET,[](){server.send_P(200,"application/javascript; charset=utf-8",MAP_PICKER_JS);});
  onPost("/api/simulation/config",[](){
    if(txEnabled || bleTxActive() || txWifi){sendJson(409,"{\"error\":\"请先停止发射，再应用选点\"}");return;}
    lab::SimConfig next=simConfig;double lat,lon,count,model;
    if(!lab::number(server.arg("lat").c_str(),-85,85,lat) ||
       !lab::number(server.arg("lon").c_str(),-180,180,lon) ||
       !lab::number(server.arg("count").c_str(),1,100,count) || (count!=1 && count!=100) ||
       !lab::number(server.arg("model").c_str(),-1,lab::SimModelCount-1,model) || model!=floor(model)){
      sendJson(400,"{\"error\":\"请填写有效的 WGS-84 中心坐标，数量只能为 1 或 100\"}");return;
    }
    next.centerLat=lat;next.centerLon=lon;next.count=uint16_t(count);next.model=int8_t(model);
    next.seed=esp_random();if(!next.seed)next.seed=1;
    if(!lab::valid(next)){sendJson(400,"{\"error\":\"模拟配置无效\"}");return;}
    simConfig=next;simWifiCursor={};simBleCursor={};simFailures=0;
    sendJson(200,simulationRoster());
  });
  onPost("/api/simulation/regenerate",[](){
    if(txEnabled || bleTxActive() || txWifi){sendJson(409,"{\"error\":\"请先停止发射，再生成新的一批设备\"}");return;}
    simConfig.seed=esp_random();if(!simConfig.seed)simConfig.seed=1;
    simWifiCursor={};simBleCursor={};simFailures=0;
    sendJson(200,simulationRoster());
  });
  onPost("/api/simulation/start",[](){
    lab::SimConfig next=simConfig;double duration=0,seed=0;
    if(!server.hasArg("duration") || !lab::number(server.arg("duration").c_str(),30,600,duration) || duration!=floor(duration) ||
       !server.hasArg("seed") || !lab::number(server.arg("seed").c_str(),1,4294967295.0,seed) || seed!=floor(seed) || uint32_t(seed)!=simConfig.seed){
      sendJson(400,"{\"error\":\"时长须为 30–600 整数秒；设备批次过期时请重新生成\"}");return;
    }
    const String transport=server.arg("transport");
    if(transport!="both" && transport!="wifi" && transport!="ble"){sendJson(400,"{\"error\":\"请选择有效的发射方式\"}");return;}
    next.duration=uint16_t(duration);next.wifi=transport!="ble";next.ble=transport!="wifi";
    bool ok=startSimulation(next);if(ok)sendObservations();else sendJson(503,"{\"error\":\""+escape(txError)+"\"}");
  });
  onPost("/api/led",[](){
    if(txEnabled || bleTxActive() || txWifi){sendJson(409,"{\"error\":\"请先停止发射，再更换灯光引脚\"}");return;}
    const String pin=server.arg("pin");
    if(pin!="38" && pin!="48"){sendJson(400,"{\"error\":\"灯光引脚只支持 GPIO38 或 GPIO48\"}");return;}
    uint8_t next=uint8_t(pin.toInt());Preferences prefs;bool saved=false;
    if(prefs.begin("ridlab2",false)){saved=prefs.putUChar("ledpin",next)==1;prefs.end();}
    if(!saved){sendJson(500,"{\"error\":\"灯光设置保存失败\"}");return;}
    if(next!=statusLedPin){rgbLedWrite(statusLedPin,0,0,0);rmtDeinit(statusLedPin);pinMode(statusLedPin,INPUT);statusLedPin=next;}
    updateStatusLed(true);sendObservations();
  });
  onPost("/api/tx/start", []() {
    lab::Config next; String error;
    if (!readTx(next, error)) { sendJson(400, "{\"error\":\"" + escape(error) + "\"}"); return; }
    if (!persistTx(next)) { sendJson(500, "{\"error\":\"参数保存失败\"}"); return; }
    bool ok = startTx(next);
    if (!ok) { sendJson(503, "{\"error\":\"" + escape(txError) + "\"}"); return; }
    sendObservations();
  });
  onPost("/api/tx/save", []() {
    if (txEnabled) { sendJson(409, "{\"error\":\"发射中请使用应用参数，或先停止发射\"}"); return; }
    lab::Config next; String error;
    if (!readTx(next, error)) { sendJson(400, "{\"error\":\"" + escape(error) + "\"}"); return; }
    if (!persistTx(next)) { sendJson(500, "{\"error\":\"参数保存失败\"}"); return; }
    txConfig = next; sendObservations();
  });
  onPost("/api/tx/stop", []() {
    stopTx();
    if (bleTxActive() || txWifi) { sendJson(503, "{\"error\":\"停止失败，请断电重启\"}"); return; }
    sendObservations();
  });
  server.on("/api/config", HTTP_GET, []() {
    sendJson(200, "{\"ssid\":\"" + escape(ssid) + "\",\"open\":" + (password.length() ? "false" : "true") + ",\"channel\":6}");
  });
  onPost("/api/wifi", []() {
    if (txEnabled) { sendJson(409, "{\"error\":\"请先停止发射，再修改热点\"}"); return; }
    if (!server.hasArg("ssid") || !server.hasArg("pass")) {
      sendJson(400, "{\"error\":\"缺少热点名称或密码\"}"); return;
    }
    String next = server.arg("ssid"); next.trim();
    String pass = server.arg("pass");
    bool open = server.arg("open") == "1";
    if (open) pass = "";
    else if (!pass.length()) pass = password;
    if (next.length() == 0 || next.length() > 32 || (!open && (pass.length() < 8 || pass.length() > 63)) ||
        !lab::validText(next.c_str(), next.length())) {
      sendJson(400, "{\"error\":\"名称须为 1–32 字节；加密热点密码须为 8–63 字节\"}"); return;
    }
    for (size_t i = 0; i < next.length(); ++i)
      if (uint8_t(next[i]) < 32) { sendJson(400, "{\"error\":\"名称含无效字符\"}"); return; }
    for (size_t i = 0; i < pass.length(); ++i)
      if (uint8_t(pass[i]) < 32 || uint8_t(pass[i]) > 126) {
        sendJson(400, "{\"error\":\"密码请使用 ASCII 可打印字符\"}"); return;
      }
    ApConfig ap; memcpy(ap.ssid, next.c_str(), next.length()+1); memcpy(ap.pass, pass.c_str(), pass.length()+1);
    Preferences prefs; prefs.begin("ridlab2", false);
    bool ok = prefs.putBytes("ap", &ap, sizeof(ap)) == sizeof(ap);
    prefs.end();
    sendJson(ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"保存失败\"}");
    if (ok) rebootAt = millis() + 1500;
  });
  onPost("/api/wifi/reset", []() {
    if (txEnabled) { sendJson(409, "{\"error\":\"请先停止发射，再恢复热点\"}"); return; }
    resetConfig(); sendJson(200, "{\"ok\":true}"); rebootAt = millis() + 1500;
  });
  server.onNotFound([]() {
    if (server.uri().startsWith("/api/")) { sendJson(404, "{\"error\":\"接口不存在\"}"); return; }
    server.sendHeader("Location", "http://192.168.4.1/", true); server.send(302, "text/plain", "");
  });
  server.begin();
}
// Serial-only hardware check exercises the live HTTP server while loop() runs.
// Explicit serial diagnostic only: exercises the same form POST endpoints as UI.
// No task runs at boot. The final stop leaves both radios out of TX mode.
void simulationApiProbe(void *) {
  auto request=[](const char *path,const char *body=nullptr){
    WiFiClient client;String response;
    if(!client.connect(IPAddress(127,0,0,1),80,3000)){Serial.println("CHECK CONNECT_FAILED");return;}
    if(body)client.printf("POST %s HTTP/1.1\r\nHost: 192.168.4.1\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",path,unsigned(strlen(body)),body);
    else client.printf("GET %s HTTP/1.1\r\nHost: 192.168.4.1\r\nConnection: close\r\n\r\n",path);
    uint32_t start=millis();while((client.connected()||client.available())&&uint32_t(millis()-start)<5000){while(client.available())response+=char(client.read());vTaskDelay(1);}
    Serial.printf("CHECK %s %s\n",path,response.substring(0,response.indexOf('\r')).c_str());
    Serial.println(response.substring(response.indexOf("\r\n\r\n")+4));client.stop();
  };
  request("/api/status");
  request("/api/simulation/config","lat=30&lon=120&count=2&model=-1");
  request("/api/simulation/config","lat=30&lon=120&count=1&model=4");
  char body[128];snprintf(body,sizeof(body),"seed=%lu&duration=30&transport=both",(unsigned long)simConfig.seed);
  request("/api/simulation/start",body);vTaskDelay(pdMS_TO_TICKS(1500));request("/api/simulation");
  request("/api/simulation/config","lat=31.23&lon=121.47&count=100&model=-1");
  vTaskDelay(pdMS_TO_TICKS(1500));request("/api/simulation");request("/api/status");request("/api/tx/stop","");
  request("/api/simulation/config","lat=31.23&lon=121.47&count=100&model=-1");
  snprintf(body,sizeof(body),"seed=%lu&duration=30&transport=both",(unsigned long)simConfig.seed);
  request("/api/simulation/start",body);vTaskDelay(pdMS_TO_TICKS(1500));request("/api/simulation");
  vTaskDelay(pdMS_TO_TICKS(20000));request("/api/simulation");request("/api/status");
  // Let the 30-second timer expire, then verify safe stopped state.
  vTaskDelay(pdMS_TO_TICKS(9000));request("/api/status");
  request("/api/tx/stop","");Serial.println("CHECK DONE");
  probeTask=nullptr;vTaskDelete(nullptr);
}

// Explicit USB command 'r': exercises real HTTP endpoints. Never runs at boot.
void radioApiProbe(void *) {
  auto request=[](const char *path,const String &body=String(),bool post=false){
    WiFiClient client;String response;
    if(!client.connect(IPAddress(127,0,0,1),80,3000)){Serial.println("RADIOCHECK CONNECT_FAILED");return;}
    if(post)client.printf("POST %s HTTP/1.1\r\nHost: 192.168.4.1\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",path,unsigned(body.length()),body.c_str());
    else client.printf("GET %s HTTP/1.1\r\nHost: 192.168.4.1\r\nConnection: close\r\n\r\n",path);
    uint32_t start=millis();while((client.connected()||client.available())&&uint32_t(millis()-start)<7000){while(client.available())response+=char(client.read());vTaskDelay(1);}
    Serial.printf("RADIOCHECK %s %s\n",path,response.substring(0,response.indexOf('\r')).c_str());
    Serial.println(response.substring(response.indexOf("\r\n\r\n")+4));client.stop();
  };
  auto encode=[](const char *text){String out;const char *hex="0123456789ABCDEF";for(const uint8_t *p=(const uint8_t *)text;*p;++p){if(isalnum(*p)||*p=='-'||*p=='_'||*p=='.')out+=char(*p);else{out+='%';out+=hex[*p>>4];out+=hex[*p&15];}}return out;};
  lab::Config original=txConfig;
  String restore="uasId="+encode(original.uasId)+"&opId="+encode(original.operatorId)+"&lat="+String(original.lat,7)+"&lon="+String(original.lon,7)+"&alt="+String(original.altitude,1)+"&opLat="+String(original.opLat,7)+"&opLon="+String(original.opLon,7)+"&opAlt="+String(original.opAltitude,1)+"&duration="+String(original.duration);
  request("/api/radio");
  if(!wifiPowerOptions.count || !blePowerOptions.count){Serial.println("RADIOCHECK NO_CAPABILITIES");probeTask=nullptr;vTaskDelete(nullptr);return;}
  int wifi=wifiPowerOptions.items[wifiPowerOptions.count-1].requested,ble=blePowerOptions.items[blePowerOptions.count-1].requested;
  String maximum="wifiQuarterDbm="+String(wifi)+"&bleDbm="+String(ble);
  request("/api/radio","wifiQuarterDbm=9&bleDbm=-12&wifiScan=1&bleScan=1&rssiFloor=-127",true);
  request("/api/radio","wifiQuarterDbm=8&bleDbm=-12&wifiScan=1.5&bleScan=1&rssiFloor=-127",true);
  request("/api/radio",maximum+"&wifiScan=1&bleScan=1&rssiFloor=-127",true);
  request("/api/monitor","enabled=1",true);vTaskDelay(pdMS_TO_TICKS(6500));request("/api/status");
  request("/api/radio",maximum+"&wifiScan=2&bleScan=2&rssiFloor=-127",true);
  vTaskDelay(pdMS_TO_TICKS(10000));request("/api/status");
  request("/api/radio",maximum+"&wifiScan=0&bleScan=0&rssiFloor=-90",true);
  vTaskDelay(pdMS_TO_TICKS(6000));request("/api/status");
  request("/api/monitor","enabled=0",true);
  request("/api/radio",maximum+"&wifiScan=1&bleScan=1&rssiFloor=-127",true);
  request("/api/tx/start","uasId=SIM-POWER-LEGACY&opId=SIM-TEST&lat=30&lon=120&alt=30&opLat=30.001&opLon=120.001&opAlt=0&duration=5",true);
  request("/api/radio",maximum+"&wifiScan=1&bleScan=1&rssiFloor=-127",true);
  vTaskDelay(pdMS_TO_TICKS(6000));request("/api/status");request("/api/tx/save",restore,true);
  request("/api/simulation/config","lat=30&lon=120&count=100&model=-1",true);
  request("/api/simulation/start",String("seed=")+String(simConfig.seed)+"&duration=30&transport=both",true);
  vTaskDelay(pdMS_TO_TICKS(18000));request("/api/status");request("/api/tx/stop","",true);
  // Exercise low power as well; then retain the highest verified levels, balanced RX, no filter.
  request("/api/radio",String("wifiQuarterDbm=")+String(wifiPowerOptions.items[0].requested)+"&bleDbm="+String(blePowerOptions.items[0].requested)+"&wifiScan=1&bleScan=1&rssiFloor=-127",true);
  request("/api/radio",maximum+"&wifiScan=1&bleScan=1&rssiFloor=-127",true);
  request("/api/status");Serial.println("RADIOCHECK DONE");probeTask=nullptr;vTaskDelete(nullptr);
}

// Serial-only diagnostic: no over-air RID transmission. Feed synthetic receive
// records through the real queue, and drain chunked HTTP with constant memory.
class BenchSink: public Stream {
 public:
  size_t bytes=0;char first=0,last=0;
  int available() override{return 0;}
  int read() override{return -1;}
  int peek() override{return -1;}
  void flush() override{}
  size_t write(uint8_t c) override{if(!bytes)first=c;last=c;++bytes;return 1;}
  size_t write(const uint8_t *p,size_t n) override{if(n){if(!bytes)first=p[0];last=p[n-1];bytes+=n;}return n;}
};
void stabilityProbe(void *) {
  unsigned good=0,bad=0;uint32_t maxMs=0;
  auto request=[&](const char *path,const char *body=nullptr){
    HTTPClient http;http.setTimeout(8000);http.setReuse(false);
    http.begin(String("http://127.0.0.1")+path);
    const uint32_t started=millis();
    int status;
    if(body){http.addHeader("Content-Type","application/x-www-form-urlencoded");status=http.POST(body);}
    else status=http.GET();
    BenchSink sink;const int received=status==200?http.writeToStream(&sink):-1;
    const uint32_t elapsed=millis()-started;if(elapsed>maxMs)maxMs=elapsed;
    bool ok=status==200 && received>0 && sink.first=='{' && sink.last=='}';
    if(ok)++good;else {++bad;Serial.printf("STABILITY ERROR path=%s status=%d received=%d bytes=%u first=%d last=%d elapsed=%lu\n",path,status,received,unsigned(sink.bytes),sink.first,sink.last,(unsigned long)elapsed);}
    http.end();return ok;
  };
  request("/api/monitor","enabled=1");
  Serial.println("STABILITY BEGIN passive scan + 50 synthetic receive rows + HTTP; no RID TX");
  for(unsigned step=0;step<180;++step) {
    if(step%20==0)for(unsigned i=0;i<50;++i){
      rid::Frame f{};f.mac[0]=2;f.mac[4]=0xFE;f.mac[5]=i;f.length=25;f.received=millis();
      f.payload[0]=2;f.payload[1]=0x12;f.rssi=-50;
      snprintf((char*)f.payload+2,21,"BENCH-%03u",i);
      xQueueSend(rxQueue,&f,pdMS_TO_TICKS(50));vTaskDelay(pdMS_TO_TICKS(3));
    }
    request("/api/status");
    if(step%30==29){request("/api/monitor","enabled=0");request("/api/monitor","enabled=1");}
    if(step%20==19)Serial.printf("STABILITY step=%u ok=%u fail=%u maxMs=%lu heap=%u min=%u rows=%u sweeps=%lu scanError=%d tx=%d\n",
      step+1,good,bad,(unsigned long)maxMs,ESP.getFreeHeap(),ESP.getMinFreeHeap(),unsigned(table.count),(unsigned long)sweeps,wifiScanError,txEnabled);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  request("/api/monitor","enabled=0");request("/api/clear","");
  Serial.printf("STABILITY DONE ok=%u fail=%u maxMs=%lu heap=%u min=%u\n",good,bad,(unsigned long)maxMs,ESP.getFreeHeap(),ESP.getMinFreeHeap());
  probeTask=nullptr;vTaskDelete(nullptr);
}

void diagnosticDump(void *) {
  HTTPClient http;http.setTimeout(20000);http.setReuse(false);http.begin("http://127.0.0.1/api/diagnostics");
  const int status=http.GET();Serial.println("CAPTURE JSON BEGIN");
  const int bytes=status==200?http.writeToStream(&Serial):-1;
  Serial.printf("\nCAPTURE JSON END http=%d bytes=%d\n",status,bytes);http.end();probeTask=nullptr;vTaskDelete(nullptr);
}
void probe(void *) {
  const char *paths[] = {"/", "/navigation.js", "/map-picker.js", "/api/status", "/api/config", "/api/health", "/api/radio", "/api/simulation"};
  for (const char *path : paths) {
    WiFiClient client;
    if (!client.connect(IPAddress(127,0,0,1), 80, 3000)) {
      Serial.printf("PROBE %s CONNECT_FAILED errno=%d\n", path, errno); continue;
    }
    client.printf("GET %s HTTP/1.1\r\nHost: 192.168.4.1\r\nConnection: close\r\n\r\n", path);
    String response;
    uint32_t start = millis();
    while ((client.connected() || client.available()) && uint32_t(millis()-start) < 5000) {
      while (client.available()) response += char(client.read());
      vTaskDelay(1);
    }
    Serial.printf("PROBE %s bytes=%u %s\n", path, unsigned(response.length()), response.substring(0,response.indexOf('\r')).c_str());
    if (strncmp(path, "/api/", 5) == 0) Serial.println(response.substring(response.indexOf("\r\n\r\n")+4));
    client.stop();
  }
  probeTask = nullptr; vTaskDelete(nullptr);
}
} // namespace

void setup() {
  Serial.begin(115200);initializeStatusLed();delay(600);
  Serial.printf("\nRID Workbench 2.21 | flash=%u psram=%u reset=%d\n", ESP.getFlashChipSize(), ESP.getPsramSize(), int(esp_reset_reason()));
  pinMode(0, INPUT_PULLUP);
  rxQueue = xQueueCreate(48, sizeof(rid::Frame));
  if (!rxQueue) { Serial.println("FATAL: receive queue allocation failed"); return; }
  ssid = DefaultSsid; password = DefaultPass;
  simConfig.seed=esp_random();if(!simConfig.seed)simConfig.seed=1;

  Preferences prefs;
  if (prefs.begin("ridlab2", true)) {
    ApConfig ap;
    if (prefs.getBytesLength("ap") == sizeof(ap) && prefs.getBytes("ap", &ap, sizeof(ap)) == sizeof(ap) &&
        ap.version == 1 && strnlen(ap.ssid,33) > 0 && strnlen(ap.ssid,33) <= 32 && strnlen(ap.pass,64) <= 63) {
      size_t length = strlen(ap.pass);
      if (!length || length >= 8) { ssid = ap.ssid; password = ap.pass; }
    }
    lab::RadioConfig savedRadio;
    if(prefs.getBytesLength("radio")==sizeof(savedRadio) && prefs.getBytes("radio",&savedRadio,sizeof(savedRadio))==sizeof(savedRadio) && lab::valid(savedRadio))radioConfig=savedRadio;
    lab::Config config;
    if (prefs.getBytesLength("tx") == sizeof(config) && prefs.getBytes("tx", &config, sizeof(config)) == sizeof(config) && lab::valid(config))
      txConfig = config;
    prefs.end();
  }
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(false);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  if (!WiFi.softAP(ssid.c_str(), password.c_str(), Channel, 0, 4)) { Serial.println("FATAL: hotspot failed"); return; }
  wifi_country_t country{}; memcpy(country.cc,"CN",3);country.schan=1;country.nchan=13;country.max_tx_power=20;country.policy=WIFI_COUNTRY_POLICY_MANUAL;
  esp_err_t countryError=esp_wifi_set_country(&country);
  Serial.printf("COUNTRY CN 1-13 result=%d\n",int(countryError));
  esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_SCAN_DONE,scanEvent,nullptr);
  WiFi.onEvent([](arduino_event_id_t event,arduino_event_info_t info){
    if(event==ARDUINO_EVENT_WIFI_AP_STADISCONNECTED){++apDisconnects;Serial.printf("AP disconnect reason=%u\n",info.wifi_ap_stadisconnected.reason);}
  });
  esp_wifi_get_mac(WIFI_IF_AP, apMac);
  // Local subnet only: do not advertise this offline board as an Internet
  // router or DNS resolver. This lets mobile OSes retain cellular routing.
  auto *apNetif=esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if(apNetif) {
    uint8_t offer=0;
    esp_err_t stopped=esp_netif_dhcps_stop(apNetif);
    esp_err_t router=esp_netif_dhcps_option(apNetif,ESP_NETIF_OP_SET,ESP_NETIF_ROUTER_SOLICITATION_ADDRESS,&offer,sizeof(offer));
    esp_err_t dnsOption=esp_netif_dhcps_option(apNetif,ESP_NETIF_OP_SET,ESP_NETIF_DOMAIN_NAME_SERVER,&offer,sizeof(offer));
    esp_err_t started=esp_netif_dhcps_start(apNetif);
    localOnlyDhcp=router==ESP_OK && dnsOption==ESP_OK && started==ESP_OK;
    Serial.printf("LOCALNET stop=%d router=%d dns=%d start=%d localOnly=%d\n",stopped,router,dnsOption,started,localOnlyDhcp);
  }
  esp_wifi_set_ps(WIFI_PS_NONE);
  configureWeb();
  NimBLEDevice::init("");
  initializeRadio();
  bleReady.store(true);
  wifi_promiscuous_filter_t filter{}; filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(wifiCallback);
  setMonitoring(false); // Standby, RGB off; reception starts only on user request.
  Serial.printf("READY AP=%s URL=http://192.168.4.1/ BLE=%d WiFi=%d channel=%u\n", ssid.c_str(), scanning.load(), wifiListening, Channel);
  Serial.println("Commands: i=status p=pause/resume t=HTTP check h=passive stability b=10s single s=30s simulated-100 x=stop");
}
void loop() {
  if (!rxQueue) { delay(100); return; }
  tickTx();
  if(txEnabled && captureActive.load()){endCapture();captureEnded=millis();}
  tickCapture();
  updateStatusLed();
  tickWifiScan();
  server.handleClient();
  rid::Frame f;
  // Bound per-loop processing so busy radio traffic cannot starve HTTP/DNS.
  const uint32_t decodeStarted=micros();
  for (int i = 0; i < 16 && uint32_t(micros()-decodeStarted)<3000 && xQueueReceive(rxQueue, &f, 0) == pdTRUE; ++i){if(table.accept(f))++rxStages[DecodedPayload];else ++rxStages[RejectedPayload];}
  table.expire(millis());
  static uint32_t lastScanCheck = 0;
  if (uint32_t(millis()-lastScanCheck) >= 5000) {
    lastScanCheck = millis();
    if (!txEnabled && monitoring.load() && bleReady.load() && !ble_gap_disc_active()) startBleScan();
  }
  if (rebootAt && int32_t(millis()-rebootAt) >= 0) ESP.restart();
  static uint32_t heldAt = 0; static bool held = false;
  if (digitalRead(0) == LOW) {
    if (!held) { held = true; heldAt = millis(); }
    if (uint32_t(millis()-heldAt) >= 5000) { stopTx(); resetConfig(); ESP.restart(); }
  } else held = false;
  if (Serial.available()) {
    char c = Serial.read();
    if((c=='c' || c=='e') && !txEnabled){if(startCapture(c=='e')){if(!monitoring.load())setMonitoring(true);pauseCaptureWifi();Serial.println("CAPTURE started 60 seconds, passive receive only");}else Serial.println("CAPTURE allocation failed");}
    if(c=='d' && !probeTask){endCapture();captureEnded=millis();tickCapture();if(xTaskCreate(diagnosticDump,"diagDump",6144,nullptr,1,&probeTask)!=pdPASS)Serial.println("CAPTURE dump allocation failed");}
    if (c == 'p' && !txEnabled) setMonitoring(!monitoring.load());
    if (c == 'b') { lab::Config config = txConfig; config.duration = 10; startTx(config); }
    if (c == 's') { lab::SimConfig config=simConfig;config.duration=30;startSimulation(config); }
    if (c == 'x') stopTx();
    if(c=='r' && !probeTask && !txEnabled && xTaskCreate(radioApiProbe,"radioApiProbe",8192,nullptr,1,&probeTask)!=pdPASS)Serial.println("RADIOCHECK TASK_ALLOCATION_FAILED");
    if(c=='v' && !probeTask && !txEnabled && xTaskCreate(simulationApiProbe,"simApiProbe",8192,nullptr,1,&probeTask)!=pdPASS)Serial.println("CHECK TASK_ALLOCATION_FAILED");
    if(c=='h' && !probeTask && !txEnabled && xTaskCreate(stabilityProbe,"stability",6144,nullptr,1,&probeTask)!=pdPASS)Serial.println("STABILITY TASK_ALLOCATION_FAILED");
    if (c == 't' && !probeTask && xTaskCreate(probe, "httpProbe", 6144, nullptr, 1, &probeTask) != pdPASS)
      Serial.println("PROBE TASK_ALLOCATION_FAILED");
    if (c == 'i') Serial.printf("STATUS monitor=%d BLE=%d WiFi=%d devices=%u packets=%lu drops=%lu heap=%u clients=%u tx=%d adv=%d txwifi=%d bleReports=%lu wifiReports=%lu ble5Reports=%lu codedReports=%lu sweeps=%lu scanError=%d mask=%04lx\n",
      monitoring.load(), scanning.load(), wifiListening, unsigned(table.count),
      (unsigned long)table.packets, (unsigned long)dropped.load(), ESP.getFreeHeap(), WiFi.softAPgetStationNum(),
      txEnabled, bleTxActive(), txWifi, (unsigned long)bleReports.load(), (unsigned long)wifiReports.load(), (unsigned long)bleExtendedReports.load(), (unsigned long)bleCodedReports.load(), (unsigned long)sweeps, wifiScanError, (unsigned long)wifiChannelMask.load());
    if (c == 'i') Serial.printf("SIM mode=%d wifiDevices=%u bleDevices=%u wifiFrames=%lu bleSlots=%lu wifiCycles=%lu bleCycles=%lu wifiCycleMs=%lu bleCycleMs=%lu failures=%lu\n",
      txSimulation,simWifiCursor.unique,simBleCursor.unique,(unsigned long)simWifiCursor.slots,(unsigned long)simBleCursor.slots,(unsigned long)simWifiCursor.cycles,(unsigned long)simBleCursor.cycles,(unsigned long)simWifiCursor.lastCycleMs,(unsigned long)simBleCursor.lastCycleMs,(unsigned long)simFailures);
    if(c=='i')Serial.println(String("VERSION 2.21 catalog=")+rid::ModelCatalogVersion+" prefixes="+String(sizeof(rid::SerialModels)/sizeof(rid::SerialModels[0])));
    if(c=='i')Serial.println(String("RECEPTION ")+receptionProgress());
    if(c=='i')Serial.println(String("RADIO " )+radioSettingsJson());
    if(c=='i')Serial.printf("LED pin=%u mode=%s on=%d periodMs=2000\n",statusLedPin,lab::ledModeName(statusLed.mode),appliedLed.r || appliedLed.g);
  }
  delay(2);
}
