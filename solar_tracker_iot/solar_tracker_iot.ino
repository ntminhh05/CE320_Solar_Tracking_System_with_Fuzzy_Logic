// =========================================================
// DUAL-AXIS SOLAR TRACKER - FUZZY LOGIC + IoT MQTT
// Platform : ESP32 (Arduino Framework)
// Libraries: ESP32Servo, eFLL (Fuzzy), Adafruit_INA219,
//            NTPClient, WiFiUdp, PubSubClient, ArduinoJson
// =========================================================

#include <WiFi.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <ESP32Servo.h>
#include <Fuzzy.h>

// ---------------------------------------------------------
// 1. CẤU HÌNH PHẦN CỨNG
// ---------------------------------------------------------
// Định nghĩa các chân cho 4 con LDR
const int LDR_TL = 32; // Trên - Trái (Top-Left)
const int LDR_TR = 33; // Trên - Phải (Top-Right)
const int LDR_BL = 34; // Dưới - Trái (Bottom-Left)
const int LDR_BR = 35; // Dưới - Phải (Bottom-Right)

// Thêm các biến toàn cục để lưu giá trị đã lọc cho 4 cảm biến
float filteredTL = 0, filteredTR = 0, filteredBL = 0, filteredBR = 0;

// Hệ số lọc alpha (0 < alpha < 1). 
const float ALPHA = 0.2;

// Định nghĩa chân cho 2 Servo
const int SERVO_PAN_PIN = 12;  // Trục Ngang (Đế)
const int SERVO_TILT_PIN = 13; // Trục Dọc (Gật)

Servo servoPan;
Servo servoTilt;

// Các biến lưu góc hiện tại của Servo
float currentPanPos = 0;
float currentTiltPos = 90;

// Giới hạn góc quay để bảo vệ dây điện và cơ khí
const int PAN_MIN = 0;
const int PAN_MAX = 180;
const int TILT_MIN = 20; 
const int TILT_MAX = 160; 

// ---------------------------------------------------------
// 2. CẤU HÌNH IoT
// ---------------------------------------------------------
// *** THAY ĐỔI các thông tin sau cho phù hợp dự án ***
const char* WIFI_SSID     = "<wifi_đang_dùng>";
const char* WIFI_PASSWORD = "<pass wifi>";

const char* MQTT_BROKER   = "broker.hivemq.com";
const int   MQTT_PORT     = 1883;
const char* MQTT_TOPIC    = "uit/solar_tracker/data";
// Client ID nên là duy nhất để tránh conflict trên broker public
const char* MQTT_CLIENT_ID = "ESP32_SolarTracker_UIT_001";

// Khoảng thời gian publish MQTT (ms)
const unsigned long MQTT_PUBLISH_INTERVAL = 1000;
// Khoảng thời gian thử reconnect MQTT (ms) - tránh spam
const unsigned long MQTT_RECONNECT_INTERVAL = 5000;

// ---------------------------------------------------------
// 3. ĐỐI TƯỢNG TOÀN CỤC
// ---------------------------------------------------------
// --- IoT ---
WiFiClient      wifiClient;
PubSubClient    mqttClient(wifiClient);
WiFiUDP         ntpUDP;
// UTC+7 (Giờ Việt Nam) = 7 * 3600 = 25200 giây
NTPClient       timeClient(ntpUDP, "pool.ntp.org", 25200, 60000);
Adafruit_INA219 ina219;

// --- Fuzzy Logic ---
Fuzzy *fuzzyPan  = new Fuzzy();
Fuzzy *fuzzyTilt = new Fuzzy();

// --- Biến chia sẻ giữa Fuzzy loop và MQTT task ---
// Dùng volatile vì được đọc/ghi từ nhiều "ngữ cảnh" logic khác nhau
volatile float   g_errorPan  = 0;
volatile float   g_errorTilt = 0;
volatile float g_voltage   = 0.0;
volatile float g_current   = 0.0;
volatile float g_power     = 0.0;

// --- Bộ đếm thời gian không chặn ---
unsigned long lastMqttPublishTime  = 0;
unsigned long lastMqttReconnectTime = 0;
unsigned long lastInaReadTime      = 0;

// Khoảng thời gian đọc INA219 (ms) - 200ms là hợp lý
const unsigned long INA_READ_INTERVAL = 200;

// ==========================================================
// PHẦN A: CÀI ĐẶT FUZZY LOGIC (Giữ nguyên từ code gốc)
// ==========================================================
void setupFuzzyLogic(Fuzzy *fuzzySystem) {
  // --- INPUT: Sai số ánh sáng đã chuẩn hóa
  FuzzyInput *errorInput = new FuzzyInput(1);

  FuzzySet *errNB = new FuzzySet(-1.0, -1.0, -0.488, -0.122);
  FuzzySet *errNS = new FuzzySet(-0.488, -0.300, -0.122, -0.012); 
  FuzzySet *errZE = new FuzzySet(-0.049, -0.020, 0.020, 0.049);   
  FuzzySet *errPS = new FuzzySet(0.012, 0.122, 0.300, 0.488);     
  FuzzySet *errPB = new FuzzySet(0.122, 0.488, 1.0, 1.0);

  errorInput->addFuzzySet(errNB);
  errorInput->addFuzzySet(errNS);
  errorInput->addFuzzySet(errZE);
  errorInput->addFuzzySet(errPS);
  errorInput->addFuzzySet(errPB);
  fuzzySystem->addFuzzyInput(errorInput);

  // --- OUTPUT: Bước nhảy góc (Delta Angle) ---
  FuzzyOutput *deltaAngle = new FuzzyOutput(1);

  FuzzySet *MoveFastNeg = new FuzzySet(-5, -5, -4, -2); 
  FuzzySet *MoveSlowNeg = new FuzzySet(-3, -1, -0.5, 0); 
  FuzzySet *Stop        = new FuzzySet(-0.8, -0.2, 0.2, 0.8); 
  FuzzySet *MoveSlowPos = new FuzzySet(0, 0.5, 1, 3);    
  FuzzySet *MoveFastPos = new FuzzySet(2, 4, 5, 5);    

  deltaAngle->addFuzzySet(MoveFastNeg);
  deltaAngle->addFuzzySet(MoveSlowNeg);
  deltaAngle->addFuzzySet(Stop);
  deltaAngle->addFuzzySet(MoveSlowPos);
  deltaAngle->addFuzzySet(MoveFastPos);
  fuzzySystem->addFuzzyOutput(deltaAngle);

  // --- HỆ LUẬT MỜ (RULE BASE) ---
  // Luật 1: NẾU Error là NB THÌ quay Lùi Nhanh
  FuzzyRuleAntecedent *ifNB = new FuzzyRuleAntecedent();
  ifNB->joinSingle(errNB);
  FuzzyRuleConsequent *thenMFN = new FuzzyRuleConsequent();
  thenMFN->addOutput(MoveFastNeg);
  fuzzySystem->addFuzzyRule(new FuzzyRule(1, ifNB, thenMFN));

  // Luật 2: NẾU Error là NS THÌ quay Lùi Chậm
  FuzzyRuleAntecedent *ifNS = new FuzzyRuleAntecedent();
  ifNS->joinSingle(errNS);
  FuzzyRuleConsequent *thenMSN = new FuzzyRuleConsequent();
  thenMSN->addOutput(MoveSlowNeg);
  fuzzySystem->addFuzzyRule(new FuzzyRule(2, ifNS, thenMSN));

  // Luật 3: NẾU Error là ZE THÌ Đứng yên
  FuzzyRuleAntecedent *ifZE = new FuzzyRuleAntecedent();
  ifZE->joinSingle(errZE);
  FuzzyRuleConsequent *thenStop = new FuzzyRuleConsequent();
  thenStop->addOutput(Stop);
  fuzzySystem->addFuzzyRule(new FuzzyRule(3, ifZE, thenStop));

  // Luật 4: NẾU Error là PS THÌ quay Tiến Chậm
  FuzzyRuleAntecedent *ifPS = new FuzzyRuleAntecedent();
  ifPS->joinSingle(errPS);
  FuzzyRuleConsequent *thenMSP = new FuzzyRuleConsequent();
  thenMSP->addOutput(MoveSlowPos);
  fuzzySystem->addFuzzyRule(new FuzzyRule(4, ifPS, thenMSP));

  // Luật 5: NẾU Error là PB THÌ quay Tiến Nhanh
  FuzzyRuleAntecedent *ifPB = new FuzzyRuleAntecedent();
  ifPB->joinSingle(errPB);
  FuzzyRuleConsequent *thenMFP = new FuzzyRuleConsequent();
  thenMFP->addOutput(MoveFastPos);
  fuzzySystem->addFuzzyRule(new FuzzyRule(5, ifPB, thenMFP));
}

// Hàm EMA Filter
float EMA_Filter(float current_raw, float last_filtered, float alpha) {
  return (alpha * current_raw) + ((1.0 - alpha) * last_filtered);
}

// ==========================================================
// PHẦN B: CÁC HÀM IoT MỚI
// ==========================================================

// ----------------------------------------------------------
// B1. setupWiFi() - Kết nối Wi-Fi (blocking OK trong setup)
// ----------------------------------------------------------
void setupWiFi() {
  Serial.print("[WiFi] Connecting to: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Chờ tối đa 15 giây - chỉ blocking trong setup(), chấp nhận được
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected! IP: " + WiFi.localIP().toString());
  } else {
    // Không kết nối được nhưng KHÔNG treo chương trình
    // ESP32 sẽ tự reconnect trong loop() khi cần publish
    Serial.println("\n[WiFi] Failed to connect. Will retry in loop.");
  }
}

// ----------------------------------------------------------
// B2. setupMQTT() - Cấu hình MQTT client
// ----------------------------------------------------------
void setupMQTT() {
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  // Tăng buffer size để JSON không bị cắt xén
  mqttClient.setBufferSize(512);
  Serial.println("[MQTT] Client configured -> " + String(MQTT_BROKER));
}

// ----------------------------------------------------------
// B3. reconnectMQTT() - Thử kết nối lại MQTT (non-blocking)
//     Trả về true nếu đã kết nối, false nếu chưa
// ----------------------------------------------------------
bool reconnectMQTT() {
  // Chỉ thử reconnect theo khoảng MQTT_RECONNECT_INTERVAL
  unsigned long now = millis();
  if (now - lastMqttReconnectTime < MQTT_RECONNECT_INTERVAL) {
    return false; // Chưa đến lúc thử lại
  }
  lastMqttReconnectTime = now;

  // Kiểm tra Wi-Fi trước
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Disconnected! Reconnecting...");
    WiFi.reconnect();
    return false;
  }

  Serial.print("[MQTT] Attempting connection... ");
  // Thử connect với Client ID. Không dùng username/password (broker public)
  if (mqttClient.connect(MQTT_CLIENT_ID)) {
    Serial.println("Connected!");
    return true;
  } else {
    Serial.print("Failed, rc=");
    Serial.println(mqttClient.state());
    return false;
  }
}

// ----------------------------------------------------------
// B4. setupINA219() - Khởi tạo cảm biến điện INA219 (I2C)
// ----------------------------------------------------------
void setupINA219() {
  // INA219 mặc định địa chỉ I2C là 0x40
  if (!ina219.begin()) {
    Serial.println("[INA219] ERROR: Sensor not found! Check wiring (SDA/SCL).");
    // Không treo chương trình, g_voltage/current/power sẽ = 0
  } else {
    // Cấu hình range phù hợp với pin mặt trời nhỏ (32V, 1A)
    ina219.setCalibration_32V_1A();
    Serial.println("[INA219] Initialized (32V/1A mode).");
  }
}

// ----------------------------------------------------------
// B5. readPower() - Đọc dữ liệu điện từ INA219 (non-blocking)
//     Gọi trong loop(), chỉ đọc sau mỗi INA_READ_INTERVAL ms
// ----------------------------------------------------------
void readPower() {
  if (millis() - lastInaReadTime < INA_READ_INTERVAL) return;
  lastInaReadTime = millis();

  // Đọc điện áp bus (V) và dòng điện (mA -> A)
  float busVoltage_V  = ina219.getBusVoltage_V();
  float current_mA    = ina219.getCurrent_mA();

  // Loại bỏ nhiễu: nếu dòng âm (lỗi offset INA219), gán về 0
  if (current_mA < 0) current_mA = 0;

  // Cập nhật biến toàn cục
  g_voltage = busVoltage_V;
  g_current = current_mA / 1000.0; // Chuyển mA -> A
  g_power   = g_voltage * g_current; // P = U * I (Watt)
}

// ----------------------------------------------------------
// B6. publishData() - Đóng gói JSON và publish MQTT (non-blocking)
//     Gọi trong loop(), chỉ publish sau mỗi MQTT_PUBLISH_INTERVAL ms
// ----------------------------------------------------------
void publishData() {
  if (millis() - lastMqttPublishTime < MQTT_PUBLISH_INTERVAL) return;
  lastMqttPublishTime = millis();

  // Kiểm tra kết nối MQTT, thử reconnect nếu cần
  if (!mqttClient.connected()) {
    reconnectMQTT();
    // Nếu vẫn chưa kết nối được thì bỏ qua lần publish này
    if (!mqttClient.connected()) return;
  }

  // Cập nhật NTP time (hàm này non-blocking, chỉ cập nhật khi đến interval)
  timeClient.update();

  // --- Đóng gói JSON ---
  // Dùng StaticJsonDocument để tránh heap fragmentation
  StaticJsonDocument<256> doc;

  doc["time"]      = timeClient.getEpochTime(); // Unix timestamp UTC+7
  doc["timeStr"]   = timeClient.getFormattedTime(); // "HH:MM:SS"
  doc["voltage"]   = serialized(String(g_voltage, 3)); // 3 chữ số thập phân
  doc["current"]   = serialized(String(g_current, 4));
  doc["power"]     = serialized(String(g_power, 4));
  doc["errorPan"]  = (int)g_errorPan;
  doc["errorTilt"] = (int)g_errorTilt;
  doc["panAngle"]  = (int)currentPanPos;
  doc["tiltAngle"] = (int)currentTiltPos;

  // Serialize JSON thành string
  char jsonBuffer[256];
  size_t jsonLength = serializeJson(doc, jsonBuffer);

  // --- Publish lên MQTT ---
  bool success = mqttClient.publish(MQTT_TOPIC, jsonBuffer, jsonLength);

  if (success) {
    Serial.print("[MQTT] Published -> ");
    Serial.println(jsonBuffer);
  } else {
    Serial.println("[MQTT] Publish FAILED (buffer overflow?)");
    // Có thể tăng mqttClient.setBufferSize() nếu cần
  }
}

// ==========================================================
// PHẦN C: SETUP VÀ LOOP
// ==========================================================

void setup() {
  Serial.begin(115200);
  Serial.println("\n==============================");
  Serial.println(" Fuzzy Logic Solar Tracker IoT");
  Serial.println("==============================");

  // --- Cấu hình Servo ---
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  servoPan.setPeriodHertz(50);
  servoTilt.setPeriodHertz(50);
  servoPan.attach(SERVO_PAN_PIN, 500, 2400);
  servoTilt.attach(SERVO_TILT_PIN, 500, 2400);
  servoPan.write(currentPanPos);
  servoTilt.write(currentTiltPos);
  delay(1000);

  // --- Khởi tạo Fuzzy Logic ---
  setupFuzzyLogic(fuzzyPan);
  setupFuzzyLogic(fuzzyTilt);
  Serial.println("[Fuzzy] Controllers initialized.");

  // --- Khởi tạo I2C và INA219 ---
  // ESP32 mặc định SDA=21, SCL=22; thay đổi nếu nối dây khác
  Wire.begin(21, 22);
  setupINA219();

  // --- Kết nối Wi-Fi và MQTT ---
  setupWiFi();
  setupMQTT();

  // --- Khởi động NTP Client ---
  timeClient.begin();
  timeClient.forceUpdate(); // Đồng bộ ngay lần đầu
  Serial.println("[NTP] Time: " + timeClient.getFormattedTime());

  Serial.println("[System] Setup complete. Entering main loop...\n");
}

void loop() {
  // =====================================================
  // TÁC VỤ 1: FUZZY LOGIC TRACKER (~100ms/cycle)
  // Đây là tác vụ thời gian thực - KHÔNG bị delay bởi IoT
  // =====================================================
  int rawTL = analogRead(LDR_TL);
  int rawTR = analogRead(LDR_TR);
  int rawBL = analogRead(LDR_BL);
  int rawBR = analogRead(LDR_BR);

  // Lọc dữ liệu bằng EMA
  filteredTL = EMA_Filter(rawTL, filteredTL, ALPHA);
  filteredTR = EMA_Filter(rawTR, filteredTR, ALPHA);
  filteredBL = EMA_Filter(rawBL, filteredBL, ALPHA);
  filteredBR = EMA_Filter(rawBR, filteredBR, ALPHA);

  // Tính toán sai số sử dụng giá trị đã lọc + chuẩn hóa
  g_errorPan = (float)((filteredTR + filteredBR) - (filteredTL + filteredBL)) / (filteredTL + filteredTR + filteredBL + filteredBR);
  g_errorTilt = (float)((filteredTL + filteredTR) - (filteredBL + filteredBR)) / (filteredTL + filteredTR + filteredBL + filteredBR);


  // Fuzzy Pan
  fuzzyPan->setInput(1, g_errorPan);
  fuzzyPan->fuzzify();
  float deltaPan = fuzzyPan->defuzzify(1);

  // Fuzzy Tilt
  fuzzyTilt->setInput(1, g_errorTilt);
  fuzzyTilt->fuzzify();
  float deltaTilt = fuzzyTilt->defuzzify(1);

  // Cập nhật và constrain góc
  currentPanPos  = constrain(currentPanPos  + deltaPan,  PAN_MIN,  PAN_MAX);
  currentTiltPos = constrain(currentTiltPos + deltaTilt, TILT_MIN, TILT_MAX);

  servoPan.write(currentPanPos);
  servoTilt.write(currentTiltPos);

  // =====================================================
  // TÁC VỤ 2: ĐỌC CẢM BIẾN INA219 (non-blocking, 200ms)
  // Hàm tự kiểm tra thời gian, return ngay nếu chưa đến lúc
  // =====================================================
  readPower();

  // =====================================================
  // TÁC VỤ 3: XỬ LÝ MQTT (non-blocking, 1000ms)
  // mqttClient.loop() phải được gọi liên tục để duy trì kết nối
  // =====================================================
  mqttClient.loop();
  publishData();

  // =====================================================
  // CHU KỲ 100ms của Fuzzy Logic - Dùng delay() chỉ ở đây
  // Đây là delay duy nhất, đủ ngắn để không ảnh hưởng IoT
  // (MQTT reconnect timeout = 5000ms >> 100ms)
  // =====================================================
  delay(100);
}
