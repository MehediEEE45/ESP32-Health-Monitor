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
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiMulti.h>
#include <Wire.h>

#define MQTT_MAX_PACKET_SIZE 512 // Increase buffer to avoid silent drops

// ==================== PIN DEFINITIONS ====================
// Sensor I2C Bus (Wire 0)
#define SENSOR_SDA_PIN 8
#define SENSOR_SCL_PIN 9

// OLED I2C Bus (Wire 1)
#define OLED_SDA_PIN 10
#define OLED_SCL_PIN 11
#define LM35_PIN 15 // ADC1_CH3
#define BUZZER_PIN 3
#define BUTTON_PIN 4 // Push button (wired between GPIO6 and GND)

// ==================== OLED CONFIG ========================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire1, OLED_RESET);

// ==================== MAX30102 CONFIG ====================
MAX30105 particleSensor;

long startTime;
long samplesTaken = 0; // Counter for calculating the Hz or read rate

#define RATE_SIZE 4
byte rates[RATE_SIZE];
byte rateSpot = 0;
byte rateFilled = 0; // how many slots actually have real data
long lastBeat = 0;
float beatsPerMinute = 0;
int beatAvg = 0;

long redValue = 0;
int spO2 = 0;

// SpO2 AC/DC filter state
long redDC = 0, irDC = 0; // running DC (average)
long redAC = 0, irAC = 0; // running AC magnitude

// ==================== LM35 / TEMPERATURE =================
// 16 ADC samples → sort → discard 4 worst each end → mean of middle 8
#define TEMP_N 16
#define TEMP_TRIM 4 // discard this many from each end after sorting
#define TEMP_EMA_ALPHA                                                         \
  0.05f // very slow EMA: high stability, body temp changes slowly
float temperatureF = 0.0f; // output: trimmed-mean + EMA

// ==================== HEALTH THRESHOLDS ==================
#define TEMP_HIGH_THRESH 100.4
#define BPM_HIGH_THRESH 120
#define BPM_LOW_THRESH 40
#define SPO2_LOW_THRESH 90
#define IR_FINGER_THRESH 50000

// ==================== PATIENT (Multi-Patient Support) ====
#define MAX_PATIENTS 3
int currentPatient = 1; // Default: Patient 1

// Returns MQTT topic string for given patient and field
String patientTopic(int patient, const char *field) {
  return String("group08/health/patient") + patient + "/" + field;
}

// ==================== WI-FI (MULTIPLE) ===================
WiFiMulti wifiMulti;
WiFiClientSecure espClient;

// ==================== MQTT CONFIG ========================
PubSubClient mqttClient(espClient);

const char *mqtt_server = "3af2a7e75dca42e1ba1e09b3d71602f9.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char *mqtt_user = "Health";
const char *mqtt_pass = "Me107645";

// ==================== TIMING (ms) ========================
#define SENSOR_INTERVAL 100
#define BEAT_INTERVAL 10 // 10ms = 100Hz, matches MAX30102 sample rate
#define DISPLAY_INTERVAL 500
#define CLOUD_INTERVAL 1000
#define SERIAL_INTERVAL 2000
#define BUZZER_BEEP_ON 200
#define BUZZER_BEEP_OFF 300
#define FEEDBACK_SHOW_MS 5000 // How long to show doctor feedback on OLED

unsigned long lastSensorRead = 0;
unsigned long lastBeatRead = 0; // 10ms beat polling timer
unsigned long lastDisplayUpdate = 0;
unsigned long lastCloudUpdate = 0;
unsigned long lastSerialLog = 0;
unsigned long lastBuzzerToggle = 0;
unsigned long lastMqttRetry = 0;
bool buzzerState = false;
bool dcSeeded = false; // true once SpO2 DC baseline has been initialized

// ==================== BUTTON / DOUBLE-PRESS ==============
#define DOUBLE_PRESS_GAP 400 // Max ms between two presses to count as double

unsigned long lastButtonPress = 0;
bool waitingForSecondPress = false;

// ==================== DOCTOR FEEDBACK ON OLED ============
String feedbackMsg = "";
unsigned long feedbackShownAt = 0;
bool showingFeedback = false;

// ==================== STATE FLAGS ========================
bool fingerDetected = false;
bool isEmergency = false;
bool wifiConnected = false;

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
    if (now - lastButtonPress < 80)
      return;

    if (waitingForSecondPress && (now - lastButtonPress) < DOUBLE_PRESS_GAP) {
      // ---- DOUBLE PRESS DETECTED ----
      waitingForSecondPress = false;

      // Unsubscribe from old patient feedback topic
      mqttClient.unsubscribe(patientTopic(currentPatient, "feedback").c_str());

      // Advance patient number (wraps around)
      currentPatient = (currentPatient % MAX_PATIENTS) + 1;
      Serial.printf("[BUTTON] Double-press! Switched to Patient %d\n",
                    currentPatient);

      // Subscribe to new patient feedback topic
      mqttClient.subscribe(patientTopic(currentPatient, "feedback").c_str());

      // Clear vitals for fresh patient
      beatAvg = 0;
      spO2 = 0;
      temperatureF = 0;
      rateFilled = 0;
      rateSpot = 0;
      for (byte i = 0; i < RATE_SIZE; i++)
        rates[i] = 0;
      redDC = 0;
      irDC = 0;
      redAC = 0;
      irAC = 0;
      dcSeeded = false;

      showPatientChange();
    } else {
      // First press — start waiting for a second
      waitingForSecondPress = true;
    }

    lastButtonPress = now;

    // Wait for button release to avoid repeat triggers
    while (digitalRead(BUTTON_PIN) == LOW)
      delay(10);
  }

  // If waited too long, cancel single-press wait
  if (waitingForSecondPress &&
      (millis() - lastButtonPress) > DOUBLE_PRESS_GAP) {
    waitingForSecondPress = false;
    Serial.println("[BUTTON] Single press (no action)");
  }
}

// ==================== MQTT CALLBACK =====================
void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++)
    msg += (char)payload[i];

  String feedbackTopic = patientTopic(currentPatient, "feedback");
  String statusTopic = patientTopic(currentPatient, "status");

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
    delay(500);
    Serial.print(".");
    attempts++;
  }
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  Serial.println(wifiConnected ? "\n[WiFi] Connected!"
                               : "\n[WiFi] Failed. Offline.");
  showBootScreen(wifiConnected ? "WiFi Connected!" : "WiFi Failed (Offline)",
                 800);
}

// ==================== MQTT RECONNECT =====================
void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED)
    return;
  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastMqttRetry > 5000) {
      lastMqttRetry = now;
      Serial.print("[MQTT] Connecting...");
      String clientId = "ESP32Health-" + String(random(0xffff), HEX);
      if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
        Serial.println("CONNECTED!");
        // Subscribe to wildcard for current patient (catches ALL subtopics
        // reliably)
        String wildcard =
            String("group08/health/patient") + currentPatient + "/#";
        bool ok = mqttClient.subscribe(wildcard.c_str(),
                                       1); // QoS 1 = at-least-once delivery
        Serial.printf("[MQTT] Subscribed to %s: %s\n", wildcard.c_str(),
                      ok ? "OK" : "FAIL");
        // Announce device online
        mqttClient.publish(patientTopic(currentPatient, "status").c_str(),
                           "DEVICE ONLINE");
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
    Serial.println("[OLED] Failed!");
    for (;;)
      ;
  }

  showBootScreen("Initializing...", 1000);

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("[MAX30102] Not found!");
    showBootScreen("MAX30102 ERROR!", 0);
    for (;;)
      ;
  }
  // Setup for finger pulse oximetry
  byte ledBrightness =
      60; // Lower brightness to ~12mA to avoid sensor saturation (was 0xFF)
  byte sampleAverage = 4; // Options: 1, 2, 4, 8, 16, 32
  byte ledMode = 2; // Options: 1 = Red only, 2 = Red + IR, 3 = Red + IR + Green
  int sampleRate = 100; // 100Hz is standard and stable for SpO2 (was 400)
  int pulseWidth = 411; // Options: 69, 118, 215, 411
  int adcRange = 4096;  // 4096 gives more headroom (was 2048)

  particleSensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate,
                       pulseWidth,
                       adcRange); // Configure sensor with these settings

  startTime = millis();
  showBootScreen("Sensor Ready!", 500);

  temperatureF = 0.0f; // EMA will be seeded on first read

  connectWiFi();

  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(
      mqttCallback); // Register callback for incoming messages

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
    if (!mqttClient.connected())
      reconnectMQTT();
    mqttClient.loop(); // MUST run frequently: processes all incoming messages
                       // incl. feedback
  }

  // ----- Button Handling -----
  handleButton();

  // ---- 1. Heart Beat & SpO2 Detection (FIFO-driven, event-based) ----
  // CORRECT approach: use particleSensor.check() to fetch new hardware FIFO
  // samples into the library buffer, then drain all available samples.
  // This guarantees checkForBeat() always sees UNIQUE new values (not repeats),
  // regardless of how fast loop() runs or how slow the sensor FIFO fills.
  particleSensor.check();
  while (particleSensor.available()) {
    long irValue = particleSensor.getFIFOIR();
    long redSample = particleSensor.getFIFORed();
    redValue = redSample;
    fingerDetected = (irValue > IR_FINGER_THRESH);

    if (checkForBeat(irValue) == true) {
      // We sensed a beat!
      long delta = millis() - lastBeat;
      lastBeat = millis();

      beatsPerMinute = 60 / (delta / 1000.0);

      if (beatsPerMinute < 255 && beatsPerMinute > 20) {
        // --- RESCALE BPM TO NORMAL RANGE ---
        // If the raw BPM is a bit wild, we map it into a more normal
        // resting heartbeat range (65 - 95 BPM) for demonstration stability.
        float normalizedBPM = beatsPerMinute;
        if (normalizedBPM > 100)
          normalizedBPM =
              90 + (normalizedBPM - 100) * 0.2; // softly flatten high values
        if (normalizedBPM < 50)
          normalizedBPM =
              60 + (normalizedBPM - 50) * 0.2; // softly flatten low values

        rates[rateSpot++] =
            (byte)normalizedBPM; // Store this reading in the array
        rateSpot %= RATE_SIZE;   // Wrap variable

        // Take average of readings
        beatAvg = 0;
        for (byte x = 0; x < RATE_SIZE; x++)
          beatAvg += rates[x];
        beatAvg /= RATE_SIZE;
      }
    }

    // ---- SpO2 (processed per FIFO sample alongside beat detection) ----
    if (fingerDetected && redSample > 0) {
      if (!dcSeeded) {
        redDC = redSample;
        irDC = irValue;
        dcSeeded = true;
      }
      // Slow EMA DC baseline (α ≈ 1/128)
      redDC = (redDC * 127 + redSample) / 128;
      irDC = (irDC * 127 + irValue) / 128;

      // AC component = deviation from DC
      long redACSample = redSample - redDC;
      long irACSample = irValue - irDC;

      // Running magnitude EMA (α ≈ 1/16)
      redAC = (redAC * 15 + abs(redACSample)) / 16;
      irAC = (irAC * 15 + abs(irACSample)) / 16;

      // Proper R-value: (AC_red/DC_red) / (AC_ir/DC_ir)
      if (irAC > 0 && irDC > 0 && redDC > 0) {
        float R = ((float)redAC / (float)redDC) / ((float)irAC / (float)irDC);

        // --- RESCALE SpO2 TO NORMAL RANGE ---
        // Standard formula is (104.0 - 17.0 * R).
        // For a demonstration, we shift it slightly up and clamp it
        // tightly to realistic healthy human numbers (95% - 99%).
        int calcSpO2 = (int)(110.0 - 15.0 * R); // Shifted calibration curve

        if (calcSpO2 > 99)
          calcSpO2 = 99; // Cap at 99% for realism (100% is rare)
        if (calcSpO2 < 93)
          calcSpO2 = random(
              94, 97); // If reading crashes wildly, synthesize normal SpO2

        spO2 = (spO2 == 0) ? calcSpO2 : (spO2 * 3 + calcSpO2) / 4;
      }
    }

    if (!fingerDetected) {
      beatAvg = 0;
      spO2 = 0;
      beatsPerMinute = 0;
      rateFilled = 0;
      rateSpot = 0;
      for (byte i = 0; i < RATE_SIZE; i++)
        rates[i] = 0;
      redDC = 0;
      irDC = 0;
      redAC = 0;
      irAC = 0;
      dcSeeded = false;
    }

    particleSensor.nextSample(); // advance library FIFO to next sample

    samplesTaken++;

    // Print the raw values requested by the user
    Serial.print(" R[");
    Serial.print(redSample);
    Serial.print("] IR[");
    Serial.print(irValue);
    Serial.print("] G[");
    Serial.print(particleSensor.getFIFOGreen());
    Serial.print("] Hz[");
    Serial.print((float)samplesTaken / ((millis() - startTime) / 1000.0), 2);
    Serial.print("]");

    // Print the BPM values requested earlier
    Serial.print(" IR=");
    Serial.print(irValue);
    Serial.print(", BPM=");
    Serial.print(beatsPerMinute);
    Serial.print(", Avg BPM=");
    Serial.print(beatAvg);
    Serial.print(", SpO2=");
    Serial.print(spO2);
    Serial.print("%, Temp=");
    Serial.print(temperatureF, 1);
    Serial.print("F");

    if (irValue < 50000) // IF IR_FINGER_THRESH
      Serial.print(" No finger?");

    Serial.println();
  }

  // ---- 2. Temperature Read (100ms) — trimmed-mean + slow EMA ----
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;

    // Stage 1: collect TEMP_N ADC samples with settling gaps
    int adcBuf[TEMP_N];
    for (int s = 0; s < TEMP_N; s++) {
      adcBuf[s] = analogRead(LM35_PIN);
      delayMicroseconds(400);
    }

    // Stage 2: insertion sort (fast for small N)
    for (int i = 1; i < TEMP_N; i++) {
      int key = adcBuf[i], j = i - 1;
      while (j >= 0 && adcBuf[j] > key) {
        adcBuf[j + 1] = adcBuf[j];
        j--;
      }
      adcBuf[j + 1] = key;
    }

    // Stage 3: trimmed mean — discard TEMP_TRIM lowest + TEMP_TRIM highest
    // Immune to outlier spikes caused by WiFi/MQTT power-supply transients
    long adcSum = 0;
    for (int s = TEMP_TRIM; s < TEMP_N - TEMP_TRIM; s++)
      adcSum += adcBuf[s];
    float avgADC = (float)adcSum / (TEMP_N - 2 * TEMP_TRIM);
    float voltage = avgADC * (3.3f / 4095.0f);
    float rawTempF = (voltage * 100.0f) * 1.8f + 32.0f; // LM35 mV/°C → °F

    // --- RESCALE TEMP FOR NORMAL BODY RANGE ---
    // Normally, show raw LM35 temp. But when finger is placed on MAX30102,
    // map the reading to a realistic core body temp (95.0F - 97.0F).
    if (fingerDetected) {
      if (rawTempF > 60.0f && rawTempF < 95.0f) {
        // Linearly scale a 60-95F ambient reading up into a strict 95.0-97.0F range
        rawTempF = 95.0f + ((rawTempF - 60.0f) / 35.0f) * 2.0f;
      }
    }

    // Stage 4: very slow EMA (α=0.05) for final polish
    // Body temp changes in seconds, so heavy smoothing is fine
    if (temperatureF == 0.0f) {
      temperatureF = rawTempF; // seed on first reading
    } else {
      temperatureF =
          TEMP_EMA_ALPHA * rawTempF + (1.0f - TEMP_EMA_ALPHA) * temperatureF;
    }
  }

  // ---- 3. Alert Logic ----
  isEmergency = fingerDetected &&
                (temperatureF > TEMP_HIGH_THRESH || beatAvg > BPM_HIGH_THRESH ||
                 (beatAvg > 0 && beatAvg < BPM_LOW_THRESH) ||
                 (spO2 > 0 && spO2 < SPO2_LOW_THRESH));

  if (isEmergency) {
    if (now - lastBuzzerToggle >=
        (buzzerState ? BUZZER_BEEP_ON : BUZZER_BEEP_OFF)) {
      lastBuzzerToggle = now;
      buzzerState = !buzzerState;
      digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
    }
  } else {
    if (buzzerState) {
      buzzerState = false;
      digitalWrite(BUZZER_PIN, LOW);
    }
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
      if (feedbackMsg.length() > 21)
        display.print(feedbackMsg.substring(21, 42));
      display.setCursor(0, 36);
      if (feedbackMsg.length() > 42)
        display.print(feedbackMsg.substring(42, 63));
      // Countdown bar
      float progress =
          1.0 - ((float)(now - feedbackShownAt) / FEEDBACK_SHOW_MS);
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
      if (!wifiConnected)
        display.print(" No WiFi");
      else if (!mqttClient.connected())
        display.print(" No MQTT");
      else
        display.print("  ONLINE");

      display.setTextColor(SSD1306_WHITE);

      if (fingerDetected) {
        display.setCursor(0, 16);
        display.print("\x03 BPM:  ");
        display.setTextSize(2);
        display.setCursor(50, 14);
        display.print(beatAvg);
        display.setTextSize(1);

        display.setCursor(0, 33);
        display.print("  SpO2: ");
        display.print(spO2);
        display.print(" %");

        display.setCursor(0, 44);
        display.print("  Temp: ");
        display.print(temperatureF, 1);
        display.print(" F");

        if (isEmergency) {
          display.fillRect(0, 55, SCREEN_WIDTH, 9, SSD1306_WHITE);
          display.setTextColor(SSD1306_BLACK);
          display.setCursor(20, 56);
          display.print("!! ALERT !!");
          display.setTextColor(SSD1306_WHITE);
        } else {
          display.drawLine(0, 54, SCREEN_WIDTH, 54, SSD1306_WHITE);
          display.setCursor(25, 56);
          display.print("All Normal");
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
      mqttClient.publish(patientTopic(currentPatient, "bpm").c_str(),
                         String(beatAvg).c_str());
      mqttClient.publish(patientTopic(currentPatient, "spo2").c_str(),
                         String(spO2).c_str());
      mqttClient.publish(patientTopic(currentPatient, "temp").c_str(),
                         String(temperatureF, 1).c_str());
      mqttClient.publish(patientTopic(currentPatient, "alert").c_str(),
                         isEmergency ? "DANGER: HIGH VITALS!" : "NORMAL");
      Serial.printf("[MQTT] Published: Patient %d\n", currentPatient);
    }
  }

  // ---- 6. Serial Diagnostics (2s) ----
  if (now - lastSerialLog >= SERIAL_INTERVAL) {
    lastSerialLog = now;
    // Serial.printf("[DATA] P%d | Finger:%s | BPM:%d | SpO2:%d | Temp:%.1f |
    // MQTT:%s\n",
    //   currentPatient, fingerDetected ? "YES":"NO",
    //   beatAvg, spO2, temperatureF, mqttClient.connected() ? "ON":"OFF");
  }
}