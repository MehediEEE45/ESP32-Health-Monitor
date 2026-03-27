/*
 * =====================================================
 *  IoT-Based Health Monitoring System
 *  ESP32-S3 WROOM | MAX30102 | LM35 | OLED | Buzzer
 *  Course: EEE 330 | Group 08
 *  Protocol: MQTT with Multi-WiFi Failover
 * =====================================================
 */

#include "MAX30105.h"
#include "heartRate.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <PubSubClient.h> // Required for MQTT
#include <WiFi.h>
#include <WiFiClientSecure.h> // TLS
#include <WiFiMulti.h>        // Required for multiple WiFi networks
#include <Wire.h>

// ==================== PIN DEFINITIONS ====================
#define I2C_SDA_PIN 8
#define I2C_SCL_PIN 9
#define LM35_PIN 4 // ADC1_CH3
#define BUZZER_PIN 5

// ==================== OLED CONFIG ========================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ==================== MAX30102 CONFIG ====================
MAX30105 particleSensor;

// Heart Rate averaging
#define RATE_SIZE 8
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;
float beatsPerMinute = 0;
int beatAvg = 0;

// SpO2 using Red/IR ratio
long redValue = 0;
int spO2 = 0;

// ==================== LM35 / TEMPERATURE =================
#define TEMP_SAMPLES 10
float tempReadings[TEMP_SAMPLES];
byte tempIndex = 0;
bool tempBufferFull = false;
float temperatureC = 0.0;

// ==================== HEALTH THRESHOLDS ==================
#define TEMP_HIGH_THRESH 38.0 // Celsius
#define BPM_HIGH_THRESH 120
#define BPM_LOW_THRESH 40
#define SPO2_LOW_THRESH 90
#define IR_FINGER_THRESH 50000 // IR value when finger is placed

// ==================== WI-FI (MULTIPLE) ===================
WiFiMulti wifiMulti;
WiFiClientSecure espClient;

// ==================== MQTT CONFIG ========================
PubSubClient mqttClient(espClient);

// Update this with your broker. Using HiveMQ public broker for demo.
const char *mqtt_server = "3af2a7e75dca42e1ba1e09b3d71602f9.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char *mqtt_user = "Health";
const char *mqtt_pass = "Me107645";

// Configure MQTT Topics
const char *topic_bpm = "group08/health/bpm";
const char *topic_spo2 = "group08/health/spo2";
const char *topic_temp = "group08/health/temp";
const char *topic_alert = "group08/health/alert";

// ==================== TIMING (ms) ========================
#define SENSOR_INTERVAL 100
#define DISPLAY_INTERVAL 500
#define CLOUD_INTERVAL 1000 // Send to MQTT every 1 second (MQTT is fast)
#define SERIAL_INTERVAL 2000
#define BUZZER_BEEP_ON 200
#define BUZZER_BEEP_OFF 300

unsigned long lastSensorRead = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastCloudUpdate = 0;
unsigned long lastSerialLog = 0;
unsigned long lastBuzzerToggle = 0;
unsigned long lastMqttRetry = 0;
bool buzzerState = false;

// ==================== STATE FLAGS ========================
bool fingerDetected = false;
bool isEmergency = false;
bool wifiConnected = false;

// ==================== BOOT ANIMATION ====================
void showBootScreen(const char *msg, int delayMs) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Draw border
  display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);

  // Title
  display.setCursor(14, 4);
  display.setTextSize(1);
  display.print("Health Monitor");

  // Separator line
  display.drawLine(4, 14, 123, 14, SSD1306_WHITE);

  // Message centered
  display.setCursor(10, 28);
  display.setTextSize(1);
  display.print(msg);

  display.display();
  delay(delayMs);
}

// ==================== SETUP WI-FI & MQTT =================
void setupWiFi() {
  Serial.println("\n[WiFi] Configuring Known Networks...");

  espClient.setInsecure(); // Required for HiveMQ Cloud TLS connection

  // ADD ALL YOUR NETWORKS HERE:
  wifiMulti.addAP("MiM", "Ha20202021");          // Network 1
  wifiMulti.addAP("SecondNetwork", "Password2"); // Network 2
  wifiMulti.addAP("ThirdNetwork", "Password3");  // Network 3

  showBootScreen("Connecting WiFi...", 0);
  Serial.print("[WiFi] Attempting connection...");

  // The ESP32 will auto-scan and connect to the strongest known network
  if (wifiMulti.run() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("[WiFi] Connected to: " + WiFi.SSID());
    Serial.println("[WiFi] IP Address:   " + WiFi.localIP().toString());
    showBootScreen("WiFi Connected!", 800);
    wifiConnected = true;
  } else {
    Serial.println("\n[WiFi] Connection Failed. Offline Mode.");
    showBootScreen("WiFi Failed (Offline)", 1000);
    wifiConnected = false;
  }
}

void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED)
    return; // Must have Wi-Fi first

  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastMqttRetry > 5000) { // Retry every 5 seconds without blocking
      lastMqttRetry = now;
      Serial.print("[MQTT] Connecting to broker...");

      // Create a random client ID to avoid collisions
      String clientId = "ESP32Health-";
      clientId += String(random(0xffff), HEX);

      // Attempt connection with username and password
      if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
        Serial.println("CONNECTED!");
        mqttClient.publish("group08/health/status", "SYSTEM ONLINE");
      } else {
        Serial.print("failed, rc=");
        Serial.print(mqttClient.state());
        Serial.println(" (retrying later)");
      }
    }
  }
}

// ==================== SETUP ==============================
void setup() {
  Serial.begin(115200);
  Serial.println("\n===== Health Monitor Starting =====");

  // I2C for ESP32-S3
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  analogReadResolution(12);

  // OLED Init
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[OLED] SSD1306 allocation failed!");
    for (;;)
      ;
  }
  showBootScreen("Initializing...", 1000);

  // MAX30102 Init
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("[MAX30102] Sensor not found!");
    showBootScreen("MAX30102 ERROR!", 0);
    for (;;)
      ;
  }
  Serial.println("[MAX30102] Sensor initialized.");

  // Optimal settings
  particleSensor.setup(60, 4, 2, 100, 411, 4096);
  particleSensor.setPulseAmplitudeRed(0x0A);
  particleSensor.setPulseAmplitudeIR(60);

  showBootScreen("Sensor Ready!", 500);

  for (int i = 0; i < TEMP_SAMPLES; i++)
    tempReadings[i] = 0;

  // Wi-Fi Setup
  setupWiFi();

  // MQTT Server binding
  mqttClient.setServer(mqtt_server, mqtt_port);

  showBootScreen("System Ready!", 1000);
  Serial.println("===== System Ready =====\n");
}

// ==================== LOOP ===============================
void loop() {
  unsigned long now = millis();

  // ----- WiFi & MQTT Maintenance -----
  // wifiMulti.run() automatically reconnects to the strongest AP if
  // disconnected
  if (wifiMulti.run() != WL_CONNECTED) {
    wifiConnected = false;
  } else {
    wifiConnected = true;
    if (!mqttClient.connected()) {
      reconnectMQTT();
    }
    mqttClient.loop(); // Important! Keeps the MQTT connection alive
  }

  // ---- 1. Continuous Heart Beat Detection ----
  long irValue = particleSensor.getIR();
  redValue = particleSensor.getRed();
  fingerDetected = (irValue > IR_FINGER_THRESH);

  if (fingerDetected && checkForBeat(irValue)) {
    long delta = now - lastBeat;
    lastBeat = now;
    beatsPerMinute = 60.0 / (delta / 1000.0);

    if (beatsPerMinute > 30 && beatsPerMinute < 255) {
      rates[rateSpot++] = (byte)beatsPerMinute;
      rateSpot %= RATE_SIZE;
      beatAvg = 0;
      for (byte x = 0; x < RATE_SIZE; x++)
        beatAvg += rates[x];
      beatAvg /= RATE_SIZE;
    }
  }

  // ---- 2. Periodic Sensor Reads (100ms) ----
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;

    // -- LM35 with averaging --
    int rawADC = analogRead(LM35_PIN);
    float voltage = rawADC * (3.3 / 4095.0);
    float currentTemp = voltage * 100.0;

    tempReadings[tempIndex++] = currentTemp;
    if (tempIndex >= TEMP_SAMPLES) {
      tempIndex = 0;
      tempBufferFull = true;
    }
    float sum = 0;
    int count = tempBufferFull ? TEMP_SAMPLES : tempIndex;
    for (int i = 0; i < count; i++)
      sum += tempReadings[i];
    if (count > 0)
      temperatureC = sum / count;

    // -- SpO2 Estimation --
    if (fingerDetected && redValue > 0) {
      float ratio = (float)redValue / (float)irValue;
      int calcSpO2 = (int)(110.0 - 25.0 * ratio);
      if (calcSpO2 > 100)
        calcSpO2 = 100;
      if (calcSpO2 < 0)
        calcSpO2 = 0;
      spO2 = (spO2 == 0) ? calcSpO2 : (spO2 * 3 + calcSpO2) / 4;
    }

    if (!fingerDetected) {
      beatAvg = 0;
      spO2 = 0;
      beatsPerMinute = 0;
    }
  }

  // ---- 3. Alert Logic ----
  isEmergency = false;
  if (fingerDetected) {
    if (temperatureC > TEMP_HIGH_THRESH || beatAvg > BPM_HIGH_THRESH ||
        (beatAvg > 0 && beatAvg < BPM_LOW_THRESH) ||
        (spO2 > 0 && spO2 < SPO2_LOW_THRESH)) {
      isEmergency = true;
    }
  }

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

  // ---- 4. OLED Display Update (500ms) ----
  if (now - lastDisplayUpdate >= DISPLAY_INTERVAL) {
    lastDisplayUpdate = now;
    display.clearDisplay();

    display.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setTextSize(1);
    display.setCursor(4, 2);
    display.print("Health Monitor");

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
      display.print(temperatureC, 1);
      display.print(" C");

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
      display.setTextSize(1);
      display.print("Place your finger");
      display.setCursor(15, 38);
      display.print("on the sensor...");
    }
    display.display();
  }

  // ---- 5. MQTT Cloud Upload (1000ms updates) ----
  // MQTT is lightweight, so we can send data much faster than ThingSpeak!
  if (now - lastCloudUpdate >= CLOUD_INTERVAL) {
    lastCloudUpdate = now;

    // Send data to MQTT as long as finger is detected, even if calculating BPM
    if (wifiConnected && mqttClient.connected() && fingerDetected) {
      // Convert numbers to Strings, then to char arrays for MQTT publishing
      String strBPM = String(beatAvg);
      String strSpO2 = String(spO2);
      String strTemp = String(temperatureC, 1);

      mqttClient.publish(topic_bpm, strBPM.c_str());
      mqttClient.publish(topic_spo2, strSpO2.c_str());
      mqttClient.publish(topic_temp, strTemp.c_str());

      if (isEmergency)
        mqttClient.publish(topic_alert, "DANGER: HIGH VITALS!");
      else
        mqttClient.publish(topic_alert, "NORMAL");

      Serial.println("[MQTT] Published vitals.");
    }
  }

  // ---- 6. Serial Diagnostics (2s) ----
  if (now - lastSerialLog >= SERIAL_INTERVAL) {
    lastSerialLog = now;
    Serial.print("[DATA] Finger:");
    Serial.print(fingerDetected ? "YES" : "NO");
    Serial.print(" | BPM:");
    Serial.print(beatAvg);
    Serial.print(" | SpO2:");
    Serial.print(spO2);
    Serial.print(" | Temp:");
    Serial.print(temperatureC, 1);
    Serial.print(" | MQTT:");
    Serial.println(mqttClient.connected() ? "ON" : "OFF");
  }
}