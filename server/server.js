/*
 * =====================================================
 *  Health Monitor Backend Server
 *  MQTT → MongoDB → REST API
 *  Stores patient vitals every 30 seconds (averaged)
 * =====================================================
 */
require('dotenv').config();
const express = require('express');
const cors = require('cors');
const mongoose = require('mongoose');
const mqtt = require('mqtt');

const app = express();
app.use(cors());
app.use(express.json());

const PORT = process.env.PORT || 5000;

// ==================== MONGOOSE SCHEMA ====================
const vitalSchema = new mongoose.Schema({
  patientId: { type: Number, required: true, index: true },
  bpm:       { type: Number, default: 0 },
  spo2:      { type: Number, default: 0 },
  temp:      { type: Number, default: 0 },
  status:    { type: String, default: 'NORMAL' },
  timestamp: { type: Date, default: Date.now, index: true },
});

const Vital = mongoose.model('Vital', vitalSchema);

// ==================== FEEDBACK SCHEMA ====================
const feedbackSchema = new mongoose.Schema({
  patientId: { type: Number, required: true, index: true },
  message:   { type: String, required: true },
  sentBy:    { type: String, default: 'doctor' },
  timestamp: { type: Date, default: Date.now },
});

const Feedback = mongoose.model('Feedback', feedbackSchema);

// ==================== MQTT LISTENER ======================
// Buffering system: gather bpm/spo2/temp readings per patient
// and save an averaged record every SAVE_INTERVAL ms
const SAVE_INTERVAL = 30000; // 30 seconds
const patientBuffers = {}; // { 1: { bpm: [], spo2: [], temp: [], status: '' }, ... }

function getBuffer(patientId) {
  if (!patientBuffers[patientId]) {
    patientBuffers[patientId] = { bpm: [], spo2: [], temp: [], status: 'NORMAL' };
  }
  return patientBuffers[patientId];
}

function avg(arr) {
  if (arr.length === 0) return 0;
  return Math.round(arr.reduce((a, b) => a + b, 0) / arr.length * 10) / 10;
}

function connectMQTT() {
  const client = mqtt.connect(process.env.MQTT_BROKER, {
    username: process.env.MQTT_USER,
    password: process.env.MQTT_PASS,
    clientId: `server-backend-${Math.random().toString(16).slice(2)}`,
    clean: true,
    reconnectPeriod: 5000,
  });

  client.on('connect', () => {
    console.log('✅ [MQTT] Connected to HiveMQ Cloud');
    // Subscribe to ALL patients with wildcard: group08/health/patient+/+
    client.subscribe('group08/health/patient+/+', { qos: 1 }, (err) => {
      if (err) console.error('❌ [MQTT] Subscribe error:', err);
      else console.log('📡 [MQTT] Subscribed to group08/health/patient+/+');
    });
  });

  client.on('message', (topic, payload) => {
    const val = payload.toString();
    // Topic format: group08/health/patient{N}/{field}
    const parts = topic.split('/');
    // parts = ['group08', 'health', 'patient1', 'bpm']
    if (parts.length < 4) return;

    const patientStr = parts[2]; // 'patient1'
    const field = parts[3];      // 'bpm', 'spo2', 'temp', 'alert', 'feedback', 'status'
    const patientId = parseInt(patientStr.replace('patient', ''), 10);
    if (isNaN(patientId)) return;

    const buf = getBuffer(patientId);
    const num = parseFloat(val);

    switch (field) {
      case 'bpm':
        if (!isNaN(num)) buf.bpm.push(num);
        break;
      case 'spo2':
        if (!isNaN(num)) buf.spo2.push(num);
        break;
      case 'temp':
        if (!isNaN(num)) buf.temp.push(num);
        break;
      case 'alert':
        buf.status = val;
        break;
      case 'feedback':
        // Also save doctor feedback to DB
        saveFeedback(patientId, val);
        break;
      default:
        break;
    }
  });

  client.on('error', (err) => console.error('❌ [MQTT] Error:', err.message));
  client.on('offline', () => console.log('⚠️ [MQTT] Offline, reconnecting...'));

  return client;
}

// Save feedback to MongoDB
async function saveFeedback(patientId, message) {
  try {
    await Feedback.create({ patientId, message });
    console.log(`💬 [DB] Saved feedback for P${patientId}: "${message}"`);
  } catch (err) {
    console.error('❌ [DB] Feedback save error:', err.message);
  }
}

// Periodic flush: average buffered readings and save to DB
async function flushBuffers() {
  for (const [id, buf] of Object.entries(patientBuffers)) {
    // Only save if we have at least some data
    if (buf.bpm.length === 0 && buf.spo2.length === 0 && buf.temp.length === 0) continue;

    const record = {
      patientId: parseInt(id),
      bpm:  avg(buf.bpm),
      spo2: avg(buf.spo2),
      temp: avg(buf.temp),
      status: buf.status || 'NORMAL',
    };

    try {
      await Vital.create(record);
      console.log(`📊 [DB] Saved P${id}: BPM=${record.bpm} SpO2=${record.spo2} Temp=${record.temp} (${buf.bpm.length} samples averaged)`);
    } catch (err) {
      console.error(`❌ [DB] Save error for P${id}:`, err.message);
    }

    // Clear the buffer
    buf.bpm = []; buf.spo2 = []; buf.temp = [];
  }
}

// ==================== REST API ===========================

// GET /api/history/:patientId  — returns vitals from the last 24 hours
app.get('/api/history/:patientId', async (req, res) => {
  try {
    const patientId = parseInt(req.params.patientId);
    const hours = parseInt(req.query.hours) || 24;
    const since = new Date(Date.now() - hours * 60 * 60 * 1000);

    const records = await Vital.find({
      patientId,
      timestamp: { $gte: since }
    }).sort({ timestamp: 1 }).lean();

    res.json({
      patient: patientId,
      count: records.length,
      from: since.toISOString(),
      data: records.map(r => ({
        bpm: r.bpm,
        spo2: r.spo2,
        temp: r.temp,
        status: r.status,
        time: r.timestamp,
      })),
    });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// GET /api/feedback/:patientId  — returns feedback history
app.get('/api/feedback/:patientId', async (req, res) => {
  try {
    const patientId = parseInt(req.params.patientId);
    const records = await Feedback.find({ patientId })
      .sort({ timestamp: -1 })
      .limit(50)
      .lean();

    res.json({
      patient: patientId,
      count: records.length,
      data: records.map(r => ({
        message: r.message,
        sentBy: r.sentBy,
        time: r.timestamp,
      })),
    });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// GET /api/stats/:patientId  — returns daily summary stats
app.get('/api/stats/:patientId', async (req, res) => {
  try {
    const patientId = parseInt(req.params.patientId);
    const since = new Date(Date.now() - 24 * 60 * 60 * 1000);

    const records = await Vital.find({
      patientId,
      timestamp: { $gte: since },
    }).lean();

    if (records.length === 0) {
      return res.json({ patient: patientId, message: 'No data in last 24h' });
    }

    const bpms  = records.map(r => r.bpm).filter(v => v > 0);
    const spo2s = records.map(r => r.spo2).filter(v => v > 0);
    const temps = records.map(r => r.temp).filter(v => v > 0);

    const stat = (arr) => ({
      min:  arr.length ? Math.min(...arr) : 0,
      max:  arr.length ? Math.max(...arr) : 0,
      avg:  arr.length ? Math.round(arr.reduce((a,b) => a+b, 0) / arr.length * 10) / 10 : 0,
      count: arr.length,
    });

    res.json({
      patient: patientId,
      period: '24h',
      totalReadings: records.length,
      bpm:  stat(bpms),
      spo2: stat(spo2s),
      temp: stat(temps),
      alerts: records.filter(r => r.status && r.status.includes('DANGER')).length,
    });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// Health check
app.get('/api/health', (req, res) => {
  res.json({ status: 'ok', uptime: process.uptime() });
});

// ==================== START ==============================
async function start() {
  console.log('\n🏥 Health Monitor Backend Starting...\n');

  // 1. Connect to MongoDB
  try {
    await mongoose.connect(process.env.MONGO_URI);
    console.log('✅ [MongoDB] Connected to Atlas');
  } catch (err) {
    console.error('❌ [MongoDB] Connection failed:', err.message);
    console.error('   ➤ Make sure you have set MONGO_URI in server/.env');
    process.exit(1);
  }

  // 2. Connect to MQTT
  connectMQTT();

  // 3. Start periodic buffer flush
  setInterval(flushBuffers, SAVE_INTERVAL);
  console.log(`⏱️  [DB] Will save averaged readings every ${SAVE_INTERVAL / 1000}s`);

  // 4. Start Express
  app.listen(PORT, () => {
    console.log(`\n🚀 Server running on http://localhost:${PORT}`);
    console.log(`📊 API endpoints:`);
    console.log(`   GET /api/history/:patientId?hours=24`);
    console.log(`   GET /api/feedback/:patientId`);
    console.log(`   GET /api/stats/:patientId`);
    console.log(`   GET /api/health\n`);
  });
}

start();
