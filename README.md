# [プチヘム](protopedia.net/prototype/4014)
 プチヘムは家庭用蓄電池、エコキュートを、次の日のソーラーパネルの発電量予測に基づいて制御するシステムです。本体にはエアコンから取得した外気温を表示するディスプレイが付いています。

## 制御フロー
<img src="https://raw.githubusercontent.com/kasys1422/Model-View-Test-1/main/flow.png" height="" width="100%" alt="制御の流れ" align="centor"  style="border-radius:35px;">

## 構成パーツ
1. ESP32
2. 4桁7セグメントディスプレイ(TM1637)
3. 適当なケース

## 使用するライブラリ
1. ArduinoJson (version6)
2. Grove_4Digital_Display
3. EL_dev_arduino
4. NTPClient

## 注意点
1. 環境やパーツによっては動きません。
2. コード中の制御方式で必ずしも消費電力を削減できるわけではありません。適切な改良を施してください。
3. コードはMITライセンスです。自己責任で利用してください。
