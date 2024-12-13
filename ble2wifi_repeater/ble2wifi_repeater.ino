/*
  ble2wifi_repeater.ino
  Seeed XIAO BLE nRF52840でBLEのアドバタイズを中継する
  
  Copyright (c) 2024 Kaz  (https://akibabara.com/blog/)
  Released under the MIT license.
  see https://opensource.org/licenses/MIT
*/

#define DEBUG 1
#include <Arduino.h>
#if DEBUG == 1
#include <Adafruit_TinyUSB.h>   // for Serial
#endif
#include <map>

// 設定
const uint8_t REPEATER_ID = 77;   // リピーターID (advData.repeater)
const uint8_t TTL_MAX = 2;        // 中継するTTLの最大値
const uint8_t XIAO[4] = { 0xFF, 0xFF, 0x12, 0x36 };   // XIAO nRF52840識別値(FFFFは固定)
const size_t RELAY_MAX = 7;   // 中継するデバイスの最大数（7が限界(32x7 = 224 == ADV_INTERVAL_STD)）
const uint16_t ADV_INTERVAL_STD = 224;   // 標準的なアドバタイズ間隔 140ms  nn*0.625
const uint32_t RELAY_EXPIRE = 1000 * 8;    // データの有効期限（アドバタイズが終了する時間）[mS]

// 中継するデバイスのType,ID
const uint16_t RELAY_TI[RELAY_MAX] = {
  0x0A01,
  0x0A02,
  0x0A03,
  0x0A04
};

// BLE関連
#include "bluefruit.h"

// 外部QSPI Flash Memory（省電力化のために使用）
#include <Adafruit_SPIFlash.h>
Adafruit_FlashTransport_QSPI flashTransport;
Adafruit_SPIFlash flash(&flashTransport);

// 送信するアドバタイズの構造体（nRF52840では2バイト未満はパディングされるので順番に注意）
struct AdvData {
  uint8_t maker[4]; // maker_id 子機(nRF52840)の識別用
  uint8_t type;     // 子機種別
  uint8_t id;       // 子機ID
  uint8_t ttl;      // TTL (time to live)
  uint8_t repeater; // リピーターID
  uint16_t seq;     // シーケンス番号   -- ここまで共通フォーマット
  int16_t volt;     // 電圧データ
  int16_t temp;     // 温度データ
};

// デバッグに便利なマクロ定義 --------
#define sp(x) Serial.println(x)
#define spn(x) Serial.print(x)
#define spp(k,v) Serial.println(String(k)+"="+String(v))
#define spf(fmt, ...) Serial.printf(fmt, __VA_ARGS__)
#define array_length(x) (sizeof(x) / sizeof(x[0]))


// 過去に受信したデータの記憶 (Type+ID+Seqで区別)
const size_t HIST_MAX = 20;             // 保存する最大件数
uint32_t tisqHist[HIST_MAX];
uint16_t tisqHistIdx = 0;

// 中継するキューの情報
struct QueueData {
  uint8_t stat;   // 0=無効 1=実行中
  uint8_t  data[26];
  uint16_t len;
  uint32_t expire;
};
std::map<uint16_t, QueueData> queueData;

// キューに追加
void addQueue(uint16_t ti, QueueData &item) {
  item.data[6] ++;   // TTLをカウントアップ
  item.data[7] = REPEATER_ID;  // リピーターIDを付加
  queueData[ti] = item;
  sp("addQueue id="+String(ti & 0xFF));
}

// キューを削除
void deleteQueue(uint16_t ti) {
  QueueData item = {0};
  queueData[ti] = item;
  sp("deleteQueue id="+String(ti & 0xFF));
}

// 有効期限切れのキューを削除
void removeExpiredQueue() {
  uint32_t now = millis();
  for (auto& [ti, item] : queueData) {
    if (item.stat > 0 && (int32_t)(item.expire - now) < 0) {
      sp("removeExpiredQueue id="+String(ti & 0xFF));
      deleteQueue(ti);
    }
  }
}

// キューの数
uint16_t getQueueSize() {
  uint16_t num = 0;
  for (auto& [ti, item] : queueData) {
    if (item.stat > 0) num++;
  }
  return num;
}

// 履歴の最後に追加する、登録上限を超えたものは古いものから削除
void addHistory(uint32_t tisq) {
  if (tisqHistIdx >= HIST_MAX-1) {
    for (int i=1; i<HIST_MAX; i++) tisqHist[i-1] = tisqHist[i];
    tisqHist[HIST_MAX-1] = tisq;
  } else {
    tisqHist[tisqHistIdx++] = tisq;
    if (tisqHistIdx > HIST_MAX-1) tisqHistIdx = HIST_MAX-1;
  }
}

// 指定したtisq(Type,ID,Seq)が履歴に存在するか？
bool inHistory(uint32_t tisq) {
  bool hit = false;
  for (int i=0; i<tisqHistIdx; i++) {
    if (tisq == tisqHist[i]) {
      hit = true;
      break;
    }
  }
  return hit;
}


// 初期化
void setup() {
  if (DEBUG) {
    Serial.begin(115200);
    while (!Serial) { // これをやるとバッテリー駆動時に進まなくなるのでUSB接続必須
      delay(10);   
      if (millis() > 5000) break;
    }
    sp("System Start!");
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

  // 起動LED表示（赤）
  digitalWrite(LED_RED, LOW);
  delay(500);
  digitalWrite(LED_RED, HIGH);

  // BLEの設定
  Bluefruit.autoConnLed(false);
  Bluefruit.begin(1, 1);        // ペリフェラル1、セントラル1
  Bluefruit.setTxPower(8);      // 送信強度　最小 -40, 最大 +8 dBm

  // BLEアドバタイズの設定（送信）
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.setType(BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED);
  Bluefruit.Advertising.setInterval(32, 224);  // 20,160ms間隔で送信  nn*0.625 　この数字は後で上書きされるから意味がない
  Bluefruit.Advertising.setFastTimeout(1);  // 高速アドバタイズの終了時間 0=継続（0にするとなぜかstart()が効かない）　この数字は後で上書きされるから意味がない

  // BLEスキャンの設定（受信）
  Bluefruit.Scanner.setRxCallback(advScanCallback);
  Bluefruit.Scanner.restartOnDisconnect(false);
  Bluefruit.Scanner.setInterval(160, 160);      // n x 0.625ms: 100ms間隔で100ms間受信する
  Bluefruit.Scanner.useActiveScan(false);       // false=パッシブスキャン
  Bluefruit.Scanner.start(0);                   // 0=永続する
  sp("initialized!");

  // WDTの設定
  NRF_WDT->CONFIG         = 0x01;     // Configure WDT to run when CPU is asleep
  NRF_WDT->CRV            = 1+32768*120;    // CRV = timeout * 32768 + 1
  NRF_WDT->RREN           = 0x01;     // Enable the RR[0] reload register
  NRF_WDT->TASKS_START    = 1;        // Start WDT       
}

/*
[ADV  2031444] Packet received from xx:xx:xx:xx:xx:xx
       PAYLOAD 16 bytes
               0F-FF-FF-FF-12-35-0A-01-00-00-02-1B-8F-0E-15-09
          RSSI -42 dBm
      ADV TYPE Non-connectable undirected
 MAN SPEC DATA FF-FF-12-35-0A-01-00-00-02-1B-8F-0E-15-09
*/


// アドバタイズ受信時のコールバック
void advScanCallback(ble_gap_evt_adv_report_t* report) {
  filterAdvData(report);
  Bluefruit.Scanner.resume();   // スキャンを再開
}

// アドバタイズから目的のデータのみ抽出する
void filterAdvData(ble_gap_evt_adv_report_t* report) {
  uint8_t len = 0;
  uint8_t buff[26];
  memset(buff, 0, sizeof(buff));

  // 対象外のデバイスを無視する
  if (report->type.scan_response) return;   // 応答パケットは無視
  if (report->type.connectable) return;     // 接続可能なデバイスは無視
  if (report->type.directed) return;        // 直接接続するデバイスは無視
  len = Bluefruit.Scanner.parseReportByType(report, BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, buff, sizeof(buff));
  if (len < 4 || len > 26) return;
  // if (buff[0] == 0XFF) {  // for デバッグ
  //   spn("Receive: ");
  //   Serial.printBuffer(buff, len, '-');
  //   sp("");
  // }
  if (memcmp(buff, XIAO, 4) != 0) return;   // XIAO nRF52840以外は無視

  // デバッグ！！！　特定のリピーターIDからのみ受信
  // if (buff[7] != 77) return; 

  // 中継対象以外のデバイスを無視する
  uint16_t nowTi = buff[4] << 8 | buff[5]; // type|id
  bool hit = false;
  for (int i=0; i<RELAY_MAX; i++) {
    if (nowTi == RELAY_TI[i]) {
      hit = true;
      break;
    }
    if (RELAY_TI[i] == 0) break;
  }
  if (!hit) return;

  // 直前と連続する同じデータは無視する　負荷低減のための簡易版
  static uint32_t lastTisq;
  uint32_t nowTisq = nowTi << 16 | buff[8] << 8 | buff[9]; // type|id|seq[2]
  if (nowTisq == lastTisq) return;
  lastTisq = nowTisq;

  // 過去に受信した同じデータは無視する
  // spf("nowTisq=%08X ",nowTisq);
  if (inHistory(nowTisq)) {   // 存在した場合
    // sp("exists");
    return;
  } else {  // 存在しなかった場合
    addHistory(nowTisq);
    // sp("New");
  }

  // TTLを超えたデータは破棄する
  if (buff[6] > TTL_MAX) return;
  if (buff[7] == REPEATER_ID) return;

  // 転送するデータをキューに格納する
  spn("Add Queue: ");
  Serial.printBuffer(buff, len, '-');
  sp("");
  QueueData item = {
    .stat = 1,
    .len = len,
    .expire = millis() + RELAY_EXPIRE,
  };
  memcpy(item.data, buff, len);
  addQueue(nowTi, item);   // キューに入れる
}

// アドバタイズする
void sendAdv(uint8_t buff[], uint16_t len, uint16_t interval) {
  // アドバタイズ中なら中断
  if (Bluefruit.Advertising.isRunning()) {
    Bluefruit.Advertising.stop();
  }
  if (len < 1) return;
  // データを送信
  Bluefruit.Advertising.clearData();
  Bluefruit.Advertising.setInterval(interval, interval);  // 送信頻度  
  Bluefruit.Advertising.setFastTimeout(1);  // 高速アドバタイズの終了時間 0=継続（0にするとなぜかstart()が効かない）
  Bluefruit.Advertising.addData(BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, buff, len);
  Bluefruit.Advertising.start(9);   // アドバタイズを開始、9秒後に終了（実際はstop()で終了させる）
}

// メイン
void loop() {
  // WDT Update
  NRF_WDT->RR[0] = WDT_RR_RR_Reload;
  delay(100);

  // 時分割処理
  uint16_t qnum = getQueueSize();
  if (qnum > 0) {
    uint16_t interval = ADV_INTERVAL_STD / qnum;
    if (interval < 32) interval = 32;
    uint16_t intervalMs = interval * 0.625;
    spf("Queue=%d interval=%d (%dmS)\n", qnum, interval, intervalMs);

    // キューにあるデータを順番に1回ずつアドバタイズする
    for (auto& [ti, item] : queueData) {
      uint8_t buff[26] = {0};
      if (item.stat == 1) {
        memcpy(buff, item.data, item.len);
        sendAdv(buff, item.len, interval+100); // アドバタイズする
        delay(intervalMs);
        Bluefruit.Advertising.stop(); // アドバタイズ中断
        delay(40);
      }
    }
  }

  removeExpiredQueue();   // 時間切れのキューを削除
}