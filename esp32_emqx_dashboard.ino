/*
 * ESP32 <-> EMQX Cloud MQTT (TLS - port 8883)
 * Board: ESP32 / ESP32-S3
 *
 * Thư viện cần cài qua Library Manager:
 *   - PubSubClient (by Nick O'Leary)
 *   - ArduinoJson   (by Benoit Blanchon)  v6.x hoặc v7.x
 *
 * TÍNH NĂNG:
 * - 3 LED (đỏ / xanh / vàng), mỗi LED có 1 nút nhấn vật lý tương ứng để bật/tắt tại chỗ.
 * - Mỗi khi trạng thái LED thay đổi (do nút nhấn HOẶC do lệnh từ web), board publish
 *   ngay trạng thái mới nhất lên topic STATE (có retain=true) -> web luôn thấy dữ liệu
 *   mới nhất kể cả khi vừa mới subscribe.
 * - Board subscribe topic CMD để nhận lệnh điều khiển từ dashboard web (điều khiển 2 chiều).
 * - Vẫn publish heartbeat định kỳ (uptime, heap) mỗi 5s kèm trạng thái LED hiện tại.
 * 
 *
 * TOPIC MQTT:
 *   espplc/{DEVICE_ID}/state  -> board publish (retained) mỗi khi có thay đổi + heartbeat 5s
 *                                Payload: {"uptime":123,"heap":45000,"red":true,"green":false,"yellow":true}
 *   espplc/{DEVICE_ID}/cmd    -> web publish lệnh xuống board, chỉ cần gửi field muốn đổi
 *                                Ví dụ: {"red":true}  hoặc {"red":true,"yellow":false}
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ================== CẤU HÌNH ==================
const char* WIFI_SSID     = "PHUCLOC";
const char* WIFI_PASSWORD = "Phucloctho79";

// Lấy 3 thông tin này từ EMQX Cloud Dashboard > Deployment > Overview
const char* MQTT_HOST = "ic67b711.ala.asia-southeast1.emqxsl.com";  // Connection Address
const int   MQTT_PORT = 8883;                                      // TLS port

// Lấy từ tab Authentication trong Deployment
const char* MQTT_USER = "esp32_test";
const char* MQTT_PASS = "123456";

// Client ID phải DUY NHẤT cho mỗi thiết bị (nếu trùng, thiết bị cũ sẽ bị đá ra)
const char* MQTT_CLIENT_ID = "esp32_espplc_001";

// ID thiết bị dùng trong topic (nên trùng / liên quan tới MQTT_CLIENT_ID cho dễ theo dõi)
const char* DEVICE_ID = "esp32_espplc_001";

char TOPIC_STATE[64];
char TOPIC_CMD[64];

// ================== CHÂN LED / NÚT NHẤN ==================
const uint8_t LED_RED_PIN    = 22;
const uint8_t LED_GREEN_PIN  = 21;
const uint8_t LED_YELLOW_PIN = 17;

const uint8_t BTN_RED_PIN    = 23;
const uint8_t BTN_GREEN_PIN  = 18;
const uint8_t BTN_YELLOW_PIN = 19;

const unsigned long DEBOUNCE_MS = 40; // chống dội phím

struct BtnLed {
  uint8_t btnPin;
  uint8_t ledPin;
  const char* key;      // key dùng trong JSON: "red" / "green" / "yellow"
  bool ledState;         // trạng thái LED hiện tại
  bool stableState;      // trạng thái nút đã qua debounce (true = đang nhấn)
  bool lastReading;       // lần đọc gần nhất (chưa debounce)
  unsigned long lastChange;
};

BtnLed controls[3] = {
  { BTN_RED_PIN,    LED_RED_PIN,    "red",    false, false, false, 0 },
  { BTN_GREEN_PIN,  LED_GREEN_PIN,  "green",  false, false, false, 0 },
  { BTN_YELLOW_PIN, LED_YELLOW_PIN, "yellow", false, false, false, 0 },
};

volatile bool stateDirty = false; // set = true khi cần publish ngay (nút nhấn / có lệnh mới)

// Dán nội dung file CA cert (.pem) tải từ EMQX Cloud vào đây,
// giữ nguyên format kể cả dòng BEGIN/END CERTIFICATE
const char* ca_cert = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH
MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI
2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx
1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ
q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz
tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ
vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP
BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV
5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY
1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4
NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG
Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91
8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe
pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl
MrY=
-----END CERTIFICATE-----
)EOF";
// ================================================

WiFiClientSecure espClient;
PubSubClient client(espClient);

unsigned long lastPublish = 0;
const unsigned long PUBLISH_INTERVAL = 5000; // heartbeat mỗi 5s

void connectWiFi() {
  Serial.printf("Đang kết nối WiFi: %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("WiFi OK, IP: %s\n", WiFi.localIP().toString().c_str());
}

// Publish trạng thái hiện tại (uptime, heap, red, green, yellow) lên TOPIC_STATE
void publishState(bool retained) {
  StaticJsonDocument<256> doc;
  doc["uptime"] = millis() / 1000;
  doc["heap"]   = ESP.getFreeHeap();
  for (auto &c : controls) doc[c.key] = c.ledState;

  char buf[256];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  client.publish(TOPIC_STATE, buf, retained);
  Serial.printf("[MQTT] Publish state (%u bytes): %s\n", (unsigned)n, buf);
}

// Callback khi nhận được message từ topic đã subscribe (lệnh điều khiển từ web)
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.printf("[MQTT] Nhận từ topic '%s': %s\n", topic, msg.c_str());

  if (strcmp(topic, TOPIC_CMD) != 0) return; // chỉ xử lý topic lệnh

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err) {
    Serial.printf("[MQTT] Lỗi parse JSON lệnh: %s\n", err.c_str());
    return;
  }

  bool changed = false;
  for (auto &c : controls) {
    if (doc.containsKey(c.key)) {
      bool newVal = doc[c.key].as<bool>();
      if (newVal != c.ledState) {
        c.ledState = newVal;
        c.stableState = newVal ? false : c.stableState; // không ảnh hưởng debounce nút vật lý
        digitalWrite(c.ledPin, c.ledState ? HIGH : LOW);
        changed = true;
        Serial.printf("[CMD] LED %s -> %s\n", c.key, c.ledState ? "ON" : "OFF");
      }
    }
  }

  if (changed) publishState(true); // xác nhận ngay trạng thái mới về cho web
}

void connectMQTT() {
  espClient.setCACert(ca_cert);

  while (!client.connected()) {
    Serial.print("Đang kết nối MQTT broker...");
    if (client.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
      Serial.println(" thành công!");
      client.subscribe(TOPIC_CMD);
      Serial.printf("Đã subscribe topic lệnh: %s\n", TOPIC_CMD);
      publishState(true); // publish trạng thái ngay khi (re)connect
    } else {
      Serial.printf(" thất bại, mã lỗi=%d. Thử lại sau 3s\n", client.state());
      delay(3000);
    }
  }
}

// Đọc 3 nút nhấn có debounce, toggle LED tương ứng khi phát hiện cạnh nhấn xuống
void updateButtons() {
  for (auto &c : controls) {
    bool reading = (digitalRead(c.btnPin) == LOW); // LOW = đang nhấn (INPUT_PULLUP)

    if (reading != c.lastReading) {
      c.lastChange = millis();
    }

    if ((millis() - c.lastChange) > DEBOUNCE_MS) {
      if (reading != c.stableState) {
        c.stableState = reading;
        if (c.stableState) { // cạnh nhấn xuống -> toggle LED
          c.ledState = !c.ledState;
          digitalWrite(c.ledPin, c.ledState ? HIGH : LOW);
          Serial.printf("[BTN] Nút %s được nhấn -> LED %s = %s\n", c.key, c.key, c.ledState ? "ON" : "OFF");
          stateDirty = true;
        }
      }
    }

    c.lastReading = reading;
  }
}

void setup() {
  delay(2000);
  Serial.begin(115200);

  snprintf(TOPIC_STATE, sizeof(TOPIC_STATE), "espplc/%s/state", DEVICE_ID);
  snprintf(TOPIC_CMD,   sizeof(TOPIC_CMD),   "espplc/%s/cmd",   DEVICE_ID);

  for (auto &c : controls) {
    pinMode(c.ledPin, OUTPUT);
    digitalWrite(c.ledPin, LOW);
    pinMode(c.btnPin, INPUT_PULLUP);
  }

  connectWiFi();

  client.setServer(MQTT_HOST, MQTT_PORT);
  client.setCallback(mqttCallback);
  client.setBufferSize(512); // đủ chỗ cho JSON + tránh publish bị cắt
  connectMQTT();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  if (!client.connected()) {
    connectMQTT();
  }
  client.loop();

  updateButtons();

  if (stateDirty) {
    stateDirty = false;
    publishState(true); // đổi trạng thái do nút nhấn -> báo lên web ngay lập tức
  }

  // Heartbeat định kỳ (kèm trạng thái LED hiện tại) để web luôn biết thiết bị còn sống
  if (millis() - lastPublish > PUBLISH_INTERVAL) {
    lastPublish = millis();
    publishState(true);
  }
}
