/*
 * Victron MultiPlus-II GX -> CrowPanel 1.28" Round Display  ("Reactor HUD")
 * Hardware: Elecrow CrowPanel 1.28inch HMI ESP32 Rotary Display
 *           (ESP32-S3, 8MB PSRAM / 16MB flash, GC9A01 240x240 round IPS,
 *            CST816D touch, rotary crown + push button, 5x RGB LEDs)
 *
 * Reads live values from a Victron Venus OS / GX over MQTT and shows them on
 * the round panel as an animated, anti-aliased sci-fi HUD. No HomeKit, no cloud.
 *
 * Rendering: everything is drawn into a 2x (480x480) off-screen buffer and
 * downscaled to the 240x240 panel with pushRotateZoomWithAA -> smooth arcs,
 * circles and (vector) text. All coordinates below are LOGICAL 240-space,
 * wrapped in U() to scale into the 2x buffer.
 *
 * Interaction:
 *   Crown (rotate) .... switch page:  Reactor > Power > Battery > Clock
 *   Button (press) .... cycle LED mode: SOC-bar > off > status-glow
 *   Touch ............. tap / slide-up = next page, slide-down = previous
 *   RGB LEDs .......... ambient status (colour = charge state, breathing)
 *
 * Libraries: LovyanGFX, PubSubClient, ArduinoJson, "Adafruit NeoPixel",
 *            + local CST816D.h/.cpp (touch driver, from Elecrow's example).
 * Build/flash: see flash.sh (ESP32S3 Dev Module, hwcdc, 16MB, OPI PSRAM).
 */

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <time.h>
#include <Adafruit_NeoPixel.h>
#include "CST816D.h"

#include "arduino_secrets.h"

#ifndef SECRET_VEBUS_INSTANCE
#define SECRET_VEBUS_INSTANCE "275"
#endif
#ifndef SECRET_TZ
#define SECRET_TZ "EET-2EEST,M3.5.0/3,M10.5.0/4"   // Europe/Kyiv default
#endif

// ============================================================
//  Configuration
// ============================================================
const char* WIFI_SSID      = SECRET_WIFI_SSID;
const char* WIFI_PASSWORD  = SECRET_WIFI_PASSWORD;
const char* VICTRON_IP     = SECRET_VICTRON_IP;
const int   VICTRON_PORT   = 1883;
const char* VRM_ID         = SECRET_VRM_ID;
const char* VEBUS_INSTANCE = SECRET_VEBUS_INSTANCE;
const char* POSIX_TZ       = SECRET_TZ;

// ============================================================
//  Pins (verified against Elecrow's RotaryScreen_1_28_new example)
// ============================================================
#define PIN_SCLK        10
#define PIN_MOSI        11
#define PIN_DC           3
#define PIN_CS           9
#define PIN_RST         14
#define PIN_BACKLIGHT   46
#define PIN_PWR1         1
#define PIN_PWR2         2
#define PIN_PWR_LIGHT   40
#define ENC_A           45     // crown A / CLK
#define ENC_B           42     // crown B / DT
#define BTN             41     // crown push button (active LOW)
#define LED_PIN         48     // 5x WS2812
#define LED_N            5
#define TP_SDA           6
#define TP_SCL           7
#define TP_RST          13
#define TP_INT           5

// ============================================================
//  Display driver
// ============================================================
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel_instance;
  lgfx::Bus_SPI      _bus_instance;
public:
  LGFX(void) {
    { auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST; cfg.spi_mode = 0;
      cfg.freq_write = 80000000; cfg.freq_read = 20000000;
      cfg.spi_3wire = true; cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_SCLK; cfg.pin_mosi = PIN_MOSI;
      cfg.pin_miso = -1; cfg.pin_dc = PIN_DC;
      _bus_instance.config(cfg); _panel_instance.setBus(&_bus_instance); }
    { auto cfg = _panel_instance.config();
      cfg.pin_cs = PIN_CS; cfg.pin_rst = PIN_RST; cfg.pin_busy = -1;
      cfg.memory_width = 240; cfg.memory_height = 240;
      cfg.panel_width = 240; cfg.panel_height = 240;
      cfg.offset_x = 0; cfg.offset_y = 0; cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8; cfg.dummy_read_bits = 1;
      cfg.readable = false; cfg.invert = true;
      cfg.rgb_order = false; cfg.dlen_16bit = false; cfg.bus_shared = false;
      _panel_instance.config(cfg); }
    setPanel(&_panel_instance);
  }
};

LGFX              gfx;
LGFX_Sprite       spr(&gfx);
Adafruit_NeoPixel strip(LED_N, LED_PIN, NEO_GRB + NEO_KHZ800);
CST816D           touch(TP_SDA, TP_SCL, TP_RST, TP_INT);

// 2x supersample geometry. Draw in LOGICAL 240-space via U(); C = centre.
#define SS 2
#define U(v)  ((int)((v) * SS))
static const int W = 240 * SS;   // buffer size 480
static const int C = 120 * SS;   // centre 240

// ============================================================
//  Palette (RGB565)
// ============================================================
#define SF_BG        0x0000
#define SF_TEAL      0x0B6F
#define SF_CYAN      0x3F1F   // accent (#3FE3FF)
#define SF_CYAN_MID  0x2C7E
#define SF_CYAN_DIM  0x114B
#define SF_TRACK     0x10A2
#define SF_WHITE     0xE7BF   // ice
#define SF_AMBER     0xFD63
#define SF_RED       0xF9EB
#define SF_FLUX      0x7ADF   // grid import
#define SF_SLATE     0x5B96

// ============================================================
//  MQTT + live values
// ============================================================
String topic(const char* p)     { return String("N/") + VRM_ID + "/" + p; }
String readTopic(const char* p)  { return String("R/") + VRM_ID + "/" + p; }

float g_soc = 0, g_voltage = 0, g_battPower = 0, g_acOutP = 0, g_gridP = 0;
bool  g_dataReady = false;

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
unsigned long lastKeepalive = 0, lastReconnect = 0, lastFrame = 0, lastLed = 0;
const unsigned long KEEPALIVE_MS = 30000, RECONNECT_MS = 5000, FRAME_MS = 50;

enum Page { P_REACTOR, P_POWER, P_BATTERY, P_CLOCK, P_SETTINGS, PAGE_COUNT };
volatile int page = P_REACTOR;
int  ledMode = 0;                 // 0=off (default), 1=status, 2=SOC bar
int  backlightPct = 80;           // backlight brightness setting (10..100)
int  settingsSel = 0;             // highlighted settings row
bool settingsEdit = false;        // when true, crown changes the selected value
const int N_SETTINGS = 2;
int  encLastCLK;
bool btnDown = false, touchDown = false;
unsigned long btnTime = 0, touchTime = 0;

void applyBacklight() { ledcWrite(PIN_BACKLIGHT, backlightPct * 255 / 100); }

void mqttCallback(char* topicStr, byte* payload, unsigned int length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;
  float v = doc["value"] | 0.0f;
  String t = String(topicStr);
  if      (t.indexOf("Dc/Battery/Soc")     >= 0) g_soc       = v;
  else if (t.indexOf("Dc/Battery/Voltage") >= 0) g_voltage   = v;
  else if (t.indexOf("Dc/Battery/Power")   >= 0) g_battPower = v;
  else if (t.indexOf("Ac/ActiveIn/P")      >= 0) g_gridP     = v;
  else if (t.indexOf("Ac/Out/P")           >= 0) g_acOutP    = v;
  else return;
  g_dataReady = true;
}
void mqttReconnect() {
  if (mqtt.connected()) return;
  if (millis() - lastReconnect < RECONNECT_MS) return;
  lastReconnect = millis();
  if (mqtt.connect("VictronCircularDisplay")) {
    mqtt.subscribe(topic("system/0/Dc/Battery/Soc").c_str());
    mqtt.subscribe(topic("system/0/Dc/Battery/Voltage").c_str());
    mqtt.subscribe(topic("system/0/Dc/Battery/Power").c_str());
    mqtt.subscribe((topic("vebus/") + VEBUS_INSTANCE + "/Ac/Out/P").c_str());
    mqtt.subscribe((topic("vebus/") + VEBUS_INSTANCE + "/Ac/ActiveIn/P").c_str());
    mqtt.publish(readTopic("keepalive").c_str(), "");
    lastKeepalive = millis();
  }
}
void mqttKeepAlive() {
  if (millis() - lastKeepalive > KEEPALIVE_MS) {
    mqtt.publish(readTopic("keepalive").c_str(), "");
    lastKeepalive = millis();
  }
}

// ============================================================
//  Helpers
// ============================================================
static inline void polar(float r, float deg, int &x, int &y) {
  float a = deg * 0.01745329f;
  x = C + (int)lroundf(r * cosf(a));
  y = C + (int)lroundf(r * sinf(a));
}
uint16_t lerp565(uint16_t a, uint16_t b, float t) {
  int ra=(a>>11)&31, ga=(a>>5)&63, ba=a&31;
  int rb=(b>>11)&31, gb=(b>>5)&63, bb=b&31;
  return ((ra+(int)((rb-ra)*t))<<11) | ((ga+(int)((gb-ga)*t))<<5) | (ba+(int)((bb-ba)*t));
}
String fmtPower(float w) {
  float a = fabsf(w);
  if (a >= 1000.0f) { char b[16]; snprintf(b,sizeof(b),"%.2fkW",a/1000.0f); return String(b); }
  return String((int)roundf(a)) + "W";
}
uint16_t socColor(float s) { return s<20 ? SF_RED : s<50 ? SF_AMBER : SF_CYAN; }

enum { ST_LOW, ST_CHARGING, ST_DISCHARGING, ST_IDLE };
int chargeState() {
  if (g_soc < 20)       return ST_LOW;
  if (g_battPower < -5) return ST_CHARGING;
  if (g_battPower >  5) return ST_DISCHARGING;
  return ST_IDLE;
}
const char* chargeLabel(int s) {
  switch (s){ case ST_LOW:return "LOW BATTERY"; case ST_CHARGING:return "CHARGING";
              case ST_DISCHARGING:return "DISCHARGE"; default:return "STANDBY"; }
}
uint16_t chargeColor(int s) {
  switch (s){ case ST_LOW:return SF_RED; case ST_CHARGING:return SF_CYAN;
              case ST_DISCHARGING:return SF_AMBER; default:return SF_SLATE; }
}
bool timeNow(char* hm, char* hms, char* date) {
  struct tm ti;
  if (!getLocalTime(&ti, 8)) return false;
  if (hm)   strftime(hm,   6, "%H:%M", &ti);
  if (hms)  strftime(hms,  9, "%H:%M:%S", &ti);
  if (date) strftime(date, 16, "%a %d %b", &ti);
  return true;
}

// ============================================================
//  Drawing (logical 240-space via U())
// ============================================================
void txt(const lgfx::IFont* f, uint8_t size, uint16_t col, textdatum_t d = middle_center) {
  spr.setFont(f); spr.setTextSize(size); spr.setTextColor(col); spr.setTextDatum(d);
}

// SOC gauge: thin 270deg ring, gap at bottom, teal->accent gradient + head pip.
void drawGauge(float soc, uint16_t endCol) {
  spr.fillArc(C, C, U(110), U(103), 135, 405, SF_TRACK);
  float aV = 135 + 270 * constrain(soc, 0.0f, 100.0f) / 100.0f;
  for (float a = 135; a < aV; a += 5) {
    float u  = (a - 135) / ((aV - 135) < 1 ? 1 : (aV - 135));
    float a2 = (a + 5.5f < aV) ? a + 5.5f : aV;
    spr.fillArc(C, C, U(110), U(103), a, a2, lerp565(SF_TEAL, endCol, u));
  }
  int hx, hy; polar(U(106.5), aV, hx, hy);
  spr.fillSmoothCircle(hx, hy, U(4), SF_WHITE);
}

// Orbiting flow pips (only when power flows): ccw = charge, cw = discharge.
void drawFlowRing(float t) {
  float p = fabsf(g_battPower);
  if (p < 10) return;
  float dir = g_battPower < 0 ? -1 : 1;
  float sp  = 0.15f + (p > 2500 ? 1.0f : p / 2500.0f) * 1.1f;
  uint16_t col = g_battPower < 0 ? SF_CYAN : SF_AMBER;
  float base = t * sp * 90 * dir;
  for (int i = 0; i < 12; i++) {
    int x, y; polar(U(92), base + i * 30.0f, x, y);
    spr.fillSmoothCircle(x, y, U(2), col);
  }
}

void drawBezel(bool connected) {
  for (int d = 0; d < 360; d += 30) {
    int x0,y0,x1,y1; polar(U(111), d, x0, y0); polar(U(118), d, x1, y1);
    spr.drawWideLine(x0, y0, x1, y1, SS, SF_CYAN_DIM);
  }
  spr.fillTriangle(C-U(5), U(6), C+U(5), U(6), C, U(15), connected ? SF_CYAN : SF_RED);
}

void drawPageDots() {
  for (int i = 0; i < PAGE_COUNT; i++) {
    int x = C + (int)((i - (PAGE_COUNT-1)/2.0f) * U(12));
    spr.fillSmoothCircle(x, U(206), i==page ? U(3) : U(2), i==page ? SF_CYAN : SF_CYAN_DIM);
  }
}

void topClock() {
  char hm[6];
  txt(&fonts::FreeSansBold9pt7b, SS, SF_CYAN_MID);
  spr.drawString(timeNow(hm,0,0) ? hm : "--:--", C, U(28));
}

// ---- pages ----
void pageReactor(float t) {
  float soc = constrain(g_soc, 0.0f, 100.0f);
  uint16_t sc = socColor(soc);
  int st = chargeState();

  drawGauge(soc, sc);
  drawFlowRing(t);

  char hm[6];
  txt(&fonts::FreeSansBold9pt7b, SS, SF_CYAN_MID);
  spr.drawString(timeNow(hm,0,0) ? hm : "--:--", C, U(48));

  char sb[8]; snprintf(sb, sizeof(sb), "%d", (int)roundf(soc));
  txt(&fonts::FreeSansBold24pt7b, 3, sc);
  spr.drawString(sb, C, U(112));
  txt(&fonts::FreeSansBold12pt7b, SS, sc, middle_left);
  spr.drawString("%", C + U(46), U(94));

  char vb[16]; snprintf(vb, sizeof(vb), "%.1f V", g_voltage);
  txt(&fonts::FreeSansBold12pt7b, SS, SF_WHITE);
  spr.drawString(vb, C, U(152));

  txt(&fonts::FreeSansBold9pt7b, SS, chargeColor(st));
  spr.drawString(chargeLabel(st), C, U(180));
}

void powerRow(int y, const char* label, const char* val, uint16_t col, int dir) {
  txt(&fonts::FreeSansBold9pt7b, SS, SF_SLATE, middle_left);
  spr.drawString(label, U(58), y);
  if (dir) {
    int ax = U(116);
    if (dir > 0) spr.fillTriangle(ax-U(4), y+U(3), ax+U(4), y+U(3), ax, y-U(4), col);
    else         spr.fillTriangle(ax-U(4), y-U(3), ax+U(4), y-U(3), ax, y+U(4), col);
  }
  txt(&fonts::FreeSansBold12pt7b, SS, col, middle_right);
  spr.drawString(val, U(184), y);
}
void pagePower(float t) {
  topClock();
  txt(&fonts::FreeSansBold9pt7b, SS, SF_CYAN);
  spr.drawString("POWER FLOW", C, U(52));
  powerRow(U(92),  "GRID", fmtPower(g_gridP).c_str(),
           g_gridP > 5 ? SF_FLUX : SF_SLATE, g_gridP > 5 ? -1 : 0);
  powerRow(U(126), "BATT", fmtPower(g_battPower).c_str(),
           g_battPower < -5 ? SF_CYAN : g_battPower > 5 ? SF_AMBER : SF_SLATE,
           g_battPower < -5 ? -1 : g_battPower > 5 ? 1 : 0);
  powerRow(U(160), "LOAD", fmtPower(g_acOutP).c_str(), SF_CYAN, g_acOutP > 5 ? 1 : 0);
}

void pageBattery(float t) {
  topClock();
  float soc = constrain(g_soc, 0.0f, 100.0f);
  uint16_t sc = socColor(soc);
  int st = chargeState();

  txt(&fonts::FreeSansBold9pt7b, SS, SF_SLATE);
  spr.drawString("BATTERY", C, U(52));

  char vb[8]; snprintf(vb, sizeof(vb), "%.1f", g_voltage);
  txt(&fonts::FreeSansBold24pt7b, 3, SF_WHITE);
  spr.drawString(vb, C - U(10), U(104));
  txt(&fonts::FreeSansBold12pt7b, SS, SF_CYAN_MID, middle_left);
  spr.drawString("V", C + U(54), U(96));

  char l[24]; snprintf(l, sizeof(l), "%d%%   %s", (int)roundf(soc), fmtPower(g_battPower).c_str());
  txt(&fonts::FreeSansBold12pt7b, SS, sc);
  spr.drawString(l, C, U(150));
  txt(&fonts::FreeSansBold9pt7b, SS, chargeColor(st));
  spr.drawString(chargeLabel(st), C, U(176));
}

void pageClock(float t) {
  char hm[6], hms[9], date[16];
  bool ok = timeNow(hm, hms, date);
  float soc = constrain(g_soc, 0.0f, 100.0f);

  spr.fillArc(C, C, U(114), U(110), 135, 405, SF_TRACK);
  float aV = 135 + 270 * soc / 100.0f;
  if (aV > 136) spr.fillArc(C, C, U(114), U(110), 135, aV, socColor(soc));

  txt(&fonts::FreeSansBold24pt7b, 3, SF_WHITE);
  spr.drawString(ok ? hm : "--:--", C, U(110));
  if (ok) { txt(&fonts::FreeSansBold12pt7b, SS, SF_CYAN_MID); spr.drawString(hms + 6, C, U(146)); }
  txt(&fonts::FreeSansBold9pt7b, SS, SF_SLATE);
  spr.drawString(ok ? date : "no time sync", C, U(170));
}

void settingRow(int y, int idx, const char* label, const char* val) {
  bool sel = (settingsSel == idx);
  bool edit = sel && settingsEdit;
  txt(&fonts::FreeSansBold9pt7b, SS, sel ? SF_CYAN : SF_SLATE, middle_left);
  spr.drawString(label, U(58), y);
  if (sel) spr.fillSmoothCircle(U(48), y, U(3), SF_CYAN);   // selection marker
  txt(&fonts::FreeSansBold9pt7b, SS, edit ? SF_WHITE : sel ? SF_CYAN : SF_CYAN_MID, middle_right);
  spr.drawString(val, U(edit ? 178 : 184), y);
  if (edit) {                                               // edit chevrons around value
    txt(&fonts::FreeSansBold9pt7b, SS, SF_CYAN, middle_center);
    spr.drawString("<", U(96), y);
  }
}
void pageSettings(float t) {
  txt(&fonts::FreeSansBold9pt7b, SS, SF_CYAN);
  spr.drawString("SETTINGS", C, U(46));

  char b[8]; snprintf(b, sizeof(b), "%d%%", backlightPct);
  settingRow(U(96),  0, "BRIGHT", b);
  settingRow(U(132), 1, "LEDS", ledMode == 0 ? "OFF" : ledMode == 1 ? "STATUS" : "SOC BAR");

  txt(&fonts::FreeSansBold9pt7b, SS, SF_SLATE);
  spr.drawString(settingsEdit ? "turn crown" : "press knob to edit", C, U(176));
}

// ---- dispatch + AA downscale ----
void render() {
  const bool connected = mqtt.connected();
  const bool haveData  = connected && g_dataReady;
  const float t = millis() / 1000.0f;

  spr.fillSprite(SF_BG);
  drawBezel(connected);

  if (!haveData) {
    txt(&fonts::FreeSansBold12pt7b, SS, connected ? SF_CYAN : SF_AMBER);
    spr.drawString(connected ? "SYNC" : "LINK", C, C - U(6));
    txt(&fonts::FreeSansBold9pt7b, SS, SF_SLATE);
    spr.drawString(connected ? "awaiting data" : "connecting", C, C + U(16));
  } else {
    switch (page) {
      case P_REACTOR: pageReactor(t); break;
      case P_POWER:   pagePower(t);   break;
      case P_BATTERY: pageBattery(t); break;
      case P_CLOCK:   pageClock(t);   break;
      case P_SETTINGS: pageSettings(t); break;
    }
    drawPageDots();
  }
  spr.pushRotateZoomWithAA((float)(W/4), (float)(W/4), 0.0f, 1.0f/SS, 1.0f/SS);
}

// ============================================================
//  Inputs
// ============================================================
void adjustSetting(int sel, int dir) {
  if (sel == 0)      backlightPct = constrain(backlightPct + dir * 10, 10, 100), applyBacklight();
  else if (sel == 1) ledMode = (ledMode + dir + 3) % 3;
}
void changePage(int dir) {
  page = (page + dir + PAGE_COUNT) % PAGE_COUNT;
  settingsEdit = false;                    // leaving edit whenever we switch pages
}
void pollEncoder() {
  int clk = digitalRead(ENC_A);
  if (clk != encLastCLK) {
    if (clk == HIGH) {
      int dir = (digitalRead(ENC_B) != clk) ? 1 : -1;
      if (page == P_SETTINGS && settingsEdit) adjustSetting(settingsSel, dir);
      else                                    changePage(dir);
    }
    encLastCLK = clk;
  }
}
void pollButton() {
  bool down = digitalRead(BTN) == LOW;
  if (down && !btnDown && millis() - btnTime > 250) {
    btnTime = millis();
    if (page == P_SETTINGS) {
      if (!settingsEdit) { settingsEdit = true; settingsSel = 0; }
      else if (++settingsSel >= N_SETTINGS) { settingsEdit = false; settingsSel = 0; }
    }
  }
  btnDown = down;
}
void pollTouch() {
  uint16_t x, y; uint8_t g;
  bool t = touch.getTouch(&x, &y, &g);
  if (!t && touchDown && millis() - touchTime > 300) {
    touchTime = millis();
    changePage(g == SlideDown ? -1 : 1);
  }
  touchDown = t;
}

// ============================================================
//  RGB LEDs
// ============================================================
void ledStatusRGB(uint8_t &r, uint8_t &g, uint8_t &b) {
  switch (chargeState()) {
    case ST_LOW:         r=255; g=30;  b=45;  break;
    case ST_CHARGING:    r=30;  g=200; b=255; break;
    case ST_DISCHARGING: r=255; g=140; b=10;  break;
    default:             r=15;  g=120; b=160; break;
  }
}
void updateLeds() {
  if (ledMode == 0) { strip.clear(); strip.show(); return; }   // off (default)
  uint8_t r, g, b; ledStatusRGB(r, g, b);
  uint32_t c = strip.Color(r*0.55f, g*0.55f, b*0.55f);          // static, no animation
  if (ledMode == 1) {                                           // status: all lit
    for (int i = 0; i < LED_N; i++) strip.setPixelColor(i, c);
  } else {                                                      // SOC bar
    int lit = constrain((int)ceilf(g_soc / 20.0f), 0, LED_N);
    for (int i = 0; i < LED_N; i++) strip.setPixelColor(i, i < lit ? c : 0);
  }
  strip.show();
}

// ============================================================
//  SETUP / LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n=== Victron Reactor HUD ===");

  pinMode(PIN_PWR1, OUTPUT); digitalWrite(PIN_PWR1, HIGH);
  pinMode(PIN_PWR2, OUTPUT); digitalWrite(PIN_PWR2, HIGH);
  pinMode(PIN_PWR_LIGHT, OUTPUT); digitalWrite(PIN_PWR_LIGHT, LOW);
  ledcAttach(PIN_BACKLIGHT, 5000, 8); applyBacklight();   // PWM backlight
  pinMode(ENC_A, INPUT); pinMode(ENC_B, INPUT); pinMode(BTN, INPUT_PULLUP);
  encLastCLK = digitalRead(ENC_A);

  strip.begin(); strip.setBrightness(255); strip.clear(); strip.show();
  touch.begin();

  gfx.init(); gfx.setRotation(0); gfx.fillScreen(SF_BG);
  spr.setColorDepth(16); spr.setPsram(true);
  if (!spr.createSprite(W, W)) { Serial.println("[GFX] 2x sprite alloc failed!"); }
  spr.setPivot(C, C);
  render();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.printf("\n[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());

  configTzTime(POSIX_TZ, "pool.ntp.org", "time.google.com");

  mqtt.setServer(VICTRON_IP, VICTRON_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
}

void loop() {
  mqttReconnect();
  if (mqtt.connected()) { mqtt.loop(); mqttKeepAlive(); }

  pollEncoder();
  pollButton();
  pollTouch();

  if (millis() - lastLed >= 500) { lastLed = millis(); updateLeds(); }  // static; refresh state colour

  if (millis() - lastFrame >= FRAME_MS) {
    lastFrame = millis();
    render();
  }
}
