/*
  ble2wifi_server.ino
  Seeed XIAO BLE nRF52840から受信したBLEのアドバタイズデータを、WiFiでWebサーバーに送信し、SDカードに保存する

  Copyright (c) 2024 Kaz  (https://akibabara.com/blog/)
  Released under the MIT license.
  see https://opensource.org/licenses/MIT
*/

#include <M5Unified.h>
//#include <Arduino.h>

// ネットワーク設定
#define WIFI_SSID "xxxxxxxx"
#define WIFI_PASS "xxxxxxxx"
const String WEB_API_URL = "http://xxxxxxxx/bletest.php";  // 送信先URL

// 設定
#define SDCARD_SAVE 1   // SDカードに保存する
const size_t RCV_CNT_MAX = 20;  // 同時に受信するデータの最大件数
const uint8_t XIAO[4] = { 0xFF, 0xFF, 0x12, 0x36 };   // XIAO nRF52840識別値(FFFFは固定)
const uint16_t RECONNECT_ERRCNT = 10;   // 一定回数以上サーバー送信に失敗したらWiFiを再接続させる

// BLE関連
#include <BLEDevice.h>
BLEScan* pBLEScan;

// SD関連
#include <SD.h>
const String FILENAME_PREFIX = "/xiao";
File file;

// Wi-Fi関連
#include <WiFi.h>
#include <HTTPClient.h>

// 過去に受信したデータの記憶
#include <map>
std::map<uint32_t, int8_t> tisqHist;
const size_t HIST_MAX = 30;   // 重複排除用の最大保存件数

// 受信したアドバタイズのキュー
#include <queue>
#include <mutex>
struct RcvData {
  uint8_t type; // 子機種別
  uint8_t id;   // 子機ID
  uint8_t ttl;  // TTL (time to live)
  uint8_t repeater; // リピーターID
  uint16_t seq; // シーケンス番号
  float volt;   // バッテリー電圧
  float temp;   // SoC温度
  int8_t rssi; // 電波強度 RSSI -50非常に強い　-80弱い
  uint32_t millis;   // 時刻情報
};
std::queue<RcvData> rcvdatas;
std::mutex queueMutex;    // スレッドセーフのためのミューテックス

// デバッグに便利なマクロ定義 --------
#define sp(x) Serial.println(x)
#define spn(x) Serial.print(x)
#define spp(k,v) Serial.println(String(k)+"="+String(v))
#define spf(fmt, ...) Serial.printf(fmt, __VA_ARGS__)


// Wi-Fi接続する
bool wifiConnect() {
  bool stat = false;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wifi connecting.");
    for (int j=0; j<10; j++) {
      WiFi.disconnect();
      WiFi.mode(WIFI_STA);
      WiFi.begin(WIFI_SSID, WIFI_PASS);  //  Wi-Fi APに接続
      for (int i=0; i<10; i++) {
        if (WiFi.status() == WL_CONNECTED) break;
        Serial.print(".");
        delay(500);
      }
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("connected!");
        Serial.println(WiFi.localIP());
        stat = true;
        break;
      } else {
        Serial.println("failed");
        WiFi.disconnect();
      }
    }
  }
  return stat;
}

// アドバタイズ受信時のコールバック
class advScanCallback : public BLEAdvertisedDeviceCallbacks {
public:
  void onResult(BLEAdvertisedDevice dev) override {
    const uint8_t* buff = reinterpret_cast<const uint8_t*>(dev.getManufacturerData().data());
    int len = dev.getManufacturerData().length();
    // 対象外のデバイスを除外
    if (len < 4 || len > 26) return;
    if (memcmp(buff, XIAO, 4) != 0) return;   // XIAO nRF52840以外は無視

    // デバッグ！！！　特定のリピーターIDからのみ受信
    // if (buff[7] != 88) return; 

    // デバッグ！！！　FFで始まるデータをすべて表示する
    if (buff[0] == 0xFF) {
      spf("Scanned: Device(%02d): ", rcvdatas.size());
      for (int j=0; j<len; j++) spf("%02X ",buff[j]);
      sp("");
    }

    // 過去に受信した同じデータは無視する
    static uint32_t lastTisq;
    uint32_t nowTisq = buff[4] << 24 | buff[5] << 16 | buff[8] << 8 | buff[9]; // type|id|seq[2]
    if (tisqHist.find(nowTisq) != tisqHist.end()) {   // 存在した場合
      return;
    } else {  // 存在しなかった場合
      if (tisqHist.size() >= HIST_MAX) {
        tisqHist.erase(tisqHist.begin()); // 最古のデータを削除
      }
      tisqHist[nowTisq] = (uint8_t)dev.getRSSI();   // 新規追加（RSSIを保存しているが現在は活用してない）
    }

    // XIAO nRF52840からのデータを格納する
    RcvData rcvtmp;
    rcvtmp.type = buff[4];
    rcvtmp.id = buff[5];
    rcvtmp.ttl = buff[6];
    rcvtmp.repeater = buff[7];
    rcvtmp.seq = buff[9] << 8 | buff[8];
    if (rcvtmp.type == 10) {  // 種別10: 実験用 volt(i2) temp(i2)
      rcvtmp.volt = (float)(buff[11] << 8 | buff[10]) / 1000.00;
      rcvtmp.temp = (float)(buff[13] << 8 | buff[12]) / 100.00;
      rcvtmp.rssi = (uint8_t)dev.getRSSI();  // 電波強度
      rcvtmp.millis = millis();
    }
    // キューに保存
    std::lock_guard<std::mutex> lock(queueMutex);
    int overnum = rcvdatas.size() + 1 - RCV_CNT_MAX;
    if (overnum > 0) {
      for (int i=0; i<overnum; i++) rcvdatas.pop();   // キューがいっぱいなら古いキューを削除する
    }
    rcvdatas.push(rcvtmp);  // キューに保存
  }
};

// 初期化
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg); 
	Serial.begin(115200);
  delay(1000);
  sp("System Start!");

  // ディスプレイの設定
  M5.Lcd.init();
  M5.Lcd.setColorDepth(16);
  M5.Lcd.fillScreen(TFT_BLUE);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setClipRect(2,2, M5.Lcd.width()-4, M5.Lcd.height()-4);  //描画範囲
  M5.Lcd.setScrollRect(2,2, M5.Lcd.width()-4, M5.Lcd.height()-4);  //スクロール範囲
  M5.Lcd.setTextScroll(true);
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setCursor(2, 4);

  // Wi-Fi接続
  wifiConnect();

  // SDカードをマウントする
  if (SDCARD_SAVE) {
    spn("SD Card mounting...");
    while (!SD.begin(GPIO_NUM_4, SPI, 15000000)) {
      spn(".");
      delay(500);
    }
    sp("done");
  }

  // BLEの設定
  sp("init BLE device...");
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new advScanCallback(), true);  // アドバタイズ受信時のコールバック関数（true=同じMACアドレスからの重複チェックをしない）
  pBLEScan->setActiveScan(false);   // false=パッシブスキャンにする
  pBLEScan->start(0, nullptr);    // スキャン開始（0=永続する）
}

// メイン
void loop() {
  static int errcnt = 0;
  delay(100);
  if (rcvdatas.empty()) return;

  // キューから1個ずつデータを取り出す
  while (!rcvdatas.empty()) {
    queueMutex.lock();
    RcvData rcvdata = rcvdatas.front();
    rcvdatas.pop();
    queueMutex.unlock();

    // ディスプレイに表示
    M5.Lcd.printf("[%02d] %.2fV %.1fC %d %d\n", rcvdata.id, rcvdata.volt, rcvdata.temp, rcvdata.rssi, rcvdata.seq);
    spf("DATA %02X %.2fV %.1fC %d %d\n", rcvdata.id, rcvdata.volt, rcvdata.temp, rcvdata.rssi, rcvdata.seq);

    // SDカードに保存する、サーバーに送信する
    RcvData rcvtmps[1] = { rcvdata };  
    saveSdcard(rcvtmps, 1);
    if (postServer(rcvtmps, 1)) errcnt = 0;
    else errcnt++;
  }

  // 一定回数以上連続してサーバー送信に失敗したらWiFiを再接続させる（なぜかWiFiが切れることがある）
  if (errcnt >= RECONNECT_ERRCNT) {
    sp("WiFi Re-connect");
    M5.Lcd.println("WiFi Re-connect");
    WiFi.disconnect();
    delay(1000);
    wifiConnect();
    errcnt = 0;
  }

  delay(10);
}

// Webサーバーに送信
bool postServer(struct RcvData* td, int cnt) {
  HTTPClient http;
  bool success = false;
  sp("Send to Web Server");

  // POSTデータ作成
  String jsonData = "{\"data\":[";
  char buff[64];
  for (int i=0; i<cnt; i++) {
    sprintf(buff, "{\"id\":%d,", td[i].id);
    jsonData += buff;
    sprintf(buff, "\"volt\":%.2f,", td[i].volt);
    jsonData += buff;
    sprintf(buff, "\"temp\":%.1f,", td[i].temp);
    jsonData += buff;
    sprintf(buff, "\"rssi\":%d,", td[i].rssi);
    jsonData += buff;
    sprintf(buff, "\"seq\":%d}", td[i].seq);
    jsonData += buff;
    if (i < cnt-1) jsonData += ",";
  }
  jsonData += "],\"count\":"+String(cnt)+"}";
  sp(jsonData);

  // 送信
  http.begin(WEB_API_URL); //HTTP
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Content-Length", String(jsonData.length()));
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  int httpCode = http.POST(jsonData);
  sp("[HTTP] POST code="+String(httpCode));

  // 送信後の処理
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    sp(payload);
    success = true;
  } else {
    sp("[HTTP] Server Error! code="+String(httpCode));
    M5.Lcd.println("Server Error! code="+String(httpCode));
    delay(1000);
  }
  http.end();
  return success;
}

// SDカードに保存　NTP/RTC不使用ver　あくまでSDはバックアップ目的
bool saveSdcard(struct RcvData* td, int cnt) {
  bool err = false;
  if (!SDCARD_SAVE) return false;
  for (int i=0; i<cnt; i++) {
    String path = FILENAME_PREFIX + "_" + String(td[i].id) + ".csv";
    spn("[SD] savefile "+path+" ... ");
    if (!SD.exists(path)) {
      file = SD.open(path, FILE_WRITE);
      file.print("Time,ID,Volt,Temp,RSSI,Seq\n");
      file.close();
    }
    file = SD.open(path, FILE_APPEND);
    if (file) {
      file.printf("%d,%d,%.2f,%.1f,%d,%d\n", (td[i].millis/1000), td[i].id, td[i].volt, td[i].temp, td[i].rssi, td[i].seq);
      file.close();
      sp("done");
    } else {
      err = true;
      sp("failed!");
    }
  }
  if (err) {
    M5.Lcd.println("SD Card Save Failed");
    delay(1000);
  }
  return !err;
}