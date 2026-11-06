// SensorNode — bench instrumentation for the gated pulse rig (Pico 2 W)
//
// A SECOND board, deliberately. It could all run on the generator Pico, but
// keeping them apart buys three things: a sensor fault cannot take the
// waveform down, the noisy analog front end stays physically away from the
// pulse timing, and the MCP server can poll it as an independent instrument
// exactly the way it already polls the generator and the scope. One of them
// being down does not blind the others.
//
// It speaks the same dialect as the generator on purpose -- GET /state for
// JSON, GET /cmd?c=<command> for anything the serial console takes, `?` for
// help -- so host tooling written for one works for the other.
//
// WHAT THIS DOES NOT DO YET, on purpose: there is no ADS1115 driver and no
// calibration store. Neither can be tested without the hardware in front of
// it, and untested driver code that silently returns plausible numbers is
// worse than none on a rig whose whole point is measurement. The I2C scan
// below will show the chip the moment it is wired, and the driver goes in
// then.
//
// Pin map (see also HARDWARE.md for the generator's own):
//   GP0  pin 1   I2C0 SDA  -> ADS1115, pressure module
//   GP1  pin 2   I2C0 SCL
//   GP2  pin 4   SYNC IN   <- generator GP22 (pin 29), the cycle marker
//   GP3  pin 5   peak-hold reset (drives 2N7000 gates) -- reserved, unused yet
//   GP15 pin 20  status LED
//   GP26 pin 31  ADC0
//   GP27 pin 32  ADC1
//   GP28 pin 34  ADC2

#include <Wire.h>

// Forward-declared for arduino-cli's auto-generated prototypes, which are
// inserted above the first function definition -- so any type named in a
// signature must be declared before that point.
struct SyncView;

#if __has_include("secrets.h")
  #include "secrets.h"
#endif
#ifndef WIFI_SSID
  #define WIFI_SSID ""
  #define WIFI_PASS ""
#endif
#ifndef MDNS_NAME
  #define MDNS_NAME "gatedsensor"
#endif

#include <WiFi.h>
#include <WebServer.h>
#include <LEAmDNS.h>

static const uint PIN_SDA        = 0;
static const uint PIN_SCL        = 1;
static const uint PIN_SYNC       = 2;    // cycle marker in, from the generator
static const uint PIN_HOLD_RESET = 3;    // reserved for the peak-hold front end
static const uint PIN_STATUS_LED = 15;   // NOT PIN_LED: the rpipico2w
                                         // variant already defines that as 64

static const uint ADC_PINS[]  = { 26, 27, 28 };
static const uint ADC_BOARD[] = { 31, 32, 34 };

// ---------------------------------------------------------------------------
// Sync input
// ---------------------------------------------------------------------------
//
// The generator's GP22 goes high for burst 1 of its repeating pattern and low
// for the rest, so its period IS the pattern period and its duty is 1/ring_len.
// Measuring it here does two useful things before any analog exists: it proves
// the inter-board link is real, and it gives every future sample an unambiguous
// position in the pattern rather than "some burst, probably".
//
// Timestamps come from an edge interrupt rather than polling. The pattern
// period is milliseconds, so micros() in an ISR is far more resolution than
// needed -- but polling from loop() would alias against the WiFi stack's
// servicing and produce a period that wanders for no physical reason.

static volatile uint32_t syncLastRise = 0;
static volatile uint32_t syncPeriodUs = 0;
static volatile uint32_t syncHighUs   = 0;
static volatile uint32_t syncEdges    = 0;
static volatile uint32_t syncLastEdge = 0;

static void syncIsr() {
  uint32_t now = micros();
  if (gpio_get(PIN_SYNC)) {
    if (syncLastRise) syncPeriodUs = now - syncLastRise;
    syncLastRise = now;
  } else if (syncLastRise) {
    syncHighUs = now - syncLastRise;
  }
  syncLastEdge = now;
  syncEdges++;
}

struct SyncView { uint32_t period, high, edges; bool live; };

// Copy the ISR's state atomically. Reading the fields one at a time invites a
// period from one pattern paired with a high-time from the next, which reads as
// a duty glitch that is not in the signal.
static SyncView syncRead() {
  noInterrupts();
  SyncView v { syncPeriodUs, syncHighUs, syncEdges, false };
  uint32_t last = syncLastEdge;
  interrupts();
  // Stale means unplugged. Without this a stopped generator leaves the last
  // good numbers sitting there looking perfectly healthy.
  v.live = v.edges > 0 && (uint32_t)(micros() - last) < 2000000UL;
  return v;
}

// ---------------------------------------------------------------------------
// I2C
// ---------------------------------------------------------------------------

struct KnownDev { uint8_t addr; const char *name; };
static const KnownDev KNOWN[] = {
  { 0x28, "pressure (Honeywell-style)" }, { 0x29, "pressure (Honeywell-style)" },
  { 0x40, "INA219 / HTU21" },
  { 0x48, "ADS1115 ch0" }, { 0x49, "ADS1115 ch1" },
  { 0x4A, "ADS1115 ch2" }, { 0x4B, "ADS1115 ch3" },
  { 0x60, "MCP4725 DAC" },  { 0x61, "MCP4725 DAC" },
  { 0x76, "BMP/BME280" },   { 0x77, "BMP/BME280" },
};
static const char *knownName(uint8_t a) {
  for (auto &k : KNOWN) if (k.addr == a) return k.name;
  return "unknown";
}

static uint8_t i2cFound[16];
static uint8_t i2cCount = 0;

static void i2cScan() {
  i2cCount = 0;
  for (uint8_t a = 0x08; a <= 0x77 && i2cCount < sizeof(i2cFound); a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) i2cFound[i2cCount++] = a;
  }
}

// ---------------------------------------------------------------------------
// ADC
// ---------------------------------------------------------------------------
//
// Oversampled because the RP2350's SAR is mediocre -- roughly 9-10 effective
// bits against its nominal 12 -- and a single conversion on a floating input is
// mostly noise. This is a stopgap: the real path is an external 16-bit
// converter behind an analog peak-hold, which is why the numbers below are
// reported in millivolts and NOT dressed up as amps.

static const uint32_t ADC_OVERSAMPLE = 64;
static const float    ADC_VREF_MV    = 3300.0f;

static float adcMillivolts(uint pin) {
  uint32_t acc = 0;
  for (uint32_t i = 0; i < ADC_OVERSAMPLE; i++) acc += analogRead(pin);
  return (acc / (float)ADC_OVERSAMPLE) * ADC_VREF_MV / 4095.0f;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

struct StringPrint : public Print {
  String s;
  size_t write(uint8_t c) override { s += (char)c; return 1; }
  size_t write(const uint8_t *b, size_t n) override {
    for (size_t i = 0; i < n; i++) s += (char)b[i];
    return n;
  }
};

static WebServer server(80);
static bool serverUp = false;

// Matches the generator's cadence. Long enough not to thrash a busy AP, short
// enough that a router reboot costs seconds rather than a bench trip.
static const uint32_t WIFI_RETRY_MS = 15000;
static uint32_t wifiLastTry = 0;

static const char *wifiStatusName(int s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "idle";
    case WL_NO_SSID_AVAIL:   return "AP NOT FOUND";       // wrong SSID, or out of range
    case WL_SCAN_COMPLETED:  return "scanned";
    case WL_CONNECTED:       return "connected";
    case WL_CONNECT_FAILED:  return "AUTH FAILED";        // wrong password
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED:    return "disconnected";
    default:                 return "unknown";
  }
}

static void printJson(Print &o) {
  SyncView sv = syncRead();
  o.print("{\"ok\":1,\"node\":\"sensor\"");
  o.print(",\"sync_live\":");   o.print(sv.live ? 1 : 0);
  o.print(",\"sync_edges\":");  o.print(sv.edges);
  if (sv.live && sv.period) {
    o.print(",\"pattern_period_us\":"); o.print(sv.period);
    o.print(",\"pattern_hz\":");        o.print(1000000.0 / sv.period, 3);
    o.print(",\"marker_high_us\":");    o.print(sv.high);
    o.print(",\"marker_duty_pct\":");   o.print(100.0 * sv.high / sv.period, 2);
    // duty is 1/ring_len, so this recovers the generator's ring length without
    // having to ask it -- a useful cross-check that the two boards agree.
    o.print(",\"implied_ring_len\":");  o.print((int)(sv.period / (double)sv.high + 0.5));
  }
  o.print(",\"adc_mv\":[");
  for (uint i = 0; i < count_of(ADC_PINS); i++) {
    if (i) o.print(",");
    o.print(adcMillivolts(ADC_PINS[i]), 1);
  }
  o.print("],\"chip_c\":"); o.print(analogReadTemp(), 2);
  o.print(",\"i2c\":[");
  for (uint8_t i = 0; i < i2cCount; i++) {
    if (i) o.print(",");
    o.print("{\"addr\":\"0x"); if (i2cFound[i] < 16) o.print("0");
    o.print(i2cFound[i], HEX);
    o.print("\",\"guess\":\""); o.print(knownName(i2cFound[i])); o.print("\"}");
  }
  o.print("],\"i2c_count\":"); o.print(i2cCount);
  o.print(",\"uptime_s\":");   o.print(millis() / 1000);
  o.print(",\"wifi\":\"");
  if (WiFi.status() == WL_CONNECTED) o.print(WiFi.localIP()); else o.print("offline");
  o.print("\",\"wifi_status\":\""); o.print(wifiStatusName(WiFi.status()));
  o.print("\",\"mac\":\"");         o.print(WiFi.macAddress());
  o.println("\"}");
}

static void printState(Print &o) {
  SyncView sv = syncRead();
  o.println("=== SENSOR NODE ===");
  o.print("sync GP2   : ");
  if (!sv.live) {
    o.println("NO SIGNAL -- wire generator GP22 (pin 29) to this board's GP2 (pin 4),");
    o.println("             and a ground between the two boards");
  } else {
    o.print("live, pattern "); o.print(sv.period / 1000.0, 3);
    o.print(" ms ("); o.print(1000000.0 / sv.period, 2); o.println(" Hz)");
    o.print("             marker high "); o.print(sv.high / 1000.0, 3);
    o.print(" ms = "); o.print(100.0 * sv.high / sv.period, 1);
    o.print("%  -> ring length ");
    o.println((int)(sv.period / (double)sv.high + 0.5));
  }
  o.print("edges seen : "); o.println(sv.edges);
  o.println("adc        : (floating inputs read noise -- this is expected)");
  for (uint i = 0; i < count_of(ADC_PINS); i++) {
    o.print("  GP"); o.print(ADC_PINS[i]);
    o.print(" pin "); o.print(ADC_BOARD[i]);
    o.print("  "); o.print(adcMillivolts(ADC_PINS[i]), 1); o.println(" mV");
  }
  o.print("chip temp  : "); o.print(analogReadTemp(), 1); o.println(" C");
  o.print("i2c GP0/1  : ");
  if (!i2cCount) {
    o.println("nothing found (SCAN to re-check after wiring)");
  } else {
    o.println();
    for (uint8_t i = 0; i < i2cCount; i++) {
      o.print("  0x"); if (i2cFound[i] < 16) o.print("0");
      o.print(i2cFound[i], HEX); o.print("  "); o.println(knownName(i2cFound[i]));
    }
  }
  o.print("wifi       : ");
  if (WiFi.status() == WL_CONNECTED) {
    o.print(WiFi.SSID()); o.print("  http://"); o.print(WiFi.localIP());
    o.print("/  ("); o.print(MDNS_NAME); o.println(".local)");
  } else if (!strlen(WIFI_SSID)) {
    o.println("disabled (no secrets.h)");
  } else {
    // "connecting..." on its own is useless when it never succeeds: it cannot
    // tell "cannot see the AP" from "wrong password" from "associated but no
    // DHCP", and those want three different fixes. Print what the driver
    // actually says, plus the MAC, which is the number you need to look the
    // board up in a router's client or reservation list.
    o.print(wifiStatusName(WiFi.status()));
    o.print(" ("); o.print(WiFi.status()); o.print(")  ssid ");
    o.print(WIFI_SSID);
    o.print("  mac "); o.print(WiFi.macAddress());
    o.print("  retry in ");
    o.print((int32_t)(WIFI_RETRY_MS - (millis() - wifiLastTry)) / 1000);
    o.println("s");
  }
}

static void printHelp(Print &o) {
  o.println("=== SENSOR NODE COMMANDS ===");
  o.println("  P        state");
  o.println("  J        state as one line of JSON");
  o.println("  SCAN     re-scan the I2C bus (after wiring something)");
  o.println("  SYNC     reset the sync statistics");
  o.println("  ?        this help");
  o.println("HTTP: GET /state, GET /cmd?c=<command>");
}

static void runCommand(char *s, Print &o) {
  while (*s == ' ') s++;
  for (char *p = s; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
  size_t n = strlen(s);
  while (n && s[n - 1] == ' ') s[--n] = 0;
  if (!n) return;

  if (!strcmp(s, "P"))    { printState(o); return; }
  if (!strcmp(s, "J"))    { printJson(o);  return; }
  if (!strcmp(s, "?"))    { printHelp(o);  return; }
  if (!strcmp(s, "SCAN")) {
    i2cScan();
    o.print("i2c scan: "); o.print(i2cCount); o.println(" device(s)");
    printState(o); return;
  }
  if (!strcmp(s, "SYNC")) {
    noInterrupts();
    syncEdges = 0; syncPeriodUs = 0; syncHighUs = 0; syncLastRise = 0;
    interrupts();
    o.println("sync statistics cleared"); return;
  }
  o.print("Unknown command: "); o.println(s);
  o.println("Type ? for help");
}

// ---------------------------------------------------------------------------

static char     cmdBuf[128];
static uint16_t cmdLen = 0;

static void pollSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdLen) { cmdBuf[cmdLen] = 0; runCommand(cmdBuf, Serial); }
      cmdLen = 0;
    } else if (cmdLen < sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = c;
    }
  }
}

// The generator serves the UI; this board serves the data. Different origins,
// so without this header the browser silently refuses the fetch. Allowing "*"
// is right here and nowhere else: this is an unauthenticated instrument on a
// trusted LAN, and it is already readable by anything that can reach it.
static void cors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
}

static void handleCmd() {
  cors();
  String q = server.arg("c");
  if (!q.length()) { server.send(400, "text/plain", "missing ?c="); return; }
  char buf[128];
  q.toCharArray(buf, sizeof(buf));
  StringPrint sp;
  runCommand(buf, sp);
  server.send(200, "text/plain", sp.s);
}

static void handleState() {
  cors();
  StringPrint sp;
  printJson(sp);
  server.send(200, "application/json", sp.s);
}

static void netPoll() {
  static bool wasUp = false;
  if (!strlen(WIFI_SSID)) return;
  bool up = (WiFi.status() == WL_CONNECTED);

  if (up && !wasUp) {
    Serial.print("wifi: "); Serial.print(WiFi.SSID());
    Serial.print("  http://"); Serial.print(WiFi.localIP()); Serial.println("/");
    if (!serverUp) {
      server.on("/state", handleState);
      server.on("/cmd",   handleCmd);
      server.on("/", []{ server.send(200, "text/plain",
                          "sensor node -- try /state or /cmd?c=P\n"); });
      server.onNotFound([]{ server.send(404, "text/plain", "no"); });
      server.begin();
      serverUp = true;
    }
    // Outside the serverUp guard on purpose. On a reconnect the handlers are
    // already registered but the mDNS responder is not -- leave this inside and
    // the board comes back reachable by IP while gatedsensor.local stays dead,
    // which is a confusing half-recovery to debug.
    MDNS.begin(MDNS_NAME);
    MDNS.addService("http", "tcp", 80);
    Serial.print("mdns: http://"); Serial.print(MDNS_NAME); Serial.println(".local/");
  }
  if (!up && wasUp) Serial.println("wifi: link lost");
  wasUp = up;

  if (up) {
    if (serverUp) server.handleClient();
    MDNS.update();
    return;
  }

  // Retry. Without this the board is stranded by any single failed attempt --
  // an AP that was rebooting when we came up, a DHCP server not yet answering
  // -- and the only recovery is someone noticing and power-cycling it. The
  // generator has always retried; this board not doing so is why it sat at
  // "connecting..." while the generator walked back onto the same AP.
  if (millis() - wifiLastTry > WIFI_RETRY_MS) {
    wifiLastTry = millis();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_STATUS_LED, OUTPUT);
  pinMode(PIN_HOLD_RESET, OUTPUT);
  digitalWrite(PIN_HOLD_RESET, LOW);

  // Pull-DOWN here, unlike the generator's inputs. The sync line is driven
  // push-pull from the other board, so there is no contact to pull up for --
  // and a pull-down means an unplugged wire reads a clean, unambiguous "no
  // signal" instead of floating high and inventing edges.
  pinMode(PIN_SYNC, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(PIN_SYNC), syncIsr, CHANGE);

  analogReadResolution(12);

  Wire.setSDA(PIN_SDA);
  Wire.setSCL(PIN_SCL);
  Wire.begin();
  i2cScan();

  if (strlen(WIFI_SSID)) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(MDNS_NAME);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    wifiLastTry = millis();          // first retry one full interval from here
  }

  Serial.println();
  printState(Serial);
  Serial.println("type ? for commands");
}

void loop() {
  pollSerial();
  netPoll();

  // Heartbeat: solid-ish when sync is present, a slow blink when it is not.
  // Tells you which state the board is in from across the bench.
  SyncView sv = syncRead();
  uint32_t phase = millis() % (sv.live ? 400 : 1600);
  digitalWrite(PIN_STATUS_LED, phase < 60 ? HIGH : LOW);
}
