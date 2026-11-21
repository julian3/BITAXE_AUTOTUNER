/*
 * BitAxe 601 Gamma Auto-Tuner + Dashboard for ESP32
 * -------------------------------------------------
 * Watches ASIC temperature on one or more BitAxe units (AxeOS firmware) and
 * automatically steps their {frequency, coreVoltage} up or down along a
 * predefined "ladder" to keep temperature within an adjustable band.
 * Temperature limits and the dashboard refresh rate are user-configurable
 * from the web UI and persisted in flash. Serves a web dashboard with live
 * stats + a per-device change log (persisted in flash).
 *
 * A hamburger menu (top-right) opens three additional editable panels, all
 * persisted to flash and applied live without a reboot:
 *   - Device list: edit name/IP/voltage-offset for all 16 device slots, plus
 *     an "Autoscan" button that non-blockingly scans the local /24 for
 *     unrecognized AxeOS units and lets you add or replace a slot with one.
 *   - Voltage ladder: edit the frequency/VCore steps used by the tuner.
 *   - NTP settings: edit the (up to 2) NTP server hostnames.
 * 
 * Automatically manages DST hour change for countries like Italy. Uses two
 * NTP servers to automatically update its internal date/time clock.
 *
 * Handles max 16 devices -this hard limit may be changed in the code.
 *
 * REQUIRED LIBRARIES (Arduino Library Manager):
 *   - ArduinoJson (v6.x, e.g. 6.21.x)  https://arduinojson.org
 *   (WiFi, HTTPClient, WebServer, Preferences, time.h are all part of the
 *    ESP32 Arduino core, nothing else to install)
 *
 * BOARD: any standard ESP32 (WROOM/WROVER/S3...) dev board.
 *
 * TESTED with BitAxe Gamma 601. Should work with any BitAxe having the same
 * API endpoints.
 *
 * Put this file inside a folder named "bitaxe_autotuner" (Arduino IDE
 * requires the sketch folder name to match the .ino filename).
 *
 * ----------------------------------------------------------------------
 * INTERNAL NOTES 
 *
 * TODO(s)
 *   Plan to add a small onboard status display - likely a 4-line I2C OLED
 *   based on the SSD1306 driver (0.96"/1.3" modules, 128x32 or 128x64,
 *   very common/cheap). Would show at-a-glance: temp, current ladder
 *   step, hashrate, and last action - without needing the web dashboard.
 *   Needs: SSD1306 driver library (e.g. Adafruit_SSD1306 + Adafruit_GFX,
 *   or U8g2), I2C wiring (SDA/SCL), and a small render function called
 *   periodically from loop().
 *
 * KNOWN BUGS
 *   The activity log shows old entries. It happens with desktop browsers
 *   expecially when tab is always left open
 * 
 * COPYRIGHT
 *   
 *     Author:         Julian E. Spina
 *     Social/contact: jes@acca3.it
 *     Version:        1.0.0 (17 Aug 2026)
 *     License:        MIT
 *
 * ----------------------------------------------------------------------
 */

#include <WiFi.h>
#include <IPAddress.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <esp_task_wdt.h>

// ============================================================================
//  USER CONFIGURATION
// ============================================================================

const char *WIFI_SSID     = "YOUR_WIFI_HERE";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD_HERE";

// ASIC temperature thresholds (°C) - these are just the DEFAULTS used the very
// first time the device boots. After that, the values are loaded from/saved
// to NVS (flash) and can be changed live from the dashboard's Settings panel.
float defLimitHigh = 64.5;  // above this -> decrease settings
float defLimitLow  = 62.5;  // below this -> increase settings
float minTemp       = 45.0; // below this -> pause increase/decrease entirely (also loaded from/saved to NVS)

// Hard safety ceiling for ALL user-editable temperature values (not
// itself user-editable). No one can set Decrease-above, Increase-below, or
// Minimum-temp above this, regardless of what's typed in the form.
const float MAXIMUM_TEMP = 70.0;

// Per-device voltage offset (mV), added to EVERY ladder step's coreVoltage
// before it's sent to that specific BitAxe. User-editable (-50..+50mV) from
// the Device List panel, default 0 for every slot, loaded from/saved to NVS
// per device (see DEVICE_CFG_NS). e.g. a ladder step of 1230mV with a +15
// offset for that device actually sends 1245mV to it. Unlike the old global
// setting, each device now carries its own offset independently.
const int VOLTAGE_OFFSET_MIN_MV = -50;
const int VOLTAGE_OFFSET_MAX_MV = 50;

// Hard safety ceiling on the COMBINATION of a ladder step's coreVoltage and
// a device's voltage offset above (not itself user-editable, not exposed in
// the UI). No ladder step's effective voltage (step.mv + dev.offsetMv) may
// ever exceed this for ANY device, checked whenever a device's offset or the
// ladder itself changes.
const int MAX_VCORE_MV = 1350;

// Highest raw coreVoltage across the whole ladder - recomputed via
// computeLadderMaxMv() at boot and any time the ladder is edited. Since an
// offset applies uniformly to every step of a given device, checking just
// this single worst-case step against MAX_VCORE_MV is equivalent to
// checking every step individually for that device.
int ladderMaxMv = 0;

// Allowed values for the dashboard's auto-refresh dropdown (seconds)
const int REFRESH_OPTIONS[] = {5, 10, 30, 60, 300};
const char *REFRESH_LABELS[] = {"5s", "10s", "30s", "60s", "5 min"};
const int REFRESH_OPTIONS_COUNT = 5;
int pageRefreshSec = 10;  // default; also loaded from/saved to NVS

// How often we poll each BitAxe and evaluate the temperature rules. This is
// the CONTROL LOOP cadence and is independent from the dashboard's page
// auto-refresh above - it's just set to match the fastest refresh option (5s)
// so a 5s dashboard refresh actually shows fresh data every time.
const unsigned long CHECK_INTERVAL_MS = 5000;  // 5s

// Minimum time that must pass after a change before we allow another change
// on the SAME device (lets the temperature react before we re-judge it).
// Deliberately kept independent from CHECK_INTERVAL_MS above - the loop now
// evaluates every 5s, but an actual change still can't happen more than
// once every 10s.
const unsigned long COOLDOWN_MS = 10000;  // 10s, per your testing

// If a settings PATCH to a device fails, it's otherwise completely silent
// on the dashboard (Serial-only) - a device that's stuck refusing every
// command looks identical to one that's just comfortably within its temp
// band, with no visible sign anything is wrong. First failure is always
// logged immediately; after that, repeated failures are throttled to at
// most one log entry per this interval so a persistently-failing device
// doesn't flood the 30-entry ring buffer and push out real history.
const unsigned long PATCH_FAIL_LOG_INTERVAL_MS = 5UL * 60UL * 1000UL;  // 5 min

// After ASIC temp recovers back ABOVE minTemp (having been below it), wait
// this long before resuming normal increase/decrease evaluation - gives the
// reading/system a moment to settle instead of acting on the very first
// borderline reading after recovery. User-editable (1s steps, 0-120s),
// default 60s. Also loaded from/saved to NVS.
int minTempSettleSec = 60;

// If true, the ESP32 will call POST /api/system/restart right after a
// successful settings PATCH. Left OFF by default: on this firmware, changing
// frequency/coreVoltage via PATCH is applied on the fly (matches your own
// experience using the AxeOS UI, where you never need to reboot after
// changing values). Flip to true if you ever find your firmware needs it.
const bool RESTART_AFTER_SETTINGS_CHANGE = false;

// Timezone for log timestamps (Europe/Rome, handles CET/CEST DST automatically)
// CET-1    : Central European Time, UTC+1 (sign is inverted in POSIX standard)
// CEST     : Central European Summer Time (DST), UTC+2
// M3.5.0   : DST starts on the last (5) Sunday (0) of March (M3) at 02:00 (default)
// M10.5.0/3: DST ends on the last (5) Sunday (0) of October (M10) at 03:00 (/3)
const char *TIMEZONE_TZ  = "CET-1CEST,M3.5.0,M10.5.0/3";

// NTP server list (up to 2 hosts), user-editable from the "NTP settings"
// menu panel, loaded from/saved to NVS. Handed to the ESP32's own SNTP
// stack via configTzTime(), which already fails over from server 1 to
// server 2 and cycles back to server 1 if neither answers - we just need to
// re-issue that call whenever the list is edited, so a reboot isn't needed.
// The second host may be left blank (single-server setup).
String ntpServer1 = "pool.ntp.org";
String ntpServer2 = "time.google.com";

#define MAX_DEVICES 16

// The operation log is now a SINGLE combined log shared across all devices
// (not one ring buffer per device), with the device name as its own column.
// Capacity raised from the old per-device 10 to 30 - see the comment above
// globalLogBuf below for why this is a memory WIN, not a cost, versus the
// old per-device design.
#define LOG_SIZE 30

// ---- The ladder --------------------------------------------------------
// Ordered from LOWEST to HIGHEST performance.
// index 0            = "the first value of the list" (safest/coolest)
// index LADDER_SIZE-1 = "the last value of the list"  (most aggressive)
// "Decrease settings" -> move to a LOWER index (toward the top of this list)
// "Increase settings" -> move to a HIGHER index (toward the bottom)
struct LadderStep {
  uint16_t freqMHz;
  uint16_t mv;  // core voltage in millivolts
};

// The ladder is now user-editable at runtime from the "Voltage ladder" menu
// panel, so it lives in a fixed-capacity mutable array instead of a const
// one. DEFAULT_LADDER below seeds it on first boot only; after that the
// live contents are loaded from/saved to NVS as JSON. LADDER_SIZE is no
// longer a compile-time constant - it tracks how many of the MAX_LADDER_SIZE
// slots in `ladder[]` are actually in use.
#define MAX_LADDER_SIZE 40

const LadderStep DEFAULT_LADDER[] = {
  {525, 1175}, {575, 1175}, {650, 1175}, {660, 1175},
  {690, 1205}, {700, 1205},
  {710, 1200}, {715, 1200}, {740, 1200},
  {750, 1210}, {760, 1210}, {770, 1210},
  {780, 1215},
  {790, 1225},
  {800, 1230}, {810, 1230},
  {820, 1235},
  {830, 1240}, {835, 1240},
  {840, 1245}, {850, 1245},
  {860, 1255},
  {870, 1265},
  {880, 1270}, {890, 1270},
  {900, 1280}, {905, 1280},
  {920, 1290}, {930, 1290},
  {940, 1295},
  {950, 1300},
  {960, 1305}
};
const int DEFAULT_LADDER_SIZE = sizeof(DEFAULT_LADDER) / sizeof(DEFAULT_LADDER[0]);

LadderStep ladder[MAX_LADDER_SIZE];
int LADDER_SIZE = 0;  // populated by loadLadder() in setup()

// ---- Fleet configuration ------------------------------------------------
// Up to MAX_DEVICES BitAxes. Set enabled=true and fill name/ip for real
// units. Disabled slots are ignored everywhere (not polled, not shown).
struct DeviceConfig {
  bool enabled;
  const char *name;
  const char *ip;
};

DeviceConfig DEVICE_CONFIG[MAX_DEVICES] = {
  { true,  "BitAxe Gamma 1", "192.168.1.192" },
  { false, "BitAxe 2", "" },
  { false, "BitAxe 3", "" },
  { false, "BitAxe 4", "" },
  { false, "BitAxe 5", "" },
  { false, "BitAxe 6", "" },
  { false, "BitAxe 7", "" },
  { false, "BitAxe 8", "" },
  { false, "BitAxe 9", "" },
  { false, "BitAxe 10", "" },
  { false, "BitAxe 11", "" },
  { false, "BitAxe 12", "" },
  { false, "BitAxe 13", "" },
  { false, "BitAxe 14", "" },
  { false, "BitAxe 15", "" },
  { false, "BitAxe 16", "" },
};

// ============================================================================
//  RUNTIME STATE
// ============================================================================

struct LogEntry {
  char deviceName[24]; // snapshot of the device's name at the time of the event
  char timestamp[20];  // "2026-07-30 14:32:05" or "uptime 00:04:12" if no NTP yet
  char action[16];      // "INCREASE" / "DECREASE" / "OFFSET" / "IP CHANGED" / "NAME CHANGED"
  uint16_t fromFreq, toFreq;
  uint16_t fromMv, toMv;
  float tempAtChange;
};

struct BitaxeInfo {
  String hostname       = "";
  float hashRateGHs      = 0;
  float temp             = 0;
  float vrTemp           = 0;
  float power            = 0;
  float inputVoltageV    = 0;
  int   frequency        = 0;
  int   coreVoltageMv    = 0;   // kept as raw integer mV (exact, no float rounding)
  unsigned long uptimeSeconds   = 0;
  unsigned long sharesAccepted  = 0;
  unsigned long sharesRejected  = 0;
  float errorPercentage  = 0;   // reported directly by the firmware
  double bestDiff        = 0;   // raw difficulty numbers, formatted for display
  double bestSessionDiff = 0;
};

struct BitaxeDevice {
  bool     enabled = false;
  String   name;
  String   ip;
  int      currentIndex   = -1;  // -1 = not yet synced to the ladder
  unsigned long lastChangeMillis = 0;
  bool     infoValid = false;
  BitaxeInfo info;

  // This device's own voltage offset (mV), edited from the Device List
  // panel. -50..+50mV, default 0. Added to every ladder step's coreVoltage
  // before it's sent to THIS device only (see VOLTAGE_OFFSET_MIN_MV/MAX_MV).
  int offsetMv = 0;

  // MINIMUM_TEMP recovery settle window tracking
  bool wasBelowMinTemp        = false;  // state as of the previous cycle
  bool settlingAfterMinTemp   = false;  // true while in the post-recovery settle window
  unsigned long minTempRecoveredAtMillis = 0;

  // The voltage offset (mV) baked into whatever coreVoltage we last actually
  // sent this device (ladder-step change OR a standalone offset apply).
  // Used to compute the "from" side of an OFFSET log entry and to detect
  // whether an edit to offsetMv actually changes this device's effective
  // voltage.
  int appliedOffsetMv = 0;

  // Set when this device's offsetMv changed (via the Device List panel)
  // while it was still inside its post-change cooldown window.
  // processDevice() finishes the job (resends the current step's voltage
  // with the new offset) as soon as the cooldown clears.
  bool offsetApplyPending = false;

  // How many settings PATCHes to this device have failed in a row, and when
  // we last wrote one of those failures to the visible log (see
  // PATCH_FAIL_LOG_INTERVAL_MS above). Reset to 0 on the next success.
  int consecutivePatchFails = 0;
  unsigned long lastPatchFailLogMillis = 0;
};

BitaxeDevice devices[MAX_DEVICES];
WebServer server(80);
unsigned long lastCheckMillis = 0;
bool timeSynced = false;

Preferences settingsPrefs;
const char *SETTINGS_NS = "bxsettings";

// ============================================================================
//  FORWARD DECLARATIONS
// ============================================================================
// Arduino's IDE auto-generates prototypes for every function in the sketch
// and inserts them as a block near the top of the file - but it computes
// that insertion point BEFORE parsing far enough to know custom structs like
// BitaxeDevice/LadderStep exist, which breaks the auto-generated prototypes
// for any function that takes one as a parameter. Explicitly declaring these
// functions here (after the structs, before any of them are used) makes
// Arduino skip auto-generating a prototype for them entirely, since one
// already exists at this point in the file.
void addLogEntry(BitaxeDevice &dev, const char *action, LadderStep from, LadderStep to, float tempAtChange);
bool fetchInfo(BitaxeDevice &dev);
bool sendSettings(BitaxeDevice &dev, LadderStep step);
bool restartDevice(BitaxeDevice &dev);
void applyChange(BitaxeDevice &dev, int newIndex, const char *actionLabel);
void applyPendingOffset(BitaxeDevice &dev);
void applyOffsetToDevice(BitaxeDevice &dev);
void resetDeviceRuntimeState(BitaxeDevice &dev);
void processDevice(BitaxeDevice &dev);
int settlingRemainingSeconds(BitaxeDevice &dev);
String buildDeviceCard(BitaxeDevice &dev, int idx);

void computeLadderMaxMv() {
  ladderMaxMv = 0;
  for (int i = 0; i < LADDER_SIZE; i++) {
    if (ladder[i].mv > ladderMaxMv) ladderMaxMv = ladder[i].mv;
  }
}

// True if applying offsetMv to every ladder step would keep all of them at
// or under MAX_VCORE_MV. Since the offset is uniform across the ladder,
// checking only the single highest raw step (ladderMaxMv) is equivalent to
// checking every step individually.
bool offsetWithinVcoreCeiling(int offsetMv) {
  return (ladderMaxMv + offsetMv) <= MAX_VCORE_MV;
}

void loadSettings() {
  settingsPrefs.begin(SETTINGS_NS, true);
  defLimitHigh   = settingsPrefs.getFloat("limHigh", defLimitHigh);
  defLimitLow    = settingsPrefs.getFloat("limLow", defLimitLow);
  minTemp        = settingsPrefs.getFloat("minTemp", minTemp);
  minTempSettleSec = settingsPrefs.getInt("settleSec", minTempSettleSec);
  pageRefreshSec = settingsPrefs.getInt("refresh", pageRefreshSec);
  ntpServer1     = settingsPrefs.getString("ntp1", ntpServer1);
  ntpServer2     = settingsPrefs.getString("ntp2", ntpServer2);
  settingsPrefs.end();
}

void saveSettings() {
  settingsPrefs.begin(SETTINGS_NS, false);
  settingsPrefs.putFloat("limHigh", defLimitHigh);
  settingsPrefs.putFloat("limLow", defLimitLow);
  settingsPrefs.putFloat("minTemp", minTemp);
  settingsPrefs.putInt("settleSec", minTempSettleSec);
  settingsPrefs.putInt("refresh", pageRefreshSec);
  settingsPrefs.end();
}

void saveNtpSettings() {
  settingsPrefs.begin(SETTINGS_NS, false);
  settingsPrefs.putString("ntp1", ntpServer1);
  settingsPrefs.putString("ntp2", ntpServer2);
  settingsPrefs.end();
}

// ---- Per-device name/IP/offset/enabled overrides (NVS) -------------------
// The DEVICE_CONFIG array only seeds initial values on first boot. Once a
// device's name, IP, offset, or enabled state is edited from the dashboard
// or the Device List panel, the override lives here from then on and takes
// priority over DEVICE_CONFIG.
Preferences deviceCfgPrefs;
const char *DEVICE_CFG_NS = "bxdevcfg";

void loadDeviceOverrides() {
  deviceCfgPrefs.begin(DEVICE_CFG_NS, true);
  for (int i = 0; i < MAX_DEVICES; i++) {
    String nameKey = "name" + String(i);
    String ipKey   = "ip" + String(i);
    String offKey  = "off" + String(i);
    String enKey   = "en" + String(i);
    if (deviceCfgPrefs.isKey(nameKey.c_str())) {
      devices[i].name = deviceCfgPrefs.getString(nameKey.c_str(), devices[i].name);
    }
    if (deviceCfgPrefs.isKey(ipKey.c_str())) {
      devices[i].ip = deviceCfgPrefs.getString(ipKey.c_str(), devices[i].ip);
    }
    if (deviceCfgPrefs.isKey(offKey.c_str())) {
      devices[i].offsetMv = deviceCfgPrefs.getInt(offKey.c_str(), 0);
    }
    if (deviceCfgPrefs.isKey(enKey.c_str())) {
      devices[i].enabled = deviceCfgPrefs.getBool(enKey.c_str(), devices[i].enabled);
    }
  }
  deviceCfgPrefs.end();
}

// Persists everything the Device List panel can edit for one slot in a
// single NVS transaction (name, IP, offset, enabled).
void saveDeviceOverride(int idx) {
  deviceCfgPrefs.begin(DEVICE_CFG_NS, false);
  deviceCfgPrefs.putString(("name" + String(idx)).c_str(), devices[idx].name);
  deviceCfgPrefs.putString(("ip" + String(idx)).c_str(), devices[idx].ip);
  deviceCfgPrefs.putInt(("off" + String(idx)).c_str(), devices[idx].offsetMv);
  deviceCfgPrefs.putBool(("en" + String(idx)).c_str(), devices[idx].enabled);
  deviceCfgPrefs.end();
}

bool isValidIPv4(const String &ip) {
  IPAddress test;
  return test.fromString(ip);
}

// Checked against ALL other device slots (enabled or not), per spec - hard
// blocks a save rather than just warning.
bool isDuplicateIP(const String &ip, int excludeIdx) {
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (i == excludeIdx) continue;
    if (devices[i].ip == ip) return true;
  }
  return false;
}

// ---- Voltage ladder persistence (NVS) ------------------------------------
// The ladder is user-editable from the "Voltage ladder" menu panel. Stored
// as a compact JSON array [[freq,mv],...], sorted ascending by frequency
// (auto-sorted on save - see handleSaveLadder()) since index order is what
// the increase/decrease logic relies on.
Preferences ladderPrefs;
const char *LADDER_NS = "bxladder";

void loadLadder() {
  ladderPrefs.begin(LADDER_NS, true);
  String raw = ladderPrefs.getString("ladder", "");
  ladderPrefs.end();

  if (raw.length() > 0) {
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, raw) == DeserializationError::Ok) {
      JsonArray arr = doc.as<JsonArray>();
      int i = 0;
      for (JsonArray step : arr) {
        if (i >= MAX_LADDER_SIZE) break;
        ladder[i].freqMHz = step[0] | 0;
        ladder[i].mv      = step[1] | 0;
        i++;
      }
      if (i > 0) {
        LADDER_SIZE = i;
        return;
      }
    }
  }

  // First boot, or NVS empty/corrupt: seed from the built-in defaults.
  LADDER_SIZE = min(DEFAULT_LADDER_SIZE, MAX_LADDER_SIZE);
  for (int i = 0; i < LADDER_SIZE; i++) ladder[i] = DEFAULT_LADDER[i];
}

void saveLadder() {
  ladderPrefs.begin(LADDER_NS, false);
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < LADDER_SIZE; i++) {
    JsonArray step = arr.createNestedArray();
    step.add(ladder[i].freqMHz);
    step.add(ladder[i].mv);
  }
  String out;
  serializeJson(doc, out);
  ladderPrefs.putString("ladder", out);
  ladderPrefs.end();
}

// ============================================================================
//  TIME / LOGGING HELPERS
// ============================================================================

void getTimestampString(char *out, size_t outLen, unsigned long deviceUptimeFallback) {
  struct tm timeinfo;
  if (timeSynced && getLocalTime(&timeinfo, 200)) {
    strftime(out, outLen, "%Y-%m-%d %H:%M:%S", &timeinfo);
  } else {
    unsigned long s = millis() / 1000;
    snprintf(out, outLen, "up+%02lu:%02lu:%02lu", s / 3600, (s % 3600) / 60, s % 60);
  }
}

String formatUptime(unsigned long totalSeconds) {
  unsigned long d = totalSeconds / 86400;
  unsigned long h = (totalSeconds % 86400) / 3600;
  unsigned long m = (totalSeconds % 3600) / 60;
  unsigned long s = totalSeconds % 60;
  char buf[32];
  if (d > 0) snprintf(buf, sizeof(buf), "%lud %02luh %02lum %02lus", d, h, m, s);
  else       snprintf(buf, sizeof(buf), "%02luh %02lum %02lus", h, m, s);
  return String(buf);
}

// ---- Combined operation log (shared across ALL devices) -----------------
// A single ring buffer instead of one per device - each entry carries the
// device name as its own column. Persisted as one JSON blob in its own NVS
// namespace, capacity LOG_SIZE (30) shared across the whole fleet rather
// than 30 per device.
LogEntry globalLogBuf[LOG_SIZE];
int globalLogCount = 0;  // how many valid entries
int globalLogHead  = 0;  // index of the OLDEST entry in the ring buffer

Preferences logPrefs;
const char *LOG_NS = "bxlog";

void saveGlobalLogToNVS() {
  // Serialize the ring buffer (oldest -> newest) into a compact JSON array
  DynamicJsonDocument doc(6144);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < globalLogCount; i++) {
    int idx = (globalLogHead + i) % LOG_SIZE;
    JsonObject o = arr.createNestedObject();
    o["dn"]  = globalLogBuf[idx].deviceName;
    o["ts"]  = globalLogBuf[idx].timestamp;
    o["act"] = globalLogBuf[idx].action;
    o["ff"]  = globalLogBuf[idx].fromFreq;
    o["tf"]  = globalLogBuf[idx].toFreq;
    o["fm"]  = globalLogBuf[idx].fromMv;
    o["tm"]  = globalLogBuf[idx].toMv;
    o["t"]   = globalLogBuf[idx].tempAtChange;
  }
  String out;
  serializeJson(doc, out);

  logPrefs.begin(LOG_NS, false);
  logPrefs.putString("log", out);
  logPrefs.end();
}

void loadGlobalLogFromNVS() {
  logPrefs.begin(LOG_NS, true);
  String raw = logPrefs.getString("log", "");
  logPrefs.end();
  if (raw.length() == 0) return;

  DynamicJsonDocument doc(6144);
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return;

  JsonArray arr = doc.as<JsonArray>();
  globalLogCount = 0;
  globalLogHead = 0;
  int i = 0;
  for (JsonObject o : arr) {
    if (i >= LOG_SIZE) break;
    LogEntry &e = globalLogBuf[i];
    strlcpy(e.deviceName, o["dn"] | "", sizeof(e.deviceName));
    strlcpy(e.timestamp, o["ts"] | "", sizeof(e.timestamp));
    strlcpy(e.action, o["act"] | "", sizeof(e.action));
    e.fromFreq = o["ff"] | 0;
    e.toFreq   = o["tf"] | 0;
    e.fromMv   = o["fm"] | 0;
    e.toMv     = o["tm"] | 0;
    e.tempAtChange = o["t"] | 0.0;
    i++;
  }
  globalLogCount = i;
}

void clearGlobalLog() {
  globalLogCount = 0;
  globalLogHead = 0;
  saveGlobalLogToNVS();
}

// Appends one entry to the shared log, tagged with dev's CURRENT name (a
// snapshot - a later rename doesn't rewrite history, matching how a
// timestamp is also a snapshot). Called from many device-scoped call sites
// (applyChange, applyPendingOffset, device-list/autoscan edits, etc).
void addLogEntry(BitaxeDevice &dev, const char *action, LadderStep from, LadderStep to, float tempAtChange) {
  int writeIdx;
  if (globalLogCount < LOG_SIZE) {
    writeIdx = (globalLogHead + globalLogCount) % LOG_SIZE;
    globalLogCount++;
  } else {
    writeIdx = globalLogHead;
    globalLogHead = (globalLogHead + 1) % LOG_SIZE;
  }

  LogEntry &e = globalLogBuf[writeIdx];
  strlcpy(e.deviceName, dev.name.c_str(), sizeof(e.deviceName));
  getTimestampString(e.timestamp, sizeof(e.timestamp), 0);
  strlcpy(e.action, action, sizeof(e.action));
  e.fromFreq = from.freqMHz;
  e.toFreq   = to.freqMHz;
  e.fromMv   = from.mv;
  e.toMv     = to.mv;
  e.tempAtChange = tempAtChange;

  saveGlobalLogToNVS();
}

// ============================================================================
//  BITAXE API
// ============================================================================

bool fetchInfo(BitaxeDevice &dev) {
  HTTPClient http;
  String url = "http://" + dev.ip + "/api/system/info";
  http.begin(url);
  http.setConnectTimeout(4000);  // TCP connect phase - can hang longer than setTimeout() alone would suggest
  http.setTimeout(4000);         // response read phase
  int code = http.GET();

  if (code != 200) {
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) return false;

  BitaxeInfo &i = dev.info;
  i.hostname   = String((const char*)(doc["hostname"] | ""));
  i.hashRateGHs = doc["hashRate"] | 0.0;                 // GH/s per AxeOS API
  i.temp        = doc["temp"] | 0.0;
  i.vrTemp      = doc["vrTemp"] | 0.0;
  i.power       = doc["power"] | 0.0;
  i.inputVoltageV = (doc["voltage"] | 0.0) / 1000.0;      // input voltage is reported in mV
  i.frequency   = doc["frequency"] | 0;
  // Prefer the measured/actual core voltage if the firmware exposes it,
  // otherwise fall back to the configured set-point.
  if (doc.containsKey("coreVoltageActual"))
    i.coreVoltageMv = doc["coreVoltageActual"] | 0;
  else
    i.coreVoltageMv = doc["coreVoltage"] | 0;
  i.uptimeSeconds  = doc["uptimeSeconds"] | 0;
  i.sharesAccepted = doc["sharesAccepted"] | 0;
  i.sharesRejected = doc["sharesRejected"] | 0;
  i.errorPercentage = doc["errorPercentage"] | 0.0;
  i.bestDiff        = doc["bestDiff"] | 0.0;
  i.bestSessionDiff = doc["bestSessionDiff"] | 0.0;

  return true;
}

bool sendSettings(BitaxeDevice &dev, LadderStep step) {
  HTTPClient http;
  String url = "http://" + dev.ip + "/api/system";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setConnectTimeout(4000);
  http.setTimeout(4000);

  String body = "{\"frequency\":" + String(step.freqMHz) +
                ",\"coreVoltage\":" + String(step.mv) + "}";

  int code = http.PATCH(body);
  http.end();

  return (code == 200 || code == 201 || code == 204);
}

// Not used by default (RESTART_AFTER_SETTINGS_CHANGE is false). Kept here,
// ready to call, in case a future firmware version needs a reboot for
// frequency/voltage changes to take effect.
bool restartDevice(BitaxeDevice &dev) {
  HTTPClient http;
  String url = "http://" + dev.ip + "/api/system/restart";
  http.begin(url);
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  int code = http.POST("");
  http.end();
  return (code == 200 || code == 204);
}

// ============================================================================
//  LADDER LOGIC
// ============================================================================

// offsetMv should be the CURRENT global voltage offset - the live voltage
// read back from the BitAxe is matched against ladder[i].mv + offsetMv (the
// effective voltage), not the raw ladder value, so the closest-match logic
// stays correct regardless of the currently configured offset.
int findClosestLadderIndex(int freqMHz, int coreVoltageMv, int offsetMv) {
  // First pass: exact frequency match
  for (int i = 0; i < LADDER_SIZE; i++) {
    if (ladder[i].freqMHz == freqMHz) return i;
  }
  // Fallback: closest frequency (then closest EFFECTIVE voltage as tiebreaker)
  int best = 0;
  int bestDiff = abs(ladder[0].freqMHz - freqMHz);
  for (int i = 1; i < LADDER_SIZE; i++) {
    int diff = abs(ladder[i].freqMHz - freqMHz);
    int bestEffMv = ladder[best].mv + offsetMv;
    int iEffMv    = ladder[i].mv + offsetMv;
    if (diff < bestDiff ||
        (diff == bestDiff && abs(iEffMv - coreVoltageMv) < abs(bestEffMv - coreVoltageMv))) {
      best = i;
      bestDiff = diff;
    }
  }
  return best;
}

void applyChange(BitaxeDevice &dev, int newIndex, const char *actionLabel) {
  // "from"/"to" carry the EFFECTIVE voltage (ladder value + offset) actually
  // sent/logged, not the raw ladder value - matches what the dashboard and
  // log display. "from" uses whatever offset was baked into the device's
  // last actual send; "to" uses this device's current offset.
  LadderStep from = ladder[dev.currentIndex];
  from.mv = (uint16_t)(from.mv + dev.appliedOffsetMv);
  LadderStep to = ladder[newIndex];
  to.mv = (uint16_t)(to.mv + dev.offsetMv);

  bool ok = sendSettings(dev, to);
  if (!ok) {
    dev.consecutivePatchFails++;
    Serial.printf("[%s] FAILED to send new settings (%d MHz / %d mV) - %d consecutive failure(s)\n",
                  dev.name.c_str(), to.freqMHz, to.mv, dev.consecutivePatchFails);
    // First failure logs immediately; after that, throttled - see
    // PATCH_FAIL_LOG_INTERVAL_MS for why.
    if (dev.consecutivePatchFails == 1 ||
        millis() - dev.lastPatchFailLogMillis >= PATCH_FAIL_LOG_INTERVAL_MS) {
      addLogEntry(dev, "PATCH FAIL", from, to, dev.info.temp);
      dev.lastPatchFailLogMillis = millis();
    }
    return;
  }
  dev.consecutivePatchFails = 0;

  addLogEntry(dev, actionLabel, from, to, dev.info.temp);
  dev.currentIndex = newIndex;
  dev.lastChangeMillis = millis();
  dev.appliedOffsetMv = dev.offsetMv;
  dev.offsetApplyPending = false;  // this send already carries the latest offset

  Serial.printf("[%s] %s: %d MHz/%dmV -> %d MHz/%dmV (temp was %.1fC)\n",
                dev.name.c_str(), actionLabel, from.freqMHz, from.mv,
                to.freqMHz, to.mv, dev.info.temp);

  if (RESTART_AFTER_SETTINGS_CHANGE) {
    restartDevice(dev);
  }
}

// Resends the device's CURRENT ladder step, but with its (already-updated)
// offsetMv baked in - used when this device's offset itself changes but no
// ladder-step change is otherwise due. Logged as its own "OFFSET" action.
void applyPendingOffset(BitaxeDevice &dev) {
  LadderStep base = ladder[dev.currentIndex];
  LadderStep from = base; from.mv = (uint16_t)(base.mv + dev.appliedOffsetMv);
  LadderStep to   = base; to.mv   = (uint16_t)(base.mv + dev.offsetMv);

  if (from.mv == to.mv) {
    dev.offsetApplyPending = false;
    return;
  }

  bool ok = sendSettings(dev, to);
  if (!ok) {
    dev.consecutivePatchFails++;
    Serial.printf("[%s] FAILED to apply new voltage offset (target %d mV) - %d consecutive failure(s)\n",
                  dev.name.c_str(), to.mv, dev.consecutivePatchFails);
    // IMPORTANT: offsetApplyPending stays true here, and processDevice()
    // checks it BEFORE ever evaluating increase/decrease - so a PATCH that
    // keeps failing silently freezes this device's auto-tuning entirely,
    // forever, with no visible sign why. Surface it in the log so that
    // can't happen unnoticed again.
    if (dev.consecutivePatchFails == 1 ||
        millis() - dev.lastPatchFailLogMillis >= PATCH_FAIL_LOG_INTERVAL_MS) {
      addLogEntry(dev, "OFFSET FAIL", from, to, dev.info.temp);
      dev.lastPatchFailLogMillis = millis();
    }
    return;  // leave offsetApplyPending set; retried on a later eligible cycle
  }
  dev.consecutivePatchFails = 0;

  addLogEntry(dev, "OFFSET", from, to, dev.info.temp);
  dev.lastChangeMillis = millis();
  dev.appliedOffsetMv = dev.offsetMv;
  dev.offsetApplyPending = false;

  Serial.printf("[%s] OFFSET applied: %dmV -> %dmV (voltage offset now %+dmV)\n",
                dev.name.c_str(), from.mv, to.mv, dev.offsetMv);
}

// Called right after a validated new offsetMv has been saved for ONE
// device (from the Device List panel). Applies it immediately if the
// device isn't in cooldown, or marks it pending (finished automatically by
// processDevice() once cooldown clears) if it is - so an offset edit always
// respects the same 10s post-change cooldown as any other voltage change.
void applyOffsetToDevice(BitaxeDevice &dev) {
  if (!dev.enabled || dev.currentIndex == -1) return;
  if (dev.appliedOffsetMv == dev.offsetMv) return;  // no change for this device

  if (millis() - dev.lastChangeMillis < COOLDOWN_MS) {
    dev.offsetApplyPending = true;
  } else {
    applyPendingOffset(dev);
  }
}

void processDevice(BitaxeDevice &dev) {
  bool wasValid = dev.infoValid;
  bool ok = fetchInfo(dev);
  dev.infoValid = ok;

  if (!ok) {
    Serial.printf("[%s] unreachable at %s\n", dev.name.c_str(), dev.ip.c_str());
    return;
  }

  // Reconnect detection: if this device was unreachable and just came back
  // (WiFi hiccup, BitAxe reboot, brief network drop...), don't trust our old
  // ladder pointer - force a fresh resync below, exactly like a first boot.
  // Without this, a reconnect leaves currentIndex stale, so the very next
  // increase/decrease steps from a rung that no longer matches what the
  // device is actually running (e.g. after a manual change made directly in
  // the AxeOS UI while the ESP32 lost track of it) - see FEATURES.txt
  // section 3, "again any time a device reconnects".
  if (!wasValid) {
    dev.currentIndex = -1;
  }

  // First successful read after boot (or reconnect, see above): sync our
  // ladder index to whatever the device is actually running right now
  // (don't count this as a change).
  if (dev.currentIndex == -1) {
    dev.currentIndex = findClosestLadderIndex(dev.info.frequency, dev.info.coreVoltageMv, dev.offsetMv);
    dev.appliedOffsetMv = dev.offsetMv;  // baseline: track future offset changes from here
    dev.offsetApplyPending = false;
    Serial.printf("[%s] synced to ladder index %d (%d MHz / %d mV)\n",
                  dev.name.c_str(), dev.currentIndex,
                  ladder[dev.currentIndex].freqMHz, dev.info.coreVoltageMv);
    return;
  }

  // If the ASIC is still cold (e.g. right after the BitAxe rebooted or
  // reconnected), skip increase/decrease entirely until it's back above
  // minTemp. Deliberately checked BEFORE the cooldown gate and doesn't touch
  // lastChangeMillis, so it never blocks a legitimate change once temp
  // recovers.
  if (dev.info.temp < minTemp) {
    dev.wasBelowMinTemp = true;
    return;
  }

  // Just recovered from being below minTemp: start a one-time 60s settle
  // window before resuming normal increase/decrease evaluation.
  if (dev.wasBelowMinTemp) {
    dev.wasBelowMinTemp = false;
    dev.settlingAfterMinTemp = true;
    dev.minTempRecoveredAtMillis = millis();
  }

  if (dev.settlingAfterMinTemp) {
    if (millis() - dev.minTempRecoveredAtMillis < (unsigned long)minTempSettleSec * 1000UL) {
      return;  // still settling, don't evaluate increase/decrease yet
    }
    dev.settlingAfterMinTemp = false;  // settle window over, resume normal operation
  }

  // A voltage-offset change arrived while this device was still in cooldown
  // from a previous change - finish applying it now if the wait is over.
  if (dev.offsetApplyPending) {
    if (millis() - dev.lastChangeMillis < COOLDOWN_MS) return;
    applyPendingOffset(dev);
    return;  // don't also evaluate increase/decrease in the same cycle
  }

  // Respect the cooldown before evaluating another change
  if (millis() - dev.lastChangeMillis < COOLDOWN_MS) return;

  if (dev.info.temp > defLimitHigh) {
    if (dev.currentIndex > 0) {
      applyChange(dev, dev.currentIndex - 1, "DECREASE");
    }
    // else: already at the first (safest) value of the list -> do nothing
  } else if (dev.info.temp < defLimitLow) {
    if (dev.currentIndex < LADDER_SIZE - 1) {
      applyChange(dev, dev.currentIndex + 1, "INCREASE");
    }
    // else: already at the last (most aggressive) value of the list -> do nothing
  }
}

// ============================================================================
//  WEB DASHBOARD
// ============================================================================

String formatMv(int mv) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d.%03d", mv / 1000, mv % 1000);
  return String(buf);
}

String formatDifficulty(double v) {
  char buf[24];
  if (v >= 1e12)      snprintf(buf, sizeof(buf), "%.2fT", v / 1e12);
  else if (v >= 1e9)  snprintf(buf, sizeof(buf), "%.2fG", v / 1e9);
  else if (v >= 1e6)  snprintf(buf, sizeof(buf), "%.2fM", v / 1e6);
  else if (v >= 1e3)  snprintf(buf, sizeof(buf), "%.2fK", v / 1e3);
  else                snprintf(buf, sizeof(buf), "%.0f", v);
  return String(buf);
}

String htmlEscape(const String &in) {
  String out = in;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  return out;
}

// Seconds remaining in the post-min-temp-recovery settle window, or 0 if not
// currently settling.
int settlingRemainingSeconds(BitaxeDevice &dev) {
  if (!dev.settlingAfterMinTemp) return 0;
  unsigned long settleMs = (unsigned long)minTempSettleSec * 1000UL;
  unsigned long elapsed = millis() - dev.minTempRecoveredAtMillis;
  if (elapsed >= settleMs) return 0;
  unsigned long remainingMs = settleMs - elapsed;
  return (int)((remainingMs + 999) / 1000);  // round up to whole seconds
}

String buildDeviceCard(BitaxeDevice &dev, int idx) {
  String h;
  String p = String(idx);  // id suffix
  h += "<div class='card'>";
  h += "<h2><span id='name-" + p + "'>" + htmlEscape(dev.name) + "</span> "
       "<span class='ip' id='ip-" + p + "'>(" + dev.ip + ")</span> "
       "<button type='button' class='edit-btn' onclick='openEditDevice(" + p +
       ")' aria-label='Edit device name and IP'>&#9998;</button></h2>";
  h += "<div class='edit-form' id='editform-" + p + "' style='display:none'>";
  h += "<input type='text' id='editname-" + p + "' value='" + htmlEscape(dev.name) +
       "' placeholder='Device name' maxlength='40' autocomplete='off'>";
  h += "<input type='text' id='editip-" + p + "' value='" + dev.ip +
       "' placeholder='192.168.1.100' inputmode='decimal' lang='en' autocomplete='off'>";
  h += "<button type='button' onclick='saveEditDevice(" + p + ")'>Save</button>";
  h += "<button type='button' class='cancel-btn' onclick='closeEditDevice(" + p + ")'>Cancel</button>";
  h += "<span id='editerr-" + p + "' class='save-status err'></span>";
  h += "</div>";

  h += "<p class='offline' id='offline-" + p + "' style='display:" + (dev.infoValid ? "none" : "block") + "'>"
       "&#9888; Device unreachable - showing last known data (if any).</p>";

  BitaxeInfo &i = dev.info;
  bool belowMinTemp = dev.infoValid && (i.temp < minTemp);
  h += "<p class='lowtemp' id='lowtemp-" + p + "' style='display:" + (belowMinTemp ? "block" : "none") + "'>"
       "&#9888; ASIC temp below minimum (" + String(i.temp, 1) + "&deg;C &lt; " + String(minTemp, 1) +
       "&deg;C) - auto-tune paused</p>";

  int settleSec = settlingRemainingSeconds(dev);
  h += "<p class='settling' id='settling-" + p + "' style='display:" + (settleSec > 0 ? "block" : "none") + "'>"
       "&#9201; Stabilizing after minimum-temp recovery - resuming auto-tune in <span id='settlesec-" + p + "'>" +
       String(settleSec) + "</span>s</p>";

  // Visible as soon as a settings PATCH fails, rather than waiting on the
  // throttled log entry - see PATCH_FAIL_LOG_INTERVAL_MS. A device stuck
  // here never gets increase/decrease evaluated again until PATCHes start
  // succeeding.
  h += "<p class='offline' id='patchfail-" + p + "' style='display:" +
       (dev.consecutivePatchFails > 0 ? "block" : "none") + "'>"
       "&#9888; Settings changes are failing (<span id='patchfailcount-" + p + "'>" +
       String(dev.consecutivePatchFails) + "</span> in a row) - auto-tune is stuck until this recovers</p>";

  float thps = i.hashRateGHs / 1000.0;
  float efficiency = thps > 0 ? (i.power / thps) : 0.0;

  h += "<div class='grid'>";

  h += "<div class='box'><h3>Mining</h3>";
  h += "<p>Hashrate: <b id='hashrate-" + p + "'>" + String(thps, 2) + " TH/s</b></p>";
  h += "<p>Error rate: <b id='error-" + p + "'>" + String(i.errorPercentage, 2) + " %</b></p>";
  h += "<p>Efficiency: <b id='efficiency-" + p + "'>" + (thps > 0 ? String(efficiency, 2) + " J/TH" : String("N/A")) + "</b></p>";
  h += "</div>";

  h += "<div class='box'><h3>Difficulty / Uptime</h3>";
  h += "<p>Best difficulty: <b id='bestdiff-" + p + "'>" + formatDifficulty(i.bestDiff) + "</b></p>";
  h += "<p>Best (session): <b id='bestsession-" + p + "'>" + formatDifficulty(i.bestSessionDiff) + "</b></p>";
  h += "<p>Uptime: <b id='uptime-" + p + "'>" + formatUptime(i.uptimeSeconds) + "</b></p>";
  h += "</div>";

  h += "<div class='box'><h3>Power</h3>";
  h += "<p>Power: <b id='power-" + p + "'>" + String(i.power, 2) + " W</b></p>";
  h += "<p>Input V: <b id='inputv-" + p + "'>" + String(i.inputVoltageV, 2) + " V</b></p>";
  h += "<p>ASIC frequency: <b id='freq-" + p + "'>" + String(i.frequency) + " MHz</b></p>";
  h += "<p>ASIC V (measured): <b id='corev-" + p + "'>" + formatMv(i.coreVoltageMv) + " V</b></p>";
  h += "</div>";

  h += "<div class='box'><h3>Heat</h3>";
  String tempClass = i.temp > defLimitHigh ? "hot" : (i.temp < defLimitLow ? "cool" : "");
  h += "<p>ASIC temp: <b id='temp-" + p + "' class='" + tempClass + "'>" + String(i.temp, 1) + " &deg;C</b></p>";
  h += "<p>VR temp: <b id='vrtemp-" + p + "'>" + String(i.vrTemp, 1) + " &deg;C</b></p>";
  h += "</div>";

  h += "</div>"; // grid

  h += "</div>"; // card
  return h;
}

// Builds the SINGLE combined operation log shared across all devices,
// positioned below the device cards. Device name is the 1st column. The
// table body scrolls (see .log-scroll CSS) so the box stays about the same
// visual height it always had (~10 rows) while holding up to LOG_SIZE (30)
// entries total.
String buildGlobalLogSection() {
  String h;
  h += "<div class='card'>";
  h += "<div class='log-header'><h3>Recent auto-tune operations (all devices)</h3>"
       "<button type='button' class='clearlog-btn' onclick='clearLog()'>&#128465; Clear log</button></div>";
  h += "<div class='log log-scroll'>";
  h += "<table><thead><tr><th>Device</th><th>Time</th><th>Action</th><th>From</th><th>To</th><th>Temp</th></tr></thead>";
  h += "<tbody id='logbody-global'>";
  if (globalLogCount == 0) {
    h += "<tr><td colspan='6' class='logempty'>No changes yet.</td></tr>";
  } else {
    for (int n = globalLogCount - 1; n >= 0; n--) {
      int idx2 = (globalLogHead + n) % LOG_SIZE;
      LogEntry &e = globalLogBuf[idx2];
      bool isReassign = (strcmp(e.action, "IP CHANGED") == 0) || (strcmp(e.action, "NAME CHANGED") == 0);
      String rowClass = (strcmp(e.action, "INCREASE") == 0) ? "inc"
                       : (strcmp(e.action, "DECREASE") == 0) ? "dec"
                       : (strcmp(e.action, "OFFSET") == 0) ? "off"
                       : "chg";
      h += "<tr class='" + rowClass + "'>";
      h += "<td>" + htmlEscape(String(e.deviceName)) + "</td>";
      h += "<td>" + String(e.timestamp) + "</td>";
      h += "<td>" + String(e.action) + "</td>";
      if (isReassign) {
        h += "<td colspan='2' class='logempty'>-</td>";
      } else {
        h += "<td>" + String(e.fromFreq) + "MHz / " + String(e.fromMv) + "mV</td>";
        h += "<td>" + String(e.toFreq) + "MHz / " + String(e.toMv) + "mV</td>";
      }
      h += "<td>" + String(e.tempAtChange, 1) + "&deg;C</td>";
      h += "</tr>";
    }
  }
  h += "</tbody></table>";
  h += "</div>"; // log
  h += "</div>"; // card
  return h;
}

String infoIcon(const String &tooltipText, bool below = false) {
  String cls = below ? "tooltip-wrap below" : "tooltip-wrap";
  return "<span class='" + cls + "'><button type='button' class='info-icon' tabindex='0' aria-label='More info'>i</button>"
         "<span class='tooltip-content'>" + tooltipText + "</span></span>";
}

String buildSettingsPanel() {
  String h;
  h += "<details class='card settings'>";
  h += "<summary>Settings</summary>";
  h += "<form id='settingsForm' method='POST' action='/savesettings'>";

  h += "<label><span class='lbl-row'>Auto-refresh " +
       infoIcon("How often this dashboard updates data on-screen. Independently of this "
                "setting, the underlying auto-tuning code communicates with BitAxe every "
                "5s.") +
       "</span><span class='unit-row'><select name='refresh'>";
  for (int i = 0; i < REFRESH_OPTIONS_COUNT; i++) {
    h += "<option value='" + String(REFRESH_OPTIONS[i]) + "'";
    if (REFRESH_OPTIONS[i] == pageRefreshSec) h += " selected";
    h += ">" + String(REFRESH_LABELS[i]) + "</option>";
  }
  h += "</select></span></label>";

  h += "<label><span class='lbl-row'>Decrease above " +
       infoIcon("If ASIC temperature rises above this value, the tuner steps DOWN to a "
                "cooler/safer setting on the ladder. Must be at least 1.0&deg;C above "
                "Increase below.") +
       "</span><span class='unit-row'><input type='number' step='0.1' min='30' max='" +
       String(MAXIMUM_TEMP, 0) + "' name='limHigh' value='" + String(defLimitHigh, 1) + "'> &deg;C</span></label>";

  h += "<label><span class='lbl-row'>Increase below " +
       infoIcon("If ASIC temperature drops below this value, the tuner steps UP to a "
                "more aggressive/faster setting on the ladder.") +
       "</span><span class='unit-row'><input type='number' step='0.1' min='30' max='" +
       String(MAXIMUM_TEMP, 0) + "' name='limLow' value='" + String(defLimitLow, 1) + "'> &deg;C</span></label>";

  h += "<label><span class='lbl-row'>Minimum temp " +
       infoIcon("Below this temperature (e.g. right after the BitAxe reboots or "
                "reconnects and is still cold), auto-tuning is paused entirely - no "
                "increase or decrease - until the ASIC warms back up above it.") +
       "</span><span class='unit-row'><input type='number' step='1' min='30' max='" +
       String(MAXIMUM_TEMP, 0) + "' name='minTemp' value='" + String(minTemp, 0) + "'> &deg;C</span></label>";

  h += "<label><span class='lbl-row'>Settle time " +
       infoIcon("After ASIC temp recovers back above Minimum temp, wait this long before "
                "resuming normal increase/decrease evaluation - avoids reacting to the "
                "very first borderline reading right after recovery.") +
       "</span><span class='unit-row'><input type='number' step='1' min='0' max='120' "
       "name='minTempSettleSec' value='" + String(minTempSettleSec) + "'> s</span></label>";

  h += "<button type='submit'>Save</button>";
  h += "<span id='saveStatus' class='save-status'></span>";
  h += "</form>";
  h += "</details>";
  return h;
}

// Builds the three hamburger-menu modals (Device list w/ Autoscan, Voltage
// ladder, NTP settings). All three are rendered hidden and toggled purely
// client-side; their inputs are pre-filled with the CURRENT server-side
// values at page load, same pattern as the Settings panel.
String buildMenuModals() {
  String h;

  // ---- Device list modal ----
  h += "<div class='modal-overlay' id='modal-devicelist'><div class='modal-box'>";
  h += "<h2>Device list<button type='button' class='modal-close' onclick=\"closeModal('modal-devicelist')\">&times;</button></h2>";
  h += "<div class='dl-wrap'><table class='dl-table'><thead><tr>"
       "<th><span class='lbl-row'>On " +
       infoIcon("The devices with the checkbox on will be managed. If the checkbox is off "
                "the device is considered disabled.", true) +
       "</span></th>"
       "<th><span class='lbl-row'>Name " +
       infoIcon("The device's name.", true) +
       "</span></th>"
       "<th><span class='lbl-row'>IP address " +
       infoIcon("The device's IP address.", true) +
       "</span></th>"
       "<th><span class='lbl-row'>Offset (mV) " +
       infoIcon("The millivolt offset, between -50mV and +50mV, which will be applied to "
                "the Voltage ladder. Some devices require a higher voltage to run at a "
                "given frequency without errors. Others will run without errors with a "
                "lower voltage.", true) +
       "</span></th>"
       "</tr></thead>"
       "<tbody id='dlBody'></tbody></table></div>";
  h += "<div id='dlErr' class='row-err'></div>";
  h += "<div class='modal-actions'>"
       "<button type='button' class='btn' onclick='saveDeviceList()'>Save</button>"
       "<button type='button' class='btn secondary' id='scanBtn' onclick='startAutoscan()'>Autoscan LAN</button>"
       "<span id='dlSaveStatus' class='save-status'></span>"
       "</div>";
  h += "<div id='scanArea' style='display:none'>"
       "<div class='scan-status' id='scanStatusText'>Scanning...</div>"
       "<div class='scan-progress'><div class='scan-progress-bar' id='scanProgressBar'></div></div>"
       "<div class='scan-results' id='scanResults'></div>"
       "<div class='modal-actions'>"
       "<button type='button' class='btn secondary' onclick='stopAutoscan()'>Stop scan</button>"
       "</div></div>";
  h += "</div></div>";

  // ---- Voltage ladder modal ----
  h += "<div class='modal-overlay' id='modal-ladder'><div class='modal-box'>";
  h += "<h2>Voltage ladder<button type='button' class='modal-close' onclick=\"closeModal('modal-ladder')\">&times;</button></h2>";
  h += "<p style='color:#9aa0aa;font-size:.85em;margin-top:0'>Ordered from safest/coolest to most "
       "aggressive/hottest. Rows are re-sorted by frequency automatically when you save. Up to " +
       String(MAX_LADDER_SIZE) + " steps, VCore capped at " + String(MAX_VCORE_MV) + "mV.</p>";
  h += "<div class='ladder-wrap'><table class='ladder-table'><thead><tr>"
       "<th>Frequency (MHz)</th><th>VCore (mV)</th><th></th></tr></thead>"
       "<tbody id='ladderBody'></tbody></table></div>";
  h += "<div id='ladderErr' class='row-err'></div>";
  h += "<div class='modal-actions'>"
       "<button type='button' class='btn secondary' onclick='addLadderRow()'>+ Add step</button>"
       "<button type='button' class='btn' onclick='saveLadder()'>Save</button>"
       "<span id='ladderSaveStatus' class='save-status'></span>"
       "</div>";
  h += "</div></div>";

  // ---- NTP settings modal ----
  h += "<div class='modal-overlay' id='modal-ntp'><div class='modal-box'>";
  h += "<h2>NTP settings<button type='button' class='modal-close' onclick=\"closeModal('modal-ntp')\">&times;</button></h2>";
  h += "<div class='ntp-form'>";
  h += "<label>Primary NTP server</label><input type='text' id='ntp1' value='" + htmlEscape(ntpServer1) + "'>";
  h += "<label>Secondary NTP server (optional)</label><input type='text' id='ntp2' value='" + htmlEscape(ntpServer2) + "'>";
  h += "</div>";
  h += "<p style='color:#9aa0aa;font-size:.85em'>If the primary doesn't answer, the ESP32 automatically "
       "falls back to the secondary, then cycles back to the primary again - no reboot needed after saving.</p>";
  h += "<div id='ntpErr' class='row-err'></div>";
  h += "<div class='modal-actions'>"
       "<button type='button' class='btn' onclick='saveNtp()'>Save</button>"
       "<span id='ntpSaveStatus' class='save-status'></span>"
       "</div>";
  h += "</div></div>";

  return h;
}

void handleRoot() {
  String html;
  html += "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>BitAxe Auto-Tuner</title>";
  html += "<style>"
          "body{font-family:Segoe UI,Arial,sans-serif;background:#12151a;color:#e6e6e6;margin:0;padding:20px;}"
          "h1{color:#f7931a;}"
          ".card{background:#1c2027;border-radius:10px;padding:16px 20px;margin-bottom:24px;box-shadow:0 2px 8px rgba(0,0,0,.4);}"
          ".card h2{margin-top:0;color:#f7931a;}"
          ".ip{color:#888;font-size:0.7em;font-weight:normal;}"
          ".grid{display:flex;flex-wrap:wrap;gap:16px;}"
          ".box{background:#20242c;border-radius:8px;padding:10px 16px;min-width:190px;flex:1;}"
          ".box h3{margin:0 0 8px 0;font-size:.85em;color:#9aa0aa;text-transform:uppercase;letter-spacing:.05em;}"
          ".box p{margin:4px 0;font-size:.95em;}"
          ".hot{color:#ff5555;} .cool{color:#5ac8fa;}"
          ".offline{color:#ff5555;font-weight:bold;}"
          ".lowtemp{color:#f7c948;font-weight:bold;}"
          ".settling{color:#5ac8fa;font-weight:bold;}"
          ".log{overflow-x:auto;-webkit-overflow-scrolling:touch;}"
          ".log table{width:100%;min-width:560px;border-collapse:collapse;font-size:.85em;}"
          ".log th,.log td{text-align:left;padding:5px 8px;border-bottom:1px solid #2c3038;white-space:nowrap;}"
          // Keeps the box at roughly the same visible height the old
          // per-device (max 10-row) log used to take up; with up to 30
          // entries now shared across all devices, the rest scrolls.
          ".log-scroll{max-height:360px;overflow-y:auto;}"
          ".log-scroll thead th{position:sticky;top:0;background:#1c2027;z-index:1;}"
          ".log tr.inc td:nth-child(2){color:#4caf50;font-weight:bold;}"
          ".log tr.dec td:nth-child(2){color:#ff5555;font-weight:bold;}"
          ".log tr.off td:nth-child(2){color:#f7931a;font-weight:bold;}"
          ".log tr.chg td:nth-child(2){color:#5ac8fa;font-weight:bold;}"
          ".logempty{color:#888;}"
          ".edit-btn{background:transparent;border:none;color:#9aa0aa;cursor:pointer;font-size:.9em;padding:2px 4px;vertical-align:middle;}"
          ".edit-btn:hover,.edit-btn:focus{color:#f7931a;outline:none;}"
          ".edit-form{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:8px 0 4px 0;}"
          ".edit-form input[type=text]{padding:6px 8px;border-radius:6px;border:1px solid #333;background:#20242c;color:#e6e6e6;font-size:.9em;}"
          ".edit-form input[id^=editname]{width:160px;}"
          ".edit-form button{padding:6px 14px;border-radius:6px;border:none;background:#f7931a;color:#12151a;font-weight:bold;cursor:pointer;font-size:.85em;}"
          ".edit-form button:hover{background:#ffab40;}"
          ".edit-form .cancel-btn{background:transparent;border:1px solid #444;color:#9aa0aa;}"
          ".edit-form .cancel-btn:hover{background:#2a2f38;border-color:#666;}"
          ".log-header{display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px;margin-top:20px;}"
          ".log-header h3{margin:0;font-size:.85em;color:#9aa0aa;text-transform:uppercase;letter-spacing:.05em;}"
          ".clearlog-btn{background:transparent;border:1px solid #444;color:#ff8a80;padding:4px 10px;border-radius:6px;cursor:pointer;font-size:.8em;}"
          ".clearlog-btn:hover{background:#2a1f1f;border-color:#ff5555;}"
          ".settings summary{color:#f7931a;font-size:1.3em;font-weight:bold;cursor:pointer;outline:none;list-style:revert;}"
          ".settings summary::-webkit-details-marker{color:#f7931a;}"
          ".settings form{display:flex;flex-wrap:wrap;gap:20px;align-items:flex-end;margin-top:16px;}"
          ".settings label{display:flex;flex-direction:column;gap:4px;font-size:.8em;color:#9aa0aa;text-transform:uppercase;letter-spacing:.04em;}"
          ".settings .unit-row{display:flex;align-items:center;gap:6px;text-transform:none;font-size:1rem;color:#e6e6e6;}"
          ".lbl-row{display:flex;align-items:center;gap:5px;}"
          ".tooltip-wrap{position:relative;display:inline-flex;}"
          ".info-icon{width:15px;height:15px;padding:0;border-radius:50%;border:1px solid #666;background:#2a2f38;"
          "color:#9aa0aa;font-size:10px;font-style:italic;font-family:Georgia,serif;line-height:1;cursor:pointer;"
          "display:inline-flex;align-items:center;justify-content:center;}"
          ".info-icon:hover,.info-icon:focus{color:#f7931a;border-color:#f7931a;outline:none;}"
          ".tooltip-content{display:none;position:absolute;bottom:135%;left:50%;transform:translateX(-50%);"
          "width:220px;max-width:70vw;background:#2a2f38;color:#e6e6e6;text-transform:none;letter-spacing:normal;"
          "font-size:.85em;line-height:1.4;padding:8px 10px;border-radius:6px;border:1px solid #444;"
          "box-shadow:0 4px 14px rgba(0,0,0,.5);z-index:20;}"
          // Icons that sit at the very top of a scrolling container (e.g.
          // Device List table headers) pop the tooltip DOWNWARD instead -
          // opening upward there gets clipped by the container's own
          // overflow box (dl-wrap needs overflow-x:auto for narrow screens,
          // which implicitly clips overflow-y too).
          ".tooltip-wrap.below .tooltip-content{bottom:auto;top:135%;}"
          ".tooltip-wrap:hover .tooltip-content,.info-icon:focus + .tooltip-content{display:block;}"
          ".settings input[type=number],.settings select{padding:6px 8px;border-radius:6px;border:1px solid #333;background:#20242c;color:#e6e6e6;font-size:1em;width:100px;}"
          ".settings button{padding:8px 18px;border-radius:6px;border:none;background:#f7931a;color:#12151a;font-weight:bold;cursor:pointer;font-size:1em;}"
          ".settings button:hover{background:#ffab40;}"
          ".save-status{margin-left:6px;font-size:.9em;font-weight:bold;}"
          ".save-status.ok{color:#4caf50;} .save-status.err{color:#ff5555;}"
          "footer{color:#666;font-size:.8em;margin-top:10px;}"
          // ---- Header / hamburger menu ----
          ".topbar{display:flex;justify-content:space-between;align-items:center;}"
          ".hamburger{background:transparent;border:1px solid #333;border-radius:8px;color:#e6e6e6;"
          "width:42px;height:38px;font-size:1.3em;cursor:pointer;line-height:1;}"
          ".hamburger:hover,.hamburger:focus{border-color:#f7931a;color:#f7931a;outline:none;}"
          ".menu-wrap{position:relative;}"
          ".menu-dropdown{display:none;position:absolute;right:0;top:46px;background:#1c2027;"
          "border:1px solid #333;border-radius:10px;min-width:200px;box-shadow:0 6px 20px rgba(0,0,0,.5);"
          "overflow:hidden;z-index:40;}"
          ".menu-dropdown.open{display:block;}"
          ".menu-dropdown button{display:block;width:100%;text-align:left;background:transparent;"
          "border:none;color:#e6e6e6;padding:12px 16px;font-size:.95em;cursor:pointer;border-bottom:1px solid #262a31;}"
          ".menu-dropdown button:last-child{border-bottom:none;}"
          ".menu-dropdown button:hover{background:#262b33;color:#f7931a;}"
          // ---- Modals ----
          ".modal-overlay{display:none;position:fixed;inset:0;background:rgba(0,0,0,.6);z-index:50;"
          "align-items:flex-start;justify-content:center;padding:30px 12px;overflow-y:auto;}"
          ".modal-overlay.open{display:flex;}"
          ".modal-box{background:#1c2027;border-radius:10px;padding:20px;width:100%;max-width:720px;"
          "box-shadow:0 8px 30px rgba(0,0,0,.6);}"
          ".modal-box h2{margin-top:0;color:#f7931a;display:flex;justify-content:space-between;align-items:center;}"
          ".modal-close{background:transparent;border:none;color:#9aa0aa;font-size:1.4em;cursor:pointer;line-height:1;}"
          ".modal-close:hover{color:#ff5555;}"
          ".modal-actions{display:flex;gap:10px;align-items:center;margin-top:16px;flex-wrap:wrap;}"
          ".btn{padding:8px 16px;border-radius:6px;border:none;background:#f7931a;color:#12151a;"
          "font-weight:bold;cursor:pointer;font-size:.9em;}"
          ".btn:hover{background:#ffab40;}"
          ".btn.secondary{background:transparent;border:1px solid #444;color:#9aa0aa;}"
          ".btn.secondary:hover{background:#2a2f38;border-color:#666;}"
          ".btn.danger{background:transparent;border:1px solid #663;color:#ff8a80;}"
          ".btn.danger:hover{background:#2a1f1f;border-color:#ff5555;}"
          ".btn:disabled{opacity:.5;cursor:not-allowed;}"
          // ---- Device list table ----
          ".dl-table{width:100%;border-collapse:collapse;font-size:.85em;}"
          ".dl-table th,.dl-table td{text-align:left;padding:6px;border-bottom:1px solid #2c3038;}"
          ".dl-table input[type=text]{width:100%;box-sizing:border-box;padding:5px 6px;border-radius:5px;"
          "border:1px solid #333;background:#20242c;color:#e6e6e6;font-size:.95em;}"
          ".dl-table input[type=number]{width:70px;padding:5px 6px;border-radius:5px;border:1px solid #333;"
          "background:#20242c;color:#e6e6e6;font-size:.95em;}"
          ".dl-table input[type=checkbox]{width:18px;height:18px;}"
          ".dl-wrap{overflow-x:auto;}"
          ".row-err{color:#ff5555;font-size:.8em;}"
          // ---- Ladder table ----
          ".ladder-table{width:100%;border-collapse:collapse;font-size:.9em;}"
          ".ladder-table th,.ladder-table td{text-align:left;padding:5px;border-bottom:1px solid #2c3038;}"
          ".ladder-table input[type=number]{width:90px;padding:5px 6px;border-radius:5px;border:1px solid #333;"
          "background:#20242c;color:#e6e6e6;}"
          ".ladder-wrap{max-height:50vh;overflow-y:auto;}"
          ".icon-btn{background:transparent;border:none;color:#9aa0aa;cursor:pointer;font-size:1em;padding:2px 6px;}"
          ".icon-btn:hover{color:#ff5555;}"
          // ---- NTP form ----
          ".ntp-form label{display:block;font-size:.8em;color:#9aa0aa;text-transform:uppercase;"
          "letter-spacing:.04em;margin-bottom:4px;margin-top:12px;}"
          ".ntp-form input[type=text]{width:100%;box-sizing:border-box;padding:8px 10px;border-radius:6px;"
          "border:1px solid #333;background:#20242c;color:#e6e6e6;font-size:1em;}"
          // ---- Autoscan ----
          ".scan-status{margin-top:14px;font-size:.9em;color:#9aa0aa;}"
          ".scan-progress{height:6px;border-radius:3px;background:#20242c;overflow:hidden;margin-top:6px;}"
          ".scan-progress-bar{height:100%;background:#f7931a;width:0%;transition:width .3s;}"
          ".scan-results{margin-top:14px;display:flex;flex-direction:column;gap:8px;}"
          ".scan-result{background:#20242c;border-radius:8px;padding:10px 12px;display:flex;"
          "justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px;}"
          ".scan-result .info{font-size:.9em;}"
          ".scan-result .info .ip{color:#9aa0aa;font-size:.85em;}"
          ".scan-result select{padding:5px 8px;border-radius:5px;border:1px solid #333;background:#20242c;color:#e6e6e6;}"
          "</style></head><body>";

  html += "<div class='topbar'><h1>BitAxe Auto-Tuner</h1>"
          "<div class='menu-wrap'>"
          "<button type='button' class='hamburger' id='menuBtn' aria-label='Menu' onclick='toggleMenu()'>&#9776;</button>"
          "<div class='menu-dropdown' id='menuDropdown'>"
          "<button type='button' onclick='openDeviceList()'>Device list</button>"
          "<button type='button' onclick='openLadder()'>Voltage ladder</button>"
          "<button type='button' onclick='openNtp()'>NTP settings</button>"
          "</div></div></div>";

  html += buildSettingsPanel();

  bool any = false;
  for (int d = 0; d < MAX_DEVICES; d++) {
    if (!devices[d].enabled) continue;
    any = true;
    html += buildDeviceCard(devices[d], d);
  }
  if (!any) {
    html += "<p>No devices enabled. Edit DEVICE_CONFIG in the sketch.</p>";
  }

  html += buildGlobalLogSection();

  html += "<footer id='footerText'>Auto-refreshes every " + String(pageRefreshSec) + "s &middot; Limits: decrease above " +
          String(defLimitHigh, 1) + "&deg;C, increase below " +
          String(defLimitLow, 1) + "&deg;C &middot; paused below " +
          String(minTemp, 1) + "&deg;C</footer>";

  html += buildMenuModals();

  // ---- Client-side JS: polls /api/status via fetch() instead of reloading
  // the page, and saves settings via fetch() instead of a normal form POST.
  html += "<script>";
  html += "let refreshSeconds=" + String(pageRefreshSec) + ";";
  html += "let limHigh=" + String(defLimitHigh, 1) + ";";
  html += "let limLow=" + String(defLimitLow, 1) + ";";
  html += "let minTemp=" + String(minTemp, 1) + ";";
  html += "const ladderMaxMv=" + String(ladderMaxMv) + ";";
  html += "const maxVcoreMv=" + String(MAX_VCORE_MV) + ";";
  html += "const maxDevices=" + String(MAX_DEVICES) + ";";
  html += "const maxLadderSize=" + String(MAX_LADDER_SIZE) + ";";
  html += "const offsetMinMv=" + String(VOLTAGE_OFFSET_MIN_MV) + ";";
  html += "const offsetMaxMv=" + String(VOLTAGE_OFFSET_MAX_MV) + ";";

  // Initial snapshot of the device list, ladder, and NTP config - the
  // Device List / Voltage Ladder / NTP Settings modals render from these
  // in-memory arrays and re-POST the whole thing back on Save, rather than
  // making a round trip just to open a panel.
  {
    DynamicJsonDocument dlDoc(2048);
    JsonArray dlArr = dlDoc.to<JsonArray>();
    for (int i = 0; i < MAX_DEVICES; i++) {
      JsonObject o = dlArr.createNestedObject();
      o["enabled"] = devices[i].enabled;
      o["name"] = devices[i].name;
      o["ip"] = devices[i].ip;
      o["offset"] = devices[i].offsetMv;
    }
    String dlJson;
    serializeJson(dlDoc, dlJson);
    html += "let initialDeviceList=" + dlJson + ";";
  }
  {
    DynamicJsonDocument ldDoc(2048);
    JsonArray ldArr = ldDoc.to<JsonArray>();
    for (int i = 0; i < LADDER_SIZE; i++) {
      JsonArray step = ldArr.createNestedArray();
      step.add(ladder[i].freqMHz);
      step.add(ladder[i].mv);
    }
    String ldJson;
    serializeJson(ldDoc, ldJson);
    html += "let initialLadder=" + ldJson + ";";
  }

  html += R"JS(
let pollTimer = null;

function pad(n) { return String(n).padStart(2, '0'); }

function formatUptime(t) {
  t = Math.floor(t);
  const d = Math.floor(t / 86400), h = Math.floor((t % 86400) / 3600),
        m = Math.floor((t % 3600) / 60), s = t % 60;
  return d > 0 ? (d + 'd ' + pad(h) + 'h ' + pad(m) + 'm ' + pad(s) + 's')
               : (pad(h) + 'h ' + pad(m) + 'm ' + pad(s) + 's');
}

function formatDifficulty(v) {
  if (v >= 1e12) return (v / 1e12).toFixed(2) + 'T';
  if (v >= 1e9)  return (v / 1e9).toFixed(2) + 'G';
  if (v >= 1e6)  return (v / 1e6).toFixed(2) + 'M';
  if (v >= 1e3)  return (v / 1e3).toFixed(2) + 'K';
  return v.toFixed(0);
}

function formatMv(mv) {
  return Math.floor(mv / 1000) + '.' + String(mv % 1000).padStart(3, '0');
}

function setText(id, text) {
  const el = document.getElementById(id);
  if (el) el.textContent = text;
}

function renderDevices(devices) {
  devices.forEach(dev => {
    const idx = dev.idx;
    const offlineEl = document.getElementById('offline-' + idx);
    if (offlineEl) offlineEl.style.display = dev.infoValid ? 'none' : 'block';

    const lowTempEl = document.getElementById('lowtemp-' + idx);
    if (lowTempEl) {
      const belowMin = dev.infoValid && dev.temp < minTemp;
      lowTempEl.style.display = belowMin ? 'block' : 'none';
      if (belowMin) {
        lowTempEl.textContent = '\u26A0 ASIC temp below minimum (' + dev.temp.toFixed(1) +
          '\u00b0C < ' + minTemp.toFixed(1) + '\u00b0C) - auto-tune paused';
      }
    }

    const settlingEl = document.getElementById('settling-' + idx);
    if (settlingEl) {
      const settleSec = dev.settleSec || 0;
      settlingEl.style.display = settleSec > 0 ? 'block' : 'none';
      if (settleSec > 0) {
        settlingEl.textContent = '\u23F1 Stabilizing after minimum-temp recovery - resuming auto-tune in ' +
          settleSec + 's';
      }
    }

    const patchFailEl = document.getElementById('patchfail-' + idx);
    if (patchFailEl) {
      const fails = dev.patchFails || 0;
      patchFailEl.style.display = fails > 0 ? 'block' : 'none';
      if (fails > 0) {
        patchFailEl.textContent = '\u26A0 Settings changes are failing (' + fails +
          ' in a row) - auto-tune is stuck until this recovers';
      }
    }

    const thps = dev.hashRateGHs / 1000;
    setText('hashrate-' + idx, thps.toFixed(2) + ' TH/s');
    setText('error-' + idx, dev.errorPercentage.toFixed(2) + ' %');
    setText('efficiency-' + idx, thps > 0 ? (dev.power / thps).toFixed(2) + ' J/TH' : 'N/A');
    setText('bestdiff-' + idx, formatDifficulty(dev.bestDiff));
    setText('bestsession-' + idx, formatDifficulty(dev.bestSessionDiff));
    setText('uptime-' + idx, formatUptime(dev.uptimeSeconds));
    setText('power-' + idx, dev.power.toFixed(2) + ' W');
    setText('inputv-' + idx, dev.inputVoltageV.toFixed(2) + ' V');
    setText('freq-' + idx, dev.frequency + ' MHz');
    setText('corev-' + idx, formatMv(dev.coreVoltageMv) + ' V');

    const tempEl = document.getElementById('temp-' + idx);
    if (tempEl) {
      tempEl.textContent = dev.temp.toFixed(1) + ' \u00b0C';
      tempEl.className = dev.temp > limHigh ? 'hot' : (dev.temp < limLow ? 'cool' : '');
    }
    setText('vrtemp-' + idx, dev.vrTemp.toFixed(1) + ' \u00b0C');
  });
}

function renderGlobalLog(log) {
  const tbody = document.getElementById('logbody-global');
  if (!tbody) return;
  if (!log || log.length === 0) {
    tbody.innerHTML = "<tr><td colspan='6' class='logempty'>No changes yet.</td></tr>";
    return;
  }
  tbody.innerHTML = log.map(e => {
    const isReassign = (e.action === 'IP CHANGED' || e.action === 'NAME CHANGED');
    const cls = e.action === 'INCREASE' ? 'inc'
              : e.action === 'DECREASE' ? 'dec'
              : e.action === 'OFFSET' ? 'off'
              : 'chg';
    const fromTo = isReassign
      ? "<td colspan='2' class='logempty'>-</td>"
      : "<td>" + e.fromFreq + "MHz / " + e.fromMv + "mV</td><td>" + e.toFreq + "MHz / " + e.toMv + "mV</td>";
    return "<tr class='" + cls + "'><td>" + escapeAttr(e.device) + "</td><td>" + e.ts + "</td><td>" + e.action + "</td>" +
           fromTo + "<td>" + e.temp.toFixed(1) + "\u00b0C</td></tr>";
  }).join('');
}

async function pollStatus() {
  try {
    const res = await fetch('/api/status');
    if (!res.ok) return;
    const data = await res.json();
    renderDevices(data.devices);
    renderGlobalLog(data.log);
  } catch (e) {
    console.error('status poll failed', e);
  }
}

function startPolling() {
  if (pollTimer) clearInterval(pollTimer);
  pollTimer = setInterval(pollStatus, refreshSeconds * 1000);
}

function showSaveStatus(msg, isError) {
  const el = document.getElementById('saveStatus');
  if (!el) return;
  el.textContent = msg;
  el.className = 'save-status ' + (isError ? 'err' : 'ok');
  setTimeout(() => { el.textContent = ''; el.className = 'save-status'; }, 3000);
}

async function clearLog() {
  if (!confirm('Clear the entire operation log (all devices)?')) return;
  try {
    const res = await fetch('/clearlog', { method: 'POST' });
    const data = await res.json();
    if (data.ok) renderGlobalLog([]);
  } catch (e) {
    console.error('clear log failed', e);
  }
}

function isValidIPv4(ip) {
  const parts = ip.split('.');
  if (parts.length !== 4) return false;
  return parts.every(p => /^\d{1,3}$/.test(p) && Number(p) >= 0 && Number(p) <= 255);
}

function openEditDevice(idx) {
  const err = document.getElementById('editerr-' + idx);
  if (err) err.textContent = '';
  document.getElementById('editform-' + idx).style.display = 'flex';
}

function closeEditDevice(idx) {
  document.getElementById('editform-' + idx).style.display = 'none';
  const err = document.getElementById('editerr-' + idx);
  if (err) err.textContent = '';
}

async function saveEditDevice(idx) {
  const nameEl = document.getElementById('editname-' + idx);
  const ipEl = document.getElementById('editip-' + idx);
  const errEl = document.getElementById('editerr-' + idx);
  const name = nameEl.value.trim();
  const ip = ipEl.value.trim();
  errEl.textContent = '';

  if (!name) { errEl.textContent = 'Name cannot be empty'; return; }
  if (!isValidIPv4(ip)) { errEl.textContent = 'Invalid IP address'; return; }

  try {
    const res = await fetch('/editdevice', { method: 'POST', body: new URLSearchParams({ idx, name, ip }) });
    const data = await res.json();
    if (data.ok) {
      document.getElementById('name-' + idx).textContent = data.name;
      document.getElementById('ip-' + idx).textContent = '(' + data.ip + ')';
      // Keep the Device List modal's in-memory snapshot in sync, otherwise
      // it would still show the pre-edit name/IP next time it's opened.
      if (initialDeviceList[idx]) {
        initialDeviceList[idx].name = data.name;
        initialDeviceList[idx].ip = data.ip;
      }
      closeEditDevice(idx);
    } else {
      errEl.textContent = data.error || 'Save failed';
    }
  } catch (e) {
    errEl.textContent = 'Save failed';
  }
}

document.addEventListener('DOMContentLoaded', () => {
  startPolling();

  const form = document.getElementById('settingsForm');
  form.addEventListener('submit', async (ev) => {
    ev.preventDefault();

    const newHigh = parseFloat(form.limHigh.value);
    const newLow  = parseFloat(form.limLow.value);
    if (!(newHigh - newLow > 1.0)) {
      showSaveStatus('Decrease above must be at least 1.0\u00b0C higher than Increase below', true);
      return;
    }

    const params = new URLSearchParams(new FormData(form));
    try {
      const res = await fetch('/savesettings', { method: 'POST', body: params });
      const data = await res.json();
      limHigh = data.limHigh;
      limLow = data.limLow;
      minTemp = data.minTemp;
      refreshSeconds = data.refresh;
      startPolling();
      document.getElementById('footerText').textContent =
        'Auto-refreshes every ' + refreshSeconds + 's \u00b7 Limits: decrease above ' +
        limHigh.toFixed(1) + '\u00b0C, increase below ' + limLow.toFixed(1) +
        '\u00b0C \u00b7 paused below ' + minTemp.toFixed(1) + '\u00b0C';
      showSaveStatus(data.ok ? 'Saved!' : 'Refresh saved, but some values were rejected', !data.ok);
    } catch (e) {
      showSaveStatus('Save failed', true);
    }
  });
});

// ============================================================================
//  HAMBURGER MENU / MODALS
// ============================================================================

function toggleMenu() {
  document.getElementById('menuDropdown').classList.toggle('open');
}

document.addEventListener('click', (ev) => {
  const wrap = document.querySelector('.menu-wrap');
  if (wrap && !wrap.contains(ev.target)) {
    document.getElementById('menuDropdown').classList.remove('open');
  }
});

function closeModal(id) {
  document.getElementById(id).classList.remove('open');
}

function openModalById(id) {
  document.getElementById('menuDropdown').classList.remove('open');
  const el = document.getElementById(id);
  el.classList.add('open');
  // The overlay div stays in the DOM (just hidden) between opens, so it
  // keeps whatever scroll position it had when last closed - reset it here
  // so the modal always opens at the top instead of wherever the user last
  // scrolled to (e.g. down at the Save button).
  el.scrollTop = 0;
}

// ---- Device list ----

function openDeviceList() {
  renderDeviceListRows();
  document.getElementById('dlErr').textContent = '';
  document.getElementById('scanArea').style.display = 'none';
  openModalById('modal-devicelist');
}

function renderDeviceListRows() {
  const tbody = document.getElementById('dlBody');
  tbody.innerHTML = '';
  initialDeviceList.forEach((d, i) => {
    const tr = document.createElement('tr');
    tr.innerHTML =
      "<td><input type='checkbox' id='dl-en-" + i + "'" + (d.enabled ? ' checked' : '') + "></td>" +
      "<td><input type='text' id='dl-name-" + i + "' value=\"" + escapeAttr(d.name) + "\" maxlength='40'></td>" +
      "<td><input type='text' id='dl-ip-" + i + "' value=\"" + escapeAttr(d.ip) + "\" placeholder='192.168.1.x'></td>" +
      "<td><input type='number' id='dl-off-" + i + "' value='" + d.offset + "' min='" + offsetMinMv + "' max='" + offsetMaxMv + "'></td>";
    tbody.appendChild(tr);
  });
}

function escapeAttr(s) {
  return String(s).replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;');
}

async function saveDeviceList() {
  const errEl = document.getElementById('dlErr');
  errEl.textContent = '';
  const rows = [];
  for (let i = 0; i < maxDevices; i++) {
    const enabled = document.getElementById('dl-en-' + i).checked;
    const name = document.getElementById('dl-name-' + i).value.trim();
    const ip = document.getElementById('dl-ip-' + i).value.trim();
    const offset = parseInt(document.getElementById('dl-off-' + i).value, 10) || 0;

    if (enabled) {
      if (!name) { errEl.textContent = 'Row ' + (i + 1) + ': name cannot be empty'; return; }
      if (!isValidIPv4(ip)) { errEl.textContent = 'Row ' + (i + 1) + ': invalid IP address'; return; }
      if (offset < offsetMinMv || offset > offsetMaxMv) {
        errEl.textContent = 'Row ' + (i + 1) + ': offset must be between ' + offsetMinMv + ' and ' + offsetMaxMv + 'mV';
        return;
      }
      if (ladderMaxMv + offset > maxVcoreMv) {
        errEl.textContent = 'Row ' + (i + 1) + ': offset would exceed max VCore';
        return;
      }
    }
    rows.push({ enabled, name, ip, offset });
  }
  // Duplicate IP check across enabled rows
  const ips = rows.filter(r => r.enabled && r.ip).map(r => r.ip);
  const dupe = ips.find((ip, i) => ips.indexOf(ip) !== i);
  if (dupe) { errEl.textContent = 'IP ' + dupe + ' is used by more than one row'; return; }

  try {
    const res = await fetch('/devicelist/save', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ devices: rows }) });
    const data = await res.json();
    if (data.ok) {
      initialDeviceList = rows;
      showModalStatus('dlSaveStatus', 'Saved! Reloading...', false);
      setTimeout(() => location.reload(), 900);
    } else {
      errEl.textContent = data.error || 'Save failed';
    }
  } catch (e) {
    errEl.textContent = 'Save failed';
  }
}

function showModalStatus(elId, msg, isError) {
  const el = document.getElementById(elId);
  el.textContent = msg;
  el.className = 'save-status ' + (isError ? 'err' : 'ok');
  setTimeout(() => { el.textContent = ''; el.className = 'save-status'; }, 3000);
}

// ---- Autoscan ----

let scanTimer = null;

async function startAutoscan() {
  document.getElementById('scanArea').style.display = 'block';
  document.getElementById('scanResults').innerHTML = '';
  document.getElementById('scanStatusText').textContent = 'Starting scan...';
  document.getElementById('scanProgressBar').style.width = '0%';
  try {
    await fetch('/autoscan/start', { method: 'POST' });
    pollAutoscanStatus();
  } catch (e) {
    document.getElementById('scanStatusText').textContent = 'Failed to start scan.';
  }
}

async function stopAutoscan() {
  if (scanTimer) clearTimeout(scanTimer);
  try { await fetch('/autoscan/stop', { method: 'POST' }); } catch (e) {}
  document.getElementById('scanArea').style.display = 'none';
}

async function pollAutoscanStatus() {
  try {
    const res = await fetch('/autoscan/status');
    const data = await res.json();
    document.getElementById('scanProgressBar').style.width = data.progress + '%';
    document.getElementById('scanStatusText').textContent = data.done
      ? ('Scan complete. ' + data.results.length + ' device(s) found.')
      : ('Scanning ' + data.currentIp + '... (' + data.progress + '%)');

    renderScanResults(data.results);

    if (!data.done) {
      scanTimer = setTimeout(pollAutoscanStatus, 800);
    }
  } catch (e) {
    document.getElementById('scanStatusText').textContent = 'Scan status unavailable.';
  }
}

function renderScanResults(results) {
  const wrap = document.getElementById('scanResults');
  wrap.innerHTML = '';
  results.forEach((r) => {
    const div = document.createElement('div');
    div.className = 'scan-result';
    const usedSlots = initialDeviceList
      .map((d, i) => ({ d, i }))
      .filter(x => x.d.enabled);
    let optionsHtml = "<option value=''>Replace...</option>";
    usedSlots.forEach(x => {
      optionsHtml += "<option value='" + x.i + "'>" + escapeAttr(x.d.name) + " (" + x.d.ip + ")</option>";
    });
    const hasFreeSlot = initialDeviceList.some(d => !d.enabled);
    div.innerHTML =
      "<div class='info'>" + escapeAttr(r.hostname || 'BitAxe') + "<div class='ip'>" + r.ip + "</div></div>" +
      "<div style='display:flex;gap:8px;align-items:center;flex-wrap:wrap'>" +
      "<button type='button' class='btn secondary' " + (hasFreeSlot ? '' : 'disabled') +
      " onclick=\"autoscanAdd('" + r.ip + "','" + escapeAttr(r.hostname || '') + "')\">Add to list</button>" +
      "<select onchange=\"if(this.value!=='')autoscanReplace('" + r.ip + "','" + escapeAttr(r.hostname || '') + "',this.value)\">" +
      optionsHtml + "</select></div>";
    wrap.appendChild(div);
  });
}

async function autoscanAdd(ip, hostname) {
  await autoscanApply('add', ip, hostname, null);
}

async function autoscanReplace(ip, hostname, targetIdx) {
  if (!confirm('Replace the device at slot ' + (parseInt(targetIdx, 10) + 1) + ' with the unit found at ' + ip + '?')) return;
  await autoscanApply('replace', ip, hostname, targetIdx);
}

async function autoscanApply(action, ip, hostname, targetIdx) {
  try {
    const body = { action, ip, hostname };
    if (targetIdx !== null) body.idx = targetIdx;
    const res = await fetch('/autoscan/apply', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
    const data = await res.json();
    if (data.ok) {
      showModalStatus('dlSaveStatus', 'Device list updated! Reloading...', false);
      setTimeout(() => location.reload(), 900);
    } else {
      alert(data.error || 'Failed to apply');
    }
  } catch (e) {
    alert('Failed to apply');
  }
}

// ---- Voltage ladder ----

let ladderRows = [];

function openLadder() {
  ladderRows = initialLadder.map(s => ({ freq: s[0], mv: s[1] }));
  renderLadderRows();
  document.getElementById('ladderErr').textContent = '';
  openModalById('modal-ladder');
}

function renderLadderRows() {
  const tbody = document.getElementById('ladderBody');
  tbody.innerHTML = '';
  ladderRows.forEach((s, i) => {
    const tr = document.createElement('tr');
    tr.innerHTML =
      "<td><input type='number' min='1' max='2000' value='" + s.freq + "' onchange='ladderRows[" + i + "].freq=parseInt(this.value,10)||0'></td>" +
      "<td><input type='number' min='1' max='" + maxVcoreMv + "' value='" + s.mv + "' onchange='ladderRows[" + i + "].mv=parseInt(this.value,10)||0'></td>" +
      "<td><button type='button' class='icon-btn' onclick='removeLadderRow(" + i + ")' aria-label='Remove step'>&times;</button></td>";
    tbody.appendChild(tr);
  });
}

function addLadderRow() {
  if (ladderRows.length >= maxLadderSize) {
    document.getElementById('ladderErr').textContent = 'Ladder is already at the maximum of ' + maxLadderSize + ' steps';
    return;
  }
  const last = ladderRows[ladderRows.length - 1] || { freq: 500, mv: 1150 };
  ladderRows.push({ freq: last.freq + 10, mv: last.mv });
  renderLadderRows();
}

function removeLadderRow(i) {
  if (ladderRows.length <= 1) {
    document.getElementById('ladderErr').textContent = 'The ladder needs at least one step';
    return;
  }
  ladderRows.splice(i, 1);
  renderLadderRows();
}

async function saveLadder() {
  const errEl = document.getElementById('ladderErr');
  errEl.textContent = '';
  for (const s of ladderRows) {
    if (!(s.freq > 0) || !(s.mv > 0)) { errEl.textContent = 'All steps need a frequency and VCore above 0'; return; }
    if (s.mv > maxVcoreMv) { errEl.textContent = 'VCore cannot exceed ' + maxVcoreMv + 'mV'; return; }
  }
  const sorted = [...ladderRows].sort((a, b) => a.freq - b.freq);
  try {
    const res = await fetch('/ladder/save', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ ladder: sorted.map(s => [s.freq, s.mv]) }) });
    const data = await res.json();
    if (data.ok) {
      initialLadder = sorted.map(s => [s.freq, s.mv]);
      showModalStatus('ladderSaveStatus', 'Saved! Reloading...', false);
      setTimeout(() => location.reload(), 900);
    } else {
      errEl.textContent = data.error || 'Save failed';
    }
  } catch (e) {
    errEl.textContent = 'Save failed';
  }
}

// ---- NTP settings ----

function openNtp() {
  document.getElementById('ntpErr').textContent = '';
  openModalById('modal-ntp');
}

async function saveNtp() {
  const errEl = document.getElementById('ntpErr');
  errEl.textContent = '';
  const ntp1 = document.getElementById('ntp1').value.trim();
  const ntp2 = document.getElementById('ntp2').value.trim();
  if (!ntp1) { errEl.textContent = 'Primary NTP server cannot be empty'; return; }

  try {
    const res = await fetch('/ntpsettings/save', { method: 'POST', body: new URLSearchParams({ ntp1, ntp2 }) });
    const data = await res.json();
    if (data.ok) {
      showModalStatus('ntpSaveStatus', 'Saved!', false);
      closeModal('modal-ntp');
    } else {
      errEl.textContent = data.error || 'Save failed';
    }
  } catch (e) {
    errEl.textContent = 'Save failed';
  }
}
)JS";
  html += "</script>";

  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

void handleApiStatus() {
  DynamicJsonDocument doc(24576);
  JsonArray arr = doc.createNestedArray("devices");

  for (int d = 0; d < MAX_DEVICES; d++) {
    if (!devices[d].enabled) continue;
    BitaxeDevice &dev = devices[d];
    BitaxeInfo &i = dev.info;

    JsonObject o = arr.createNestedObject();
    o["idx"] = d;
    o["infoValid"] = dev.infoValid;
    o["hashRateGHs"] = i.hashRateGHs;
    o["errorPercentage"] = i.errorPercentage;
    o["bestDiff"] = i.bestDiff;
    o["bestSessionDiff"] = i.bestSessionDiff;
    o["uptimeSeconds"] = i.uptimeSeconds;
    o["power"] = i.power;
    o["inputVoltageV"] = i.inputVoltageV;
    o["frequency"] = i.frequency;
    o["coreVoltageMv"] = i.coreVoltageMv;
    o["temp"] = i.temp;
    o["vrTemp"] = i.vrTemp;
    o["settleSec"] = settlingRemainingSeconds(dev);
    o["patchFails"] = dev.consecutivePatchFails;
  }

  // Single combined log, shared across all devices - newest first.
  JsonArray logArr = doc.createNestedArray("log");
  for (int n = globalLogCount - 1; n >= 0; n--) {
    int idx2 = (globalLogHead + n) % LOG_SIZE;
    LogEntry &e = globalLogBuf[idx2];
    JsonObject lo = logArr.createNestedObject();
    lo["device"] = e.deviceName;
    lo["ts"] = e.timestamp;
    lo["action"] = e.action;
    lo["fromFreq"] = e.fromFreq;
    lo["toFreq"] = e.toFreq;
    lo["fromMv"] = e.fromMv;
    lo["toMv"] = e.toMv;
    lo["temp"] = e.tempAtChange;
  }

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleClearLog() {
  clearGlobalLog();
  Serial.println("Operation log cleared by user");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSaveSettings() {
  bool limitsOk = true;
  if (server.hasArg("limHigh") && server.hasArg("limLow")) {
    float newHigh = server.arg("limHigh").toFloat();
    float newLow  = server.arg("limLow").toFloat();
    // sanity check: keep values within a sane range (capped at the fixed
    // MAXIMUM_TEMP safety ceiling), and require at least 1.0C of separation
    // between the two limits (too close together and the control loop would
    // oscillate every check cycle)
    if ((newHigh - newLow) > 1.0 &&
        newHigh >= 30 && newHigh <= MAXIMUM_TEMP &&
        newLow >= 30 && newLow <= MAXIMUM_TEMP) {
      defLimitHigh = newHigh;
      defLimitLow  = newLow;
    } else {
      limitsOk = false;
    }
  }
  if (server.hasArg("minTemp")) {
    float newMin = server.arg("minTemp").toFloat();
    if (newMin >= 30 && newMin <= MAXIMUM_TEMP) {
      minTemp = newMin;
    } else {
      limitsOk = false;
    }
  }
  if (server.hasArg("minTempSettleSec")) {
    int newSettleSec = server.arg("minTempSettleSec").toInt();
    if (newSettleSec >= 0 && newSettleSec <= 120) {
      minTempSettleSec = newSettleSec;
    } else {
      limitsOk = false;
    }
  }
  if (server.hasArg("refresh")) {
    int newRefresh = server.arg("refresh").toInt();
    for (int i = 0; i < REFRESH_OPTIONS_COUNT; i++) {
      if (REFRESH_OPTIONS[i] == newRefresh) {
        pageRefreshSec = newRefresh;
        break;
      }
    }
  }

  saveSettings();

  DynamicJsonDocument doc(256);
  doc["ok"] = limitsOk;
  doc["limHigh"] = defLimitHigh;
  doc["limLow"] = defLimitLow;
  doc["minTemp"] = minTemp;
  doc["minTempSettleSec"] = minTempSettleSec;
  doc["refresh"] = pageRefreshSec;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// Resets a device's tuning/runtime state as if it just came back online at
// a brand-new IP - used whenever a slot's IP is reassigned (manual edit,
// Device List save, or Autoscan add/replace), since as far as we know it's
// now a physically different unit. Ladder position is re-derived from the
// live BitAxe on the next successful poll (processDevice() sees
// currentIndex == -1). Does NOT touch name/ip/offsetMv/enabled/logs.
void resetDeviceRuntimeState(BitaxeDevice &dev) {
  dev.currentIndex = -1;
  dev.lastChangeMillis = 0;
  dev.wasBelowMinTemp = false;
  dev.settlingAfterMinTemp = false;
  dev.minTempRecoveredAtMillis = 0;
  dev.appliedOffsetMv = 0;
  dev.offsetApplyPending = false;
  dev.infoValid = false;
}

void handleEditDevice() {
  bool ok = false;
  String err;
  int idx = -1;

  if (!server.hasArg("idx")) {
    err = "Missing device index";
  } else {
    idx = server.arg("idx").toInt();
    if (idx < 0 || idx >= MAX_DEVICES) {
      err = "Invalid device index";
      idx = -1;
    }
  }

  if (idx >= 0) {
    String newName = server.hasArg("name") ? server.arg("name") : devices[idx].name;
    String newIp   = server.hasArg("ip")   ? server.arg("ip")   : devices[idx].ip;
    newName.trim();
    newIp.trim();
    if (newName.length() > 40) newName = newName.substring(0, 40);

    bool ipChanged   = server.hasArg("ip") && (newIp != devices[idx].ip);
    bool nameChanged = server.hasArg("name") && (newName != devices[idx].name);

    if (newName.length() == 0) {
      err = "Name cannot be empty";
    } else if (server.hasArg("ip") && !isValidIPv4(newIp)) {
      err = "Invalid IP address";
    } else if (server.hasArg("ip") && isDuplicateIP(newIp, idx)) {
      err = "That IP is already used by another device";
    } else {
      devices[idx].name = newName;
      devices[idx].ip = newIp;
      saveDeviceOverride(idx);

      if (ipChanged) {
        // Log the reassignment under this slot's history before resetting
        // its tuning state.
        LadderStep zero = {0, 0};
        float tempAtChange = devices[idx].infoValid ? devices[idx].info.temp : 0.0;
        addLogEntry(devices[idx], "IP CHANGED", zero, zero, tempAtChange);

        // Treat as a fresh reconnect: as far as we know this is now a
        // physically different unit at this address, so ladder position,
        // cooldown, and minimum-temp/settle state are all reset. The
        // boot-time auto-sync logic re-runs on the next successful poll of
        // the new IP (processDevice() sees currentIndex == -1).
        resetDeviceRuntimeState(devices[idx]);
      } else if (nameChanged) {
        LadderStep zero = {0, 0};
        float tempAtChange = devices[idx].infoValid ? devices[idx].info.temp : 0.0;
        addLogEntry(devices[idx], "NAME CHANGED", zero, zero, tempAtChange);
      }

      ok = true;
    }
  }

  DynamicJsonDocument doc(384);
  doc["ok"] = ok;
  if (ok) {
    doc["idx"] = idx;
    doc["name"] = devices[idx].name;
    doc["ip"] = devices[idx].ip;
  } else {
    doc["error"] = err;
  }
  String out;
  serializeJson(doc, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

// ============================================================================
//  DEVICE LIST / LADDER / NTP SETTINGS (hamburger menu panels)
// ============================================================================

// Saves the whole 16-row Device List in one shot (all-or-nothing: if ANY
// enabled row fails validation, nothing is written). Body is JSON:
// {"devices":[{"enabled":bool,"name":str,"ip":str,"offset":int}, ... x16]}
void handleDeviceListSave() {
  String body = server.hasArg("plain") ? server.arg("plain") : "";
  DynamicJsonDocument doc(3072);
  if (deserializeJson(doc, body) != DeserializationError::Ok || !doc.containsKey("devices")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Malformed request\"}");
    return;
  }
  JsonArray arr = doc["devices"].as<JsonArray>();
  if (arr.size() != MAX_DEVICES) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Expected exactly " + String(MAX_DEVICES) + " rows\"}");
    return;
  }

  // ---- Validate everything first (all-or-nothing save) ----
  String seenIps[MAX_DEVICES];
  int seenCount = 0;
  for (int i = 0; i < MAX_DEVICES; i++) {
    JsonObject row = arr[i];
    bool enabled = row["enabled"] | false;
    if (!enabled) continue;

    String name = String((const char*)(row["name"] | ""));
    String ip   = String((const char*)(row["ip"] | ""));
    int offset  = row["offset"] | 0;
    name.trim();

    if (name.length() == 0) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"Row " + String(i + 1) + ": name cannot be empty\"}");
      return;
    }
    if (!isValidIPv4(ip)) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"Row " + String(i + 1) + ": invalid IP address\"}");
      return;
    }
    if (offset < VOLTAGE_OFFSET_MIN_MV || offset > VOLTAGE_OFFSET_MAX_MV || !offsetWithinVcoreCeiling(offset)) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"Row " + String(i + 1) + ": offset out of range or exceeds max VCore\"}");
      return;
    }
    for (int j = 0; j < seenCount; j++) {
      if (seenIps[j] == ip) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"IP " + ip + " is used by more than one row\"}");
        return;
      }
    }
    seenIps[seenCount++] = ip;
  }

  // ---- All valid: apply ----
  for (int i = 0; i < MAX_DEVICES; i++) {
    JsonObject row = arr[i];
    bool enabled = row["enabled"] | false;
    String name = String((const char*)(row["name"] | devices[i].name.c_str()));
    String ip   = String((const char*)(row["ip"] | devices[i].ip.c_str()));
    int offset  = row["offset"] | devices[i].offsetMv;
    name.trim();
    if (name.length() > 40) name = name.substring(0, 40);
    if (!enabled) { name = devices[i].name; ip = devices[i].ip; }  // don't clobber a disabled slot's saved identity

    bool ipChanged      = enabled && (ip != devices[i].ip);
    bool offsetChanged  = enabled && (offset != devices[i].offsetMv);
    bool becameEnabled  = enabled && !devices[i].enabled;

    devices[i].enabled  = enabled;
    devices[i].name     = name;
    devices[i].ip       = ip;
    devices[i].offsetMv = offset;
    saveDeviceOverride(i);

    if (ipChanged || becameEnabled) {
      resetDeviceRuntimeState(devices[i]);
    } else if (offsetChanged) {
      applyOffsetToDevice(devices[i]);
    }
  }

  server.send(200, "application/json", "{\"ok\":true}");
}

// Saves the voltage ladder. Body is JSON: {"ladder":[[freq,mv], ...]}.
// Re-sorted ascending by frequency server-side too (defense in depth - the
// client already sorts before sending). Rejected outright if it would push
// ANY currently enabled device's effective voltage over MAX_VCORE_MV.
void handleLadderSave() {
  String body = server.hasArg("plain") ? server.arg("plain") : "";
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, body) != DeserializationError::Ok || !doc.containsKey("ladder")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Malformed request\"}");
    return;
  }
  JsonArray arr = doc["ladder"].as<JsonArray>();
  int n = arr.size();
  if (n < 1 || n > MAX_LADDER_SIZE) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Ladder must have 1-" + String(MAX_LADDER_SIZE) + " steps\"}");
    return;
  }

  LadderStep candidate[MAX_LADDER_SIZE];
  int candidateMaxMv = 0;
  for (int i = 0; i < n; i++) {
    JsonArray step = arr[i];
    int freq = step[0] | 0;
    int mv   = step[1] | 0;
    if (freq <= 0 || mv <= 0 || mv > MAX_VCORE_MV) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"Every step needs a frequency above 0 and a VCore between 1 and " + String(MAX_VCORE_MV) + "mV\"}");
      return;
    }
    candidate[i].freqMHz = freq;
    candidate[i].mv = mv;
    if (mv > candidateMaxMv) candidateMaxMv = mv;
  }
  // sort ascending by frequency
  for (int i = 1; i < n; i++) {
    LadderStep key = candidate[i];
    int j = i - 1;
    while (j >= 0 && candidate[j].freqMHz > key.freqMHz) {
      candidate[j + 1] = candidate[j];
      j--;
    }
    candidate[j + 1] = key;
  }

  // Reject if this ladder would exceed MAX_VCORE_MV for any enabled device's offset
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!devices[i].enabled) continue;
    if (candidateMaxMv + devices[i].offsetMv > MAX_VCORE_MV) {
      server.send(400, "application/json",
                   "{\"ok\":false,\"error\":\"Would exceed max VCore for device '" + devices[i].name + "' - lower its offset first\"}");
      return;
    }
  }

  for (int i = 0; i < n; i++) ladder[i] = candidate[i];
  LADDER_SIZE = n;
  computeLadderMaxMv();
  saveLadder();

  // Ladder contents/indices may have shifted - force every device to
  // re-derive its ladder position from what it's actually running, same as
  // a fresh boot/reconnect.
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (devices[i].enabled) devices[i].currentIndex = -1;
  }

  server.send(200, "application/json", "{\"ok\":true}");
}

// Saves the NTP server list and re-issues configTzTime() immediately so the
// new server(s) take effect without a reboot. Secondary host is optional -
// if blank, the primary is used for both slots (matches the "if the last
// one in list doesn't answer we go back to the first" behavior with only
// one real host configured).
void handleNtpSave() {
  if (!server.hasArg("ntp1")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing primary NTP server\"}");
    return;
  }
  String ntp1 = server.arg("ntp1");
  String ntp2 = server.hasArg("ntp2") ? server.arg("ntp2") : "";
  ntp1.trim();
  ntp2.trim();
  if (ntp1.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Primary NTP server cannot be empty\"}");
    return;
  }
  if (ntp1.length() > 64 || ntp2.length() > 64) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Server hostname too long\"}");
    return;
  }

  ntpServer1 = ntp1;
  ntpServer2 = ntp2;
  saveNtpSettings();
  configTzTime(TIMEZONE_TZ, ntpServer1.c_str(), ntpServer2.length() > 0 ? ntpServer2.c_str() : ntpServer1.c_str());

  server.send(200, "application/json", "{\"ok\":true}");
}

// ============================================================================
//  AUTOSCAN (non-blocking /24 LAN scan for BitAxe units)
// ============================================================================
// Advances by at most one candidate host per loop() iteration (see
// scanStep(), called from loop()), so the web server, watchdog, and normal
// device polling all stay responsive for the ~1 minute a full /24 scan can
// take. The browser polls /autoscan/status for live progress and results.

#define AUTOSCAN_MAX_RESULTS 24

struct AutoscanFound {
  String ip;
  String hostname;
};

struct AutoscanState {
  bool running = false;
  int hostIndex = 1;              // next host octet to try (1..254)
  IPAddress baseIp;                // this LAN's /24 base, e.g. 192.168.1.0
  AutoscanFound results[AUTOSCAN_MAX_RESULTS];
  int resultCount = 0;
};

AutoscanState autoscan;

// Called once per loop() iteration. No-op unless a scan is running. Each
// call performs AT MOST one short (200ms-capped) HTTP probe, so a single
// call never blocks the rest of loop() for long.
void scanStep() {
  if (!autoscan.running) return;
  if (autoscan.hostIndex > 254) {
    autoscan.running = false;
    return;
  }

  int hostOctet = autoscan.hostIndex++;
  IPAddress candidate = autoscan.baseIp;
  candidate[3] = hostOctet;
  String candidateIp = candidate.toString();

  if (candidate == WiFi.localIP()) return;  // skip the ESP32 itself

  for (int i = 0; i < MAX_DEVICES; i++) {
    if (devices[i].ip == candidateIp) return;  // already known, skip
  }

  HTTPClient http;
  String url = "http://" + candidateIp + "/api/system/info";
  http.begin(url);
  http.setConnectTimeout(200);
  http.setTimeout(200);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, payload) == DeserializationError::Ok &&
        (doc.containsKey("ASICModel") || doc.containsKey("hostname"))) {
      if (autoscan.resultCount < AUTOSCAN_MAX_RESULTS) {
        AutoscanFound &f = autoscan.results[autoscan.resultCount];
        f.ip = candidateIp;
        const char *hn = doc["hostname"] | (const char*)(doc["ASICModel"] | "BitAxe");
        f.hostname = String(hn);
        autoscan.resultCount++;
      }
    }
  }
  http.end();

  if (autoscan.hostIndex > 254) autoscan.running = false;
}

void handleAutoscanStart() {
  IPAddress local = WiFi.localIP();
  autoscan.baseIp = IPAddress(local[0], local[1], local[2], 0);
  autoscan.hostIndex = 1;
  autoscan.resultCount = 0;
  autoscan.running = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleAutoscanStop() {
  autoscan.running = false;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleAutoscanStatus() {
  DynamicJsonDocument doc(2048);
  doc["running"] = autoscan.running;
  doc["done"] = !autoscan.running;
  int progress = ((autoscan.hostIndex - 1) * 100) / 254;
  if (progress > 100) progress = 100;
  doc["progress"] = progress;
  IPAddress cur = autoscan.baseIp;
  cur[3] = min(autoscan.hostIndex, 254);
  doc["currentIp"] = cur.toString();
  JsonArray results = doc.createNestedArray("results");
  for (int i = 0; i < autoscan.resultCount; i++) {
    JsonObject o = results.createNestedObject();
    o["ip"] = autoscan.results[i].ip;
    o["hostname"] = autoscan.results[i].hostname;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// Applies a found device to the Device List. Body is JSON:
// {"action":"add"|"replace","ip":str,"hostname":str,"idx":int (replace only)}
void handleAutoscanApply() {
  String body = server.hasArg("plain") ? server.arg("plain") : "";
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Malformed request\"}");
    return;
  }
  String action = String((const char*)(doc["action"] | ""));
  String ip     = String((const char*)(doc["ip"] | ""));
  String hostname = String((const char*)(doc["hostname"] | ""));
  ip.trim();
  hostname.trim();

  if (!isValidIPv4(ip)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid IP address\"}");
    return;
  }

  if (action == "add") {
    int slot = -1;
    for (int i = 0; i < MAX_DEVICES; i++) {
      if (!devices[i].enabled) { slot = i; break; }
    }
    if (slot == -1) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"Device list is full\"}");
      return;
    }
    if (isDuplicateIP(ip, slot)) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"That IP is already used by another device\"}");
      return;
    }
    String name = hostname.length() > 0 ? hostname : ("BitAxe " + String(slot + 1));
    if (name.length() > 40) name = name.substring(0, 40);

    devices[slot].enabled = true;
    devices[slot].name = name;
    devices[slot].ip = ip;
    devices[slot].offsetMv = 0;
    saveDeviceOverride(slot);
    resetDeviceRuntimeState(devices[slot]);

    DynamicJsonDocument out(256);
    out["ok"] = true;
    out["idx"] = slot;
    String o; serializeJson(out, o);
    server.send(200, "application/json", o);
    return;
  }

  if (action == "replace") {
    if (!doc.containsKey("idx")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing target device index\"}");
      return;
    }
    int idx = doc["idx"] | -1;
    if (idx < 0 || idx >= MAX_DEVICES) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid device index\"}");
      return;
    }
    if (isDuplicateIP(ip, idx)) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"That IP is already used by another device\"}");
      return;
    }

    LadderStep zero = {0, 0};
    float tempAtChange = devices[idx].infoValid ? devices[idx].info.temp : 0.0;
    addLogEntry(devices[idx], "IP CHANGED", zero, zero, tempAtChange);

    devices[idx].enabled = true;
    devices[idx].ip = ip;
    saveDeviceOverride(idx);
    resetDeviceRuntimeState(devices[idx]);

    DynamicJsonDocument out(256);
    out["ok"] = true;
    out["idx"] = idx;
    String o; serializeJson(out, o);
    server.send(200, "application/json", o);
    return;
  }

  server.send(400, "application/json", "{\"ok\":false,\"error\":\"Unknown action\"}");
}

// ============================================================================
//  SETUP / LOOP
// ============================================================================

// Heartbeat LED: flashes briefly every check cycle (5s) so you can tell at a
// glance the sketch is still alive and the main loop isn't stuck on anything.
// Your board (generic ESP32-WROOM-32 DevKit / "DOIT DevKit V1" clone) has no
// user-controllable onboard LED - the LED near the USB port is hardwired to
// 3.3V and can't be driven by code. Wire an external LED instead:
//   GPIO4 --[ ~220-330 ohm resistor ]--> LED anode (+, longer leg)
//   LED cathode (-, shorter leg) --> GND
// GPIO4 is safe on this board (not used for flash/boot/UART). If you later
// use a different ESP32 board that DOES define LED_BUILTIN, that's used
// automatically instead.
#ifdef LED_BUILTIN
const int LED_PIN = LED_BUILTIN;
#else
const int LED_PIN = 4;
#endif
const unsigned long LED_BLINK_MS = 150;  // how long the flash stays on
unsigned long ledOffAtMillis = 0;
bool ledOn = false;

void heartbeatBlink() {
  digitalWrite(LED_PIN, HIGH);
  ledOn = true;
  ledOffAtMillis = millis() + LED_BLINK_MS;
}

// Watchdog: if the main loop ever truly freezes (e.g. a stuck TCP call after
// a network outage), the ESP32 automatically reboots instead of needing a
// manual power-cycle. Settings + per-device logs are persisted in NVS and
// the ladder position re-syncs from the live BitAxe on boot, so an automatic
// reboot recovers cleanly with no data loss.
const int WDT_TIMEOUT_SEC = 20;

// Rate-limit how often we retry WiFi.reconnect() while disconnected, instead
// of hammering it every single loop iteration (which can itself destabilize
// the WiFi driver).
const unsigned long WIFI_RECONNECT_INTERVAL_MS = 5000;
unsigned long lastReconnectAttemptMillis = 0;

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nBitAxe Auto-Tuner starting...");

  // ---- watchdog: start it as early as possible so setup() is protected too ----
  // Core 3.x's TWDT API takes a config struct; the Arduino framework may have
  // already initialized it (common on core 3.x), so fall back to reconfigure
  // in that case instead of failing.
  esp_task_wdt_config_t twdtConfig = {
    .timeout_ms = (uint32_t)WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = 0,     // we only need our own loop task watched
    .trigger_panic = true    // reboot the chip on timeout
  };
  esp_err_t wdtErr = esp_task_wdt_init(&twdtConfig);
  if (wdtErr == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&twdtConfig);
  }
  esp_task_wdt_add(NULL);  // watch the current (setup/loop) task

  // ---- heartbeat LED ----
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // ---- load the voltage ladder (from NVS, or seed the built-in defaults
  // on first boot), then precompute its highest raw mV step, used to
  // validate per-device voltage offsets against MAX_VCORE_MV ----
  loadLadder();
  computeLadderMaxMv();

  // ---- load persisted settings (limits, refresh interval, NTP servers) ----
  loadSettings();

  // ---- init device runtime state from config ----
  for (int i = 0; i < MAX_DEVICES; i++) {
    devices[i].enabled = DEVICE_CONFIG[i].enabled;
    devices[i].name = DEVICE_CONFIG[i].name;
    devices[i].ip = DEVICE_CONFIG[i].ip;
  }

  // ---- apply any saved name/IP overrides (from the pencil-icon edit UI) on
  // top of the DEVICE_CONFIG seed values above ----
  loadDeviceOverrides();

  // ---- load the single combined operation log (shared across all devices) ----
  loadGlobalLogFromNVS();

  // ---- WiFi ----
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to WiFi '%s'", WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    delay(500);
    Serial.print(".");
    esp_task_wdt_reset();  // feed the watchdog while we wait for WiFi
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connection FAILED - will keep retrying in the background.");
  }

  // ---- NTP time (for log timestamps) ----
  configTzTime(TIMEZONE_TZ, ntpServer1.c_str(), ntpServer2.length() > 0 ? ntpServer2.c_str() : ntpServer1.c_str());
  struct tm ti;
  if (getLocalTime(&ti, 8000)) {
    timeSynced = true;
    Serial.println("Time synced via NTP.");
  } else {
    Serial.println("NTP sync failed (will keep trying); log timestamps will show uptime until then.");
  }

  // ---- Web server ----
  server.on("/", handleRoot);
  server.on("/api/status", handleApiStatus);
  server.on("/savesettings", HTTP_POST, handleSaveSettings);
  server.on("/clearlog", HTTP_POST, handleClearLog);
  server.on("/editdevice", HTTP_POST, handleEditDevice);
  server.on("/devicelist/save", HTTP_POST, handleDeviceListSave);
  server.on("/ladder/save", HTTP_POST, handleLadderSave);
  server.on("/ntpsettings/save", HTTP_POST, handleNtpSave);
  server.on("/autoscan/start", HTTP_POST, handleAutoscanStart);
  server.on("/autoscan/stop", HTTP_POST, handleAutoscanStop);
  server.on("/autoscan/status", HTTP_GET, handleAutoscanStatus);
  server.on("/autoscan/apply", HTTP_POST, handleAutoscanApply);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Web dashboard ready.");

  lastCheckMillis = millis() - CHECK_INTERVAL_MS; // trigger an immediate first check
}

void loop() {
  esp_task_wdt_reset();  // feed the watchdog every iteration

  server.handleClient();

  // Non-blocking heartbeat LED turn-off
  if (ledOn && millis() >= ledOffAtMillis) {
    digitalWrite(LED_PIN, LOW);
    ledOn = false;
  }

  // Keep WiFi alive, but don't hammer reconnect() every single iteration
  if (WiFi.status() != WL_CONNECTED &&
      millis() - lastReconnectAttemptMillis >= WIFI_RECONNECT_INTERVAL_MS) {
    lastReconnectAttemptMillis = millis();
    WiFi.reconnect();
  }

  // Opportunistically pick up NTP once WiFi is up, if we haven't yet
  if (!timeSynced && WiFi.status() == WL_CONNECTED) {
    struct tm ti;
    if (getLocalTime(&ti, 10)) timeSynced = true;
  }

  // Advance the LAN autoscan (if running) by at most one host this
  // iteration - see scanStep() for why this is safe to call unconditionally.
  if (WiFi.status() == WL_CONNECTED) scanStep();

  if (millis() - lastCheckMillis >= CHECK_INTERVAL_MS) {
    lastCheckMillis = millis();
    heartbeatBlink();  // flash the LED every time this cycle actually runs
    if (WiFi.status() == WL_CONNECTED) {
      for (int i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].enabled) {
          processDevice(devices[i]);
          esp_task_wdt_reset();  // feed again after each device (matters more once several are enabled)
        }
      }
    }
  }
}
