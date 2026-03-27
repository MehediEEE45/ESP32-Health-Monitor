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

// Generates the MQTT topic set for a given patient number
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

  // Keep ref in sync with prop
  useEffect(() => { activePatientRef.current = activePatient; }, [activePatient]);

  // When activePatient changes, re-subscribe to new topics & reset vitals
  useEffect(() => {
    const client = clientRef.current;
    if (!client || !connected) return;

    // Unsubscribe all old patient topics
    for (let p = 1; p <= 3; p++) {
      Object.values(getTopics(p)).forEach(t => client.unsubscribe(t));
    }
    // Subscribe to new patient topics
    const topics = getTopics(activePatient);
    Object.values(topics).forEach(t => client.subscribe(t));

    // Reset vitals for fresh patient view
    setVitals({ bpm: '--', spo2: '--', temp: '--' });
    setAlertMsg('NORMAL');
    setHistory([]);
    setMessages(m => [...m, {
      type: 'system',
      text: `Switched to Patient ${activePatient}`,
      time: new Date().toLocaleTimeString()
    }]);
  }, [activePatient, connected]);

  // Initial MQTT connection (runs once)
  useEffect(() => {
    const client = mqtt.connect(BROKER_URL, MQTT_OPTIONS);
    clientRef.current = client;

    client.on('connect', () => {
      setConnected(true);
      // Subscribe to initial patient topics
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
            return [...h.slice(-29), point];
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

  return { connected, vitals, alertMsg, messages, history, sendFeedback };
}
