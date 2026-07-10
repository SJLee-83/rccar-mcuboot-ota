/*
 * ESP32 게이트웨이 — MCUboot RC카 
 *
 * 역할:
 *   WiFi -> MQTT 구독(RCCar/command) -> JSON 파싱 ->
 *   cmd_string을 SPI 1바이트(w/x/a/d/s/f)로 변환 -> Renesas RA6E1로 송신
 *
 * 체인:
 *   [Qt GUI] --MQTT--> [Broker(Rpi5)] --MQTT--> [ESP32(this)] --SPI--> [Renesas] --> 모터
 *
 * Renesas hal_entry.c 수신 규격:
 *   'w'=전진  'x'=후진  'a'=좌  'd'=우  's'=중립  'f'=정지
 *
 * 라이브러리(Arduino IDE):
 *   - PubSubClient (MQTT)
 *   - ArduinoJson
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <SPI.h>

// ---------- 사용자 설정 (환경에 맞게 수정) ----------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// MQTT 브로커(Rpi5 또는 mosquitto PC) IP
const char* MQTT_SERVER = "192.168.0.22";
const int   MQTT_PORT   = 1883;

const char* TOPIC_COMMAND = "RCCar/command";
const char* TOPIC_SENSING = "RCCar/sensing";  // (센서 역방향 사용 시)

// ---------- SPI 핀 (Renesas RA6E1 Slave <-> ESP32 Master) ----------
// 이 대화에서 확인된 배선:
//   Renesas P402-CS(GPIO5), P102-SCK(GPIO18),
//   P101-MOSI(GPIO23), P100-MISO(GPIO19)
//   GND 공통 필수
#define PIN_SCK   18
#define PIN_MISO  19
#define PIN_MOSI  23
#define PIN_SS     5

SPIClass* hspi = nullptr;

WiFiClient espClient;
PubSubClient mqtt(espClient);

// ---------- SPI 1바이트 송신 ----------
void spi_send_byte(uint8_t b) {
  hspi->beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_SS, LOW);
  hspi->transfer(b);
  digitalWrite(PIN_SS, HIGH);
  hspi->endTransaction();
}

// ---------- cmd_string -> SPI 1바이트 매핑 ----------
// Renesas 수신 규격에 맞춤
uint8_t cmd_to_byte(const char* cmd) {
  if (strcmp(cmd, "go")    == 0) return 'w';
  if (strcmp(cmd, "back")  == 0) return 'x';
  if (strcmp(cmd, "left")  == 0) return 'a';
  if (strcmp(cmd, "right") == 0) return 'd';
  if (strcmp(cmd, "mid")   == 0) return 's';
  if (strcmp(cmd, "stop")  == 0) return 'f';
  // start / exit 등은 주행 명령이 아니므로 무시(0)
  return 0;
}

// ---------- MQTT 수신 콜백 ----------
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  // command 토픽만 처리
  if (strcmp(topic, TOPIC_COMMAND) != 0) return;

  // JSON 파싱: {"time":..,"cmd_string":"go","arg_string":100,"is_finish":1}
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.print("JSON parse error: ");
    Serial.println(err.c_str());
    return;
  }

  const char* cmd = doc["cmd_string"] | "";
  Serial.printf("recv cmd: %s\n", cmd);

  uint8_t b = cmd_to_byte(cmd);
  if (b != 0) {
    spi_send_byte(b);
    Serial.printf("  -> SPI sent: '%c' (0x%02X)\n", b, b);
  } else {
    Serial.printf("  -> ignored (non-drive cmd)\n");
  }
}

// ---------- WiFi 연결 ----------
void setup_wifi() {
  Serial.printf("Connecting WiFi: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nWiFi connected. IP: %s\n", WiFi.localIP().toString().c_str());
}

// ---------- MQTT 재연결 ----------
void mqtt_reconnect() {
  while (!mqtt.connected()) {
    Serial.print("MQTT connecting...");
    String clientId = "ESP32-RCCar-" + String(random(0xffff), HEX);
    if (mqtt.connect(clientId.c_str())) {
      Serial.println("connected");
      mqtt.subscribe(TOPIC_COMMAND, 1);
      Serial.printf("subscribed: %s\n", TOPIC_COMMAND);
    } else {
      Serial.printf("failed rc=%d, retry in 2s\n", mqtt.state());
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // SPI 초기화 (HSPI)
  hspi = new SPIClass(HSPI);
  hspi->begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_SS);
  pinMode(PIN_SS, OUTPUT);
  digitalWrite(PIN_SS, HIGH);

  setup_wifi();
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqtt_callback);
}

void loop() {
  if (!mqtt.connected()) {
    mqtt_reconnect();
  }
  mqtt.loop();
}
