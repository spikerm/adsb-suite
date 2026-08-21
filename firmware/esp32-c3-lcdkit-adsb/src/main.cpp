#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>
#include "config.h"

constexpr int PIN_LCD_BL=5, PIN_ENC_A=10, PIN_ENC_B=6, PIN_ENC_SW=9, PIN_RGB=8;
constexpr uint16_t BG=TFT_BLACK, FG=TFT_WHITE, DIM=0x7BEF, ACCENT=TFT_CYAN, COLOR_OK=TFT_GREEN, ALERT=TFT_RED;

TFT_eSPI tft;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Adafruit_NeoPixel rgb(1, PIN_RGB, NEO_GRB + NEO_KHZ800);

struct AircraftDot {
  char flight[10]{};
  char hex[8]{};
  char squawk[5]{};
  float distanceKm=0, bearing=0, speedKt=0;
  int altitudeFt=0, track=0;
};
struct Summary {
  int aircraft=0, withPosition=0;
  float msgRate=0, maxRangeKm=0;
  bool sourceOnline=false;
  uint32_t ageSeconds=0;
  AircraftDot nearest;
};

Summary summary;
AircraftDot planes[24];
size_t planeCount=0;
String lastAlert;
uint32_t alertUntil=0, lastMqttAttempt=0, lastWifiAttempt=0, lastRender=0, lastButtonMs=0;
int page=0, lastEncA=HIGH;
const int radarRanges[]={25,50,100,200,400};
int radarRangeIndex=2;

String topic(const char *leaf){ return String(MQTT_BASE_TOPIC)+"/"+leaf; }
void setLed(uint8_t r,uint8_t g,uint8_t b){ rgb.setPixelColor(0,rgb.Color(r,g,b)); rgb.show(); }
void copyText(char *dst,size_t n,JsonVariantConst v){ strlcpy(dst,v|"",n); }

void header(const String &title){
  tft.fillScreen(BG); tft.drawCircle(120,120,118,DIM);
  tft.setTextDatum(TC_DATUM); tft.setTextColor(ACCENT,BG); tft.drawString(title,120,16,2);
  tft.drawFastHLine(72,38,96,DIM);
}
void footer(const char *s){ tft.setTextDatum(BC_DATUM); tft.setTextColor(DIM,BG); tft.drawString(s,120,224,2); }

void drawOverview(){
  header("ADS-B");
  tft.setTextDatum(MC_DATUM); tft.setTextColor(summary.sourceOnline?COLOR_OK:ALERT,BG); tft.drawString(String(summary.aircraft),120,75,4);
  tft.setTextColor(FG,BG); tft.drawString("AIRCRAFT",120,103,2);
  tft.setTextDatum(TL_DATUM); tft.setTextColor(DIM,BG);
  tft.drawString("POS",52,131,2); tft.drawString("MSG/S",52,153,2); tft.drawString("MAX",52,175,2);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(FG,BG);
  tft.drawString(String(summary.withPosition),190,131,2); tft.drawString(String(summary.msgRate,0),190,153,2);
  tft.drawString(String(summary.maxRangeKm,0)+" km",190,175,2); footer("druk: volgende");
}

void drawRadar(){
  header(String("RADAR  ")+radarRanges[radarRangeIndex]+" km");
  const int cx=120,cy=128,r=82; tft.drawCircle(cx,cy,r,DIM); tft.drawCircle(cx,cy,r/2,DIM);
  tft.drawFastHLine(cx-r,cy,r*2,DIM); tft.drawFastVLine(cx,cy-r,r*2,DIM); tft.fillCircle(cx,cy,3,ACCENT);
  float range=radarRanges[radarRangeIndex];
  for(size_t i=0;i<planeCount;i++){
    const auto &a=planes[i]; if(a.distanceKm<=0||a.distanceKm>range) continue;
    float ang=radians(a.bearing-90.0f), rr=min((float)r,(a.distanceKm/range)*r);
    int x=cx+(int)(cosf(ang)*rr), y=cy+(int)(sinf(ang)*rr);
    bool e=!strcmp(a.squawk,"7500")||!strcmp(a.squawk,"7600")||!strcmp(a.squawk,"7700");
    tft.fillCircle(x,y,e?4:2,e?ALERT:FG);
  }
  tft.setTextDatum(MC_DATUM); tft.setTextColor(DIM,BG); tft.drawString("N",cx,cy-r-8,2);
  footer("draai: bereik  druk: volgende");
}

void drawNearest(){
  header("NEAREST"); const auto &a=summary.nearest;
  String id=strlen(a.flight)?String(a.flight):String(a.hex); if(!id.length()) id="---";
  tft.setTextDatum(MC_DATUM); tft.setTextColor(FG,BG); tft.drawString(id,120,67,4);
  tft.setTextColor(ACCENT,BG); tft.drawString(String(a.distanceKm,1)+" km",120,103,2);
  tft.setTextDatum(TL_DATUM); tft.setTextColor(DIM,BG); tft.drawString("ALT",53,130,2); tft.drawString("SPD",53,153,2); tft.drawString("TRK",53,176,2);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(FG,BG); tft.drawString(String(a.altitudeFt)+" ft",188,130,2);
  tft.drawString(String(a.speedKt,0)+" kt",188,153,2); tft.drawString(String(a.track)+" deg",188,176,2); footer("druk: volgende");
}

void drawStatus(){
  header("RECEIVER"); tft.setTextDatum(MC_DATUM); tft.setTextColor(summary.sourceOnline?COLOR_OK:ALERT,BG);
  tft.drawString(summary.sourceOnline?"ONLINE":"OFFLINE",120,72,4);
  tft.setTextDatum(TL_DATUM); tft.setTextColor(DIM,BG); tft.drawString("Wi-Fi",48,119,2); tft.drawString("MQTT",48,143,2); tft.drawString("Data age",48,167,2);
  tft.setTextDatum(TR_DATUM); tft.setTextColor(FG,BG); tft.drawString(WiFi.status()==WL_CONNECTED?"OK":"DOWN",192,119,2);
  tft.drawString(mqtt.connected()?"OK":"DOWN",192,143,2); tft.drawString(String(summary.ageSeconds)+" s",192,167,2); footer("druk: volgende");
}

void render(){
  if(page==0) drawOverview(); else if(page==1) drawRadar(); else if(page==2) drawNearest(); else drawStatus();
  if(lastAlert.length()&&millis()<alertUntil){
    tft.drawCircle(120,120,116,ALERT); tft.drawCircle(120,120,114,ALERT); tft.fillRoundRect(28,193,184,25,8,ALERT);
    tft.setTextDatum(MC_DATUM); tft.setTextColor(TFT_WHITE,ALERT); tft.drawString(lastAlert.substring(0,24),120,205,2);
  }
  lastRender=millis();
}

void parsePlane(JsonObjectConst o,AircraftDot &a){
  copyText(a.flight,sizeof(a.flight),o["flight"]); copyText(a.hex,sizeof(a.hex),o["hex"]); copyText(a.squawk,sizeof(a.squawk),o["squawk"]);
  a.distanceKm=o["distance_km"]|0.0f; a.bearing=o["bearing"]|0.0f; a.altitudeFt=o["altitude_ft"]|0;
  a.speedKt=o["speed_kt"]|0.0f; a.track=o["track"]|0;
}

void onMqtt(char *topicName,byte *payload,unsigned int length){
  JsonDocument doc; if(deserializeJson(doc,payload,length)) return; String t(topicName);
  if(t.endsWith("/summary")){
    summary.aircraft=doc["aircraft"]|0; summary.withPosition=doc["with_position"]|0; summary.msgRate=doc["msg_rate"]|0.0f;
    summary.maxRangeKm=doc["max_range_km"]|0.0f; summary.sourceOnline=doc["source_online"]|false; summary.ageSeconds=doc["age_seconds"]|0;
    if(doc["nearest"].is<JsonObjectConst>()) parsePlane(doc["nearest"].as<JsonObjectConst>(),summary.nearest);
  }else if(t.endsWith("/aircraft")){
    planeCount=0; for(JsonObjectConst o:doc.as<JsonArrayConst>()){ if(planeCount>=24) break; parsePlane(o,planes[planeCount++]); }
  }else if(t.endsWith("/alert")){
    const char *event=doc["event"]|"ALERT", *flight=doc["flight"]|"", *sq=doc["squawk"]|"";
    lastAlert=String(event)+" "+(strlen(flight)?flight:sq); alertUntil=millis()+12000; setLed(60,0,0);
  }
  render();
}

void connectWifi(){
  if(WiFi.status()==WL_CONNECTED) return; if(lastWifiAttempt&&millis()-lastWifiAttempt<10000) return; lastWifiAttempt=millis();
  WiFi.mode(WIFI_STA); WiFi.setHostname(DEVICE_NAME); WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
}
void connectMqtt(){
  if(WiFi.status()!=WL_CONNECTED||mqtt.connected()) return; if(lastMqttAttempt&&millis()-lastMqttAttempt<5000) return; lastMqttAttempt=millis();
  String id=String(DEVICE_NAME)+"-"+String((uint32_t)ESP.getEfuseMac(),HEX), st=topic("status"); bool ok;
  if(strlen(MQTT_USERNAME)) ok=mqtt.connect(id.c_str(),MQTT_USERNAME,MQTT_PASSWORD,st.c_str(),0,true,"offline");
  else ok=mqtt.connect(id.c_str(),st.c_str(),0,true,"offline");
  if(!ok) return; mqtt.publish(st.c_str(),"online",true); mqtt.subscribe(topic("summary").c_str()); mqtt.subscribe(topic("aircraft").c_str()); mqtt.subscribe(topic("alert").c_str()); setLed(0,18,0);
}

void readEncoder(){
  int a=digitalRead(PIN_ENC_A); if(a!=lastEncA&&a==LOW){ int d=digitalRead(PIN_ENC_B)==HIGH?1:-1;
    if(page==1){ radarRangeIndex+=d; if(radarRangeIndex<0)radarRangeIndex=4; if(radarRangeIndex>4)radarRangeIndex=0; }
    else { page+=d; if(page<0)page=3; if(page>3)page=0; } render(); }
  lastEncA=a;
  if(digitalRead(PIN_ENC_SW)==LOW&&millis()-lastButtonMs>350){ lastButtonMs=millis(); page=(page+1)%4; render(); }
}

void setup(){
  Serial.begin(115200); pinMode(PIN_LCD_BL,OUTPUT); digitalWrite(PIN_LCD_BL,HIGH);
  pinMode(PIN_ENC_A,INPUT_PULLUP); pinMode(PIN_ENC_B,INPUT_PULLUP); pinMode(PIN_ENC_SW,INPUT_PULLUP);
  rgb.begin(); rgb.clear(); rgb.show(); tft.init(); tft.setRotation(0); tft.fillScreen(BG);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(FG,BG); tft.drawString("ADS-B",120,102,4); tft.setTextColor(DIM,BG); tft.drawString("connecting...",120,132,2);
  mqtt.setServer(MQTT_HOST,MQTT_PORT); mqtt.setCallback(onMqtt); mqtt.setBufferSize(8192); mqtt.setKeepAlive(30); connectWifi();
}

void loop(){
  connectWifi(); connectMqtt(); if(mqtt.connected()) mqtt.loop(); readEncoder();
  if(lastAlert.length()&&millis()>alertUntil){ lastAlert=""; setLed(mqtt.connected()?0:30,mqtt.connected()?18:0,0); render(); }
  if(millis()-lastRender>5000) render(); delay(2);
}
