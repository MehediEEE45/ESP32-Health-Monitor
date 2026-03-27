import { useState, useEffect, useRef } from 'react';
import {
  LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer
} from 'recharts';
import { useMqtt } from './useMqtt';
import './index.css';

// —— Helpers ——
function Clock() {
  const [t, setT] = useState(new Date());
  useEffect(() => { const id = setInterval(() => setT(new Date()), 1000); return () => clearInterval(id); }, []);
  return <span className="header-time">{t.toLocaleTimeString()}</span>;
}

// —— Vital Card ——
function VitalCard({ cls, icon, label, value, unit, sublabel, barPct, barColor, isAlert }) {
  return (
    <div className={`glass-card vital-card vital-${cls} ${isAlert ? 'alert-active' : ''}`}>
      <div className="vital-header">
        <div className="vital-icon-wrap">{icon}</div>
        <div style={{ textAlign: 'right' }}>
          <div className="vital-label">{label}</div>
          <div className="vital-trend">{sublabel}</div>
        </div>
      </div>
      <div className="vital-value-row">
        <span className="vital-value">{value}</span>
        <span className="vital-unit">{unit}</span>
      </div>
      <div className="vital-bar">
        <div className="vital-bar-fill" style={{ width: `${Math.min(100, Math.max(0, barPct))}%`, background: barColor }} />
      </div>
    </div>
  );
}

// —— Custom tooltip for chart ——
const CustomTooltip = ({ active, payload, label }) => {
  if (!active || !payload?.length) return null;
  return (
    <div style={{
      background: 'rgba(8,12,20,0.95)', border: '1px solid rgba(61,158,255,0.2)',
      borderRadius: '10px', padding: '10px 14px', fontSize: '0.78rem'
    }}>
      <p style={{ color: '#7fa6c9', marginBottom: 6 }}>{label}</p>
      {payload.map(p => (
        <p key={p.dataKey} style={{ color: p.color, fontWeight: 600 }}>
          {p.name}: <span style={{ color: '#e8f4ff' }}>{p.value}</span>
        </p>
      ))}
    </div>
  );
};

// —— Feedback Panel ——
const PRESETS = ['Rest and hydrate', 'Take Paracetamol', 'Visit clinic ASAP', 'Monitor closely', 'All vitals normal ✓'];

function FeedbackPanel({ messages, onSend }) {
  const [text, setText] = useState('');
  const bottomRef = useRef(null);
  useEffect(() => { bottomRef.current?.scrollIntoView({ behavior: 'smooth' }); }, [messages]);

  const handleSend = () => { if (text.trim()) { onSend(text); setText(''); } };

  return (
    <div className="glass-card feedback-panel">
      <div className="feedback-panel-header">
        <span style={{ fontSize: '1.2rem' }}>💬</span>
        <h3>Doctor Feedback</h3>
      </div>

      <div className="messages-list">
        {messages.map((m, i) => (
          <div key={i} className={`message-bubble ${m.type}`}>
            {m.text}
            <div className="msg-time">{m.time}</div>
          </div>
        ))}
        <div ref={bottomRef} />
      </div>

      <div className="feedback-form">
        <div className="feedback-presets">
          {PRESETS.map(p => (
            <button key={p} className="preset-btn" onClick={() => onSend(p)}>{p}</button>
          ))}
        </div>
        <div className="feedback-input-wrap">
          <textarea
            className="feedback-textarea"
            placeholder="Type medical instruction..."
            value={text}
            onChange={e => setText(e.target.value)}
            onKeyDown={e => { if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); handleSend(); } }}
          />
          <button className="send-btn" onClick={handleSend} disabled={!text.trim()} title="Send">
            ➤
          </button>
        </div>
      </div>
    </div>
  );
}

// —— Main App ——
export default function App() {
  const [activePatient, setActivePatient] = useState(1);
  const { connected, vitals, alertMsg, messages, history, sendFeedback } = useMqtt(activePatient);
  const isEmergency = alertMsg?.includes('DANGER');
  const fingerOn = vitals.bpm !== '--';

  const bpmVal  = parseFloat(vitals.bpm)  || 0;
  const spo2Val = parseFloat(vitals.spo2) || 0;
  const tempVal = parseFloat(vitals.temp) || 0;

  return (
    <div className="app-wrapper">
      {/* HEADER */}
      <header className="header">
        <div className="header-brand">
          <div className="logo-icon">🫀</div>
          <div>
            <h1>HealthMonitor Pro</h1>
            <span>Doctor Dashboard — Group 08</span>
          </div>
        </div>
        <div className="header-right">
          <Clock />
          <div className={`connection-status ${connected ? 'connected' : 'disconnected'}`}>
            <div className="status-dot" />
            {connected ? 'MQTT Online' : 'Connecting...'}
          </div>
        </div>
      </header>

      <main className="main-content">
        {/* Emergency Banner */}
        {isEmergency && (
          <div className="emergency-banner">
            <span className="icon">🚨</span>
            <div>
              <p>CRITICAL PATIENT ALERT</p>
              <small>Vitals exceeded safe thresholds — immediate attention required</small>
            </div>
          </div>
        )}

        {/* Patient Selector */}
        <div className="patient-bar">
          {[1, 2, 3].map((p) => (
            <div
              key={p}
              className={`patient-chip ${activePatient === p ? 'active' : ''}`}
              onClick={() => setActivePatient(p)}
            >
              <div className="patient-avatar">P{p}</div>
              Patient 0{p}
            </div>
          ))}
        </div>

        {/* Vitals Grid */}
        <div className="vitals-grid">
          <VitalCard
            cls="bpm"
            icon="❤️"
            label="Heart Rate"
            value={vitals.bpm}
            unit="BPM"
            sublabel={bpmVal > 100 ? '⚠ High' : bpmVal < 40 && bpmVal > 0 ? '⚠ Low' : 'Normal'}
            barPct={(bpmVal / 180) * 100}
            barColor="linear-gradient(90deg, #ff4560, #ff8c00)"
            isAlert={bpmVal > 120 || (bpmVal < 40 && bpmVal > 0)}
          />
          <VitalCard
            cls="spo2"
            icon="🫁"
            label="SpO₂ Level"
            value={vitals.spo2}
            unit="%"
            sublabel={spo2Val < 90 && spo2Val > 0 ? '⚠ Low O₂' : 'Saturation OK'}
            barPct={spo2Val}
            barColor="linear-gradient(90deg, #3d9eff, #00e5ff)"
            isAlert={spo2Val < 90 && spo2Val > 0}
          />
          <VitalCard
            cls="temp"
            icon="🌡️"
            label="Body Temperature"
            value={vitals.temp}
            unit="°C"
            sublabel={tempVal > 38 ? '⚠ Fever' : tempVal > 0 ? 'Normal' : '—'}
            barPct={tempVal > 0 ? ((tempVal - 35) / 8) * 100 : 0}
            barColor="linear-gradient(90deg, #feb019, #ff6b35)"
            isAlert={tempVal > 38}
          />
        </div>

        {/* No finger message */}
        {!fingerOn && connected && (
          <div className="no-data-overlay">
            <div className="big-icon">👆</div>
            <p>Waiting for patient to place finger on the MAX30102 sensor...</p>
          </div>
        )}

        {/* Bottom grid: Chart + Feedback */}
        <div className="bottom-grid">
          {/* Vitals Chart */}
          <div className="glass-card">
            <div className="chart-panel-header">
              <h3>📈 Vitals History (Live)</h3>
              <div className="chart-legend">
                <div className="legend-item"><div className="legend-dot" style={{ background: '#ff4560' }}/> BPM</div>
                <div className="legend-item"><div className="legend-dot" style={{ background: '#3d9eff' }}/> SpO₂</div>
                <div className="legend-item"><div className="legend-dot" style={{ background: '#feb019' }}/> Temp</div>
              </div>
            </div>
            {history.length > 0 ? (
              <ResponsiveContainer width="100%" height={280}>
                <LineChart data={history} margin={{ top: 5, right: 10, left: -20, bottom: 5 }}>
                  <CartesianGrid stroke="rgba(255,255,255,0.04)" />
                  <XAxis dataKey="time" tick={{ fill: '#4a6a8a', fontSize: 11 }} tickLine={false} />
                  <YAxis tick={{ fill: '#4a6a8a', fontSize: 11 }} tickLine={false} axisLine={false} />
                  <Tooltip content={<CustomTooltip />} />
                  <Line type="monotone" dataKey="bpm"  name="BPM"  stroke="#ff4560" strokeWidth={2} dot={false} activeDot={{ r: 5 }} />
                  <Line type="monotone" dataKey="spo2" name="SpO₂" stroke="#3d9eff" strokeWidth={2} dot={false} activeDot={{ r: 5 }} />
                  <Line type="monotone" dataKey="temp" name="Temp" stroke="#feb019" strokeWidth={2} dot={false} activeDot={{ r: 5 }} />
                </LineChart>
              </ResponsiveContainer>
            ) : (
              <div style={{ height: 280, display: 'flex', alignItems: 'center', justifyContent: 'center', color: '#4a6a8a', fontSize: '0.85rem' }}>
                Chart will populate when data arrives from the device...
              </div>
            )}
          </div>

          {/* Doctor Feedback */}
          <FeedbackPanel messages={messages} onSend={sendFeedback} />
        </div>
      </main>

      <footer className="footer">
        EEE 330 — Basic Communication Engineering Lab · Group 08 · Shahjalal University of Science & Technology, Sylhet
      </footer>
    </div>
  );
}
