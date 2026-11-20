#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_task_wdt.h>
#include <math.h>

// ============================================================================
// 1. ตั้งค่าโหมดการทำงาน (เปลี่ยนเป็น 0 / 1 เมื่อพร้อมใช้ฮาร์ดแวร์และโมเดลจริง)
// ============================================================================
#define USE_MOCK_SENSORS   1   // 1 = จำลองค่าเซ็นเซอร์, 0 = อ่านจากเซ็นเซอร์จริง
#define USE_EDGE_IMPULSE   0   // 1 = รันโมเดล Edge Impulse จริง, 0 = จำลองผลลัพธ์ AI

#if USE_EDGE_IMPULSE
  // เปลี่ยนชื่อไฟล์ .h ให้ตรงกับไลบรารีที่ Export มาจาก Edge Impulse Studio
  #include <Uefa_ProjectSPU-project-1_inferencing.h>
#endif

// ============================================================================
// 2. ค่าคงที่และพารามิเตอร์ระบบ (Config)
// ============================================================================
namespace Config {
  constexpr const char* FW_VERSION = "1.2.1";

  // Wi-Fi & ThingsBoard
  constexpr const char* WIFI_SSID = "HUAWEI MATEBOOK UEFA";
  constexpr const char* WIFI_PASS = "Pooree2903";
  constexpr const char* TB_SERVER = "thingsboard.cloud";
  constexpr uint16_t    TB_PORT   = 1883;
  constexpr const char* TB_TOKEN  = "VbDeFq8i3MLM39YfEgyn";
  constexpr const char* CLIENT_ID = "ESP32-NODE-01";

  // Hardware Pins
  constexpr int  BUZZER_PIN         = 18;
  constexpr bool BUZZER_ACTIVE_HIGH = true;  // เปลี่ยนเป็น false หากโมดูลสั่งติดด้วย LOW
  constexpr bool BUZZER_ENABLED     = true;

  // RGB LED: R = GPIO 42, B = GPIO 41, G = GPIO 40
  constexpr int  LED_RED_PIN        = 42;
  constexpr int  LED_BLUE_PIN       = 41;
  constexpr int  LED_GREEN_PIN      = 40;
  constexpr bool LED_ACTIVE_HIGH    = false; // false = Common Anode (ถ้าเป็น Common Cathode ให้เปลี่ยนเป็น true)

  // Timing & Buffer
  constexpr unsigned long TELEMETRY_MS    = 3000UL;
  constexpr unsigned long WIFI_RETRY_MS   = 10000UL;
  constexpr unsigned long MQTT_RETRY_MS   = 5000UL;
  constexpr uint32_t      WDT_TIMEOUT_SEC = 15;
  constexpr size_t        MQTT_BUF_SIZE   = 512;

  // Default AI Thresholds (ปรับผ่าน ThingsBoard Shared Attributes ได้)
  constexpr float DEFAULT_WARNING_TH = 0.60f;
  constexpr float DEFAULT_DANGER_TH  = 0.80f;
} // namespace Config

// ============================================================================
// 3. โครงสร้างข้อมูลและตัวแปรสถานะ (Runtime State)
// ============================================================================
struct SensorReading {
  float pm25;
  float pm10;
  float temperature;
  float humidity;
  bool  valid;
};

struct AiResult {
  float       anomaly_score;
  float       confidence;
  const char* label;
  const char* status; // "normal", "warning", "danger", "sensor_error"
};

struct RuntimeConfig {
  float anomalyWarning = Config::DEFAULT_WARNING_TH;
  float anomalyDanger  = Config::DEFAULT_DANGER_TH;
  bool  buzzerMuted    = false;
} runtimeCfg;

WiFiClient   espClient;
PubSubClient mqtt(espClient);

const char* currentAlarmStatus = "normal";

// ควบคุมการร้องของ Buzzer เมื่อเข้าสู่สถานะไฟแดง
constexpr unsigned long RED_BUZZER_DURATION_MS = 3000UL;
bool buzzerAlarmActive = false;
unsigned long buzzerAlarmStartMs = 0;

bool isRedAlarmStatus(const char* status) {
  // สถานะอื่นนอกเหนือจาก normal และ warning จะแสดงไฟแดง
  return (status == nullptr ||
          (strcmp(status, "normal") != 0 && strcmp(status, "warning") != 0));
}

// ============================================================================
// 4. ระบบไฟ RGB LED และ Active Buzzer (Non-blocking)
// ============================================================================
void setLedPin(int pin, bool on) {
  const uint8_t level = on
    ? (Config::LED_ACTIVE_HIGH ? HIGH : LOW)
    : (Config::LED_ACTIVE_HIGH ? LOW : HIGH);
  digitalWrite(pin, level);
}

void turnOffAllStatusLeds() {
  // ปิด LED ทุกสีก่อนเลือกเปิดสีที่ต้องการ
  setLedPin(Config::LED_RED_PIN, false);
  setLedPin(Config::LED_BLUE_PIN, false);
  setLedPin(Config::LED_GREEN_PIN, false);
}

void setStatusLed(const char* status) {
  // ปิดทุกสีก่อน แล้วเว้นช่วงสั้น ๆ (break-before-make)
  // เพื่อไม่ให้มีการเปิด LED มากกว่าหนึ่งสีในจังหวะเปลี่ยนสถานะ
  turnOffAllStatusLeds();
  delayMicroseconds(1000);

  if (status != nullptr && strcmp(status, "normal") == 0) {
    setLedPin(Config::LED_GREEN_PIN, true);  // ปกติ = เขียว (GPIO 40)
  } else if (status != nullptr && strcmp(status, "warning") == 0) {
    setLedPin(Config::LED_BLUE_PIN, true);   // เฝ้าระวัง = ฟ้า (GPIO 41)
  } else {
    // อันตราย หรือเซ็นเซอร์/AI ผิดพลาด = แดง (GPIO 42)
    setLedPin(Config::LED_RED_PIN, true);
  }
}

void initStatusLed() {
  pinMode(Config::LED_RED_PIN, OUTPUT);
  pinMode(Config::LED_BLUE_PIN, OUTPUT);
  pinMode(Config::LED_GREEN_PIN, OUTPUT);

  // กำหนดให้ LED ทุกสีดับก่อนเริ่มระบบ
  turnOffAllStatusLeds();
  delayMicroseconds(1000);
  setStatusLed("normal");
}

void setBuzzerHardware(bool on) {
  if (!Config::BUZZER_ENABLED) on = false;
  const uint8_t level = on
    ? (Config::BUZZER_ACTIVE_HIGH ? HIGH : LOW)
    : (Config::BUZZER_ACTIVE_HIGH ? LOW : HIGH);
  digitalWrite(Config::BUZZER_PIN, level);
}

void initBuzzer() {
  pinMode(Config::BUZZER_PIN, OUTPUT);
  setBuzzerHardware(false);
}

void updateAlarmStatus(const char* status) {
  // หากไม่มีสถานะที่ถูกต้อง ให้ถือเป็น sensor_error และแสดงไฟแดง
  const bool wasRed = isRedAlarmStatus(currentAlarmStatus);
  currentAlarmStatus = (status != nullptr) ? status : "sensor_error";
  const bool isRed = isRedAlarmStatus(currentAlarmStatus);

  setStatusLed(currentAlarmStatus);

  // เริ่มร้อง 3 วินาทีเฉพาะตอนเปลี่ยนจากสถานะที่ไม่ใช่สีแดงเป็นสีแดง
  // หากยังอยู่สีแดงต่อเนื่อง จะไม่เริ่มจับเวลาใหม่ซ้ำ ๆ
  if (isRed && !wasRed) {
    buzzerAlarmStartMs = millis();
    buzzerAlarmActive = true;
  } else if (!isRed) {
    buzzerAlarmActive = false;
    setBuzzerHardware(false);
  }
}

void tickBuzzer() {
  if (runtimeCfg.buzzerMuted || !Config::BUZZER_ENABLED) {
    setBuzzerHardware(false);
    return;
  }

  if (buzzerAlarmActive &&
      (millis() - buzzerAlarmStartMs < RED_BUZZER_DURATION_MS)) {
    // Active Buzzer ทำงานต่อเนื่องเป็นเวลา 3 วินาที
    setBuzzerHardware(true);
  } else {
    buzzerAlarmActive = false;
    setBuzzerHardware(false);
  }
}

// ============================================================================
// 5. ระบบอ่านเซ็นเซอร์และการตรวจสอบความถูกต้อง (NaN Guard)
// ============================================================================
bool isReadingValid(const SensorReading& s) {
  if (isnan(s.pm25) || isnan(s.pm10) || isnan(s.temperature) || isnan(s.humidity)) {
    return false;
  }
  if (s.pm25 < 0.0f || s.pm10 < 0.0f) return false;
  if (s.temperature < -40.0f || s.temperature > 125.0f) return false;
  if (s.humidity < 0.0f || s.humidity > 100.0f) return false;
  return true;
}

SensorReading readSensors() {
  SensorReading s{};

#if USE_MOCK_SENSORS

  // สุ่มรูปแบบข้อมูล 0 = Normal, 1 = Warning, 2 = Danger
  int mockMode = random(0, 3);

  if (mockMode == 0) {
    // NORMAL : ค่าปกติ
    s.pm25        = 12.5f + (random(-15, 16) / 10.0f);
    s.pm10        = 24.0f + (random(-20, 21) / 10.0f);
    s.temperature = 28.0f + (random(-10, 11) / 10.0f);
    s.humidity    = 55.0f + (random(-50, 51) / 10.0f);
  }
  else if (mockMode == 1) {
    // WARNING : ค่าเริ่มเข้าใกล้ระดับเฝ้าระวัง
    s.pm25        = 30.0f + (random(-20, 21) / 10.0f);
    s.pm10        = 100.0f + (random(-50, 51) / 10.0f);
    s.temperature = 33.0f + (random(-10, 11) / 10.0f);
    s.humidity    = 68.0f + (random(-20, 21) / 10.0f);
  }
  else {
    // DANGER : ค่าสูงผิดปกติ ใช้ทดสอบระบบแจ้งเตือน
    s.pm25        = 85.0f + (random(-50, 51) / 10.0f);
    s.pm10        = 200.0f + (random(-100, 101) / 10.0f);
    s.temperature = 38.0f + (random(-10, 11) / 10.0f);
    s.humidity    = 80.0f + (random(-20, 21) / 10.0f);
  }

#else

  // อ่านค่าจากฮาร์ดแวร์จริง
  // เช่น PMS5003 / SHT31 / BME280

  // s.pm25        = ...;
  // s.pm10        = ...;
  // s.temperature = ...;
  // s.humidity    = ...;

#endif

s.valid = isReadingValid(s);
return s;
}

// ============================================================================
// 6. ระบบประมวลผล Edge Impulse AI
// ============================================================================
const char* classifyAnomalyStatus(float score) {
  if (score >= runtimeCfg.anomalyDanger)  return "danger";
  if (score >= runtimeCfg.anomalyWarning) return "warning";
  return "normal";
}

AiResult runEdgeImpulseInference(const SensorReading& data) {
  if (!data.valid) {
    return AiResult{ 0.0f, 0.0f, "sensor_fault", "sensor_error" };
  }

#if USE_EDGE_IMPULSE
  float features[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE] = {
    data.pm25,
    data.pm10,
    data.temperature,
    data.humidity
  };

  signal_t signal;
  int err = numpy::signal_from_buffer(features, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);
  if (err != 0) {
    Serial.printf("[AI] Signal error: %d\n", err);
    return AiResult{ 0.0f, 0.0f, "signal_error", "sensor_error" };
  }

  ei_impulse_result_t result = { 0 };
  EI_IMPULSE_ERROR res = run_classifier(&signal, &result, false);
  if (res != EI_IMPULSE_OK) {
    Serial.printf("[AI] Classifier error: %d\n", res);
    return AiResult{ 0.0f, 0.0f, "infer_error", "sensor_error" };
  }

  float maxConf = 0.0f;
  const char* bestLabel = "unknown";
  for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
    if (result.classification[ix].value > maxConf) {
      maxConf   = result.classification[ix].value;
      bestLabel = result.classification[ix].label;
    }
  }

  float anomalyScore = 0.0f;
  #if EI_CLASSIFIER_HAS_ANOMALY == 1
    anomalyScore = result.anomaly;
  #endif

  return AiResult{
    anomalyScore,
    maxConf,
    bestLabel,
    classifyAnomalyStatus(anomalyScore)
  };
#else
  // จำลองคะแนน AI จากค่าที่อ่านได้ เพื่อทดสอบ Normal / Warning / Danger
  float mockScore = 0.15f;

  if (data.pm25 >= 60.0f || data.pm10 >= 160.0f ||
      data.temperature >= 36.0f || data.humidity >= 90.0f) {
    mockScore = 0.90f;  // Danger
  } else if (data.pm25 >= 25.0f || data.pm10 >= 80.0f ||
             data.temperature >= 32.0f || data.humidity >= 65.0f) {
    mockScore = 0.70f;  // Warning
  }

  const char* mockStatus = classifyAnomalyStatus(mockScore);
  return AiResult{
    mockScore,
    0.94f,
    mockStatus,
    mockStatus
  };
#endif
}

// ============================================================================
// 7. การเชื่อมต่อ Wi-Fi, ThingsBoard MQTT และรับคำสั่งจาก Dashboard
// ============================================================================
void publishClientAttributes() {
  if (!mqtt.connected()) return;

  char attrPayload[256];
  snprintf(
    attrPayload,
    sizeof(attrPayload),
    "{\"fw_version\":\"%s\",\"ip_address\":\"%s\",\"buzzer_pin\":%d,\"mock_sensors\":%s,\"edge_impulse\":%s}",
    Config::FW_VERSION,
    WiFi.localIP().toString().c_str(),
    Config::BUZZER_PIN,
    USE_MOCK_SENSORS ? "true" : "false",
    USE_EDGE_IMPULSE ? "true" : "false"
  );
  mqtt.publish("v1/devices/me/attributes", attrPayload);
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  char msg[256];
  const size_t copyLen = (length < sizeof(msg) - 1) ? length : (sizeof(msg) - 1);
  memcpy(msg, payload, copyLen);
  msg[copyLen] = '\0';

  Serial.printf("[MQTT RX] %s -> %s\n", topic, msg);

  // สั่งปิด/เปิดเสียงเตือนจาก ThingsBoard (Shared Attributes หรือ RPC)
  if (strstr(msg, "\"buzzerMuted\":true") || strstr(msg, "\"mute_buzzer\":true")) {
    runtimeCfg.buzzerMuted = true;
    Serial.println("[SYS] Buzzer MUTED via ThingsBoard");
  } else if (strstr(msg, "\"buzzerMuted\":false") || strstr(msg, "\"mute_buzzer\":false")) {
    runtimeCfg.buzzerMuted = false;
    Serial.println("[SYS] Buzzer UNMUTED via ThingsBoard");
  }

  // อัปเดต Threshold จาก Shared Attributes
  char* warnPtr = strstr(msg, "\"anomaly_warning\":");
  if (warnPtr) {
    float val = strtof(warnPtr + 18, nullptr);
    if (val > 0.0f && val < 1.0f) runtimeCfg.anomalyWarning = val;
  }

  char* dangerPtr = strstr(msg, "\"anomaly_danger\":");
  if (dangerPtr) {
    float val = strtof(dangerPtr + 17, nullptr);
    if (val > 0.0f && val <= 2.0f) runtimeCfg.anomalyDanger = val;
  }

  // ตอบกลับ RPC Request
  if (strncmp(topic, "v1/devices/me/rpc/request/", 26) == 0) {
    const char* reqId = topic + 26;
    char respTopic[64];
    snprintf(respTopic, sizeof(respTopic), "v1/devices/me/rpc/response/%s", reqId);
    mqtt.publish(respTopic, "{\"status\":\"ok\"}");
  }
}

void handleWiFiNonBlocking() {
  if (WiFi.status() == WL_CONNECTED) return;

  static unsigned long lastAttempt = 0;
  const unsigned long now = millis();
  if (lastAttempt == 0 || (now - lastAttempt >= Config::WIFI_RETRY_MS)) {
    lastAttempt = now;
    Serial.println("[WiFi] Connecting...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(Config::WIFI_SSID, Config::WIFI_PASS);
  }
}

void handleMqttNonBlocking() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) return;

  static unsigned long lastAttempt = 0;
  const unsigned long now = millis();
  if (lastAttempt == 0 || (now - lastAttempt >= Config::MQTT_RETRY_MS)) {
    lastAttempt = now;
    Serial.print("[MQTT] Connecting to ThingsBoard... ");

    if (mqtt.connect(Config::CLIENT_ID, Config::TB_TOKEN, nullptr)) {
      Serial.println("Connected!");
      mqtt.subscribe("v1/devices/me/attributes");
      mqtt.subscribe("v1/devices/me/rpc/request/+");

      mqtt.publish("v1/devices/me/attributes/request/1", "{\"sharedKeys\":\"anomaly_warning,anomaly_danger,mute_buzzer\"}");
      publishClientAttributes();
    } else {
      Serial.printf("Failed (rc=%d)\n", mqtt.state());
    }
  }
}

void publishTelemetry(const SensorReading& s, const AiResult& ai) {
  char payload[384];

  if (s.valid) {
    snprintf(
      payload,
      sizeof(payload),
      "{\"pm25\":%.1f,\"pm10\":%.1f,\"temperature\":%.1f,\"humidity\":%.1f,"
      "\"ai_status\":\"%s\",\"ai_label\":\"%s\",\"anomaly_score\":%.2f,\"ai_confidence\":%.2f,"
      "\"buzzer_muted\":%s,\"rssi\":%d,\"uptime_s\":%lu}",
      s.pm25, s.pm10, s.temperature, s.humidity,
      ai.status, ai.label, ai.anomaly_score, ai.confidence,
      runtimeCfg.buzzerMuted ? "true" : "false",
      WiFi.RSSI(),
      millis() / 1000UL
    );
  } else {
    snprintf(
      payload,
      sizeof(payload),
      "{\"ai_status\":\"sensor_error\",\"ai_label\":\"sensor_fault\",\"rssi\":%d,\"uptime_s\":%lu}",
      WiFi.RSSI(),
      millis() / 1000UL
    );
  }

  Serial.println(payload);

  if (mqtt.connected()) {
    mqtt.publish("v1/devices/me/telemetry", payload);
  }
}

// ============================================================================
// 8. Watchdog, Setup & Main Loop
// ============================================================================
void initWatchdog() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = Config::WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
#else
  esp_task_wdt_init(Config::WDT_TIMEOUT_SEC, true);
#endif
  esp_task_wdt_add(nullptr);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Factory Edge AI Node Starting ===");

  initStatusLed();
  initBuzzer();
  initWatchdog();

  WiFi.setAutoReconnect(true);
  mqtt.setBufferSize(Config::MQTT_BUF_SIZE);
  mqtt.setServer(Config::TB_SERVER, Config::TB_PORT);
  mqtt.setCallback(onMqttMessage);
}

void loop() {
  esp_task_wdt_reset();

  handleWiFiNonBlocking();
  handleMqttNonBlocking();
  if (mqtt.connected()) {
    mqtt.loop();
  }

  tickBuzzer();

  static unsigned long lastPublish = 0;
  const unsigned long now = millis();
  if (now - lastPublish >= Config::TELEMETRY_MS) {
    lastPublish = now;

    SensorReading sensors = readSensors();
    AiResult ai = runEdgeImpulseInference(sensors);

    updateAlarmStatus(ai.status);
    publishTelemetry(sensors, ai);
  }
}