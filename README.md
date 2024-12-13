# BLEのアドバタイズの送信・中継・受信するデモ
これはセンサーで受信したデータをBLEのアドバタイズで送信し、そのアドバタイズを中継（受信→再送信）し、さらにそのアドバタイズを受信してデータをSDカードに保存、およびWiFi経由でWebサーバーに送信するデモです。特に実用性はありません。

関連ブログ
https://akibabara.com/blog/tag/nrf52840

## ファイル一覧
| ファイル | 用途 | 対象ハードウェア |
| ---- | ---- | ---- |
| ble2wifi_client.ino | バッテリー電圧等のデータを、BLEのアドバタイズデータにのせて送信するプログラム | Seeed XIAO BLE nRF52840 |
| ble2wifi_repeater.ino | アドバタイズを中継するプログラム | Seeed XIAO BLE nRF52840 |
| ble2wifi_server.ino | 受信したBLEのアドバタイズデータを、WiFiでWebサーバーに送信し、SDカードに保存するプログラム | M5Stack Core2 |
| bletest.php | M5Stackから送信されたデータをCSVに格納するプログラム | Web Server |

