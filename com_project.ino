/*
 * =====================================================
 *  IoT-Based Health Monitoring System
 *  ESP32-S3 WROOM | MAX30102 | LM35 | OLED | Buzzer
 *  Course: EEE 330 | Group 08
 *  Protocol: MQTT with Multi-WiFi Failover
 *  Features: Multi-Patient, Doctor Feedback on OLED,
 *             Double-Press Button to Switch Patient
 * =====================================================
 */

#include "MAX30105.h"
#include "heartRate.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <PubSubClient.h>
#define MQTT_MAX_PACKET_SIZE 512  // Increase buffer to avoid silent drops
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiMulti.h>
#include <Wire.h>

// ==================== PIN DEFINITIONS ====================
// Sensor I2C Bus (Wire 0)
#define SENSOR_SDA_PIN  8
#define SENSOR_SCL_PIN  9

// OLED I2C Bus (Wire 1)
#define OLED_SDA_PIN    10
#define OLED_SCL_PIN    11
#define LM35_PIN     15   // ADC1_CH3
#define BUZZER_PIN   3
#define BUTTON_PIN   4   // Push button (wired between GPIO6 and GND)

// ==================== OLED CONFIG ========================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDR     0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire1, OLED_RESET);

// ==================== MAX30102 CONFIG ====================
MAX30105 particleSensor;

#define RATE_SIZE 8
byte rates[RATE_SIZE];
byte rateSpot = 0;
byte rateFilled = 0;    // how many slots actually have real data
long lastBeat = 0;
float beatsPerMinute = 0;
int beatAvg = 0;

long redValue = 0;
int spO2 = 0;

// SpO2 AC/DC filter state
long redDC = 0, irDC = 0;          // running DC (average)
long redAC = 0, irAC = 0;          // running AC magnitude

// ==================== LM35 / TEMPERATURE =================
#define TEMP_SAMPLES 10
float tempReadings[TEMP_SAMPLES];
byte tempIndex = 0;
bool tempBufferFull = false;
float temperatureF = 0.0;

// ==================== HEALTH THRESHOLDS ==================
#define TEMP_HIGH_THRESH  100.4
#define BPM_HIGH_THRESH   120
#define BPM_LOW_THRESH    40
#define SPO2_LOW_THRESH   90
#define IR_FINGER_THRESH  50000

// ==================== PATIENT (Multi-Patient Support) ====
#define MAX_PATIENTS 3
int currentPatient = 1;   // Default: Patient 1

// Returns MQTT topic string for given patient and field
String patientTopic(int patient, const char* field) {
  return String("group08/health/patient") + patient + "/" + field;
}

// ==================== WI-FI (MULTIPLE) ===================
WiFiMulti wifiMulti;
WiFiClientSecure espClient;

// ==================== MQTT CONFIG ========================
PubSubClient mqttClient(espClient);

const char *mqtt_server = "3af2a7e75dca42e1ba1e09b3d71602f9.s1.eu.hivemq.cloud";
const int   mqtt_port   = 8883;
const char *mqtt_user   = "Health";
const char *mqtt_pass   = "Me107645";

// ==================== TIMING (ms) ========================
#define SENSOR_INTERVAL    100
#define DISPLAY_INTERVAL   500
#define CLOUD_INTERVAL     1000
#define SERIAL_INTERVAL    2000
#define BUZZER_BEEP_ON     200
#define BUZZER_BEEP_OFF    300
#define FEEDBACK_SHOW_MS   5000   // How long to show doctor feedback on OLED

unsigned long lastSensorRead    = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastCloudUpdate   = 0;
unsigned long lastSerialLog     = 0;
unsigned long lastBuzzerToggle  = 0;
unsigned long lastMqttRetry     = 0;
bool buzzerState = false;

// ==================== BUTTON / DOUBLE-PRESS ==============
#define DOUBLE_PRESS_GAP 400  // Max ms between two presses to count as double

unsigned long lastButtonPress = 0;
bool waitingForSecondPress = false;

// ==================== DOCTOR FEEDBACK ON OLED ============
String feedbackMsg = "";
unsigned long feedbackShownAt = 0;
bool showingFeedback = false;

// ==================== STATE FLAGS ========================
bool fingerDetected = false;
bool isEmergency    = false;
bool wifiConnected  = false;

// ==================== HELPERS ============================
void showBootScreen(const char *msg, int delayMs) {
  display.clearDisplay();
  display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  display.setCursor(14, 4);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.print("Health Monitor");
  display.drawLine(4, 14, 123, 14, SSD1306_WHITE);
  display.setCursor(10, 28);
  display.print(msg);
  display.display();
  delay(delayMs);
}

// Show patient number on OLED briefly
void showPatientChange() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(25, 8);
  display.print("PATIENT SWITCH");
  display.drawLine(4, 20, 123, 20, SSD1306_WHITE);
  display.setCursor(35, 30);
  display.setTextSize(2);
  display.print("P");
  display.print(currentPatient);
  display.setTextSize(1);
  display.setCursor(20, 52);
  display.print("Double-press again");
  display.display();
  delay(1500);
}

// ==================== BUTTON HANDLER =====================
void handleButton() {
  // Button is INPUT_PULLUP: LOW means pressed
  if (digitalRead(BUTTON_PIN) == LOW) {
    unsigned long now = millis();

    // Debounce: ignore very rapid noise
    if (now - lastButtonPress < 80) return;

    if (waitingForSecondPress && (now - lastButtonPress) < DOUBLE_PRESS_GAP) {
      // ---- DOUBLE PRESS DETECTED ----
      waitingForSecondPress = false;

      // Unsubscribe from old patient feedback topic
      mqttClient.unsubscribe(patientTopic(currentPatient, "feedback").c_str());

      // Advance patient number (wraps around)
      currentPatient = (currentPatient % MAX_PATIENTS) + 1;
      Serial.printf("[BUTTON] Double-press! Switched to Patient %d\n", currentPatient);

      // Subscribe to new patient feedback topic
      mqttClient.subscribe(patientTopic(currentPatient, "feedback").c_str());

      // Clear vitals for fresh patient
      beatAvg = 0; spO2 = 0; temperatureF = 0;
      rateFilled = 0; rateSpot = 0;
      for (byte i = 0; i < RATE_SIZE; i++) rates[i] = 0;
      redDC = 0; irDC = 0; redAC = 0; irAC = 0;

      showPatientChange();
    } else {
      // First press — start waiting for a second
      waitingForSecondPress = true;
    }

    lastButtonPress = now;

    // Wait for button release to avoid repeat triggers
    while (digitalRead(BUTTON_PIN) == LOW) delay(10);
  }

  // If waited too long, cancel single-press wait
  if (waitingForSecondPress && (millis() - lastButtonPress) > DOUBLE_PRESS_GAP) {
    waitingForSecondPress = false;
    Serial.println("[BUTTON] Single press (no action)");
  }
}

// ==================== MQTT CALLBACK =====================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  String feedbackTopic = patientTopic(currentPatient, "feedback");
  String statusTopic   = patientTopic(currentPatient, "status");

  if (String(topic) == feedbackTopic) {
    Serial.printf("[FEEDBACK] Doctor says: %s\n", msg.c_str());
    feedbackMsg = msg;
    feedbackShownAt = millis();
    showingFeedback = true;
    // Quick beep to alert doctor message arrived
    digitalWrite(BUZZER_PIN, HIGH);
    delay(80);
    digitalWrite(BUZZER_PIN, LOW);
    delay(50);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(80);
    digitalWrite(BUZZER_PIN, LOW);
  }

  if (String(topic) == statusTopic) {
    Serial.printf("[STATUS] %s\n", msg.c_str());
  }
}

// ==================== WIFI CONNECT =======================
void connectWiFi() {
  espClient.setInsecure();
  wifiMulti.addAP("MiM", "Ha20202021");
  wifiMulti.addAP("SecondNetwork", "Password2");
  wifiMulti.addAP("mehedi", "12345678");

  showBootScreen("Connecting WiFi...", 0);
  Serial.print("[WiFi] Connecting...");

  int attempts = 0;
  while (wifiMulti.run() != WL_CONNECTED && attempts < 20) {
    delay(500); Serial.print("."); attempts++;
  }
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  Serial.println(wifiConnected ? "\n[WiFi] Connected!" : "\n[WiFi] Failed. Offline.");
  showBootScreen(wifiConnected ? "WiFi Connected!" : "WiFi Failed (Offline)", 800);
}

// ==================== MQTT RECONNECT =====================
void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastMqttRetry > 5000) {
      lastMqttRetry = now;
      Serial.print("[MQTT] Connecting...");
      String clientId = "ESP32Health-" + String(random(0xffff), HEX);
      if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
        Serial.println("CONNECTED!");
        // Subscribe to wildcard for current patient (catches ALL subtopics reliably)
        String wildcard = String("group08/health/patient") + currentPatient + "/#";
        bool ok = mqttClient.subscribe(wildcard.c_str(), 1); // QoS 1 = at-least-once delivery
        Serial.printf("[MQTT] Subscribed to %s: %s\n", wildcard.c_str(), ok ? "OK" : "FAIL");
        // Announce device online
        mqttClient.publish(patientTopic(currentPatient, "status").c_str(), "DEVICE ONLINE");
      } else {
        Serial.printf("failed, rc=%d (retrying later)\n", mqttClient.state());
      }
    }
  }
}

// ==================== SETUP ==============================
void setup() {
  Serial.begin(115200);
  Serial.println("\n===== Health Monitor Starting =====");

  // Initialize both hardware I2C buses
  Wire.begin(SENSOR_SDA_PIN, SENSOR_SCL_PIN); // Sensors on Wire 0
  Wire1.begin(OLED_SDA_PIN, OLED_SCL_PIN);    // Display on Wire 1
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP); // Button wired to GND

  analogReadResolution(12);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[OLED] Failed!"); for (;;);
  }

  showBootScreen("Initializing...", 1000);

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("[MAX30102] Not found!"); showBootScreen("MAX30102 ERROR!", 0); for (;;);
  }
  particleSensor.setup(60, 4, 2, 100, 411, 4096);
  particleSensor.setPulseAmplitudeRed(0x0A);
  particleSensor.setPulseAmplitudeIR(60);
  showBootScreen("Sensor Ready!", 500);

  for (int i = 0; i < TEMP_SAMPLES; i++) tempReadings[i] = 0;

  connectWiFi();

  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback); // Register callback for incoming messages

  showBootScreen("System Ready!", 1000);
  Serial.println("===== System Ready =====\n");
}

// ==================== LOOP ===============================
void loop() {
  unsigned long now = millis();

  // ----- WiFi & MQTT Maintenance -----
  // Use WiFi.status() NOT wifiMulti.run() in the main loop.
  // wifiMulti.run() scans all APs and blocks for hundreds of ms,
  // which starves mqttClient.loop() and causes connection drops.
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    // Try reconnecting WiFi every 10 seconds
    static unsigned long lastWifiRetry = 0;
    if (millis() - lastWifiRetry > 10000) {
      lastWifiRetry = millis();
      Serial.println("[WiFi] Reconnecting...");
      wifiMulti.run(); // Only call it here, not every loop iteration
    }
  } else {
    wifiConnected = true;
    if (!mqttClient.connected()) reconnectMQTT();
    mqttClient.loop(); // MUST run frequently: processes all incoming messages incl. feedback
  }

  // ----- Button Handling -----
  handleButton();

  // ---- 1. Heart Beat Detection ----
  long irValue = particleSensor.getIR();
  redValue = particleSensor.getRed();
  fingerDetected = (irValue > IR_FINGER_THRESH);

  if (fingerDetected && checkForBeat(irValue)) {
    unsigned long beatNow = millis();   // capture exact time of beat, not stale `now`
    long delta = beatNow - lastBeat;
    lastBeat = beatNow;
    beatsPerMinute = 60.0 / (delta / 1000.0);
    if (beatsPerMinute > 30 && beatsPerMinute < 255) {
      rates[rateSpot++] = (byte)beatsPerMinute;
      rateSpot %= RATE_SIZE;
      if (rateFilled < RATE_SIZE) rateFilled++;  // track how many slots are real
      beatAvg = 0;
      for (byte x = 0; x < rateFilled; x++) {    // only average real samples
        // walk backwards from rateSpot to get most recent rateFilled samples
        byte idx = (rateSpot + RATE_SIZE - 1 - x) % RATE_SIZE;
        beatAvg += rates[idx];
      }
      beatAvg /= rateFilled;
    }
  }

  // ---- 2. Sensor Reads (100ms) ----
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;

    int rawADC = analogRead(LM35_PIN);
    float voltage = rawADC * (3.3 / 4095.0);
    float currentTempC = voltage * 100.0;
    float currentTemp = currentTempC * 1.8 + 32.0;
    tempReadings[tempIndex++] = currentTemp;
    if (tempIndex >= TEMP_SAMPLES) { tempIndex = 0; tempBufferFull = true; }
    float sum = 0;
    int count = tempBufferFull ? TEMP_SAMPLES : tempIndex;
    for (int i = 0; i < count; i++) sum += tempReadings[i];
    if (count > 0) temperatureF = sum / count;

    if (fingerDetected && redValue > 0) {
      // Update DC baseline with slow EMA (α ~= 1/128)
      redDC = (redDC * 127 + redValue) / 128;
      irDC  = (irDC  * 127 + irValue)  / 128;

      // AC component = deviation from DC baseline
      long redACSample = redValue - redDC;
      long irACSample  = irValue  - irDC;

      // Running RMS-style magnitude (slow EMA of abs values)
      redAC = (redAC * 15 + abs(redACSample)) / 16;
      irAC  = (irAC  * 15 + abs(irACSample))  / 16;

      // Proper SpO2 R-value: (AC_red/DC_red) / (AC_ir/DC_ir)
      if (irAC > 0 && irDC > 0 && redDC > 0) {
        float R = ((float)redAC / (float)redDC) / ((float)irAC / (float)irDC);
        // Empirical calibration curve: SpO2 = 104 - 17*R
        int calcSpO2 = (int)(104.0 - 17.0 * R);
        if (calcSpO2 > 100) calcSpO2 = 100;
        if (calcSpO2 < 70)  calcSpO2 = 70;  // physiological floor
        spO2 = (spO2 == 0) ? calcSpO2 : (spO2 * 3 + calcSpO2) / 4;
      }
    }

    if (!fingerDetected) {
      beatAvg = 0; spO2 = 0; beatsPerMinute = 0;
      rateFilled = 0; rateSpot = 0;
      for (byte i = 0; i < RATE_SIZE; i++) rates[i] = 0;
      redDC = 0; irDC = 0; redAC = 0; irAC = 0;
    }
  }

  // ---- 3. Alert Logic ----
  isEmergency = fingerDetected && (
    temperatureF > TEMP_HIGH_THRESH ||
    beatAvg > BPM_HIGH_THRESH ||
    (beatAvg > 0 && beatAvg < BPM_LOW_THRESH) ||
    (spO2 > 0 && spO2 < SPO2_LOW_THRESH)
  );

  if (isEmergency) {
    if (now - lastBuzzerToggle >= (buzzerState ? BUZZER_BEEP_ON : BUZZER_BEEP_OFF)) {
      lastBuzzerToggle = now;
      buzzerState = !buzzerState;
      digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
    }
  } else {
    if (buzzerState) { buzzerState = false; digitalWrite(BUZZER_PIN, LOW); }
  }

  // ---- 4. OLED Display (500ms) ----
  if (now - lastDisplayUpdate >= DISPLAY_INTERVAL) {
    lastDisplayUpdate = now;
    display.clearDisplay();

    // Auto-clear feedback after FEEDBACK_SHOW_MS
    if (showingFeedback && (now - feedbackShownAt > FEEDBACK_SHOW_MS)) {
      showingFeedback = false;
    }

    if (showingFeedback) {
      // ---- DOCTOR FEEDBACK SCREEN ----
      display.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
      display.setTextSize(1);
      display.setCursor(20, 2);
      display.print("Doctor Message");
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 16);
      // Word wrap: print up to 21 chars per line across 3 lines
      display.print(feedbackMsg.substring(0, 21));
      display.setCursor(0, 26);
      if (feedbackMsg.length() > 21) display.print(feedbackMsg.substring(21, 42));
      display.setCursor(0, 36);
      if (feedbackMsg.length() > 42) display.print(feedbackMsg.substring(42, 63));
      // Countdown bar
      float progress = 1.0 - ((float)(now - feedbackShownAt) / FEEDBACK_SHOW_MS);
      display.drawRect(0, 55, SCREEN_WIDTH, 8, SSD1306_WHITE);
      display.fillRect(0, 55, (int)(SCREEN_WIDTH * progress), 8, SSD1306_WHITE);
    } else {
      // ---- NORMAL VITALS SCREEN ----
      // Header
      display.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
      display.setTextSize(1);
      display.setCursor(4, 2);
      display.print("P");
      display.print(currentPatient);
      display.print(" | Health Monitor");

      display.setTextColor(SSD1306_WHITE);
      // Status top-right
      display.setCursor(85, 2);
      if (!wifiConnected) display.print(" No WiFi");
      else if (!mqttClient.connected()) display.print(" No MQTT");
      else display.print("  ONLINE");

      display.setTextColor(SSD1306_WHITE);

      if (fingerDetected) {
        display.setCursor(0, 16);
        display.print("\x03 BPM:  ");
        display.setTextSize(2);
        display.setCursor(50, 14);
        display.print(beatAvg);
        display.setTextSize(1);

        display.setCursor(0, 33);
        display.print("  SpO2: "); display.print(spO2); display.print(" %");

        display.setCursor(0, 44);
        display.print("  Temp: "); display.print(temperatureF, 1); display.print(" F");

        if (isEmergency) {
          display.fillRect(0, 55, SCREEN_WIDTH, 9, SSD1306_WHITE);
          display.setTextColor(SSD1306_BLACK);
          display.setCursor(20, 56); display.print("!! ALERT !!");
          display.setTextColor(SSD1306_WHITE);
        } else {
          display.drawLine(0, 54, SCREEN_WIDTH, 54, SSD1306_WHITE);
          display.setCursor(25, 56); display.print("All Normal");
        }
      } else {
        display.setCursor(10, 25);
        display.print("Place finger on");
        display.setCursor(15, 38);
        display.print("the sensor...");
      }
    }
    display.display();
  }

  // ---- 5. MQTT Publish (1s) ----
  if (now - lastCloudUpdate >= CLOUD_INTERVAL) {
    lastCloudUpdate = now;
    if (wifiConnected && mqttClient.connected() && fingerDetected) {
      mqttClient.publish(patientTopic(currentPatient, "bpm").c_str(), String(beatAvg).c_str());
      mqttClient.publish(patientTopic(currentPatient, "spo2").c_str(), String(spO2).c_str());
      mqttClient.publish(patientTopic(currentPatient, "temp").c_str(), String(temperatureF, 1).c_str());
      mqttClient.publish(patientTopic(currentPatient, "alert").c_str(), isEmergency ? "DANGER: HIGH VITALS!" : "NORMAL");
      Serial.printf("[MQTT] Published: Patient %d\n", currentPatient);
    }
  }

  // ---- 6. Serial Diagnostics (2s) ----
  if (now - lastSerialLog >= SERIAL_INTERVAL) {
    lastSerialLog = now;
    Serial.printf("[DATA] P%d | Finger:%s | BPM:%d | SpO2:%d | Temp:%.1f | MQTT:%s\n",
      currentPatient, fingerDetected ? "YES":"NO",
      beatAvg, spO2, temperatureF, mqttClient.connected() ? "ON":"OFF");
  }
}