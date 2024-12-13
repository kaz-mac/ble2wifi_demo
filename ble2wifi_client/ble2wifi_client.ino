/*
  ble2wifi_client.ino
  Seeed XIAO BLE nRF52840でバッテリー電圧等のデータを、BLEのアドバタイズデータにのせて送信する
  
  Copyright (c) 2024 Kaz  (https://akibabara.com/blog/)
  Released under the MIT license.
  see https://opensource.org/licenses/MIT
*/

#define DEBUG 1
#include <Arduino.h>
#if DEBUG == 1
#include <Adafruit_TinyUSB.h>   // for Serial
#endif

// 設定
const uint8_t DEVICE_TYPE = 0x0A; // 子機種別 (advData.type)
const uint8_t DEVICE_ID   = 0x04;   // 子機ID (advData.id)
const uint8_t XIAO[4] = { 0xFF, 0xFF, 0x12, 0x36 };   // XIAO nRF52840識別値(FFFFは固定)
const uint16_t ADV_INTERVAL = 256 ;   // アドバタイズの送信間隔　省電力160ms間隔で送信　nn*0.625  (32, 224);

// BLE関連
#include "bluefruit.h"

// 外部QSPI Flash Memory（省電力化のために使用）
#include <Adafruit_SPIFlash.h>
Adafruit_FlashTransport_QSPI flashTransport;
Adafruit_SPIFlash flash(&flashTransport);

// 送信するデータの構造体（nRF52840では2バイト未満はパディングされるので順番に注意）
typedef struct {
  uint8_t maker[4]; // maker_id 子機(nRF52840)の識別用
  uint8_t type;     // 子機種別
  uint8_t id;       // 子機ID
  uint8_t ttl;      // TTL (time to live)
  uint8_t repeater; // リピーターID
  uint16_t seq;     // シーケンス番号   -- ここまで共通フォーマット
  int16_t volt;     // 電圧データ
  int16_t temp;     // 温度データ
} AdvData;
AdvData advData = {
  .maker = { XIAO[0], XIAO[1], XIAO[2], XIAO[3] },
  .type = DEVICE_TYPE,
  .id = DEVICE_ID,
  .ttl = 0,
  .repeater = 0,
  .seq = 1,
};

// デバッグに便利なマクロ定義 --------
#define sp(x) Serial.println(x)
#define spn(x) Serial.print(x)
#define spp(k,v) Serial.println(String(k)+"="+String(v))
#define spf(fmt, ...) Serial.printf(fmt, __VA_ARGS__)
#define array_length(x) (sizeof(x) / sizeof(x[0]))

// 測定してデータを送信する
void measure() {
  // バッテリー電圧の測定
  int vbat_raw = analogRead(PIN_VBAT);
  int vbat_mv = vbat_raw * 2400 / 1023; // VREF = 2.4V, 10bit A/D
  vbat_mv = vbat_mv * 1510 / 510;       // 1M + 510k / 510k
  advData.volt = (int16_t)vbat_mv;
  if (DEBUG) sp(advData.volt);

  // CPUの温度測定
  advData.temp = (int16_t)(readCPUTemperature() * 100.0);

  // アドバタイズ中なら一旦中断（たぶんしなくていい）
  if (Bluefruit.Advertising.isRunning()) {
    Bluefruit.Advertising.stop();
  }

  // データを送信
  if (advData.seq > 9999) advData.seq = 0;
  Bluefruit.Advertising.clearData();
  Bluefruit.Advertising.addData(BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, &advData, sizeof(advData));
  Bluefruit.Advertising.start(9);   // アドバタイズを開始、引数は終了する時間(s)
  advData.seq++;
}

// 初期化
void setup() {
  if (DEBUG) {
    Serial.begin(115200);
    pinMode(LED_RED, OUTPUT);
    pinMode(LED_GREEN, OUTPUT);
    pinMode(LED_BLUE, OUTPUT);
    digitalWrite(LED_RED, HIGH);
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_BLUE, HIGH);
  }

  // オンボードQSPI Flash MemoryをDeep Power-downモードにして省電力化する
  flashTransport.begin();
  flashTransport.runCommand(0xB9);
  delayMicroseconds(5);
  flashTransport.end();

  // バッテリー電圧測定の準備
  analogReference(AR_INTERNAL_2_4); // VREF = 2.4V
  analogReadResolution(10);         // 10bit A/D
  pinMode(VBAT_ENABLE, OUTPUT);
  digitalWrite(VBAT_ENABLE, LOW);   // VBAT_ENABLEをLOWにすると測定できる

  if (DEBUG) {
    digitalWrite(LED_RED, LOW);
    delay(500);
    digitalWrite(LED_RED, HIGH);
  }

  // BLEの設定
  Bluefruit.autoConnLed(false);
  Bluefruit.begin();
  Bluefruit.setTxPower(0);  // 送信強度　最小 -40, 最大 +8 dBm
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.setType(BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED);
  Bluefruit.Advertising.setInterval(ADV_INTERVAL, ADV_INTERVAL);  // 省電力160ms間隔で送信　nn*0.625  (32, 224);  
  //Bluefruit.Advertising.addData(BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, &advData, sizeof(advData));
  Bluefruit.Advertising.setFastTimeout(1);  // 高速アドバタイズの終了時間 0=継続（0にするとなぜかstart()が効かない）

  // WDTの設定
  NRF_WDT->CONFIG         = 0x01;     // Configure WDT to run when CPU is asleep
  NRF_WDT->CRV            = 1+32768*120;    // CRV = timeout * 32768 + 1
  NRF_WDT->RREN           = 0x01;     // Enable the RR[0] reload register
  NRF_WDT->TASKS_START    = 1;        // Start WDT       
}

// メイン
void loop() {
  if (DEBUG) {
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_BLUE, LOW);
  }

  measure();  // 測定してデータを送信する

  delay(500);
  if (DEBUG) {
    digitalWrite(LED_BLUE, HIGH);
    digitalWrite(LED_GREEN, LOW);
  }
  delay(9500);

  // WDT Update
  NRF_WDT->RR[0] = WDT_RR_RR_Reload;
}