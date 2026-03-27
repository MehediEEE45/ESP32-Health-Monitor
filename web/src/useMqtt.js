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

const TOPICS = {
  bpm:      'group08/health/bpm',
  spo2:     'group08/health/spo2',
  temp:     'group08/health/temp',
  alert:    'group08/health/alert',
  status:   'group08/health/status',
  feedback: 'group08/health/feedback',
};

export function useMqtt() {
  const clientRef = useRef(null);
  const [connected, setConnected] = useState(false);
  const [vitals, setVitals] = useState({ bpm: '--', spo2: '--', temp: '--' });
  const [alertMsg, setAlertMsg] = useState('NORMAL');
  const [messages, setMessages] = useState([
    { type: 'system', text: 'Doctor portal connected. Waiting for patient data...', time: new Date().toLocaleTimeString() }
  ]);
  const [history, setHistory] = useState([]);

  useEffect(() => {
    const client = mqtt.connect(BROKER_URL, MQTT_OPTIONS);
    clientRef.current = client;

    client.on('connect', () => {
      setConnected(true);
      Object.values(TOPICS).forEach(t => client.subscribe(t));
    });

    client.on('disconnect', () => setConnected(false));
    client.on('error', () => setConnected(false));
    client.on('offline', () => setConnected(false));

    client.on('message', (topic, payload) => {
      const val = payload.toString();
      const now = new Date();

      setVitals(prev => {
        const next = { ...prev };
        if (topic === TOPICS.bpm)  { next.bpm  = val; }
        if (topic === TOPICS.spo2) { next.spo2 = val; }
        if (topic === TOPICS.temp) { next.temp = val; }

        // Build history point whenever any vital arrives
        if (topic === TOPICS.bpm || topic === TOPICS.spo2 || topic === TOPICS.temp) {
          setHistory(h => {
            const point = {
              time: `${now.getHours()}:${String(now.getMinutes()).padStart(2,'0')}:${String(now.getSeconds()).padStart(2,'0')}`,
              bpm:  parseFloat(topic === TOPICS.bpm  ? val : prev.bpm)  || 0,
              spo2: parseFloat(topic === TOPICS.spo2 ? val : prev.spo2) || 0,
              temp: parseFloat(topic === TOPICS.temp ? val : prev.temp) || 0,
            };
            return [...h.slice(-29), point]; // keep last 30 points
          });
        }
        return next;
      });

      if (topic === TOPICS.alert) setAlertMsg(val);
      if (topic === TOPICS.status) {
        setMessages(m => [...m, { type: 'system', text: `Device: ${val}`, time: now.toLocaleTimeString() }]);
      }
    });

    return () => { client.end(); };
  }, []);

  const sendFeedback = useCallback((text) => {
    if (clientRef.current && text.trim()) {
      clientRef.current.publish(TOPICS.feedback, text.trim());
      const time = new Date().toLocaleTimeString();
      setMessages(m => [...m, { type: 'doctor', text: text.trim(), time }]);
    }
  }, []);

  return { connected, vitals, alertMsg, messages, history, sendFeedback };
}
