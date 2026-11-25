/*
  BLEFanBridge - local Alexa control for FanLamp Pro ceiling fans (no cloud, no edits needed)

  Echo --(your WiFi, local)--> ESP32 pretending to be a Philips Hue bridge --(BLE)--> fan

  NOTHING TO EDIT: flash it as-is.
   1. First boot: the board makes a WiFi network "FanBridge-Setup". Join it from your
      phone, pick your home WiFi (2.4 GHz) and enter its password.
   2. Open the board's page (shown in Serial Monitor, or http://fanbridge.local/),
      go to Settings, tap "+ Add fan", then either:
        - Learn from remote: press the light button on the fan's remote; the board copies
          that remote's identity and sends as it (best when fans share a wall switch), or
        - Pair: power-cycle the fan and tap Pair within 5 s.
   3. "Alexa, discover devices".

  Each fan appears in Alexa as a dimmable light: on/off, and brightness % = fan speed. The web
  page (http://<board-ip>/) has every button of the remote. Your own remote keeps working.

  Board:     ESP32-CAM ("AI Thinker ESP32-CAM") tested; other ESP32 boards with Bluetooth should work
  Core:      "esp32" by Espressif, 3.3.x (Boards Manager)
  Libraries: "WiFiManager" by tzapu, "ESP Async WebServer" and "Async TCP" by ESP32Async
             (Library Manager). Espalexa is included, patched, in src/Espalexa/.
  Partition: Tools > Partition Scheme > "Huge APP (3MB No OTA/1MB SPIFFS)"

  Packet encoding ported from NicoIIT/esphome-components (MIT). See fanlamp.h and README.md.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <deque>

#include "sdkconfig.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"

#include <WiFiManager.h>
#define ESPALEXA_MAXDEVICES 12  // up to 6 fans + 6 lights (library default is 10)
// Async web server: serves many connections at once. The core's WebServer handles one at a time
// and waits up to 5 s on an idle connection, which Android Chrome's spare (preconnected) sockets
// trip over: ERR_TOO_MANY_RETRIES on phones (2026-11-23).
#include <ESPAsyncWebServer.h>
#define ESPALEXA_ASYNC
#include "src/Espalexa/Espalexa.h"  // patched copy of Espalexa 2.7.0 (see src/Espalexa/README)

#include "fanlamp.h"
#include "webui.h"
#include "icons.h"

#if !defined(CONFIG_BLUEDROID_ENABLED)
#error "This sketch needs the Bluedroid BLE stack. Install the esp32 board package version 3.3.x in Boards Manager."
#endif

// ---- Optional tweaks (defaults are fine) ----------------------------
#define TX_DURATION_MS   1000   // how long each command is broadcast; 1500 if commands get missed
#define PAIR_DURATION_MS 3000
#define STATUS_LED       33     // ESP32-CAM red LED (lit when LOW). -1 on other boards.
#define MAX_FANS         6
#define LEARN_SECS       20     // how long "Learn from remote" listens
#define V1_BURST         5      // TYX v1 remote: packets per press (tx 1..5 seen)
#define V1_BURST_MIN_MS  150    // minimum broadcast time per burst packet
// ---------------------------------------------------------------------

struct FanConfig {
  String name;                  // Alexa name, e.g. "Patio Fans"
  String lightName;             // Alexa name for the light, or "" for none
  bool alexa = true;            // false: no Alexa devices for this fan (web page only)
  uint8_t speeds = 6;           // 6 or 3
  uint8_t proto = 3;            // 3 or 2: app protocol used with ownId (Pair fallback)
  uint32_t ownId = 0;           // this board's own random remote ID for this fan
  bool learned = false;         // true: send as the learned remote instead of ownId
  fanlamp::Identity learnedId = {};
};

struct FanState {
  uint8_t txCount = 0;
  bool on = false, light = false, reverse = false;
  bool speedSent = false, lightSent = false, dirSent = false;  // anything sent since boot?
  uint8_t speed = 3;            // last non-zero speed
  EspalexaDevice *alexaFan = nullptr;
  EspalexaDevice *alexaLight = nullptr;
};

struct Pending {
  uint8_t fan;
  uint8_t cmd;
  uint32_t durationMs;
  uint8_t data[fanlamp::PACKET_LEN];
};

// Learn mode. The BT callback only touches `cap` (under learnMux) and the volatile flags;
// everything else (Strings, NVS, Serial) happens in loop().
enum class LearnState : uint8_t { Idle, Listening, Done, Timeout, Unsupported };
struct LearnCapture {
  bool got;                      // first decoded FanLamp packet
  fanlamp::Decoded d;
  bool haveRaw;                  // first unsupported (e.g. v1) packet, for diagnostics
  uint8_t raw[fanlamp::PACKET_LEN];
  uint8_t rawLen;
  uint32_t seen, badCrc;
};

// Learn diagnostics: distinct packets that are very strong (a remote held next to the board)
// or contain the FanLamp F0 08 header anywhere. Printed to Serial when learning ends.
#define DIAG_SLOTS    16
#define DIAG_MIN_RSSI -55
struct DiagPkt {
  uint8_t addr[6];
  int8_t rssi;                   // strongest seen
  uint8_t len;
  bool f008;
  uint16_t count;
  uint8_t data[fanlamp::PACKET_LEN];
};
struct LearnDiag {
  DiagPkt pkt[DIAG_SLOTS];
  uint8_t n;
  uint32_t dropped;
};

static constexpr uint8_t FAN_BLOB_VERSION = 2;  // 2: Format with v1 fields
static constexpr size_t FAN_BLOB_LEN = 33;

// Sniff mode (button mapping): every distinct decoded packet is queued here by the BT callback
// and printed by loop().
#define SNIFF_RING 16
struct SniffRing {
  fanlamp::Decoded d[SNIFF_RING];
  int8_t rssi[SNIFF_RING];
  uint32_t ms[SNIFF_RING];       // millis() when heard
  uint8_t head, tail;            // callback writes head, loop reads tail
  uint32_t dropped;
};

// Press-and-hold buttons (brightness/temperature on v1 remotes). Sniffed 2026-11-22: while held the
// remote sends arg2 0x80 with tx counting up every ~100 ms; on release it repeats the last tx with
// 0x81 for ~150 ms. A quick tap is tx 1..3 then the release.
#define HOLD_BEAT_MS     80      // + the 30 ms gap between broadcasts = ~110 ms per tx
#define HOLD_RELEASE_MS  150
#define HOLD_MIN_TX      3       // a tap still sends tx 1..3, like the remote
#define HOLD_PING_MS     1500    // the page re-sends "start" every 500 ms; stop if it goes quiet
#define HOLD_MAX_MS      10000   // safety cap
struct Hold {
  bool active, stopWanted;
  uint8_t fan, tx;
  fanlamp::Command c;
  uint32_t startedAt, lastPing;
};

// All types are declared above this point: the Arduino builder inserts its generated function
// prototypes just before the first function definition, which is this one.
//
// We use the raw Bluedroid API, not the Arduino BLE library, so the core can't tell BLE is in use
// and frees the BLE controller memory at boot (btStart() then fails). Claim BLE explicitly; the
// unused Classic BT memory is still released and btStart() falls back to BLE-only mode.
extern "C" bool bleInUse(void) { return true; }

FanConfig fans[MAX_FANS];
FanState state[MAX_FANS];
uint8_t numFans = 0;

AsyncWebServer server(80);  // shared by Espalexa (Hue API) and the web UI
AsyncWebServerRequest *rq = nullptr;  // the request being handled (set by web(), under appLock)

// Async handlers run on the network task, not in loop(). appLock makes them and loop() take turns,
// so the fan state, the BLE queue, NVS and Serial are only ever touched by one task at a time.
SemaphoreHandle_t appLock = nullptr;
struct AppLock {
  AppLock() { xSemaphoreTakeRecursive(appLock, portMAX_DELAY); }
  ~AppLock() { xSemaphoreGiveRecursive(appLock); }
};
Espalexa espalexa;
Preferences prefs;
uint32_t restartAt = 0;  // millis() at which to restart, 0 = never

// ---------------------------------------------------------------------
//  Settings (NVS). Keys per fan i: fn<i> name, fl<i> light name, fb<i> binary blob
//  (FAN_BLOB_VERSION + speeds, proto, learned, ownId, learned Identity; see packFan).
// ---------------------------------------------------------------------
uint32_t newOwnId() {
  uint32_t id;
  do { id = esp_random(); } while (id == 0 || id == 0xFFFFFFFF);
  return id;
}

// Blob layout (version 2), little-endian:
//   0 version | 1 speeds | 2 proto | 3 learned | 4..7 ownId
//   learned Format: 8 proto | 9 hasFlags | 10 adFlag | 11 adType | 12 headerLen | 13..17 header
//                   18..20 prefix | 21..22 devType | 23 sign | 24 v1 bits | 25 pairArg3 | 26..27 forcedCrc2
//   28..31 learned id | 32 learned index
void packFan(const FanConfig &f, uint8_t b[FAN_BLOB_LEN]) {
  const fanlamp::Identity &w = f.learnedId;
  const fanlamp::Format &m = w.fmt;
  memset(b, 0, FAN_BLOB_LEN);
  b[0] = FAN_BLOB_VERSION;
  b[1] = f.speeds; b[2] = f.proto; b[3] = f.learned;
  for (int k = 0; k < 4; k++) b[4 + k] = (f.ownId >> (8 * k)) & 0xFF;
  b[8] = m.proto; b[9] = m.hasFlags; b[10] = m.adFlag; b[11] = m.adType;
  b[12] = m.headerLen; memcpy(b + 13, m.header, 5);
  memcpy(b + 18, m.prefix, 3);
  b[21] = m.devType & 0xFF; b[22] = m.devType >> 8;
  b[23] = m.sign; b[24] = m.v1; b[25] = m.pairArg3;
  b[26] = m.forcedCrc2 & 0xFF; b[27] = m.forcedCrc2 >> 8;
  for (int k = 0; k < 4; k++) b[28 + k] = (w.id >> (8 * k)) & 0xFF;
  b[32] = w.index;
}

bool unpackFan(const uint8_t b[FAN_BLOB_LEN], FanConfig &f) {
  if (b[0] != FAN_BLOB_VERSION) return false;
  fanlamp::Identity &w = f.learnedId;
  fanlamp::Format &m = w.fmt;
  f.speeds = b[1] == 3 ? 3 : 6;
  f.proto = b[2] == 2 ? 2 : 3;
  f.learned = b[3] != 0;
  f.ownId = 0;
  for (int k = 0; k < 4; k++) f.ownId |= (uint32_t)b[4 + k] << (8 * k);
  m.proto = b[8]; m.hasFlags = b[9] != 0; m.adFlag = b[10]; m.adType = b[11];
  m.headerLen = b[12]; memcpy(m.header, b + 13, 5);
  memcpy(m.prefix, b + 18, 3);
  m.devType = (uint16_t)b[21] | ((uint16_t)b[22] << 8);
  m.sign = b[23] != 0; m.v1 = b[24]; m.pairArg3 = b[25];
  m.forcedCrc2 = (uint16_t)b[26] | ((uint16_t)b[27] << 8);
  w.id = 0;
  for (int k = 0; k < 4; k++) w.id |= (uint32_t)b[28 + k] << (8 * k);
  w.index = b[32];
  if (f.learned && ((m.proto != 1 && m.proto != 2) || m.headerLen < 2 || m.headerLen > 5)) f.learned = false;
  return f.ownId != 0;
}

void saveFan(uint8_t i) {
  uint8_t b[FAN_BLOB_LEN];
  packFan(fans[i], b);
  String k(i);
  prefs.begin("fanbridge", false);
  prefs.putString(("fn" + k).c_str(), fans[i].name);
  prefs.putString(("fl" + k).c_str(), fans[i].lightName);
  prefs.putBool(("fa" + k).c_str(), fans[i].alexa);
  prefs.putBytes(("fb" + k).c_str(), b, sizeof(b));
  prefs.end();
}

void saveAll() {
  prefs.begin("fanbridge", false);
  prefs.putUChar("nf", numFans);
  for (uint8_t i = numFans; i < MAX_FANS; i++) {  // forget slots past the end
    String k(i);
    prefs.remove(("fn" + k).c_str());
    prefs.remove(("fl" + k).c_str());
    prefs.remove(("fa" + k).c_str());
    prefs.remove(("fb" + k).c_str());
  }
  prefs.end();
  for (uint8_t i = 0; i < numFans; i++) saveFan(i);
}

void loadSettings() {
  prefs.begin("fanbridge", true);
  numFans = prefs.getUChar("nf", 0);
  if (numFans > MAX_FANS) numFans = 0;
  bool repaired[MAX_FANS] = {};
  for (uint8_t i = 0; i < numFans; i++) {
    String k(i);
    fans[i] = FanConfig();
    fans[i].name = prefs.getString(("fn" + k).c_str(), "Fan " + String(i + 1));
    fans[i].lightName = prefs.getString(("fl" + k).c_str(), "");
    fans[i].alexa = prefs.getBool(("fa" + k).c_str(), true);
    uint8_t b[FAN_BLOB_LEN];
    if (prefs.getBytes(("fb" + k).c_str(), b, sizeof(b)) != sizeof(b) || !unpackFan(b, fans[i])) {
      fans[i].speeds = 6; fans[i].proto = 3; fans[i].learned = false;
      fans[i].ownId = newOwnId();
      repaired[i] = true;
    }
  }
  prefs.end();
  for (uint8_t i = 0; i < numFans; i++) {
    if (repaired[i]) { Serial.printf("[NVS] fan %u: settings missing/old, reset to defaults\n", i); saveFan(i); }
  }
}

// What this fan's commands are sent as
fanlamp::Identity identityOf(uint8_t i) {
  if (fans[i].learned) return fans[i].learnedId;
  return {fans[i].proto == 2 ? fanlamp::APP_V2 : fanlamp::APP_V3, fans[i].ownId, 0};
}

String hex32(uint32_t v) { char b[11]; snprintf(b, sizeof b, "0x%08X", (unsigned)v); return b; }

String kindOf(const fanlamp::Format &f) {
  if (f.proto == 1) return String(f.adType == 0x03 ? "app" : "remote") + " v1";
  return String(f.adType == 0x16 ? "remote" : "app") + (f.sign ? " v3" : " v2");
}

String hexBytes(const uint8_t *p, size_t n, const char *sep = "") {
  String s;
  char b[3];
  for (size_t i = 0; i < n; i++) {
    if (i && *sep) s += sep;
    snprintf(b, sizeof b, "%02X", p[i]);
    s += b;
  }
  return s;
}

// One line describing a decoded packet, for Serial
String describe(const fanlamp::Decoded &d) {
  const fanlamp::Format &f = d.who.fmt;
  char m[200];
  snprintf(m, sizeof m,
           "%s id %s idx %u tx %3u cmd 0x%02X p1 0x%02X args %02X %02X %02X seed 0x%04X | flags %s0x%02X type 0x%02X "
           "header %s",
           kindOf(f).c_str(), hex32(d.who.id).c_str(), d.who.index, d.tx, d.cmd.cmd, d.cmd.param1, d.cmd.arg0,
           d.cmd.arg1, d.cmd.arg2, d.seed, f.hasFlags ? "" : "none/", f.adFlag, f.adType,
           hexBytes(f.header, f.headerLen, " ").c_str());
  String s = m;
  if (f.proto == 1) {
    snprintf(m, sizeof m, " v1 bits 0x%02X pairArg3 0x%02X crc2 %s", f.v1, f.pairArg3,
             f.forcedCrc2 ? hex32(f.forcedCrc2).c_str() : "computed");
  } else {
    snprintf(m, sizeof m, " prefix %02X %02X %02X dev 0x%04X sign %d", f.prefix[0], f.prefix[1], f.prefix[2],
             f.devType, f.sign);
  }
  return s + m;
}

// ---------------------------------------------------------------------
//  BLE: advertising queue + learn-mode scanning (raw Bluedroid GAP)
// ---------------------------------------------------------------------
std::deque<Pending> queue_;
Pending current;                 // the command being broadcast now
uint8_t advBuf[fanlamp::PACKET_LEN];
volatile bool advActive = false;
uint32_t advStartedAt = 0, advDuration = 0, advStoppedAt = 0;
bool bleReady = false;

esp_ble_adv_params_t advParams = {
  .adv_int_min = 0x20,  // 20 ms
  .adv_int_max = 0x20,
  .adv_type = ADV_TYPE_NONCONN_IND,
  .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
  .peer_addr = {0},
  .peer_addr_type = BLE_ADDR_TYPE_PUBLIC,
  .channel_map = ADV_CHNL_ALL,
  .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

esp_ble_scan_params_t scanParams = {
  .scan_type = BLE_SCAN_TYPE_PASSIVE,
  .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
  .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
  .scan_interval = 0x50,  // 50 ms
  .scan_window = 0x30,    // 30 ms
  .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,  // remotes repeat the same packet; we want every one
};

portMUX_TYPE learnMux = portMUX_INITIALIZER_UNLOCKED;
LearnCapture cap;
LearnDiag diag;                  // also guarded by learnMux
SniffRing sniff;                 // also guarded by learnMux
volatile bool sniffMode = false; // listening to map buttons: log every decoded packet, save nothing
uint32_t listenSecs = LEARN_SECS;
volatile bool learnScanning = false;
volatile esp_bt_status_t scanStartStatus = ESP_BT_STATUS_SUCCESS;
LearnState learnState = LearnState::Idle;
uint8_t learnFan = 0;
uint32_t learnStartedAt = 0;
String learnMsg;

void statusLed(bool on) {
  if (STATUS_LED >= 0) digitalWrite(STATUS_LED, on ? LOW : HIGH);
}

// Called with learnMux held. Keeps distinct packets; when full, replaces the weakest one.
void diagNote(const uint8_t *addr, int rssi, const uint8_t *adv, uint8_t len, bool f008) {
  for (uint8_t i = 0; i < diag.n; i++) {
    DiagPkt &p = diag.pkt[i];
    if (p.len == len && memcmp(p.data, adv, len) == 0 && memcmp(p.addr, addr, 6) == 0) {
      if (p.count < 0xFFFF) p.count++;
      if (rssi > p.rssi) p.rssi = rssi;
      return;
    }
  }
  uint8_t slot = diag.n;
  if (slot == DIAG_SLOTS) {
    slot = 0;
    for (uint8_t i = 1; i < DIAG_SLOTS; i++)
      if (!diag.pkt[i].f008 && diag.pkt[i].rssi < diag.pkt[slot].rssi) slot = i;
    if (diag.pkt[slot].f008 || (!f008 && rssi <= diag.pkt[slot].rssi)) { diag.dropped++; return; }
  } else {
    diag.n++;
  }
  DiagPkt &p = diag.pkt[slot];
  memcpy(p.addr, addr, 6);
  p.rssi = rssi;
  p.len = len;
  p.f008 = f008;
  p.count = 1;
  memcpy(p.data, adv, len);
}

void onScanResult(const uint8_t *addr, int rssi, const uint8_t *adv, uint8_t len) {
  if (len > fanlamp::PACKET_LEN) len = fanlamp::PACKET_LEN;  // adv data only, no scan response
  fanlamp::Decoded d;
  fanlamp::DecodeResult r = fanlamp::decode(adv, len, d);   // no heap use
  bool f008 = false;
  for (uint8_t i = 0; i + 1 < len; i++) if (adv[i] == 0xF0 && adv[i + 1] == 0x08) { f008 = true; break; }
  portENTER_CRITICAL(&learnMux);
  // FanLamp-looking packets with a bad CRC are kept too (and protected like F0 08 ones): they may
  // be a remote variant the decoder gets wrong.
  bool bad = r == fanlamp::DecodeResult::BadCrc;
  if (f008 || bad || rssi >= DIAG_MIN_RSSI) diagNote(addr, rssi, adv, len, f008 || bad);
  if (sniffMode && r == fanlamp::DecodeResult::Ok) {
    uint8_t next = (sniff.head + 1) % SNIFF_RING;
    if (next == sniff.tail) sniff.dropped++;
    else { sniff.d[sniff.head] = d; sniff.rssi[sniff.head] = rssi; sniff.ms[sniff.head] = millis(); sniff.head = next; }
  }
  cap.seen++;
  if (r == fanlamp::DecodeResult::Ok && !cap.got) {
    cap.d = d;
    cap.got = true;
  } else if (r == fanlamp::DecodeResult::Unsupported && !cap.haveRaw) {
    memcpy(cap.raw, adv, len);
    cap.rawLen = len;
    cap.haveRaw = true;
  } else if (r == fanlamp::DecodeResult::BadCrc) {
    cap.badCrc++;
  }
  portEXIT_CRITICAL(&learnMux);
}

void gapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
      if (advActive && !learnScanning) esp_ble_gap_start_advertising(&advParams);
      break;
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
      if (learnScanning) esp_ble_gap_start_scanning(listenSecs + 2);  // loop() stops it sooner
      break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
      scanStartStatus = param->scan_start_cmpl.status;
      break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
      if (learnScanning && param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT)
        onScanResult(param->scan_rst.bda, param->scan_rst.rssi, param->scan_rst.ble_adv,
                     param->scan_rst.adv_data_len);
      break;
    default:
      break;
  }
}

bool bleBegin() {
  if (!btStarted() && !btStart()) {
    Serial.println("[BLE] controller start failed");
    return false;
  }
  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED && esp_bluedroid_init() != ESP_OK) {
    Serial.println("[BLE] bluedroid init failed");
    return false;
  }
  if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED && esp_bluedroid_enable() != ESP_OK) {
    Serial.println("[BLE] bluedroid enable failed");
    return false;
  }
  esp_ble_gap_register_callback(gapCallback);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);  // max power for range
  Serial.println("[BLE] ready");
  return true;
}

uint8_t nextTx(uint8_t fan) {
  FanState &s = state[fan];
  s.txCount = s.txCount >= 255 ? 1 : s.txCount + 1;  // 255 wraps to 1
  return s.txCount;
}

void sendCommand(uint8_t fan, const fanlamp::Command &c, uint32_t durationMs = TX_DURATION_MS) {
  fanlamp::Identity who = identityOf(fan);
  // The TYX-style v1 remote (seed always 0) numbers every press 1..5 and starts again at 1 on the
  // next press, so copy that burst instead of keeping a running tx count.
  bool burst = who.fmt.proto == 1 && (who.fmt.v1 & fanlamp::V1_SEED_ZERO);

  holdEnd(fan);  // another button while brightness/temperature is held: let go first, like a remote
  // drop an older, not-yet-sent command of the same kind for the same fan, so taps never pile up
  for (auto it = queue_.begin(); it != queue_.end();) {
    if (it->fan == fan && it->cmd == c.cmd) it = queue_.erase(it); else ++it;
  }

  Pending p;
  p.fan = fan;
  p.cmd = c.cmd;
  uint8_t firstTx;
  if (burst) {
    p.durationMs = max((uint32_t)V1_BURST_MIN_MS, durationMs / V1_BURST);
    firstTx = 1;
    for (uint8_t t = 1; t <= V1_BURST; t++) {
      fanlamp::encode(p.data, who, t, 0, c);
      queue_.push_back(p);
    }
    fanlamp::encode(p.data, who, firstTx, 0, c);  // for the log line below
  } else {
    firstTx = nextTx(fan);
    p.durationMs = durationMs;
    fanlamp::encode(p.data, who, firstTx, 1 + (esp_random() % 0xFFF4), c);
    queue_.push_back(p);
  }

  Serial.printf("[BLE] %s (%s %s): cmd 0x%02X p1 0x%02X args %02X %02X %02X ", fans[fan].name.c_str(),
                kindOf(who.fmt).c_str(), hex32(who.id).c_str(), c.cmd, c.param1, c.arg0, c.arg1, c.arg2);
  if (burst) Serial.printf("burst tx 1..%u, %u ms each", V1_BURST, (unsigned)p.durationMs);
  else Serial.printf("tx %u", firstTx);
  Serial.printf("%s -> ", learnScanning ? " (queued until listening ends)" : "");
  for (size_t i = 0; i < fanlamp::PACKET_LEN; i++) Serial.printf("%02X", p.data[i]);
  Serial.println();
}

Hold hold = {};

void queuePacket(uint8_t fan, const fanlamp::Command &c, uint8_t tx, uint32_t ms) {
  Pending p;
  p.fan = fan;
  p.cmd = c.cmd;
  p.durationMs = ms;
  fanlamp::encode(p.data, identityOf(fan), tx, 0, c);
  queue_.push_back(p);
}

// Ends a hold now: queue the release packet (same tx as the last press packet).
void holdRelease() {
  if (!hold.active) return;
  fanlamp::Command rel = hold.c;
  rel.arg2 = fanlamp::V1_RELEASE;
  queuePacket(hold.fan, rel, hold.tx ? hold.tx : 1, HOLD_RELEASE_MS);
  Serial.printf("[BLE] %s: hold cmd 0x%02X released after tx %u, %lu ms\n", fans[hold.fan].name.c_str(), hold.c.cmd,
                hold.tx, (unsigned long)(millis() - hold.startedAt));
  hold.active = false;
}

void holdKeepAlive(uint8_t fan, uint8_t cmd) {
  if (hold.active && hold.fan == fan && hold.c.cmd == cmd && !hold.stopWanted) hold.lastPing = millis();
}

void holdEnd(uint8_t fan) { if (hold.active && hold.fan == fan) holdRelease(); }

// start: begin (or keep alive) holding cmd; !start: let go (after at least HOLD_MIN_TX packets).
void holdButton(uint8_t fan, uint8_t cmd, bool start) {
  uint32_t now = millis();
  if (start) {
    if (hold.active && hold.fan == fan && hold.c.cmd == cmd) { hold.lastPing = now; hold.stopWanted = false; return; }
    holdRelease();  // a different button was held: let go of it first
    hold = {};
    hold.active = true;
    hold.fan = fan;
    hold.c = fanlamp::v1Press(cmd);
    hold.startedAt = hold.lastPing = now;
    Serial.printf("[BLE] %s: hold cmd 0x%02X\n", fans[fan].name.c_str(), cmd);
  } else if (hold.active && hold.fan == fan && hold.c.cmd == cmd) {
    hold.stopWanted = true;
  }
}

void holdLoop() {
  if (!hold.active) return;
  uint32_t now = millis();
  if (now - hold.lastPing > HOLD_PING_MS || now - hold.startedAt > HOLD_MAX_MS) hold.stopWanted = true;
  if (hold.stopWanted && hold.tx >= HOLD_MIN_TX) { holdRelease(); return; }
  // next press packet once everything before it has gone out
  if (queue_.empty() && !advActive && hold.tx < 255) queuePacket(hold.fan, hold.c, ++hold.tx, HOLD_BEAT_MS);
}

void bleLoop() {
  uint32_t now = millis();
  if (!learnScanning) holdLoop();
  if (advActive && now - advStartedAt >= advDuration) {
    esp_ble_gap_stop_advertising();
    advActive = false;
    advStoppedAt = now;
    statusLed(false);
  }
  if (learnScanning) return;  // hold the queue while listening for a remote
  if (!advActive && !queue_.empty() && now - advStoppedAt >= 30) {
    current = queue_.front();
    queue_.pop_front();
    memcpy(advBuf, current.data, sizeof(advBuf));
    advDuration = current.durationMs;
    advStartedAt = now;
    advActive = true;
    statusLed(true);
    esp_ble_gap_config_adv_data_raw(advBuf, sizeof(advBuf));  // starts advertising in gapCallback
  }
}

// Returns "" on success, else a message for the user.
// fan: the fan to learn for, or SNIFF_FAN to only log every decoded packet (button mapping).
static constexpr uint8_t SNIFF_FAN = 0xFF;
uint32_t sniffPrinted = 0, sniffRepeats = 0, sniffLastMs = 0;
fanlamp::Decoded sniffLast;

String learnStart(uint8_t fan, uint32_t secs) {
  if (!bleReady) return "Bluetooth isn't running, so the board can't listen. Check the serial log.";
  if (learnState == LearnState::Listening) return "Already listening; wait for it to finish.";

  portENTER_CRITICAL(&learnMux);
  memset(&cap, 0, sizeof(cap));
  memset(&diag, 0, sizeof(diag));
  sniff.head = sniff.tail = 0;
  sniff.dropped = 0;
  sniffMode = fan == SNIFF_FAN;
  portEXIT_CRITICAL(&learnMux);
  listenSecs = secs;
  sniffPrinted = 0;
  sniffRepeats = 0;
  memset(&sniffLast, 0, sizeof(sniffLast));

  if (advActive) {  // cut the current broadcast short and send it again after learning
    esp_ble_gap_stop_advertising();
    advActive = false;
    queue_.push_front(current);
  }
  scanStartStatus = ESP_BT_STATUS_SUCCESS;
  learnScanning = true;
  learnFan = fan;
  learnStartedAt = millis();
  learnState = LearnState::Listening;
  learnMsg = "";
  esp_err_t err = esp_ble_gap_set_scan_params(&scanParams);  // scanning starts in gapCallback
  if (err != ESP_OK) {
    learnScanning = false;
    learnState = LearnState::Idle;
    Serial.printf("[Learn] set_scan_params failed: %s\n", esp_err_to_name(err));
    return String("Couldn't start the Bluetooth scan (") + esp_err_to_name(err) + ").";
  }
  statusLed(true);
  if (sniffMode)
    Serial.printf("[Sniff] listening %u s: press each remote button once, a second or two apart\n", (unsigned)secs);
  else
    Serial.printf("[Learn] listening %u s for a remote for fan %u \"%s\"\n", (unsigned)secs, fan, fans[fan].name.c_str());
  return "";
}

// Prints decoded packets queued by the callback; a remote repeats each press many times,
// so identical consecutive packets are printed once.
// A remote's light-on crc quirk (fanlamp::v1LightCrc) can only be seen in a light-on packet. When
// Learn or Sniff hears one from a remote a fan has learned, store it on that fan.
void noteLightCrc(const fanlamp::Decoded &d) {
  using namespace fanlamp;
  if (d.cmd.cmd != CMD_LIGHT_ON || d.who.fmt.proto != 1 || !(d.who.fmt.v1 & V1_CRC1_ZERO)) return;
  for (uint8_t i = 0; i < numFans; i++) {
    Identity &w = fans[i].learnedId;
    if (!fans[i].learned || w.fmt.proto != 1 || w.id != d.who.id || w.index != d.who.index) continue;
    uint8_t v = (w.fmt.v1 & ~V1_LIGHT_CRC_MASK) | (d.who.fmt.v1 & V1_LIGHT_CRC_MASK);
    if (v == w.fmt.v1) continue;
    w.fmt.v1 = v;
    saveFan(i);
    Serial.printf("[Learn] fan %u (%s): light-on crc value is now %u\n", i, fans[i].name.c_str(), v1LightCrc(v));
  }
}

void sniffDrain() {
  while (true) {
    fanlamp::Decoded d;
    int8_t rssi;
    portENTER_CRITICAL(&learnMux);
    bool any = sniff.tail != sniff.head;
    uint32_t ms = 0;
    if (any) {
      d = sniff.d[sniff.tail];
      rssi = sniff.rssi[sniff.tail];
      ms = sniff.ms[sniff.tail];
      sniff.tail = (sniff.tail + 1) % SNIFF_RING;
    }
    portEXIT_CRITICAL(&learnMux);
    if (!any) return;
    bool same = d.who.id == sniffLast.who.id && d.tx == sniffLast.tx && d.cmd.cmd == sniffLast.cmd.cmd &&
                d.cmd.param1 == sniffLast.cmd.param1 && d.cmd.arg0 == sniffLast.cmd.arg0 &&
                d.cmd.arg1 == sniffLast.cmd.arg1 && d.cmd.arg2 == sniffLast.cmd.arg2;
    if (same) { sniffRepeats++; sniffLastMs = ms; continue; }
    // timing: when the previous distinct packet was last heard and how often it repeated, then
    // this packet's time relative to the sniff start
    if (sniffPrinted) Serial.printf("[Sniff]      (#%u heard %u more time(s), last at %lu ms)\n", (unsigned)sniffPrinted,
                                    (unsigned)sniffRepeats, (unsigned long)(sniffLastMs - learnStartedAt));
    sniffRepeats = 0;
    sniffLastMs = ms;
    noteLightCrc(d);
    sniffLast = d;
    sniffPrinted++;
    Serial.printf("[Sniff] #%u %6lu ms %4d dBm %s\n", (unsigned)sniffPrinted, (unsigned long)(ms - learnStartedAt), rssi,
                  describe(d).c_str());
  }
}

void learnPrintDiag() {
  static LearnDiag dg;  // static: too big for the loop task's stack comfort
  portENTER_CRITICAL(&learnMux);
  dg = diag;
  portEXIT_CRITICAL(&learnMux);
  Serial.printf("[Learn] diag: %u packet(s) that were strong (>= %d dBm) or contained F0 08, %u dropped\n", dg.n,
                DIAG_MIN_RSSI, (unsigned)dg.dropped);
  for (uint8_t i = 0; i < dg.n; i++) {
    const DiagPkt &p = dg.pkt[i];
    Serial.printf("[Learn]   %02X:%02X:%02X:%02X:%02X:%02X %4d dBm x%-4u %s len %2u ", p.addr[0], p.addr[1], p.addr[2],
                  p.addr[3], p.addr[4], p.addr[5], p.rssi, p.count, p.f008 ? "F008" : "    ", p.len);
    for (uint8_t k = 0; k < p.len; k++) Serial.printf("%02X", p.data[k]);
    Serial.println();
  }
}

void learnFinish(LearnState st, const String &msg) {
  learnScanning = false;
  esp_ble_gap_stop_scanning();
  advStoppedAt = millis();
  statusLed(false);
  learnState = st;
  learnMsg = msg;
  if (sniffMode) {
    sniffDrain();
    if (sniffPrinted) Serial.printf("[Sniff]      (#%u heard %u more time(s), last at %lu ms)\n", (unsigned)sniffPrinted,
                                    (unsigned)sniffRepeats, (unsigned long)(sniffLastMs - learnStartedAt));
    sniffMode = false;
    Serial.printf("[Sniff] %s\n", msg.c_str());
    learnPrintDiag();
    return;
  }
  Serial.printf("[Learn] %s\n", msg.c_str());
  learnPrintDiag();
}

void learnLoop() {
  if (learnState != LearnState::Listening) return;
  LearnCapture c;
  portENTER_CRITICAL(&learnMux);
  c = cap;
  portEXIT_CRITICAL(&learnMux);

  if (scanStartStatus != ESP_BT_STATUS_SUCCESS) {
    learnFinish(LearnState::Timeout, "Couldn't start the Bluetooth scan (status " + String((int)scanStartStatus) +
                                         "). Try again, or use Pair.");
    return;
  }

  if (sniffMode) {
    sniffDrain();
    if (millis() - learnStartedAt >= listenSecs * 1000UL) {
      Serial.printf("[Sniff] heard %u advertisements, %u FanLamp-looking with a bad CRC, %u dropped\n",
                    (unsigned)c.seen, (unsigned)c.badCrc, (unsigned)sniff.dropped);
      learnFinish(LearnState::Done, "Done listening: " + String(sniffPrinted) + " distinct press(es) logged to Serial.");
    }
    return;
  }

  if (c.got) {
    FanConfig &f = fans[learnFan];
    // A press other than light-on can't show the light-on crc value: keep the one already known
    // for this same remote.
    uint8_t keep = 0;
    if (f.learned && f.learnedId.fmt.proto == 1 && f.learnedId.id == c.d.who.id && c.d.cmd.cmd != fanlamp::CMD_LIGHT_ON)
      keep = f.learnedId.fmt.v1 & fanlamp::V1_LIGHT_CRC_MASK;
    f.learned = true;
    f.learnedId = c.d.who;
    f.learnedId.fmt.v1 |= keep;
    state[learnFan].txCount = c.d.tx;  // next command uses the remote's tx + 1
    saveFan(learnFan);
    char m[160];
    snprintf(m, sizeof m, "Learned remote %s (%s). Heard cmd 0x%02X p1 0x%02X arg %u %u %u.",
             hex32(c.d.who.id).c_str(), kindOf(c.d.who.fmt).c_str(), c.d.cmd.cmd, c.d.cmd.param1, c.d.cmd.arg0,
             c.d.cmd.arg1, c.d.cmd.arg2);
    Serial.printf("[Learn] fan %u: %s\n", learnFan, describe(c.d).c_str());
    learnFinish(LearnState::Done, m);
    return;
  }

  if (millis() - learnStartedAt >= listenSecs * 1000UL) {
    Serial.printf("[Learn] heard %u advertisements, %u FanLamp-looking with a bad CRC\n", (unsigned)c.seen,
                  (unsigned)c.badCrc);
    if (c.haveRaw) {
      String hex;
      char b[3];
      for (uint8_t i = 0; i < c.rawLen; i++) { snprintf(b, sizeof b, "%02X", c.raw[i]); hex += b; }
      learnFinish(LearnState::Unsupported, "Heard a FanLamp remote in a variant this board can't copy yet: raw hex " +
                                               hex + ". Use Pair for this fan instead.");
    } else {
      learnFinish(LearnState::Timeout,
                  "Didn't hear a remote. Press a button on the remote close to the board, or use Pair.");
    }
  }
}

// ---------------------------------------------------------------------
//  Fan actions
// ---------------------------------------------------------------------
// Alexa brightness % -> fan speed. 6 speeds: 1-16% = 1, 17-33% = 2, 34-50% = 3, ... 84-100% = 6
uint8_t percentToSpeed(uint8_t fan, uint8_t pct) {
  uint8_t n = fans[fan].speeds;
  if (pct == 0) return 0;
  return constrain((pct * n + 99) / 100, 1, n);
}

uint8_t speedToPercent(uint8_t fan, uint8_t speed) {
  return (uint8_t)(speed * 100 / fans[fan].speeds);  // 6 speeds: 16,33,50,66,83,100
}

// Tell Espalexa the real state so the Alexa app shows it correctly
void syncAlexa(uint8_t fan) {
  FanState &s = state[fan];
  if (s.alexaFan) {
    s.alexaFan->setPercent(speedToPercent(fan, s.speed));
    s.alexaFan->setState(s.on);
  }
  if (s.alexaLight) s.alexaLight->setState(s.light);
}

void fanSetSpeed(uint8_t fan, uint8_t speed) {
  FanState &s = state[fan];
  if (speed == 0) {
    s.on = false;
  } else {
    s.on = true;
    s.speed = constrain(speed, 1, fans[fan].speeds);
  }
  s.speedSent = true;
  sendCommand(fan, fanlamp::fanSpeedFor(identityOf(fan).fmt, s.on ? s.speed : 0, fans[fan].speeds));
}

void fanSetPower(uint8_t fan, bool on) { fanSetSpeed(fan, on ? state[fan].speed : 0); }

void fanSetLight(uint8_t fan, bool on) {
  state[fan].light = on;
  state[fan].lightSent = true;
  sendCommand(fan, on ? fanlamp::lightOn() : fanlamp::lightOff());
}

// The extra buttons (brightness, temperature, K, night, breeze) are only known for v1 remotes.
bool hasExtras(uint8_t fan) { return identityOf(fan).fmt.proto == 1; }

void fanAllOff(uint8_t fan) {
  FanState &s = state[fan];
  s.on = false; s.light = false;
  s.speedSent = s.lightSent = true;
  sendCommand(fan, fanlamp::allOff());
}

void fanSetDirection(uint8_t fan, bool reverse) {
  state[fan].reverse = reverse;
  state[fan].dirSent = true;
  sendCommand(fan, fanlamp::fanDirectionFor(identityOf(fan).fmt, reverse));
}

// ---------------------------------------------------------------------
//  Alexa (Espalexa) callbacks
// ---------------------------------------------------------------------
int findFan(EspalexaDevice *d, bool light) {
  for (uint8_t i = 0; i < numFans; i++) {
    if (d == (light ? state[i].alexaLight : state[i].alexaFan)) return i;
  }
  return -1;
}

void onAlexaFan(EspalexaDevice *d) {
  int i = findFan(d, false);
  if (i < 0) return;
  switch (d->getLastChangedProperty()) {
    case EspalexaDeviceProperty::off:
      fanSetPower(i, false);
      break;
    case EspalexaDeviceProperty::on:
      fanSetPower(i, true);  // back to the last speed
      break;
    default:                 // "set Patio Fans to 50 percent", "dim/brighten"
      fanSetSpeed(i, percentToSpeed(i, d->getPercent()));
      break;
  }
  syncAlexa(i);
}

void onAlexaLight(EspalexaDevice *d) {
  int i = findFan(d, true);
  if (i < 0) return;
  fanSetLight(i, d->getState());
}

// ---------------------------------------------------------------------
//  Web UI + JSON API
// ---------------------------------------------------------------------
String jsonEscape(const String &s) {
  String o;
  o.reserve(s.length() + 2);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) { char b[7]; snprintf(b, sizeof b, "\\u%04x", (uint8_t)c); o += b; }
    else o += c;
  }
  return o;
}

String jstr(const String &s) { return "\"" + jsonEscape(s) + "\""; }
const char *jbool(bool b) { return b ? "true" : "false"; }

// Trim, drop control characters and quotes/backslashes (Espalexa puts names in JSON as-is),
// cap at 32 bytes without splitting a UTF-8 character.
String cleanName(const String &in) {
  String o;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if ((uint8_t)c >= 0x20 && c != '"' && c != '\\') o += c;
  }
  o.trim();
  if (o.length() > 32) {
    size_t n = 32;
    while (n > 0 && ((uint8_t)o[n] & 0xC0) == 0x80) n--;
    o = o.substring(0, n);
    o.trim();
  }
  return o;
}

// One line per page/API request (not the frequent polls), to see which browser reaches the board.
void logRequest(AsyncWebServerRequest *r) {
  const String &u = r->url();
  if (u == "/status" || u == "/learnstatus" || u == "/fanctl") return;
  const AsyncWebHeader *ua = r->getHeader("User-Agent");
  Serial.printf("[HTTP] %s %s %s  (%s)\n", r->client()->remoteIP().toString().c_str(), r->methodToString(), u.c_str(),
                ua ? ua->value().substring(0, 80).c_str() : "-");
}

void sendJson(int code, const String &body) { rq->send(code, "application/json", body); }

void sendResult(bool ok, const String &msg, bool restart = false) {
  sendJson(ok ? 200 : 400, String("{\"ok\":") + jbool(ok) + ",\"restart\":" + jbool(restart) + ",\"msg\":" + jstr(msg) + "}");
  if (restart) restartAt = millis() + 800;  // after the response has gone out
}

int fanArg() {
  if (!rq->hasArg("fan")) return -1;
  int f = rq->arg("fan").toInt();
  return (f >= 0 && f < numFans) ? f : -1;
}

void sendStatic(const char *type, const char *body, bool cache) {
  AsyncWebServerResponse *r = rq->beginResponse(200, type, (const uint8_t *)body, strlen(body));
  if (cache) r->addHeader("Cache-Control", "max-age=3600");
  rq->send(r);
}

void sendPng(const uint8_t *png, size_t len) {
  AsyncWebServerResponse *r = rq->beginResponse(200, "image/png", png, len);
  r->addHeader("Cache-Control", "max-age=86400");
  rq->send(r);
}

void handleStatus() {
  String j = String("{\"learning\":") + jbool(learnState == LearnState::Listening) + ",\"fans\":[";
  for (uint8_t i = 0; i < numFans; i++) {
    const FanState &s = state[i];
    if (i) j += ',';
    j += "{\"name\":" + jstr(fans[i].name) + ",\"light\":" + jstr(fans[i].lightName) +
         ",\"speeds\":" + fans[i].speeds + ",\"on\":" + jbool(s.on) + ",\"speed\":" + s.speed +
         ",\"lightOn\":" + jbool(s.light) + ",\"reverse\":" + jbool(s.reverse) +
         ",\"speedSent\":" + jbool(s.speedSent) + ",\"lightSent\":" + jbool(s.lightSent) +
         ",\"dirSent\":" + jbool(s.dirSent) + ",\"learned\":" + jbool(fans[i].learned) +
         ",\"extras\":" + jbool(hasExtras(i)) + "}";
  }
  sendJson(200, j + "]}");
}

void handleConfig() {
  String j = String("{\"max\":") + MAX_FANS + ",\"fans\":[";
  for (uint8_t i = 0; i < numFans; i++) {
    const FanConfig &f = fans[i];
    if (i) j += ',';
    j += "{\"name\":" + jstr(f.name) + ",\"light\":" + jstr(f.lightName) + ",\"speeds\":" + f.speeds +
         ",\"proto\":" + f.proto + ",\"ownId\":" + jstr(hex32(f.ownId)) + ",\"learned\":" + jbool(f.learned) + ",\"alexa\":" + jbool(f.alexa);
    if (f.learned) j += ",\"remote\":" + jstr(hex32(f.learnedId.id)) + ",\"kind\":" + jstr(kindOf(f.learnedId.fmt));
    j += "}";
  }
  sendJson(200, j + "]}");
}

void handleFanCtl() {
  int fan = fanArg();
  String action = rq->arg("do");
  int v = rq->arg("v").toInt();
  if (fan < 0) { rq->send(400, "text/plain", "bad fan"); return; }

  if (action == "pair")        sendCommand(fan, fanlamp::pair(), PAIR_DURATION_MS);
  else if (action == "unpair") sendCommand(fan, fanlamp::unpair(), PAIR_DURATION_MS);
  else if (action == "speed")  { fanSetSpeed(fan, constrain(v, 0, 6)); syncAlexa(fan); }
  else if (action == "dir")    fanSetDirection(fan, v == 1);
  else if (action == "light")  { fanSetLight(fan, v == 1); syncAlexa(fan); }
  else if (action == "alloff") { fanAllOff(fan); syncAlexa(fan); }
  else if (action == "bright" || action == "temp" || action == "cct" || action == "night" || action == "breeze") {
    using namespace fanlamp;
    if (!hasExtras(fan)) { rq->send(400, "text/plain", "This button is only known for learned TYX (v1) remotes."); return; }
    String w;
    if (action == "bright" || action == "temp") {
      // hold=1: pressed, hold=2: still held (keep-alive every 500 ms; never starts a new hold, so a
      // late one can't restart the ramp), hold=0: released, no hold arg: a single tap
      uint8_t cmd = action == "bright" ? (v == 1 ? CMD_V1_BRIGHT_UP : CMD_V1_BRIGHT_DOWN)
                                       : (v == 1 ? CMD_V1_TEMP_UP : CMD_V1_TEMP_DOWN);
      String h = rq->arg("hold");
      if (h == "2") { holdKeepAlive(fan, cmd); rq->send(200, "text/plain", "ok"); return; }
      if (h == "1") holdButton(fan, cmd, true);
      else if (h == "0") holdButton(fan, cmd, false);
      else { holdButton(fan, cmd, true); holdButton(fan, cmd, false); }
      w = action == "bright" ? (v == 1 ? "brighter" : "dimmer") : (v == 1 ? "temperature +" : "temperature -");
      w += h == "1" ? " (holding)" : h == "0" ? " (released)" : "";
    }
    else if (action == "cct") {
      // v: 0 warm (00 FF, sniffed), 1 neutral (FF FF, sniffed), 2 cool (FF 00, guessed)
      static const uint8_t lv[3][2] = {{0x00, 0xFF}, {0xFF, 0xFF}, {0xFF, 0x00}};
      uint8_t k = constrain(v, 0, 2);
      sendCommand(fan, v1Cct(lv[k][0], lv[k][1]));
      w = k == 0 ? "warm white" : k == 1 ? "neutral white" : "cool white";
    }
    else if (action == "night") { state[fan].light = true; state[fan].lightSent = true; sendCommand(fan, v1Night()); w = "night light"; syncAlexa(fan); }
    else                        { sendCommand(fan, v1Breeze()); w = "breeze"; }
    rq->send(200, "text/plain", fans[fan].name + ": " + w + (learnScanning ? " queued (listening for a remote)" : " sent"));
    return;
  }
  else { rq->send(400, "text/plain", "bad action"); return; }

  String what = action;
  if (action == "speed") what = v ? "speed " + String(v) : "off";
  else if (action == "light") what = v == 1 ? "light on" : "light off";
  else if (action == "dir") what = v == 1 ? "reverse" : "forward";
  else if (action == "alloff") what = "all off";
  rq->send(200, "text/plain", fans[fan].name + ": " + what + (learnScanning ? " queued (listening for a remote)" : " sent"));
}

void handleFanAdd() {
  if (numFans >= MAX_FANS) { sendResult(false, "That's the maximum of " + String(MAX_FANS) + " fans."); return; }
  uint8_t i = numFans++;
  fans[i] = FanConfig();
  fans[i].name = "Fan " + String(i + 1);
  fans[i].ownId = newOwnId();
  saveAll();
  Serial.printf("[Web] added fan %u (own ID %s)\n", i, hex32(fans[i].ownId).c_str());
  sendResult(true, "Added \"" + fans[i].name + "\". Restarting; this page reloads in a few seconds.", true);
}

void handleFanSave() {
  int i = fanArg();
  if (i < 0) { sendResult(false, "Unknown fan."); return; }
  FanConfig &f = fans[i];
  String name = cleanName(rq->arg("name"));
  if (!name.length()) name = "Fan " + String(i + 1);
  String light = cleanName(rq->arg("light"));
  bool alexa = rq->hasArg("alexa") ? rq->arg("alexa") == "1" : f.alexa;
  bool restart = name != f.name || light != f.lightName || alexa != f.alexa;  // Alexa devices are created at boot
  bool hidden = f.alexa && !alexa;
  f.name = name;
  f.lightName = light;
  f.alexa = alexa;
  f.speeds = rq->arg("speeds") == "3" ? 3 : 6;
  if (rq->hasArg("proto")) f.proto = rq->arg("proto") == "2" ? 2 : 3;
  if (state[i].speed > f.speeds) state[i].speed = f.speeds;
  saveFan(i);
  Serial.printf("[Web] saved fan %d \"%s\" light \"%s\" speeds %u proto v%u alexa %u\n", i, f.name.c_str(),
                f.lightName.c_str(), f.speeds, f.proto, f.alexa);
  if (hidden) sendResult(true, "Saved. Restarting; this page reloads in a few seconds. If Alexa already found this fan, "
                               "delete it in the Alexa app.", true);
  else if (restart) sendResult(true, "Saved. Restarting; this page reloads in a few seconds. Then say \"Alexa, discover devices\".", true);
  else sendResult(true, "Saved.");
}

void handleFanRemove() {
  int i = fanArg();
  if (i < 0) { sendResult(false, "Unknown fan."); return; }
  String name = fans[i].name;
  for (uint8_t k = i; k + 1 < numFans; k++) fans[k] = fans[k + 1];
  numFans--;
  saveAll();
  Serial.printf("[Web] removed fan %d \"%s\"\n", i, name.c_str());
  sendResult(true, "Removed \"" + name + "\". Restarting; this page reloads in a few seconds. "
                   "Delete the old device in the Alexa app too.", true);
}

void handleLearn() {
  int i = fanArg();
  if (i < 0) { sendResult(false, "Unknown fan."); return; }
  String err = learnStart(i, LEARN_SECS);
  if (err.length()) sendResult(false, err);
  else sendResult(true, "Listening");
}

// GET /sniff[?secs=N]: button-mapping helper. Listens (default 60 s, max 300) and logs every
// distinct decoded remote press to Serial; saves nothing. Progress via /learnstatus.
void handleSniff() {
  int secs = rq->hasArg("secs") ? rq->arg("secs").toInt() : 60;
  secs = constrain(secs, 5, 300);
  String err = learnStart(SNIFF_FAN, secs);
  if (err.length()) sendResult(false, err);
  else sendResult(true, "Listening " + String(secs) + " s. Press each remote button once, a second or two apart. "
                        "Results go to the serial log.");
}

void handleLearnStatus() {
  const char *st = "idle";
  switch (learnState) {
    case LearnState::Listening:   st = "listening"; break;
    case LearnState::Done:        st = "done"; break;
    case LearnState::Timeout:     st = "timeout"; break;
    case LearnState::Unsupported: st = "unsupported"; break;
    default: break;
  }
  uint32_t elapsed = (millis() - learnStartedAt) / 1000;
  int secs = learnState == LearnState::Listening ? max(0, (int)listenSecs - (int)elapsed) : 0;
  sendJson(200, String("{\"state\":\"") + st + "\",\"fan\":" + learnFan + ",\"secs\":" + secs + ",\"msg\":" + jstr(learnMsg) + "}");
}

void handleForget() {
  int i = fanArg();
  if (i < 0) { sendResult(false, "Unknown fan."); return; }
  fans[i].learned = false;
  saveFan(i);
  Serial.printf("[Web] fan %d: forgot learned remote, back to own ID %s\n", i, hex32(fans[i].ownId).c_str());
  sendResult(true, "Forgot the learned remote. Pair this fan to use the board's own ID.");
}

void handleWifiReset() {
  prefs.begin("fanbridge", false);
  prefs.putBool("wifireset", true);
  prefs.end();
  Serial.println("[Web] WiFi reset requested");
  sendResult(true, "Restarting into setup mode.", true);
}

// ---------------------------------------------------------------------
void setup() {
  appLock = xSemaphoreCreateRecursiveMutex();
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\nBLE Fan Bridge (local Alexa)");
  if (STATUS_LED >= 0) { pinMode(STATUS_LED, OUTPUT); statusLed(false); }

  loadSettings();
  for (uint8_t i = 0; i < MAX_FANS; i++) state[i].txCount = 1 + (esp_random() % 200);
  for (uint8_t i = 0; i < numFans; i++) {
    fanlamp::Identity w = identityOf(i);
    Serial.printf("[Fan %u] \"%s\" light \"%s\" speeds %u, sends as %s %s%s\n", i, fans[i].name.c_str(),
                  fans[i].lightName.c_str(), fans[i].speeds, kindOf(w.fmt).c_str(), hex32(w.id).c_str(),
                  fans[i].learned ? " (learned)" : " (own ID, needs Pair)");
  }
  if (!numFans) Serial.println("[Fan] none configured yet - add one under Settings");

  // WiFi: joins the saved network, or opens the "FanBridge-Setup" network to choose one.
  bool wantReset = false;
  prefs.begin("fanbridge", false);
  wantReset = prefs.getBool("wifireset", false);
  if (wantReset) prefs.putBool("wifireset", false);
  prefs.end();

  WiFi.mode(WIFI_STA);
  {  // scope: WiFiManager and its setup portal are gone before our own web server starts
  WiFiManager wm;
  if (wantReset) wm.resetSettings();
  wm.setHostname("fanbridge");
  wm.setTitle("Fan Bridge");
  wm.setConfigPortalTimeout(300);  // 5 minutes, then reboot and try again
  statusLed(true);                 // LED stays on while waiting for WiFi setup
  if (!wm.autoConnect("FanBridge-Setup")) {
    Serial.println("[WiFi] setup timed out, restarting");
    ESP.restart();
  }
  }
  statusLed(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(true);  // required when WiFi and Bluetooth share the radio
  Serial.printf("[WiFi] connected - control page: http://%s/  (or http://fanbridge.local/)\n",
                WiFi.localIP().toString().c_str());

  bleReady = bleBegin();

  // Alexa devices
  for (uint8_t i = 0; i < numFans; i++) {
    if (!fans[i].alexa) continue;  // hidden from Alexa: web page only
    state[i].alexaFan = new EspalexaDevice(fans[i].name, onAlexaFan, EspalexaDeviceType::dimmable, 0);
    espalexa.addDevice(state[i].alexaFan);
    if (fans[i].lightName.length()) {
      state[i].alexaLight = new EspalexaDevice(fans[i].lightName, onAlexaLight, EspalexaDeviceType::onoff, 0);
      espalexa.addDevice(state[i].alexaLight);
    }
    syncAlexa(i);
  }

  MDNS.begin("fanbridge");
  // web(): runs a handler under appLock with rq set to its request
  auto web = [](const char *path, auto method, void (*fn)()) {  // method: HTTP_GET/HTTP_POST, as server.on takes them
    server.on(path, method, [fn](AsyncWebServerRequest *r) {
      AppLock lock;
      logRequest(r);
      rq = r;
      fn();
      rq = nullptr;
    });
  };
  web("/", HTTP_GET, []() { sendStatic("text/html", WEB_MAIN, false); });
  web("/settings", HTTP_GET, []() { sendStatic("text/html", WEB_SETTINGS, false); });
  web("/style.css", HTTP_GET, []() { sendStatic("text/css", WEB_CSS, true); });
  web("/manifest.json", HTTP_GET, []() { sendStatic("application/manifest+json", WEB_MANIFEST, true); });
  web("/icon.svg", HTTP_GET, []() { sendStatic("image/svg+xml", WEB_ICON, true); });
  web("/icon-192.png", HTTP_GET, []() { sendPng(ICON_192, sizeof(ICON_192)); });
  web("/icon-512-maskable.png", HTTP_GET, []() { sendPng(ICON_MASK_512, sizeof(ICON_MASK_512)); });
  web("/apple-touch-icon.png", HTTP_GET, []() { sendPng(ICON_APPLE_180, sizeof(ICON_APPLE_180)); });
  web("/status", HTTP_GET, handleStatus);
  web("/config", HTTP_GET, handleConfig);
  web("/fanctl", HTTP_GET, handleFanCtl);
  web("/fan/add", HTTP_POST, handleFanAdd);
  web("/fan/save", HTTP_POST, handleFanSave);
  web("/fan/remove", HTTP_POST, handleFanRemove);
  web("/learn", HTTP_GET, handleLearn);
  web("/learnstatus", HTTP_GET, handleLearnStatus);
  web("/sniff", HTTP_GET, handleSniff);
  web("/forget", HTTP_POST, handleForget);
  web("/wifireset", HTTP_POST, handleWifiReset);
  server.onNotFound([](AsyncWebServerRequest *r) {
    AppLock lock;  // Alexa commands call onAlexaFan/onAlexaLight from here
    logRequest(r);
    if (!espalexa.handleAlexaApiCall(r)) r->send(404, "text/plain", "Not found");
  });
  espalexa.begin(&server);  // registers its pages, starts the server and the Hue discovery responder
}

void loop() {
  {
    AppLock lock;
    espalexa.loop();  // Hue discovery (UDP); the web server runs on its own task
    learnLoop();
    bleLoop();
    if (restartAt && (int32_t)(millis() - restartAt) >= 0) ESP.restart();
  }
  delay(1);
}
