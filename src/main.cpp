#include <Arduino.h> 
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>

// Wi-Fi認証情報は .env から build_flags 経由で注入される (load_env.py 参照)
#ifndef WIFI_SSID
#define WIFI_SSID "your-wifi-ssid"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "your-wifi-password"
#endif

const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// このWi-Fiネットワーク用の固定IP設定
IPAddress localIP(192, 168, 128, 250);
IPAddress gateway(192, 168, 128, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(192, 168, 128, 1);

const int PULSE_PIN = 34;
WebServer server(80);

// 心拍検出
const int THRESHOLD = 2200;   // 波形を見て調整
unsigned long lastBeat = 0;
bool pulseHigh = false;
int currentBpm = 0;

void handleRoot() {
  File page = LittleFS.open("/index.html", "r");
  if (!page) {
    server.send(500, "text/plain", "index.html not found");
    return;
  }

  server.streamFile(page, "text/html");
  page.close();
}

void handleData() {
  int signal = analogRead(PULSE_PIN);

  if (signal > THRESHOLD && !pulseHigh) {
    pulseHigh = true;
    unsigned long now = millis();
    if (lastBeat > 0) {
      int bpm = 60000 / (now - lastBeat);
      if (bpm > 30 && bpm < 220) currentBpm = bpm;  // 異常値を除外
    }
    lastBeat = now;
  }
  if (signal < THRESHOLD - 100) pulseHigh = false;

  String json = "{\"signal\":" + String(signal) + ",\"bpm\":" + String(currentBpm) + "}";
  server.send(200, "application/json", json);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("HeartRateApp starting");
  analogReadResolution(12);

  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
    Serial.println("Upload the filesystem with: platformio run --target uploadfs");
    return;
  }
  Serial.println("LittleFS mounted");

  Serial.println("Connecting to Wi-Fi...");
  if (!WiFi.config(localIP, gateway, subnet, primaryDNS)) {
    Serial.println("Static IP configuration failed");
  }
  WiFi.begin(ssid, password);
  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("接続完了! このアドレスをブラウザで開いてください → http://");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
}

void loop() {
  server.handleClient();
}
