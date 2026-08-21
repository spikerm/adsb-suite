#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

#include "config.h"

static constexpr int PIN_LCD_BL = 5;
static constexpr int PIN_ENC_A = 10;
static constexpr int PIN_ENC_B = 6;
static constexpr int PIN_ENC_SW = 9;
static constexpr int PIN_RGB = 8;

static constexpr uint16_t COL_BG = TFT_BLACK;
static constexpr uint16_t COL_FG = TFT_WHITE;
static constexpr uint16_t COL_DIM = 0x7BEF;
static constexpr uint16_t COL_ACCENT = TFT_CYAN;
static constexpr uint16_t COL_OK = TFT_GREEN;
static constexpr uint16_t COL_WARN = TFT_YELLOW;
static constexpr uint16_t COL_ALERT = TFT_RED;

TFT_eSPI tft;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Adafruit_NeoPixel rgb(1, PIN_RGB, NEO_GRB + NEO_KHZ800);

struct AircraftDot {
  char flight[10]{};
  char hex[8]{};
  float distanceKm = 0;
  float bearing = 0;
  int altitudeFt = 0;
  float speedKt = 0;
  int track = 0;
  char squawk[5]{};
};

struct Summary {
  int aircraft = 0;
  int withPosition = 0;
  float msgRate = 0;
  float maxRangeKm = 0;
  bool sourceOnline = false;
  uint32_t ageSeconds = 0;
  AircraftDot nearest;
};

Summary summary;
AircraftDot aircraft[24];
size_t aircraftCount = 0;

String lastAlert;
uint32_t alertUntil = 0;
uint32_t lastMqttAttempt = 0;
uint32_t lastWifiAttempt = 0;
uint32_t lastRender = 0;
uint32_t lastButtonMs = 0;
int currentPage = 0;
const int pageCount = 4;
int radarRanges[] = {25, 50, 100, 200, 400};
int radarRangeIndex = 2;
int lastEncA = HIGH;

void copyText(char *dst, size_t dstLen, JsonVariantConst v) {
  const char *s = v | "";
  strlcpy(dst, s, dstLen);
}

String topic(const char *leaf) {
  return String(MQTT_BASE_TOPIC) + "/" + leaf;
}

void setLed(uint8_t r, uint8_t g, uint8_t b) {
  rgb.setPixelColor(0, rgb.Color(r, g, b));
  rgb.show();
}

void drawHeader(const char *title) {
  tft.fillScreen(COL_BG);
  tft.drawCircle(120, 120, 118, COL_DIM);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_ACCENT, COL_BG);
  tft.drawString(title, 120, 16, 2);
  tft.drawFastHLine(72, 38, 96, COL_DIM);
}

void drawFooter(const char *text) {
  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString(text, 120, 224, 2);
}

void drawOverview() {
  drawHeader("ADS-B");

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(summary.sourceOnline ? COL_OK : COL_ALERT, COL_BG);
  tft.drawString(String(summary.aircraft), 120, 75, 4);

  tft.setTextColor(COL_FG, COL_BG);
  tft.drawString("AIRCRAFT", 120, 103, 2);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("POS", 52, 131, 2);
  tft.drawString("MSG/S", 52, 153, 2);
  tft.drawString("MAX", 52, 175, 2);

  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(COL_FG, COL_BG);
  tft.drawString(String(summary.withPosition), 190, 131, 2);
  tft.drawString(String(summary.msgRate, 0), 190, 153, 2);
  tft.drawString(String(summary.maxRangeKm, 0) + " km", 190, 175, 2);

  drawFooter("druk: volgende");
}

void drawRadar() {
  drawHeader("RADAR  " + String(radarRanges[radarRangeIndex]) + " km");

  const int cx = 120;
  const int cy = 128;
  const int radius = 82;
  tft.drawCircle(cx, cy, radius, COL_DIM);
  tft.drawCircle(cx, cy, radius / 2, COL_DIM);
  tft.drawFastHLine(cx - radius, cy, radius * 2, COL_DIM);
  tft.drawFastVLine(cx, cy - radius, radius * 2, COL_DIM);
  tft.fillCircle(cx, cy, 3, COL_ACCENT);

  const float rangeKm = radarRanges[radarRangeIndex];
  for (size_t i = 0; i < aircraftCount; ++i) {
    const auto &a = aircraft[i];
    if (a.distanceKm <= 0 || a.distanceKm > rangeKm) continue;
    float ang = radians(a.bearing - 90.0f);
    float rr = min((float)radius, (a.distanceKm / rangeKm) * radius);
    int x = cx + (int)(cosf(ang) * rr);
    int y = cy + (int)(sinf(ang) * rr);
    bool emergency = !strcmp(a.squawk, "7500") || !strcmp(a.squawk, "7600") || !strcmp(a.squawk, "7700");
    tft.fillCircle(x, y, emergency ? 4 : 2, emergency ? COL_ALERT : COL_FG);
  }

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("N", cx, cy - radius - 8, 2);
  drawFooter("draai: bereik  druk: volgende");
}

void drawNearest() {
  drawHeader("NEAREST");
  const auto &a = summary.nearest;
  String ident = strlen(a.flight) ? String(a.flight) : String(a.hex);
  if (!ident.length()) ident = "---";

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_FG, COL_BG);
  tft.drawString(ident, 120, 67, 4);

  tft.setTextColor(COL_ACCENT, COL_BG);
  tft.drawString(String(a.distanceKm, 1) + " km", 120, 103, 2);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("ALT", 53, 130, 2);
  tft.drawString("SPD", 53, 153, 2);
  tft.drawString("TRK", 53, 176, 2);

  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(COL_FG, COL_BG);
  tft.drawString(String(a.altitudeFt) + " ft", 188, 130, 2);
  tft.drawString(String(a.speedKt, 0) + " kt", 188, 153, 2);
  tft.drawString(String(a.track) + " deg", 188, 176, 2);

  drawFooter("druk: volgende");
}

void drawStatus() {
  drawHeader("RECEIVER");

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(summary.sourceOnline ? COL_OK : COL_ALERT, COL_BG);
  tft.drawString(summary.sourceOnline ? "ONLINE" : "OFFLINE", 120, 72, 4);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("Wi-Fi", 48, 119, 2);
  tft.drawString("MQTT", 48, 143, 2);
  tft.drawString("Data age", 48, 167, 2);

  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(COL_FG, COL_BG);
  tft.drawString(WiFi.status() == WL_CONNECTED ? "OK" : "DOWN", 192, 119, 2);
  tft.drawString(mqtt.connected() ? "OK" : "DOWN", 192, 143, 2);
  tft.drawString(String(summary.ageSeconds) + " s", 192, 167, 2);

  drawFooter("druk: volgende");
}

void drawAlertOverlay() {
  if (!lastAlert.length() || millis() > alertUntil) return;
  tft.drawCircle(120, 120, 116, COL_ALERT);
  tft.drawCircle(120, 120, 114, COL_ALERT);
  tft.fillRoundRect(28, 193, 184, 25, 8, COL_ALERT);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, COL_ALERT);
  tft.drawString(lastAlert.substring(0, 24), 120, 205, 2);
}

void render() {
  switch (currentPage) {
    case 0: drawOverview(); break;
    case 1: drawRadar(); break;
    case 2: drawNearest(); break;
    default: drawStatus(); break;
  }
  drawAlertOverlay();
  lastRender = millis();
}

void parseAircraftObject(JsonObjectConst obj, AircraftDot &a) {
  copyText(a.flight, sizeof(a.flight), obj["flight"]);
  copyText(a.hex, sizeof(a.hex), obj["hex"]);
  copyText(a.squawk, sizeof(a.squawk), obj["squawk"]);
  a.distanceKm = obj["distance_km"] | 0.0f;
  a.bearing = obj["bearing"] | 0.0f;
  a.altitudeFt = obj["altitude_ft"] | 0;
  a.speedKt = obj["speed_kt"] | 0.0f;
  a.track = obj["track"] | 0;
}

void onMqtt(char *topicName, byte *payload, unsigned int length) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) return;

  String t(topicName);
  if (t.endsWith("/summary")) {
    summary.aircraft = doc["aircraft"] | 0;
    summary.withPosition = doc["with_position"] | 0;
    summary.msgRate = doc["msg_rate"] | 0.0f;
    summary.maxRangeKm = doc["max_range_km"] | 0.0f;
    summary.sourceOnline = doc["source_online"] | false;
    summary.ageSeconds = doc["age_seconds"] | 0;
    if (doc["nearest"].is<JsonObjectConst>()) parseAircraftObject(doc["nearest"].as<JsonObjectConst>(), summary.nearest);
  } else if (t.endsWith("/aircraft")) {
    aircraftCount = 0;
    for (JsonObjectConst obj : doc.as<JsonArrayConst>()) {
      if (aircraftCount >= 24) break;
      parseAircraftObject(obj, aircraft[aircraftCount++]);
    }
  } else if (t.endsWith("/alert")) {
    const char *event = doc["event"] | "ALERT";
    const char *flight = doc["flight"] | "";
    const char *sq = doc["squawk"] | "";
    lastAlert = String(event) + " " + (strlen(flight) ? flight : sq);
    alertUntil = millis() + 12000;
    setLed(60, 0, 0);
  }
  render();
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiAttempt < 10000 && lastWifiAttempt != 0) return;
  lastWifiAttempt = millis();
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_NAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void connectMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (millis() - lastMqttAttempt < 5000 && lastMqttAttempt != 0) return;
  lastMqttAttempt = millis();

  String clientId = String(DEVICE_NAME) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  bool ok;
  if (strlen(MQTT_USERNAME)) {
    ok = mqtt.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD, topic("status").c_str(), 0, true, "offline");
  } else {
    ok = mqtt.connect(clientId.c_str(), topic("status").c_str(), 0, true, "offline");
  }
  if (!ok) return;

  mqtt.publish(topic("status").c_str(), "online", true);
  mqtt.subscribe(topic("summary").c_str());
  mqtt.subscribe(topic("aircraft").c_str());
  mqtt.subscribe(topic("alert").c_str());
  setLed(0, 18, 0);
}

void readEncoder() {
  int a = digitalRead(PIN_ENC_A);
  if (a != lastEncA && a == LOW) {
    int direction = digitalRead(PIN_ENC_B) == HIGH ? 1 : -1;
    if (currentPage == 1) {
      radarRangeIndex += direction;
      if (radarRangeIndex < 0) radarRangeIndex = 4;
      if (radarRangeIndex > 4) radarRangeIndex = 0;
    } else {
      currentPage += direction;
      if (currentPage < 0) currentPage = pageCount - 1;
      if (currentPage >= pageCount) currentPage = 0;
    }
    render();
  }
  lastEncA = a;

  if (digitalRead(PIN_ENC_SW) == LOW && millis() - lastButtonMs > 350) {
    lastButtonMs = millis();
    currentPage = (currentPage + 1) % pageCount;
    render();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);

  rgb.begin();
  rgb.clear();
  rgb.show();

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_FG, COL_BG);
  tft.drawString("ADS-B", 120, 102, 4);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("connecting...", 120, 132, 2);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqtt);
  mqtt.setBufferSize(8192);
  mqtt.setKeepAlive(30);

  connectWifi();
}

void loop() {
  connectWifi();
  connectMqtt();
  if (mqtt.connected()) mqtt.loop();
  readEncoder();

  if (lastAlert.length() && millis() > alertUntil) {
    lastAlert = "";
    setLed(mqtt.connected() ? 0 : 30, mqtt.connected() ? 18 : 0, 0);
    render();
  }

  if (millis() - lastRender > 5000) render();
  delay(2);
}
