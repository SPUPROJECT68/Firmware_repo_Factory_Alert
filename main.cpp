#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <esp_task_wdt.h>
#include <math.h>

// ============================================================================
// 1. ตั้งค่าโหมดการทำงาน
// ============================================================================
#define USE_MOCK_SENSORS   0   // 0 = อ่านค่าจากเซ็นเซอร์จริง (PMS5003 + DHT22), 1 = จำลองค่า
#define USE_EDGE_IMPULSE   1   // 1 = รันโมเดล Edge Impulse จริง (เปลี่ยนเป็น 1 เมื่อลงไลบรารีจาก Deployment แล้ว)

#if USE_EDGE_IMPULSE
  #include <Factory_Monitoring_inferencing.h>
#endif

// ============================================================================
// 2. ค่าคงที่และพารามิเตอร์ระบบ (Config)
// ============================================================================
namespace Config {
  constexpr const char* FW_VERSION = "1.9.1";

  // Wi-Fi & ThingsBoard Cloud
  constexpr const char* WIFI_SSID = "HOME";
  constexpr const char* WIFI_PASS = "0611486655";
  constexpr const char* TB_SERVER = "thingsboard.cloud";
  constexpr uint16_t    TB_PORT   = 1883;
  constexpr const char* TB_TOKEN  = "VbDeFq8i3MLM39YfEgyn";
  constexpr const char* CLIENT_ID = "ESP32-NODE-01";

  // Hardware Pins (ESP32-S3)
  constexpr int     DHT_PIN            = 5;     // DHT22 OUT (DATA) -> IO5
  constexpr uint8_t DHT_TYPE           = DHT22;
  constexpr int     PMS_RX_PIN         = 17;    // ESP32 RX2 (IO17)   <- PMS5003 TXD
  constexpr int     PMS_TX_PIN         = 16;    // ESP32 TX2 (IO16)   -> PMS5003 RXD
  constexpr int     BUZZER_PIN         = 18;    // Active Buzzer (+)  -> IO18
  constexpr bool    BUZZER_ACTIVE_HIGH = true;
  constexpr bool    BUZZER_ENABLED     = true;

  // Status LED Pins (ESP32-S3)
  constexpr int     LED_RED_PIN        = 42;    // LED สีแดง (Danger)  -> IO42
  constexpr int     LED_BLUE_PIN       = 41;    // LED สีฟ้า (Warning) -> IO41
  constexpr int     LED_GREEN_PIN      = 40;    // LED สีเขียว (Normal) -> IO40
  constexpr bool    LED_ACTIVE_HIGH    = false; // false = Common Anode (LOW ติด)

  // Timing & Buffer
  constexpr unsigned long TELEMETRY_MS     = 3000UL;   // ส่งข้อมูลทุก 3 วินาที
  constexpr unsigned long ATTR_SYNC_MS     = 5000UL;   // เช็กค่าเกณฑ์ล่าสุดจาก ThingsBoard อัตโนมัติทุก 5 วินาที
  constexpr unsigned long PMS_TIMEOUT_MS   = 10000UL;  // ถือว่า PMS5003 ขาดการติดต่อหากไม่มีเฟรมใหม่เกิน 10 วินาที
  constexpr unsigned long WIFI_RETRY_MS    = 10000UL;
  constexpr unsigned long MQTT_RETRY_MS    = 5000UL;
  constexpr uint32_t      WDT_TIMEOUT_SEC  = 15;
  constexpr size_t        MQTT_BUF_SIZE    = 1024;

  // ค่าเกณฑ์เริ่มต้น (ใช้แสดงสี LED เบื้องต้นก่อนได้รับค่าจริงจาก Dashboard/ThingsBoard)
  constexpr float DEFAULT_PM25_WARNING      = 35.0f;
  constexpr float DEFAULT_PM25_DANGER       = 75.0f;
  constexpr float DEFAULT_PM10_WARNING      = 50.0f;
  constexpr float DEFAULT_PM10_DANGER       = 100.0f;
  constexpr float DEFAULT_TEMP_WARNING      = 30.0f;
  constexpr float DEFAULT_TEMP_DANGER       = 35.0f;
  constexpr float DEFAULT_HUM_LOW           = 40.0f;
  constexpr float DEFAULT_HUM_HIGH          = 70.0f;
  constexpr float DEFAULT_HUM_DANGER_LOW    = 30.0f;
  constexpr float DEFAULT_HUM_DANGER_HIGH   = 80.0f;
  constexpr float DEFAULT_ANOMALY_WARNING   = 0.60f;
  constexpr float DEFAULT_ANOMALY_DANGER    = 0.80f;
} // namespace Config

// ============================================================================
// 3. โครงสร้างข้อมูลและตัวแปรสถานะ (Runtime State)
// ============================================================================
struct SensorReading {
  float pm25;
  float pm10;
  float temperature;
  float humidity;
  bool  pmsValid;
  bool  dhtValid;
  bool  valid;
};

struct AiResult {
  float       anomaly_score;
  float       confidence;
  const char* label;
  const char* status; // "normal", "warning", "danger", "sensor_error"
};

struct RuntimeConfig {
  float pm25Warning        = Config::DEFAULT_PM25_WARNING;
  float pm25Danger         = Config::DEFAULT_PM25_DANGER;
  float pm10Warning        = Config::DEFAULT_PM10_WARNING;
  float pm10Danger         = Config::DEFAULT_PM10_DANGER;
  float tempWarning        = Config::DEFAULT_TEMP_WARNING;
  float tempDanger         = Config::DEFAULT_TEMP_DANGER;
  float humidityLow        = Config::DEFAULT_HUM_LOW;
  float humidityHigh       = Config::DEFAULT_HUM_HIGH;
  float humidityDangerLow  = Config::DEFAULT_HUM_DANGER_LOW;
  float humidityDangerHigh = Config::DEFAULT_HUM_DANGER_HIGH;
  float anomalyWarning     = Config::DEFAULT_ANOMALY_WARNING;
  float anomalyDanger      = Config::DEFAULT_ANOMALY_DANGER;
  bool  buzzerMuted        = false;
  bool  thresholdsSynced   = false; // ปิดเสียง Buzzer ไว้จนกว่าจะได้รับค่าเกณฑ์จาก ThingsBoard/หน้าเว็บ
} runtimeCfg;

WiFiClient    espClient;
PubSubClient  mqtt(espClient);
DHT           dht(Config::DHT_PIN, Config::DHT_TYPE);
SensorReading lastSensors{};

const char* currentAlarmStatus = "normal";

// ============================================================================
// 4. ระบบควบคุม LED แสดงสถานะ (IO40, IO41, IO42) และ Active Buzzer (IO18)
// ============================================================================
void setLedHardware(bool redOn, bool greenOn, bool blueOn) {
  const uint8_t onLevel  = Config::LED_ACTIVE_HIGH ? HIGH : LOW;
  const uint8_t offLevel = Config::LED_ACTIVE_HIGH ? LOW  : HIGH;

  digitalWrite(Config::LED_RED_PIN,   redOn   ? onLevel : offLevel);
  digitalWrite(Config::LED_GREEN_PIN, greenOn ? onLevel : offLevel);
  digitalWrite(Config::LED_BLUE_PIN,  blueOn  ? onLevel : offLevel);
}

void updateStatusLed(const char* status) {
  if (strcmp(status, "danger") == 0) {
    // ขีดอันตราย -> ไฟสีแดง (GPIO 42)
    setLedHardware(true, false, false);
  } else if (strcmp(status, "normal") == 0) {
    // สถานะปกติ -> ไฟสีเขียว (GPIO 40)
    setLedHardware(false, true, false);
  } else {
    // สถานะไม่ปกติ ("warning" หรือ "sensor_error") -> ไฟสีฟ้า (GPIO 41)
    setLedHardware(false, false, true);
  }
}

void initStatusLed() {
  pinMode(Config::LED_RED_PIN, OUTPUT);
  pinMode(Config::LED_BLUE_PIN, OUTPUT);
  pinMode(Config::LED_GREEN_PIN, OUTPUT);
  updateStatusLed("normal");
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
  currentAlarmStatus = status;
  updateStatusLed(status);
}

void tickBuzzer() {
  // ยังไม่ส่งเสียงจนกว่าจะได้รับค่าเกณฑ์จาก ThingsBoard หรือหน้าเว็บ Dashboard
  if (!runtimeCfg.thresholdsSynced || runtimeCfg.buzzerMuted || !Config::BUZZER_ENABLED) {
    setBuzzerHardware(false);
    return;
  }

  const unsigned long now = millis();

  if (strcmp(currentAlarmStatus, "danger") == 0) {
    // เตือนระดับอันตราย: ดังถี่ 200ms / หยุด 200ms
    setBuzzerHardware((now % 400UL) < 200UL);
  } else if (strcmp(currentAlarmStatus, "warning") == 0) {
    // เตือนระดับเฝ้าระวัง: ดัง 150ms ทุกๆ 2 วินาที
    setBuzzerHardware((now % 2000UL) < 150UL);
  } else {
    setBuzzerHardware(false);
  }
}

// ============================================================================
// 5. ระบบอ่านเซ็นเซอร์ PMS5003 (UART2) และ DHT22
// ============================================================================
bool readPMS5003(float& pm25, float& pm10) {
  bool gotNewFrame = false;

  while (Serial2.available() >= 32) {
    if (Serial2.peek() != 0x42) {
      Serial2.read();
      continue;
    }

    uint8_t buf[32];
    Serial2.readBytes(buf, 32);

    if (buf[0] != 0x42 || buf[1] != 0x4D) continue;

    uint16_t sum = 0;
    for (int i = 0; i < 30; i++) {
      sum += buf[i];
    }
    const uint16_t checksum = (static_cast<uint16_t>(buf[30]) << 8) | buf[31];
    if (sum != checksum) continue;

    pm25 = static_cast<float>((static_cast<uint16_t>(buf[12]) << 8) | buf[13]);
    pm10 = static_cast<float>((static_cast<uint16_t>(buf[14]) << 8) | buf[15]);
    gotNewFrame = true;
  }

  return gotNewFrame;
}

SensorReading readSensors() {
  SensorReading s{};

#if USE_MOCK_SENSORS
  s.pm25        = 12.5f + (random(-15, 16) / 10.0f);
  s.pm10        = 24.0f + (random(-20, 21) / 10.0f);
  s.temperature = 28.5f + (random(-5, 6) / 10.0f);
  s.humidity    = 55.0f + (random(-10, 11) / 10.0f);
  s.pmsValid    = true;
  s.dhtValid    = true;
#else
  static float         lastPm25      = 0.0f;
  static float         lastPm10      = 0.0f;
  static unsigned long lastPmsUpdate = 0;
  static bool          hasPmsData    = false;

  if (readPMS5003(lastPm25, lastPm10)) {
    lastPmsUpdate = millis();
    hasPmsData    = true;
  }

  if (hasPmsData && (millis() - lastPmsUpdate <= Config::PMS_TIMEOUT_MS)) {
    s.pm25     = lastPm25;
    s.pm10     = lastPm10;
    s.pmsValid = true;
  } else {
    s.pm25     = 0.0f;
    s.pm10     = 0.0f;
    s.pmsValid = false;
    Serial.println("[SENSOR] Waiting for PMS5003 data...");
  }

  s.temperature = dht.readTemperature();
  s.humidity    = dht.readHumidity();

  if (!isnan(s.temperature) && !isnan(s.humidity) &&
      s.temperature >= -40.0f && s.temperature <= 125.0f &&
      s.humidity >= 0.0f && s.humidity <= 100.0f) {
    s.dhtValid = true;
  } else {
    s.dhtValid = false;
    Serial.println("[SENSOR] Failed to read from DHT22!");
  }
#endif

  s.valid = (s.pmsValid && s.dhtValid);
  return s;
}

// ============================================================================
// 6. ระบบประมวลผลสถานะและเปรียบเทียบเกณฑ์เดียวกับหน้าเว็บ Dashboard (app.js)
// ============================================================================
const char* evaluateThresholdStatus(const SensorReading& data) {
  // 1) ขีดอันตราย (Danger -> ไฟสีแดง GPIO 42)
  if (data.pm25 > runtimeCfg.pm25Danger ||
      data.pm10 > runtimeCfg.pm10Danger ||
      data.temperature >= runtimeCfg.tempDanger ||
      data.humidity < runtimeCfg.humidityDangerLow ||
      data.humidity > runtimeCfg.humidityDangerHigh) {
    return "danger";
  }

  // 2) สถานะไม่ปกติ/เฝ้าระวัง (Warning -> ไฟสีฟ้า GPIO 41)
  if (data.pm25 > runtimeCfg.pm25Warning ||
      data.pm10 > runtimeCfg.pm10Warning ||
      data.temperature > runtimeCfg.tempWarning ||
      data.humidity < runtimeCfg.humidityLow ||
      data.humidity > runtimeCfg.humidityHigh) {
    return "warning";
  }

  // 3) สถานะปกติ (Normal -> ไฟสีเขียว GPIO 40)
  return "normal";
}

const char* classifyAnomalyStatus(float score) {
  if (score >= runtimeCfg.anomalyDanger)  return "danger";
  if (score >= runtimeCfg.anomalyWarning) return "warning";
  return "normal";
}

const char* worstStatus(const char* a, const char* b) {
  if (strcmp(a, "danger") == 0 || strcmp(b, "danger") == 0) return "danger";
  if (strcmp(a, "warning") == 0 || strcmp(b, "warning") == 0) return "warning";
  return "normal";
}

AiResult runEdgeImpulseInference(const SensorReading& data) {
  if (!data.valid) {
    return AiResult{ 0.0f, 0.0f, "sensor_fault", "sensor_error" };
  }

  const char* sensorStatus = evaluateThresholdStatus(data);

#if USE_EDGE_IMPULSE
  // เรียงลำดับแกนข้อมูลให้ตรงกับ Edge Impulse Studio:
  // [0] temperature_2m (C), [1] relative_humidity_2m (%), [2] pm2_5 (g/m), [3] pm10 (g/m)
  float features[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE] = {
    data.temperature,
    data.humidity,
    data.pm25,
    data.pm10
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
  const char* bestLabel = sensorStatus;
  #if EI_CLASSIFIER_LABEL_COUNT > 0
    for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
      if (result.classification[ix].value > maxConf) {
        maxConf   = result.classification[ix].value;
        bestLabel = result.classification[ix].label;
      }
    }
  #else
    maxConf = 0.95f;
  #endif

  float anomalyScore = 0.0f;
  #if EI_CLASSIFIER_HAS_ANOMALY == 1
    anomalyScore = result.anomaly;
  #endif

  const char* aiStatus = classifyAnomalyStatus(anomalyScore);
  return AiResult{
    anomalyScore,
    maxConf,
    bestLabel,
    worstStatus(sensorStatus, aiStatus)
  };
#else
  float score = 0.10f;
  if (strcmp(sensorStatus, "danger") == 0) {
    score = 0.85f;
  } else if (strcmp(sensorStatus, "warning") == 0) {
    score = 0.65f;
  }

  return AiResult{
    score,
    0.95f,
    sensorStatus,
    sensorStatus
  };
#endif
}

// ============================================================================
// 7. การเชื่อมต่อ Wi-Fi, ThingsBoard MQTT และรับค่าเกณฑ์แบบ Real-time
// ============================================================================
void publishTelemetry(const SensorReading& s, const AiResult& ai);

bool parseJsonFloat(const char* json, const char* key, float& outValue) {
  char searchKey[48];
  snprintf(searchKey, sizeof(searchKey), "\"%s\"", key);

  const char* keyPtr = strstr(json, searchKey);
  if (!keyPtr) return false;

  const char* colon = strchr(keyPtr + strlen(searchKey), ':');
  if (!colon) return false;

  const char* valStart = colon + 1;
  while (*valStart == ' ' || *valStart == '\"') {
    valStart++;
  }

  char* endPtr = nullptr;
  float parsed = strtof(valStart, &endPtr);
  if (endPtr == valStart || isnan(parsed)) return false;

  outValue = parsed;
  return true;
}

void requestSharedAttributes() {
  if (!mqtt.connected()) return;
  static uint16_t reqId = 1;
  char topic[64];
  snprintf(topic, sizeof(topic), "v1/devices/me/attributes/request/%u", reqId++);
  mqtt.publish(
    topic,
    "{\"sharedKeys\":\"pm25Warning,pm25Danger,pm10Warning,pm10Danger,tempWarning,tempDanger,humidityLow,humidityHigh,humidityDangerLow,humidityDangerHigh,anomalyWarning,anomalyDanger,mute_buzzer\"}"
  );
}

void publishClientAttributes() {
  if (!mqtt.connected()) return;

  char attrPayload[256];
  snprintf(
    attrPayload,
    sizeof(attrPayload),
    "{\"fw_version\":\"%s\",\"ip_address\":\"%s\",\"dht_pin\":%d,\"pms_rx\":%d,\"pms_tx\":%d,\"buzzer_pin\":%d}",
    Config::FW_VERSION,
    WiFi.localIP().toString().c_str(),
    Config::DHT_PIN,
    Config::PMS_RX_PIN,
    Config::PMS_TX_PIN,
    Config::BUZZER_PIN
  );
  mqtt.publish("v1/devices/me/attributes", attrPayload);
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  char msg[768];
  const size_t copyLen = (length < sizeof(msg) - 1) ? length : (sizeof(msg) - 1);
  memcpy(msg, payload, copyLen);
  msg[copyLen] = '\0';

  if (strstr(msg, "\"buzzerMuted\":true") || strstr(msg, "\"mute_buzzer\":true")) {
    if (!runtimeCfg.buzzerMuted) {
      runtimeCfg.buzzerMuted = true;
      Serial.println("[SYS] Buzzer MUTED via ThingsBoard");
    }
  } else if (strstr(msg, "\"buzzerMuted\":false") || strstr(msg, "\"mute_buzzer\":false")) {
    if (runtimeCfg.buzzerMuted) {
      runtimeCfg.buzzerMuted = false;
      Serial.println("[SYS] Buzzer UNMUTED via ThingsBoard");
    }
  }

  bool gotThreshold = false;
  bool valueChanged = false;

  auto syncParam = [&](const char* key, float& target, const char* logFmt) {
    float incoming = target;
    if (parseJsonFloat(msg, key, incoming)) {
      gotThreshold = true;
      if (!runtimeCfg.thresholdsSynced || fabsf(incoming - target) > 0.001f) {
        target = incoming;
        valueChanged = true;
        Serial.printf(logFmt, target);
      }
    }
  };

  syncParam("pm25Warning",        runtimeCfg.pm25Warning,        "[CFG] pm25Warning = %.1f\n");
  syncParam("pm25Danger",         runtimeCfg.pm25Danger,         "[CFG] pm25Danger = %.1f\n");
  syncParam("pm10Warning",        runtimeCfg.pm10Warning,        "[CFG] pm10Warning = %.1f\n");
  syncParam("pm10Danger",         runtimeCfg.pm10Danger,         "[CFG] pm10Danger = %.1f\n");
  syncParam("tempWarning",        runtimeCfg.tempWarning,        "[CFG] tempWarning = %.1f\n");
  syncParam("tempDanger",         runtimeCfg.tempDanger,         "[CFG] tempDanger = %.1f\n");
  syncParam("humidityLow",        runtimeCfg.humidityLow,        "[CFG] humidityLow = %.1f\n");
  syncParam("humidityHigh",       runtimeCfg.humidityHigh,       "[CFG] humidityHigh = %.1f\n");
  syncParam("humidityDangerLow",  runtimeCfg.humidityDangerLow,  "[CFG] humidityDangerLow = %.1f\n");
  syncParam("humidityDangerHigh", runtimeCfg.humidityDangerHigh, "[CFG] humidityDangerHigh = %.1f\n");
  syncParam("anomalyWarning",     runtimeCfg.anomalyWarning,     "[CFG] anomalyWarning = %.2f\n");
  syncParam("anomalyDanger",      runtimeCfg.anomalyDanger,      "[CFG] anomalyDanger = %.2f\n");

  bool justArmed = false;
  if (gotThreshold && !runtimeCfg.thresholdsSynced) {
    runtimeCfg.thresholdsSynced = true;
    justArmed = true;
    Serial.println("[SYS] Thresholds synced from ThingsBoard -> Buzzer ARMED!");
  }

  // ทันทีที่ค่าเกณฑ์เปลี่ยนแปลง ให้ประมวลผลสีไฟ LED และ Buzzer ใหม่ทันทีโดยไม่ต้องรอรอบถัดไป
  if ((valueChanged || justArmed) && lastSensors.valid) {
    AiResult ai = runEdgeImpulseInference(lastSensors);
    updateAlarmStatus(ai.status);
    tickBuzzer();
    publishTelemetry(lastSensors, ai);
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
      mqtt.subscribe("v1/devices/me/attributes", 1);
      mqtt.subscribe("v1/devices/me/attributes/response/+", 1);
      mqtt.subscribe("v1/devices/me/rpc/request/+", 1);

      requestSharedAttributes();
      publishClientAttributes();
    } else {
      Serial.printf("Failed (rc=%d)\n", mqtt.state());
    }
  }
}

void publishTelemetry(const SensorReading& s, const AiResult& ai) {
  char payload[460];

  if (s.valid) {
    snprintf(
      payload,
      sizeof(payload),
      "{\"pm25\":%.1f,\"pm10\":%.1f,\"temperature\":%.1f,\"humidity\":%.1f,"
      "\"ai_status\":\"%s\",\"ai_label\":\"%s\",\"anomaly_score\":%.2f,\"ai_confidence\":%.2f,"
      "\"thresholds_synced\":%s,\"buzzer_muted\":%s,\"rssi\":%d,\"uptime_s\":%lu}",
      s.pm25, s.pm10, s.temperature, s.humidity,
      ai.status, ai.label, ai.anomaly_score, ai.confidence,
      runtimeCfg.thresholdsSynced ? "true" : "false",
      runtimeCfg.buzzerMuted ? "true" : "false",
      WiFi.RSSI(),
      millis() / 1000UL
    );
  } else if (s.pmsValid) {
    snprintf(
      payload,
      sizeof(payload),
      "{\"pm25\":%.1f,\"pm10\":%.1f,\"ai_status\":\"dht_error\",\"ai_label\":\"dht_fault\",\"thresholds_synced\":%s,\"rssi\":%d,\"uptime_s\":%lu}",
      s.pm25, s.pm10,
      runtimeCfg.thresholdsSynced ? "true" : "false",
      WiFi.RSSI(),
      millis() / 1000UL
    );
  } else if (s.dhtValid) {
    snprintf(
      payload,
      sizeof(payload),
      "{\"temperature\":%.1f,\"humidity\":%.1f,\"ai_status\":\"pms_warming_up\",\"ai_label\":\"pms_wait\",\"thresholds_synced\":%s,\"rssi\":%d,\"uptime_s\":%lu}",
      s.temperature, s.humidity,
      runtimeCfg.thresholdsSynced ? "true" : "false",
      WiFi.RSSI(),
      millis() / 1000UL
    );
  } else {
    snprintf(
      payload,
      sizeof(payload),
      "{\"ai_status\":\"sensor_error\",\"ai_label\":\"sensor_fault\",\"thresholds_synced\":%s,\"rssi\":%d,\"uptime_s\":%lu}",
      runtimeCfg.thresholdsSynced ? "true" : "false",
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
  if (esp_task_wdt_reconfigure(&wdt_config) != ESP_OK) {
    esp_task_wdt_init(&wdt_config);
  }
#else
  esp_task_wdt_init(Config::WDT_TIMEOUT_SEC, true);
#endif
  esp_task_wdt_add(nullptr);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Factory Edge AI Node Starting (Auto-Sync & Buzzer Standby) ===");

  Serial2.setRxBufferSize(256);
  Serial2.begin(9600, SERIAL_8N1, Config::PMS_RX_PIN, Config::PMS_TX_PIN);

  dht.begin();
  pinMode(Config::DHT_PIN, INPUT_PULLUP);
  delay(2000);

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

  const unsigned long now = millis();

  // ดึงค่าเกณฑ์ล่าสุดจาก ThingsBoard อัตโนมัติทุก 5 วินาที (ไม่ต้องกดปุ่ม Reset)
  static unsigned long lastAttrSync = 0;
  if (mqtt.connected() && (now - lastAttrSync >= Config::ATTR_SYNC_MS)) {
    lastAttrSync = now;
    requestSharedAttributes();
  }

  static unsigned long lastPublish = 0;
  if (now - lastPublish >= Config::TELEMETRY_MS) {
    lastPublish = now;

    lastSensors = readSensors();
    AiResult ai = runEdgeImpulseInference(lastSensors);

    updateAlarmStatus(ai.status);
    publishTelemetry(lastSensors, ai);
  }
}
