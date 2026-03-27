import { useState, useEffect, useCallback, useRef } from 'react';
import mqtt from 'mqtt';

const BROKER_URL = 'wss://3af2a7e75dca42e1ba1e09b3d71602f9.s1.eu.hivemq.cloud:8884/mqtt';
const MQTT_OPTIONS = {
  username: 'Health',
  password: 'Me107645',
  clientId: `doctor-dashboard-${Math.random().toString(16).slice(2)}`,
  clean: true,
  reconnectPeriod: 3000,
};

// Backend API base URL (change to deployed URL for production)
const API_BASE = import.meta.env.VITE_API_URL || 'http://localhost:5000';

// Generates MQTT topic set for a given patient number
export function getTopics(patient) {
  const base = `group08/health/patient${patient}`;
  return {
    bpm:      `${base}/bpm`,
    spo2:     `${base}/spo2`,
    temp:     `${base}/temp`,
    alert:    `${base}/alert`,
    status:   `${base}/status`,
    feedback: `${base}/feedback`,
  };
}

export function useMqtt(activePatient) {
  const clientRef = useRef(null);
  const activePatientRef = useRef(activePatient);
  const [connected, setConnected] = useState(false);
  const [vitals, setVitals] = useState({ bpm: '--', spo2: '--', temp: '--' });
  const [alertMsg, setAlertMsg] = useState('NORMAL');
  const [messages, setMessages] = useState([
    { type: 'system', text: 'Doctor portal connected. Waiting for patient data...', time: new Date().toLocaleTimeString() }
  ]);
  const [history, setHistory] = useState([]);
  const [stats, setStats] = useState(null);

  // Keep ref in sync
  useEffect(() => { activePatientRef.current = activePatient; }, [activePatient]);

  // Fetch historical data and stats from backend whenever patient changes
  useEffect(() => {
    async function fetchHistory() {
      try {
        const res = await fetch(`${API_BASE}/api/history/${activePatient}?hours=24`);
        if (res.ok) {
          const json = await res.json();
          const loaded = json.data.map(d => {
            const t = new Date(d.time);
            return {
              time: `${t.getHours()}:${String(t.getMinutes()).padStart(2,'0')}`,
              bpm: d.bpm, spo2: d.spo2, temp: d.temp,
            };
          });
          setHistory(loaded.slice(-60)); // Show last 60 data points
          if (loaded.length > 0) {
            setMessages(m => [...m, {
              type: 'system',
              text: `Loaded ${json.count} historical records for Patient ${activePatient}`,
              time: new Date().toLocaleTimeString()
            }]);
          }
        }
      } catch (err) {
        console.log('[API] Backend not available, using live data only:', err.message);
      }
    }

    async function fetchStats() {
      try {
        const res = await fetch(`${API_BASE}/api/stats/${activePatient}`);
        if (res.ok) {
          const json = await res.json();
          setStats(json);
        }
      } catch (err) { /* backend offline, skip */ }
    }

    async function fetchFeedback() {
      try {
        const res = await fetch(`${API_BASE}/api/feedback/${activePatient}`);
        if (res.ok) {
          const json = await res.json();
          if (json.data && json.data.length > 0) {
            const pastMsgs = json.data.reverse().map(f => ({
              type: 'doctor',
              text: f.message,
              time: new Date(f.time).toLocaleTimeString()
            }));
            setMessages(m => [m[0], ...pastMsgs, ...m.slice(1)]);
          }
        }
      } catch (err) { /* backend offline, skip */ }
    }

    // Reset vitals for fresh patient view
    setVitals({ bpm: '--', spo2: '--', temp: '--' });
    setAlertMsg('NORMAL');
    setHistory([]);
    setStats(null);
    setMessages([{
      type: 'system',
      text: `Switched to Patient ${activePatient}. Loading history...`,
      time: new Date().toLocaleTimeString()
    }]);

    fetchHistory();
    fetchStats();
    fetchFeedback();
  }, [activePatient]);

  // MQTT re-subscription when patient changes
  useEffect(() => {
    const client = clientRef.current;
    if (!client || !connected) return;

    for (let p = 1; p <= 3; p++) {
      Object.values(getTopics(p)).forEach(t => client.unsubscribe(t));
    }
    const topics = getTopics(activePatient);
    Object.values(topics).forEach(t => client.subscribe(t));
  }, [activePatient, connected]);

  // Initial MQTT connection (runs once)
  useEffect(() => {
    const client = mqtt.connect(BROKER_URL, MQTT_OPTIONS);
    clientRef.current = client;

    client.on('connect', () => {
      setConnected(true);
      const topics = getTopics(activePatientRef.current);
      Object.values(topics).forEach(t => client.subscribe(t));
    });

    client.on('disconnect', () => setConnected(false));
    client.on('error',      () => setConnected(false));
    client.on('offline',    () => setConnected(false));

    client.on('message', (topic, payload) => {
      const val = payload.toString();
      const now = new Date();
      const TOPICS = getTopics(activePatientRef.current);

      setVitals(prev => {
        const next = { ...prev };
        if (topic === TOPICS.bpm)  next.bpm  = val;
        if (topic === TOPICS.spo2) next.spo2 = val;
        if (topic === TOPICS.temp) next.temp = val;

        if (topic === TOPICS.bpm || topic === TOPICS.spo2 || topic === TOPICS.temp) {
          setHistory(h => {
            const point = {
              time: `${now.getHours()}:${String(now.getMinutes()).padStart(2,'0')}:${String(now.getSeconds()).padStart(2,'0')}`,
              bpm:  parseFloat(topic === TOPICS.bpm  ? val : prev.bpm)  || 0,
              spo2: parseFloat(topic === TOPICS.spo2 ? val : prev.spo2) || 0,
              temp: parseFloat(topic === TOPICS.temp ? val : prev.temp) || 0,
            };
            return [...h.slice(-59), point]; // keep last 60 points
          });
        }
        return next;
      });

      if (topic === TOPICS.alert) setAlertMsg(val);
      if (topic === TOPICS.status) {
        setMessages(m => [...m, { type: 'system', text: `Device P${activePatientRef.current}: ${val}`, time: now.toLocaleTimeString() }]);
      }
    });

    return () => { client.end(); };
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  const sendFeedback = useCallback((text) => {
    if (clientRef.current && text.trim()) {
      const topic = getTopics(activePatientRef.current).feedback;
      clientRef.current.publish(topic, text.trim());
      const time = new Date().toLocaleTimeString();
      setMessages(m => [...m, { type: 'doctor', text: text.trim(), time }]);
    }
  }, []);

  return { connected, vitals, alertMsg, messages, history, stats, sendFeedback };
}
