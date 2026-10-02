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

// 固定IPを使う場合はここを接続先ネットワークのサブネットに合わせ、USE_STATIC_IP を 1 にする。
// スマホのテザリングなど割り当てサブネットが変わる環境では 0(DHCP)にしておくこと。
// サブネットの合わない固定IPを設定すると、Wi-Fiには繋がるのに他の端末から到達できない。
#define USE_STATIC_IP 0
IPAddress localIP(192, 168, 128, 250);
IPAddress gateway(192, 168, 128, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(192, 168, 128, 1);

const int PULSE_PIN = 34;
WebServer server(80);

// 心拍検出
// 拍の判定は固定値ではなく直近の振幅から自動で決める(適応しきい値)。
// 固定値だとセンサー個体・指の当て方・体温でベースラインがずれた瞬間に
// 一度も閾値を超えず、BPM が更新されなくなる(実測では信号 1800〜2100 に対し
// 固定値 2200 で検出ゼロだった)。THRESHOLD は起動直後の暫定値としてのみ使う。
const int THRESHOLD = 2200;       // 適応前の初期値
const int MIN_AMPLITUDE = 40;     // これ未満は指が離れている / ノイズとみなす
const unsigned long MIN_BEAT_INTERVAL = 300;  // ms (= 200BPM 上限)。二度打ち防止
const int MAX_AMPLITUDE = 800;    // これ超えは指のずれ・接触不良とみなし追従しない
const unsigned long ADAPT_WINDOW_MS = 2000;   // この間隔でレンジを再評価
// 波形をまとめて返すためのバッファ。WAVE_INTERVAL_MS ごとに 1 点記録する。
// ブラウザが 200ms ごとに取りに来るので 10 点前後たまる。取りこぼし用に余裕を持たせる。
const int WAVE_CAP = 64;
const unsigned long WAVE_INTERVAL_MS = 20;
unsigned long lastBeat = 0;
bool pulseHigh = false;
int currentBpm = 0;
int lastSignal = 0;
int sigMax = 0;
int sigMin = 4095;
int threshHigh = THRESHOLD;
int threshLow = THRESHOLD - 100;
unsigned long lastAdapt = 0;
int waveBuf[WAVE_CAP];
int waveCount = 0;
unsigned long lastWaveAt = 0;
const int SMOOTH_N = 4;   // 8 だと拍のピークまで削れて振幅が痩せた
int smoothBuf[SMOOTH_N] = {0};
int smoothIndex = 0;
int smoothFilled = 0;
long smoothSum = 0;

void handleRoot() {
  // 19KB の HTML をそのまま送るとテザリング経由で 2 秒以上かかり、
  // ブラウザによってはタイムアウトして開けない。gzip 版があればそちらを返す
  // (gzip_data.py がビルド時に生成する)。
  // UI と /data の形式は同時に変わるので、古い HTML がキャッシュから使われると壊れる。
  // 毎回取り直させる(6.6KB なので負荷は小さい)。
  server.sendHeader("Cache-Control", "no-store, must-revalidate");

  File page = LittleFS.open("/index.html.gz", "r");
  if (page) {
    // Content-Encoding: gzip は WebServer が .gz 拡張子を見て自動で付ける。
    // ここで手動でも付けるとヘッダが重複し、ブラウザが解凍に失敗して
    // 生バイトを表示してしまう(実際にそうなった)。付けてはいけない。
    server.streamFile(page, "text/html");
    page.close();
    return;
  }

  page = LittleFS.open("/index.html", "r");
  if (!page) {
    server.send(500, "text/plain", "index.html not found");
    return;
  }

  server.streamFile(page, "text/html");
  page.close();
}

// 脈波のサンプリングと拍の検出。
// 以前はこの処理が handleData() の中にあり、/data のリクエストが来たときにしか
// センサーを読んでいなかった。そのため通信が滞ると拍を取りこぼし、currentBpm が
// 次の拍まで前の値を保持し続けて BPM 表示が固まっていた。
// loop() から常時呼び、検出を通信から切り離す。
// 波形の実測レンジと拍の検出状況をシリアルへ 1 秒ごとに出す。
// THRESHOLD はセンサー個体・指の当て方で適正値が変わるため、これを見て調整する。
static int diagMin = 4095, diagMax = 0;
static long diagSum = 0;
static int diagCount = 0;
static int beatCount = 0;
static unsigned long lastDiag = 0;
// /data の処理性能。ESP32 が詰まっているのか、ブラウザ/回線側なのかを切り分ける。
static int reqCount = 0;
static unsigned long reqMicros = 0;
static int reqMaxBatch = 0;

void reportDiagnostics() {
  unsigned long now = millis();
  if (now - lastDiag < 1000) return;
  lastDiag = now;
  if (diagCount == 0) return;
  Serial.printf("[pulse] min=%d max=%d avg=%ld samples=%d high=%d low=%d beats/s=%d bpm=%d\n",
                diagMin, diagMax, diagSum / diagCount, diagCount, threshHigh, threshLow, beatCount, currentBpm);
  Serial.printf("[http] req/s=%d avg=%luus maxBatch=%d heap=%u\n",
                reqCount, reqCount ? reqMicros / reqCount : 0UL, reqMaxBatch, ESP.getFreeHeap());
  diagMin = 4095; diagMax = 0; diagSum = 0; diagCount = 0; beatCount = 0;
  reqCount = 0; reqMicros = 0; reqMaxBatch = 0;
}

void samplePulse() {
  int raw = analogRead(PULSE_PIN);

  // 移動平均で高周波ノイズを落とす。生値のままだと振幅が 60 程度しかない場面で
  // ノイズが立ち上がりと誤判定され、BPM が 196 まで跳ねた。
  smoothSum += raw - smoothBuf[smoothIndex];
  smoothBuf[smoothIndex] = raw;
  smoothIndex = (smoothIndex + 1) % SMOOTH_N;
  if (smoothFilled < SMOOTH_N) smoothFilled++;
  int signal = smoothSum / smoothFilled;

  lastSignal = signal;

  if (signal < diagMin) diagMin = signal;
  if (signal > diagMax) diagMax = signal;
  diagSum += signal;
  diagCount++;

  if (signal > sigMax) sigMax = signal;
  if (signal < sigMin) sigMin = signal;

  unsigned long now = millis();

  // 通信とは無関係に波形を記録しておき、リクエスト時にまとめて返す。
  if (now - lastWaveAt >= WAVE_INTERVAL_MS) {
    lastWaveAt = now;
    if (waveCount < WAVE_CAP) {
      waveBuf[waveCount++] = signal;
    } else {
      // 溢れたら最も古い点を捨てる(最新を優先)
      memmove(waveBuf, waveBuf + 1, sizeof(int) * (WAVE_CAP - 1));
      waveBuf[WAVE_CAP - 1] = signal;
    }
  }

  // 直近の窓の振幅から立ち上がり/立ち下がりのしきい値を決め直す。
  // 窓の最後に min/max を中央値へ畳んでおくと、ベースラインの移動にも追従する。
  if (now - lastAdapt >= ADAPT_WINDOW_MS) {
    lastAdapt = now;
    int amplitude = sigMax - sigMin;
    // 振幅が異常に大きい窓は指のずれや接触不良。そこに追従すると次の窓で
    // しきい値が跳ね上がって検出が止まるので、採用しない。
    if (amplitude >= MIN_AMPLITUDE && amplitude <= MAX_AMPLITUDE) {
      int nextHigh = sigMin + (amplitude * 7) / 10;  // 振幅の 70% で立ち上がり検出
      int nextLow = sigMin + (amplitude * 5) / 10;   // 50% まで下がったら次を待つ(ヒステリシス)
      // 一気に置き換えず馴らす(1 窓の外れ値でしきい値が飛ぶのを防ぐ)
      threshHigh = (threshHigh + nextHigh * 2) / 3;
      threshLow = (threshLow + nextLow * 2) / 3;
    }
    int mid = (sigMax + sigMin) / 2;
    sigMax = mid;
    sigMin = mid;
  }

  if (signal > threshHigh && !pulseHigh) {
    pulseHigh = true;
    if (now - lastBeat >= MIN_BEAT_INTERVAL) {
      if (lastBeat > 0) {
        int bpm = 60000 / (now - lastBeat);
        if (bpm > 30 && bpm < 220) currentBpm = bpm;  // 異常値を除外
      }
      lastBeat = now;
      beatCount++;
    }
  }
  if (signal < threshLow) pulseHigh = false;
}

void handleData() {
  unsigned long enter = micros();
  samplePulse();  // 応答直前の値も含める
  reqCount++;

  // 1 リクエストで直近のサンプルをまとめて返す。
  // 以前は 1 サンプル/リクエストで、ブラウザが 20ms ごと = 毎秒 50 リクエスト
  // 叩いており、単一接続で捌く WebServer が追いつかず反応が鈍っていた。
  String json;
  json.reserve(24 + waveCount * 6);
  json = "{\"signals\":[";
  for (int i = 0; i < waveCount; i++) {
    if (i) json += ',';
    json += waveBuf[i];
  }
  json += "],\"signal\":";
  json += lastSignal;   // 旧形式(単一値)。キャッシュされた古いページ向けの後方互換
  json += ",\"bpm\":";
  json += currentBpm;
  json += "}";
  waveCount = 0;  // 返した分は捨てる

  int sent = waveCount;
  server.send(200, "application/json", json);
  reqMicros += micros() - enter;
  if (sent > reqMaxBatch) reqMaxBatch = sent;
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
#if USE_STATIC_IP
  if (!WiFi.config(localIP, gateway, subnet, primaryDNS)) {
    Serial.println("Static IP configuration failed");
  }
#endif
  WiFi.begin(ssid, password);
  // ESP32 は既定で Wi-Fi のモデムスリープが有効で、AP の DTIM 間隔に応じて
  // 応答が数百ms〜数秒ばらつく。実測で / の取得が 1.3 秒 / 11 秒 / タイムアウトと
  // 暴れていたのはこれが原因(その間 ESP32 側は req/s=0 で完全に暇だった)。
  // 常時給電なので消費電力より応答性を優先する。
  WiFi.setSleep(false);
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
  samplePulse();  // 通信の有無にかかわらず拍を追い続ける
  reportDiagnostics();
}
