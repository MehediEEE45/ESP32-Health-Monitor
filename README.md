# 🫀 IoT-Based Multi-Patient Health Monitoring System

![System Overview](https://img.shields.io/badge/Status-Complete-success) ![Microcontroller](https://img.shields.io/badge/Hardware-ESP32--S3-blue) ![Protocol](https://img.shields.io/badge/Protocol-MQTT-orange) ![Stack](https://img.shields.io/badge/Stack-Node.js%20%7C%20React%20%7C%20MongoDB-lightgrey)

## 🌟 Why This Project is Needed

In modern healthcare, particularly in rural clinics, crowded hospitals, or during global health crises (like the COVID-19 pandemic), continuous patient monitoring is a massive logistical challenge. Nurses and doctors are often stretched thin, making it impossible to manually check every patient's vitals minute-by-minute. 

This leads to two major problems:
1. **Delayed Emergency Response:** Sudden drops in oxygen levels or spikes in heart rate can go unnoticed until a nurse makes their next physical round.
2. **Resource Exhaustion:** Assigning dedicated medical staff just to record repetitive sensor data is an inefficient use of highly trained professionals.

**The Solution:** 
This project serves as a low-cost, highly scalable **IoT Health Dashboard**. By leveraging a single ESP32-S3 device capable of hot-swapping between multiple patients, it automates the collection of critical vitals (Heart Rate, Blood Oxygen, and Body Temperature). The data is streamed out of the ward in real-time to a secure cloud backend, allowing a single doctor to monitor an entire ward of patients remotely from a powerful web dashboard. Crucially, the system features **bi-directional communication**, allowing a doctor to type medical feedback on the web dashboard that immediately appears on the patient's physical OLED screen at their bedside.

---

## 🛠️ System Architecture & Features

This project is a complete "Full-Stack Hardware" application divided into three tiers:

### 1. Hardware & Firmware (The Patient Node)
Built on an **ESP32-S3 WROOM**, this physical device sits by the patient's bedside.
*   **Sensors:** Utilizes a **MAX30102** for high-frequency optical measurements of Heart Rate (BPM) and Blood Oxygen Saturation (SpO₂), alongside an **LM35** analog sensor mapped precisely to core body temperature.
*   **Multi-Patient Scaling:** A physical button allows nurses to securely rotate the device between 3 different patients via a debounced "double-click" gesture, resetting the data pool and changing MQTT topics on the fly.
*   **OLED & Buzzer Interface:** Displays real-time vitals locally. If vitals cross critical thresholds (e.g., SpO₂ < 90%), a local buzzer sounds an alarm.
*   **Wi-Fi Failover:** Programmed to automatically jump between multiple stored Wi-Fi networks if one goes off-grid.

### 2. Cloud Server (The Aggregator)
A **Node.js / Express / MongoDB** backend running 24/7.
*   **Intelligent Buffering:** Subscribes to the HiveMQ cloud broker and collects high-frequency sensor ticks. Instead of spamming the database, it intelligently averages the data every 30 seconds before committing memory to MongoDB.
*   **REST API:** Serves 24-hour historical logs, daily statistical minimums/maximums, and alert counts to the frontend.

### 3. Doctor's Web Dashboard
A modern, aesthetic **React + Vite** frontend designed for medical professionals.
*   **Live Telemetry:** Uses WebSockets/MQTT to stream live, animated progress bars of BPM, Oxygen, and Temperature.
*   **Visual History:** Implements `Recharts` to chart the 24-hour medical history of any selected patient on a plotted graph.
*   **Instant Prescription Paging:** Doctors can select presets or type custom notes (e.g., *"Take Paracetamol"*, *"Monitor Closely"*) into a built-in chat UI. This message is pushed via MQTT directly to the patient's physical OLED screen on the ESP32.
