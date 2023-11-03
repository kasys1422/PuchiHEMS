#include <WiFi.h>
#include <HTTPClient.h>
#include <math.h>
#include <ArduinoJson.h>
#include <time.h>
#include "TM1637.h"
#include "EL.h"
#include "NTPClient.h"

//[環境変数]------------------------------------------------------------------------------------
// WifiのSSIDとパスワード
#define WIFI_SSID "SSID"
#define WIFI_PASS "PASS"

// OpenWeatherMapのAPIキー（実際の値に置き換える）
const char *apiKey = "api_key";

// 家の情報
double latitude = 35.0;    // 経度
double longitude = 139.0;  // 緯度
double declination = 26.5;  // 傾斜角
double azimuth = 15;       // 方位角
double kiloWattPeak = 5.5;  // キロワットピーク

// 7セグメント液晶(TM1637)のピン番号
#define PIN_CLK (13)
#define PIN_DIO (14)

// 機器のIPアドレス
const IPAddress ac_ip = IPAddress(192, 168, 1, 100);   // エアコン
const IPAddress ec_ip = IPAddress(192, 168, 1, 101);  // エコキュート
const IPAddress bt_ip = IPAddress(192, 168, 1, 102);    // 蓄電池

//----------------------------------------------------------------------------------------------

// 7セグメント液晶(TM1637)の初期化
TM1637 tm(PIN_CLK, PIN_DIO);

// NTP初期化
WiFiUDP udp;
NTPClient ntp(udp, "pool.ntp.org");

#define JST 3600 * 9

// バッファサイズを定義（必要に応じて変更する）
#define BUFFER_SIZE 256

// OpenWeatherMapのAPI URLを生成する関数
String createWeatherURL(const String &apiKey, double latitude, double longitude) {
  String url = "http://api.openweathermap.org/data/2.5/weather?";
  url += "lat=" + String(latitude, 6);    // 緯度
  url += "&lon=" + String(longitude, 6);  // 経度
  url += "&appid=" + apiKey;              // APIキー
  return url;
}

// OpenWeatherMapの予報API URLを生成する関数
String createForecastURL(const String &apiKey, double latitude, double longitude) {
  String url = "http://api.openweathermap.org/data/2.5/forecast?";
  url += "lat=" + String(latitude, 6);    // 緯度
  url += "&lon=" + String(longitude, 6);  // 経度
  url += "&appid=" + apiKey;              // APIキー
  return url;
}

// Solar Forecast APIのURLを生成する関数
String createSolarForecastURL(double latitude, double longitude, double declination, double azimuth, double kiloWattPeak) {
  String url = "http://api.forecast.solar/estimate/";
  url += String(latitude, 6);  // 緯度
  url += "/";
  url += String(longitude, 6);  // 経度
  url += "/";
  url += String(declination, 2);  // 傾斜角
  url += "/";
  url += String(azimuth, 2);  // 方位角
  url += "/";
  url += String(kiloWattPeak, 2);  // キロワットピーク
  return url;
}


// 制御用情報を入れた構造体
struct SolarInfo {
  int percent = 0;
  bool pre_heat_boiler = false;
  bool could_get_data = false;
} next_day_info;

// 関数プロトタイプ
void printNetData();
int get_temp_from_api();
SolarInfo get_solar_data_from_api();

// EchoNetLite初期化
WiFiClient client;
WiFiUDP elUDP;
EL echo(elUDP, 0x05, 0xff, 0x01);

String TEMP_API_URL;
String TEMP_API_URL_2;
String FORECAST_SOLAR_API_URL;

// 機器ID
const byte ac[] = { 0x01, 0x30, 0x01 };
const byte ec[] = { 0x02, 0x6b, 0x01 };
const byte bt[] = { 0x02, 0x7d, 0x01 };

// 制御コマンド
const byte ec_auto[] = { 0x01, 0x41 };
const byte ec_on[] = { 0x01, 0x42 };
const byte ec_off[] = { 0x01, 0x43 };
const byte battery_charge[] = { 0x01, 0x42 };
const byte battery_use[] = { 0x01, 0x43 };
const byte battery_wait[] = { 0x01, 0x44 };
const byte battery_auto[] = { 0x01, 0x46 };

// 気温
int temp = 126;
int temp_error = 0;

// バッテリー残量
int battery_percent = 0;

// 時間関連(2036年問題関連対応)
uint64_t base_time = 0;
uint32_t base_mills = 0;

uint64_t get_now_unix_time() {
  if (base_time == 0)
    return 0;
  return base_time + ((millis() - base_mills) / 1000);
}

uint64_t get_todays_unix_time(int timezone = 0) {
  uint64_t buffer = get_now_unix_time();
  return buffer - (buffer % 86400) - (timezone * 3600);
}

// Unix秒を日本時間のYYYY-MM-DD形式の文字列に変換する関数
void unix_time_to_formatted_date_JST(uint64_t unix_time, char *buffer, size_t buffer_size) {
  // Unix時間に9時間分の秒を加算して日本時間に変換
  unix_time += 32400;  // 9 hours in seconds

  struct tm timeinfo;
  gmtime_r((time_t *)&unix_time, &timeinfo);

  snprintf(buffer, buffer_size, "%04d-%02d-%02d",
           timeinfo.tm_year + 1900,
           timeinfo.tm_mon + 1,
           timeinfo.tm_mday);
}

void print_time_YYYY_MM_DD() {
  char buffer[11];

  // Unix秒を日本時間のYYYY-MM-DD形式の文字列に変換
  unix_time_to_formatted_date_JST(get_now_unix_time(), buffer, sizeof(buffer));

  Serial.println(buffer);
}

// Unix秒を入力として受け取り、その日の日本時間の12時ちょうどのUnix秒を返す関数
uint64_t getUnixTimeAtNoonJST(uint64_t unix_time) {
  // Unix時間に9時間分の秒を加算して日本時間に変換
  unix_time += 32400;  // 9 hours in seconds

  struct tm timeinfo;
  gmtime_r((time_t *)&unix_time, &timeinfo);

  // その日の12時ちょうどの時間情報を設定
  timeinfo.tm_hour = 12;
  timeinfo.tm_min = 0;
  timeinfo.tm_sec = 0;

  // 時間情報をUnix秒に変換
  uint64_t unix_time_at_noon = mktime(&timeinfo);

  // 9時間分の秒を減算してUTCに変換
  unix_time_at_noon -= 32400;  // 9 hours in seconds

  return unix_time_at_noon;
}

void update_unix_time() {
  while (true) {
    ntp.update();
    unsigned long epochTime = ntp.getEpochTime();
    base_time = epochTime;
    base_mills = millis();
    if (base_time > 100000) {
      break;
    }
    delay(1000);
  }
}

void update_unix_time(uint64_t now_unix_time) {
  base_time = now_unix_time;
  base_mills = millis();
}

void update_unix_time(uint64_t now_unix_time, uint32_t mills) {
  base_time = now_unix_time;
  base_mills = mills;
}

void update_unix_base_time(uint64_t now_unix_time) {
  base_time = now_unix_time;
}

void update_base_mills() {
  base_mills = millis();
}

void update_base_mills(uint32_t mills) {
  base_mills = mills;
}

struct tm_day {
  int tm_sec;  /* 秒 [0-61] 最大2秒までのうるう秒を考慮 */
  int tm_min;  /* 分 [0-59] */
  int tm_hour; /* 時 [0-23] */
};

tm_day get_day_time(int timezone = 0) {
  int day_time = (get_now_unix_time() + (timezone * 3600)) % 86400;
  int buffer = day_time % 3600;
  struct tm_day data;
  data.tm_sec = buffer % 60;
  data.tm_min = buffer / 60;
  data.tm_hour = day_time / 3600;
  return data;
}

void print_time() {
  if (base_time != 0) {
    char buf[12];
    tm_day day_time = get_day_time(9);
    sprintf(buf, "[%02d:%02d:%02d] ", int(day_time.tm_hour), int(day_time.tm_min), int(day_time.tm_sec));
    Serial.print(buf);
  }
}

void print_time(char *text) {
  if (base_time != 0) {
    char buf[12];
    tm_day day_time = get_day_time(9);
    sprintf(buf, "[%02d:%02d:%02d] ", int(day_time.tm_hour), int(day_time.tm_min), int(day_time.tm_sec));
    Serial.print(buf);
  }
  Serial.print(text);
}

void println_time(char *text) {
  print_time(text);
  Serial.println("");
}

void println_time() {
  println_time("");
}

#define LEAP_YEAR(Y) ((Y > 0) && !(Y % 4) && ((Y % 100) || !(Y % 400)))  // 閏年判定マクロ

uint8_t get_month(unsigned long secs) {
  unsigned long rawTime = secs / 86400L;  // 秒を日に換算
  unsigned long days = 0, year = 1970;    // 経過日数と年の初期化
  uint8_t month;
  // 各月の日数
  static const uint8_t monthDays[] = { 31, 28, 31, 30, 31, 30,
                                       31, 31, 30, 31, 30, 31 };

  // 現在の年を特定するためのループ
  while ((days += (LEAP_YEAR(year) ? 366 : 365)) <= rawTime) {
    year++;  // 次の年へ
  }

  // 現在の年における経過日数を計算
  rawTime -= days - (LEAP_YEAR(year) ? 366 : 365);

  // 月を特定するためのループ
  for (month = 0; month < 12; month++) {
    uint8_t monthLength;
    // 2月の日数を計算
    if (month == 1) {
      monthLength = LEAP_YEAR(year) ? 29 : 28;
    } else {
      monthLength = monthDays[month];  // その他の月
    }

    if (rawTime < monthLength)
      break;  // 現在の月を特定できた

    rawTime -= monthLength;  // 次の月へ
  }

  // 月の文字列を生成（先頭の0を含む）
  String monthStr = ++month < 10 ? "0" + String(month) : String(month);  // 1月は月1として扱う
  // 日付の文字列
  // String dayStr = ++rawTime < 10 ? "0" + String(rawTime) : String(rawTime);  // 月の日

  // 文字列から整数への変換
  month = monthStr.toInt();
  return month;  // 月を返却
}

bool check_battery_once = false;

// セットアップ
void setup() {
  // Serial
  Serial.begin(115200);
  delay(1000);
  Serial.print("[ESP32 Mini HEMS Controller]\n");

  // APIのURL合成
  TEMP_API_URL = createWeatherURL(apiKey, latitude, longitude);
  TEMP_API_URL_2 = createForecastURL(apiKey, latitude, longitude);
  FORECAST_SOLAR_API_URL = createSolarForecastURL(latitude, longitude, declination, azimuth, kiloWattPeak);


  // 7seg
  tm.init();
  tm.set(BRIGHT_TYPICAL);
  tm.clearDisplay();
  tm.displayStr("HELLO");
  delay(2000);

  // Wifi
  Serial.print("connect wifi\n");
  tm.displayStr("WIFI");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("wait.");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(1000);
  }
  Serial.println("\nconnected");
  // time
  Serial.println("get time");
  delay(500);
  while (base_time < 100000) {
    update_unix_time();
    print_time("unix time: ");
    Serial.println(base_time);
    delay(1000);
  }
  print_time("month: ");
  Serial.println(get_month(get_now_unix_time()));
  print_time_YYYY_MM_DD();

  tm_day day_time = get_day_time(9);
  tm.displayStr("Connected");
  tm.display(0, day_time.tm_hour / 10);
  tm.display(1, day_time.tm_hour % 10);
  tm.display(2, day_time.tm_min / 10);
  tm.display(3, day_time.tm_min % 10);
  print_time();
  Serial.println("completed");
  printNetData();
  // HEMS初期化
  echo.begin();
  // 外気温取得
  temp = get_temp_from_api();
  for (int i = 0; i < 10; i++) {
    next_day_info = get_solar_data_from_api();
    if (next_day_info.could_get_data == true) {
      tm.displayStr("");
      tm.display(1, next_day_info.percent / 10);
      tm.display(2, next_day_info.percent % 10);
      break;
    }
    delay(2000);
  }
  print_time();

  // 情報取得
  echo.sendOPC1(ac_ip, ac, EL_GET, 0xbe, nullptr);
  echo.sendOPC1(bt_ip, bt, EL_GET, 0xe4, nullptr);

  Serial.print("setup completed!\n");
}

uint32_t last_mills = millis();
// 蓄電池の状態 (0:オート,1:充電,2:放電,3:待機)
uint16_t charge_state = 0;

void loop() {
  // check wifi
  wifi_check();

  tm_day day_time = get_day_time(9);
  // エアコンから外気温情報取得
  if (day_time.tm_sec == 30 && day_time.tm_min % 15 == 2) {
    echo.sendOPC1(ac_ip, ac, EL_GET, 0xbe, nullptr);
    println_time("send packet to AC");
  } else if (day_time.tm_sec == 30 && day_time.tm_min % 5 == 2 && temp_error >= 1) {
    echo.sendOPC1(ac_ip, ac, EL_GET, 0xbe, nullptr);
    println_time("send packet to AC");
  }

  // 時刻調整（8時間おき）
  if (day_time.tm_sec == 0 && day_time.tm_min == 2 && (day_time.tm_hour + 4) % 8 == 0) {
    println_time("calibration time");
    update_unix_time();
  }

  // 日照予報取得
  if (day_time.tm_sec == 0 && day_time.tm_min == 50 && day_time.tm_hour == 22) {
    println_time("get sun data");
    for (int i = 0; i < 10; i++) {
      next_day_info = get_solar_data_from_api();
      if (next_day_info.could_get_data == true) {
        break;
      }
      delay(2000);
    }
  }

  // 機器制御
  if (next_day_info.could_get_data == true) {
    // 夜充電
    if (next_day_info.percent != 0) {
      // 残量取得
      if (day_time.tm_sec == 10 && day_time.tm_min == 55 && day_time.tm_hour == 22) {
        println_time("get battry percent");
        echo.sendOPC1(bt_ip, bt, EL_GET, 0xe4, nullptr);
      }
      // 運転モード指定
      if (day_time.tm_sec == 58 && day_time.tm_min == 59 && day_time.tm_hour == 22) {
        println_time("set battry mode");
        if (battery_percent < next_day_info.percent) {
          echo.sendOPC1(bt_ip, bt, EL_SETI, 0xda, battery_charge);
          charge_state = 1;
        } else {
          echo.sendOPC1(bt_ip, bt, EL_SETI, 0xda, battery_use);
          charge_state = 2;
        }
      }
      // 充電モードを維持
      if (day_time.tm_sec == 15 && day_time.tm_min % 3 == 0 && charge_state == 1) {
        println_time("set battry mode: charge");
        echo.sendOPC1(bt_ip, bt, EL_SETI, 0xda, battery_charge);
      }
      // 運転モードを待機に変更
      if (day_time.tm_sec == 20) {
        if (battery_percent >= next_day_info.percent && charge_state == 1) {
          println_time("set battry mode: wait");
          echo.sendOPC1(bt_ip, bt, EL_SETI, 0xda, battery_wait);
          charge_state = 3;
        }
        if (battery_percent <= next_day_info.percent && charge_state == 2) {
          println_time("set battry mode: wait");
          echo.sendOPC1(bt_ip, bt, EL_SETI, 0xda, battery_wait);
          charge_state = 3;
        }
        if (charge_state == 1 || charge_state == 2) {
          echo.sendOPC1(bt_ip, bt, EL_GET, 0xe4, nullptr);
        }
      }
      // 待機モードを維持
      if (day_time.tm_sec == 15 && day_time.tm_min % 3 == 0 && charge_state == 3) {
        println_time("set battry mode: wait");
        echo.sendOPC1(bt_ip, bt, EL_SETI, 0xda, battery_wait);
      }
      // 運転モードを自動にする
      if (day_time.tm_sec == 30 && day_time.tm_min == 59 && day_time.tm_hour == 6) {
        println_time("set battry mode");
        echo.sendOPC1(bt_ip, bt, EL_SETI, 0xda, battery_auto);
        charge_state = 0;
      }
      // 夜充電なし(5時時点で5％以下なら5％まで充電、5月から9月は無効)
    } else {
      // 残量取得
      if (day_time.tm_sec == 30 && day_time.tm_min == 58 && day_time.tm_hour == 4) {
        println_time("get battry percent");
        echo.sendOPC1(bt_ip, bt, EL_GET, 0xe4, nullptr);
      }
      // 運転モード指定
      if (day_time.tm_sec == 30 && day_time.tm_min == 1 && day_time.tm_hour == 5) {
        int month = get_month(get_now_unix_time());
        if (month < 3 || month > 10) {
          if (battery_percent < 5) {
            println_time("set battry mode: charge");
            echo.sendOPC1(bt_ip, bt, EL_SETI, 0xda, battery_charge);
            charge_state = 1;
          }
        }
      }
      // 運転モードを待機に変更
      if (day_time.tm_sec == 20 && charge_state == 1) {
        if (battery_percent >= 5) {
          println_time("set battry mode: wait");
          echo.sendOPC1(bt_ip, bt, EL_SETI, 0xda, battery_wait);
          charge_state = 3;
        }
        echo.sendOPC1(bt_ip, bt, EL_GET, 0xe4, nullptr);
      }
      // 運転モードを待機に変更（バッテリー充電なしの場合）
      if (day_time.tm_hour >= 5 && day_time.tm_hour <= 6) {
        int month = get_month(get_now_unix_time());
        if (month < 3 || month > 10) {
          if (day_time.tm_sec == 20 && charge_state == 0) {
            if (battery_percent <= 5) {
              println_time("set battry mode: wait");
              echo.sendOPC1(bt_ip, bt, EL_SETI, 0xda, battery_wait);
              charge_state = 3;
            }
            echo.sendOPC1(bt_ip, bt, EL_GET, 0xe4, nullptr);
          }
        }
      }
      // 待機モードを維持
      if (day_time.tm_sec == 15 && day_time.tm_min % 3 == 0 && charge_state == 3) {
        println_time("set battry mode: wait");
        echo.sendOPC1(bt_ip, bt, EL_SETI, 0xda, battery_wait);
      }
      // 運転モードを自動にする
      if (day_time.tm_sec == 30 && day_time.tm_min == 59 && day_time.tm_hour == 6) {
        println_time("set battry mode");
        echo.sendOPC1(bt_ip, bt, EL_SETI, 0xda, battery_auto);
        charge_state = 0;
      }
    }
    // 運転モードを自動にする(2回目)
    if (day_time.tm_sec == 45 && day_time.tm_min == 59 && day_time.tm_hour == 6) {
      println_time("set battry mode");
      echo.sendOPC1(bt_ip, bt, EL_SETI, 0xda, battery_auto);
      charge_state = 0;
    }

    // 夜沸き上げ
    if (next_day_info.pre_heat_boiler == true) {
      if (day_time.tm_sec == 0 && day_time.tm_min == 1 && day_time.tm_hour == 23) {
        println_time("set EcoCute mode: on");
        echo.sendOPC1(ec_ip, ec, EL_SETI, 0xb0, ec_on);
      }
      if (day_time.tm_sec == 30 && day_time.tm_min == 58 && day_time.tm_hour == 6) {
        echo.sendOPC1(ec_ip, ec, EL_SETI, 0xb0, ec_auto);
        println_time("set EcoCute mode: off");
        echo.sendOPC1(ec_ip, ec, EL_SETI, 0xb0, ec_off);
      }
      // 昼沸き上げ
    } else {
      if (day_time.tm_sec == 0 && day_time.tm_min == 1 && day_time.tm_hour == 23) {
        echo.sendOPC1(ec_ip, ec, EL_SETI, 0xb0, ec_auto);
        println_time("set EcoCute mode: off");
        echo.sendOPC1(ec_ip, ec, EL_SETI, 0xb0, ec_off);
      }
      int month = get_month(get_now_unix_time());
      if (month < 3 || month > 10) {
        if (day_time.tm_sec == 0 && day_time.tm_min == 1 && day_time.tm_hour == 9) {
          println_time("set EcoCute mode: on");
          echo.sendOPC1(ec_ip, ec, EL_SETI, 0xb0, ec_on);
        }
      } else {
        if (day_time.tm_sec == 0 && day_time.tm_min == 1 && day_time.tm_hour == 8) {
          println_time("set EcoCute mode: on");
          echo.sendOPC1(ec_ip, ec, EL_SETI, 0xb0, ec_on);
        }
      }

      if (day_time.tm_sec == 0 && day_time.tm_min == 59 && day_time.tm_hour == 14) {
        echo.sendOPC1(ec_ip, ec, EL_SETI, 0xb0, ec_auto);
        println_time("set EcoCute mode: off");
        echo.sendOPC1(ec_ip, ec, EL_SETI, 0xb0, ec_off);
      }
    }
  }
  while (millis() - last_mills < 1000) {
    if ((millis() - last_mills) % 10 == 0) {
      echo_net_rcv();
    }
  }
  last_mills = millis();
}

void custom_display(uint8_t bit_addr, uint8_t seg_data) {
  tm.start();
  tm.writeByte(0x44);
  tm.stop();
  tm.start();
  tm.writeByte(bit_addr | 0xc0);
  tm.writeByte(seg_data);
  tm.stop();
  tm.start();
  tm.writeByte(tm.cmd_disp_ctrl);
  tm.stop();
}

int round_int(float num) {
  if (num - int(num) < 0.5) {
    return int(num);
  } else {
    return int(num) + 1;
  }
}

// 気温表示
void show_temp(int temp) {
  if (temp <= -10) {
    temp = -9;
  } else if (temp >= 100) {
    temp = 99;
  }
  if (temp == 99) {
    for (int i = 0; i < 4; i++)
      tm.display(i, 8);
  } else {
    if (temp < 0) {
      custom_display(0, 0b1000000);
      tm.display(1, temp * -1);
    } else if (temp < 10) {
      custom_display(0, 0b0000000);
      tm.display(1, temp);
    } else {
      tm.display(0, temp / 10);
      tm.display(1, temp % 10);
    }
    custom_display(2, 0b1011000);
    if (temp_error <= 1) {
      custom_display(3, 0b0000000);
    } else {
      custom_display(3, 0b1111001);
    }
  }
}

// Wifiの切断を確認
void wifi_check() {
  if ((WiFi.status() != WL_CONNECTED)) {
    custom_display(3, 0b1111110);
    print_time();
    Serial.println("wifi disconnected");
    WiFi.disconnect();
    WiFi.reconnect();
    print_time();
    Serial.print("reconnect");
    int reconnect_timer = 0;
    while ((WiFi.status() != WL_CONNECTED)) {
      if (reconnect_timer > 120) {
        ESP.restart();
      }
      Serial.print(".");
      delay(1000);
      reconnect_timer += 1;
    }
    Serial.println("");
    print_time();
    Serial.println("connected");
    custom_display(3, 0b0000000);
    show_temp(temp);
  }
}

// APIから気温を取得
int get_temp_from_api() {
  int api_temp = 126;
  if ((WiFi.status() == WL_CONNECTED)) {

    HTTPClient http;
    // http.begin(TEMP_API_URL); //URLを指定
    if (!http.begin(client, TEMP_API_URL)) {
      print_time();
      Serial.println("Failed HTTPClient begin!");
      return api_temp;
    }
    int httpCode = http.GET();  // GETリクエストを送信

    if (httpCode > 0) {  // 返答がある場合

      String payload = http.getString();  // 返答（JSON形式）を取得
      print_time();
      Serial.print("HTTP:");
      Serial.print(httpCode);
      Serial.print(" data=");
      Serial.println(payload);

      // jsonオブジェクトの作成
      DynamicJsonDocument weatherdata(1024);

      // Deserialize the JSON document
      DeserializationError error = deserializeJson(weatherdata, payload);

      // Test if parsing succeeds.
      if (error) {
        print_time();
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
        return api_temp;
      }

      // 各データを抜き出し
      // const char* weather = weatherdata["weather"][0]["main"].as<char*>();
      const double temp = weatherdata["main"]["temp"].as<double>();
      // const double humidity = weatherdata["main"]["humidity"].as<double>();

      print_time();
      Serial.println("------------------------");
      // Serial.print("weather:");
      // Serial.println(weather);
      Serial.print("temperature:");
      Serial.println(temp - 273.15);
      // Serial.print("humidity:");
      // Serial.println(humidity);
      Serial.println("-----------------------------------");
      api_temp = round_int(temp - 273.15);
    } else {
      print_time();
      Serial.print("HTTP:");
      Serial.print(httpCode);
      Serial.print(" Error on HTTP request ");
      Serial.println(http.errorToString(httpCode));
    }

    http.end();  // リソースを解放
  }
  return api_temp;
}

// 5要素から中央値を取得
double mid5(double n1, double n2, double n3, double n4, double n5) {
  double buffer;
  double list[] = { n1, n2, n3, n4, n5 };
  for (int n = 0; n < 5; n++) {
    for (int m = n + 1; m < 5; m++) {
      if (list[m] > list[m + 1]) {
        buffer = list[m];
        list[m] = list[m + 1];
        list[m + 1] = buffer;
      }
    }
  }
  return list[2];
}

// 3要素から中央値を取得
double mid3(double n1, double n2, double n3) {
  // 並べ替えた時の2番目の値を返す
  if ((n1 >= n2 && n1 <= n3) || (n1 <= n2 && n1 >= n3)) {
    return n1;
  } else if ((n2 >= n1 && n2 <= n3) || (n2 <= n1 && n2 >= n3)) {
    return n2;
  } else {
    return n3;
  }
}

// 家の予測電力使用量
float culc_usage(float temperature) {
  float extreme_usage = 20.0;
  if (temperature <= 0.0) {
    return extreme_usage;
  } else if (temperature >= 40.0) {
    return extreme_usage;
  } else {
    float h = 20.0;
    float k = 6.0;
    float a = (14.0 - k) / ((5.0 - h) * (5.0 - h));
    return a * (temperature - h) * (temperature - h) + k;
  }
}

// エコキュートの予測電力使用量
float culc_ec_usage(float temperature) {
  // Power max and min
  float power_max = 6.0;
  float power_min = 2.0;

  // Temperature max and min
  float temp_max = 30.0;
  float temp_min = 5.0;

  if (temperature <= temp_min) {
    return power_max;
  } else if (temperature >= temp_max) {
    return power_min;
  } else {
    // Linear interpolation
    float power = power_max - ((power_max - power_min) / (temp_max - temp_min)) * (temperature - temp_min);
    return power;
  }
}

float get_nextday_temp() {
  float temp = 20;
  if ((WiFi.status() == WL_CONNECTED)) {
    HTTPClient http;
    http.begin(client, TEMP_API_URL_2);  // URLを指定
    int httpCode = http.GET();           // GETリクエストを送信
    if (httpCode > 0) {                  // 返答がある場合

      String payload = http.getString();  // 返答（JSON形式）を取得
      print_time();
      Serial.print("HTTP:");
      Serial.print(httpCode);
      Serial.print(" data=");
      Serial.println(payload);

      // jsonオブジェクトの作成
      DynamicJsonDocument weatherdata(32768);

      // Deserialize the JSON document
      DeserializationError error = deserializeJson(weatherdata, payload);

      tm_day day_time = get_day_time(9);

      int time_offset = 0;
      if (day_time.tm_hour > 13) {
        time_offset = 1;
      }

      int count = weatherdata["cnt"].as<double>();

      for (int i = 0; i < count; i++) {
        if (weatherdata["list"][i]["dt"].as<int>() == getUnixTimeAtNoonJST(get_now_unix_time() + (60 * 60 * 24 * time_offset))) {
          temp = weatherdata["list"][i]["main"]["temp_max"].as<float>() - 273.15;
          break;
        }
      }

      // Test if parsing succeeds.
      if (error) {
        print_time();
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
        return temp;
      }
      print_time();
      Serial.println("------------------------");
      Serial.print("temp[℃]:");
      Serial.println(temp);
      Serial.println("-----------------------------------");
    }

    else {
      print_time();
      Serial.print("HTTP:");
      Serial.print(httpCode);
      Serial.println(" Error on HTTP request");
    }

    http.end();  // リソースを解放
  }
  return temp;
}

SolarInfo get_solar_data_from_api() {
  SolarInfo result;
  int solar_percent = 0;
  if ((WiFi.status() == WL_CONNECTED)) {

    HTTPClient http;
    http.begin(client, FORECAST_SOLAR_API_URL);  // URLを指定
    int httpCode = http.GET();                   // GETリクエストを送信
    if (httpCode > 0) {                          // 返答がある場合

      String payload = http.getString();  // 返答（JSON形式）を取得
      print_time();
      Serial.print("HTTP:");
      Serial.print(httpCode);
      Serial.print(" data=");
      Serial.println(payload);

      // jsonオブジェクトの作成
      DynamicJsonDocument weatherdata(16384);

      // Deserialize the JSON document
      DeserializationError error = deserializeJson(weatherdata, payload);

      // Test if parsing succeeds.
      if (error) {
        print_time();
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
        return result;
      }

      tm_day day_time = get_day_time(9);

      int time_offset = 0;
      if (day_time.tm_hour > 13) {
        time_offset = 1;
      }
      char buffer[11];

      // Unix秒を日本時間のYYYY-MM-DD形式の文字列に変換
      unix_time_to_formatted_date_JST(get_now_unix_time() + (60 * 60 * 24 * time_offset), buffer, sizeof(buffer));

      float temp = get_nextday_temp();
      float raw_usage = culc_usage(temp);
      float raw_ec_usage = culc_ec_usage(temp);
      double raw_solar_data = weatherdata["result"][buffer].as<double>() / 1000;

      double but = 14.08;
      double but_ratio = 1;

      if (raw_solar_data < raw_usage + raw_ec_usage + (but / but_ratio)) {
        solar_percent = int((raw_usage + (but / but_ratio) - raw_solar_data) / but * 100.0);
        result.pre_heat_boiler = true;
      } else {
        solar_percent = int((raw_usage + raw_ec_usage + (but / but_ratio) - raw_solar_data) / but * 100.0);
        result.pre_heat_boiler = false;
      }
      solar_percent = min(solar_percent, 99);
      solar_percent = max(solar_percent, 0);
      result.percent = solar_percent;
      result.could_get_data = true;
      print_time();
      Serial.println("------------------------");
      Serial.print("raw solar data[kWh]:");
      Serial.println(raw_solar_data);
      Serial.print("chargeable percentage:");
      Serial.println(solar_percent);
      if (result.pre_heat_boiler == 1) {
        Serial.println("pre heat boiler:true");
      } else {
        Serial.println("pre heat boiler:false");
      }
      Serial.println("-----------------------------------");
    }

    else {
      print_time();
      Serial.print("HTTP:");
      Serial.print(httpCode);
      Serial.println(" Error on HTTP request");
    }

    http.end();  // リソースを解放
  }
  return result;
}

// EchoNetLite受け取り
int packetSize = 0;      // 受信データ量
byte *pdcedt = nullptr;  // テンポラリ
void echo_net_rcv() {
  packetSize = 0;
  pdcedt = nullptr;

  if (0 != (packetSize = echo.read())) {
    if (echo._rBuffer[EL_ESV] == EL_GET_RES) {
      char SEOJ_STR[7];
      sprintf(SEOJ_STR, "%02x%02x%02x", echo._rBuffer[EL_SEOJ], echo._rBuffer[EL_SEOJ + 1], echo._rBuffer[EL_SEOJ + 2]);
      print_time();
      Serial.println("------------------------");
      Serial.print("SEOJ: ");
      Serial.println(SEOJ_STR);
      Serial.print("EDT: ");
      Serial.println(echo._rBuffer[EL_EDT]);
      byte seoj[] = { echo._rBuffer[EL_SEOJ], echo._rBuffer[EL_SEOJ + 1], echo._rBuffer[EL_SEOJ + 2] };
      if (seoj[0] == 0x01 && seoj[1] == 0x30) {  // AC

        if (echo._rBuffer[EL_EDT] != 126) {
          if (echo._rBuffer[EL_EDT] > 128) {
            temp = echo._rBuffer[EL_EDT] - 256;
          } else {
            temp = echo._rBuffer[EL_EDT];
          }
          temp_error = 0;
          Serial.print("TEMP: ");
          Serial.println(temp);
          Serial.println("-----------------------------------");
        } else {
          Serial.println("could not get temp");
          int buffer = get_temp_from_api();
          if (buffer != 126) {
            temp = buffer;
            temp_error = 0;
          } else {
            temp_error += 1;
            Serial.print("could not get temp from api (try:");
            Serial.print(temp_error);
            Serial.println(")");
          }
        }
        show_temp(temp);

      }  // battery
      else if (seoj[0] == 0x02 && seoj[1] == 0x7d) {
        if (echo._rBuffer[EL_EDT] != 126) {
          battery_percent = echo._rBuffer[EL_EDT];
          Serial.print("BATTERY PERCENT: ");
          Serial.println(battery_percent);

          if (check_battery_once == false) {
            tm_day day_time = get_day_time(9);
            if (next_day_info.could_get_data == true) {
              // 夜充電
              if (next_day_info.percent != 0) {
                if (day_time.tm_hour < 7 || day_time.tm_hour == 23) {
                  // 運転モード指定
                  if (battery_percent < next_day_info.percent) {
                    charge_state = 1;
                  } else if (battery_percent > next_day_info.percent) {
                    charge_state = 2;
                  } else {
                    charge_state = 3;
                  }
                }
              }  // 夜充電なし(5時時点で5％以下なら5％まで充電、5月から9月は無効)
              else {
                // 運転モード指定
                if (day_time.tm_hour == 6 || day_time.tm_hour == 5) {
                  int month = get_month(get_now_unix_time());
                  if (month < 3 || month > 10) {
                    if (battery_percent < 5) {
                      charge_state = 1;
                    } else if (battery_percent == 5) {
                      charge_state = 3;
                    }
                  }
                }
              }
            }
            check_battery_once == true;
            Serial.print("CHARGE_STATE: ");
            Serial.println(charge_state);
          }
          Serial.println("-----------------------------------");
        }
      }
    }
  }
}

void printNetData() {
  print_time();
  Serial.println("------------------------");

  IPAddress ip = WiFi.localIP();
  Serial.print("IP  Address: ");
  Serial.println(ip);

  IPAddress dgwip = WiFi.gatewayIP();
  Serial.print("DGW Address: ");
  Serial.println(dgwip);

  IPAddress smip = WiFi.subnetMask();
  Serial.print("SM  Address: ");
  Serial.println(smip);

  Serial.println("-----------------------------------");
}