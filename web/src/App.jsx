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
  const { connected, vitals, alertMsg, messages, history, stats, sendFeedback } = useMqtt(activePatient);
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

        {/* 24-Hour Stats Summary */}
        {stats && stats.totalReadings > 0 && (
          <div className="glass-card" style={{ padding: '16px 24px' }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 10, marginBottom: 12 }}>
              <span style={{ fontSize: '1.1rem' }}>📋</span>
              <h3 style={{ fontSize: '0.9rem', fontWeight: 700 }}>24-Hour Summary — Patient 0{activePatient}</h3>
              <span style={{ marginLeft: 'auto', fontSize: '0.75rem', color: '#4a6a8a' }}>{stats.totalReadings} records</span>
            </div>
            <div style={{ display: 'grid', gridTemplateColumns: 'repeat(4, 1fr)', gap: 14, fontSize: '0.8rem' }}>
              <div style={{ padding: '10px 14px', background: 'rgba(255,69,96,0.08)', borderRadius: 10, border: '1px solid rgba(255,69,96,0.15)' }}>
                <div style={{ color: '#ff4560', fontWeight: 700, marginBottom: 4 }}>❤️ BPM</div>
                <div style={{ color: '#e8f4ff' }}>Min: {stats.bpm?.min} / Max: {stats.bpm?.max}</div>
                <div style={{ color: '#7fa6c9' }}>Avg: {stats.bpm?.avg}</div>
              </div>
              <div style={{ padding: '10px 14px', background: 'rgba(61,158,255,0.08)', borderRadius: 10, border: '1px solid rgba(61,158,255,0.15)' }}>
                <div style={{ color: '#3d9eff', fontWeight: 700, marginBottom: 4 }}>🫁 SpO₂</div>
                <div style={{ color: '#e8f4ff' }}>Min: {stats.spo2?.min}% / Max: {stats.spo2?.max}%</div>
                <div style={{ color: '#7fa6c9' }}>Avg: {stats.spo2?.avg}%</div>
              </div>
              <div style={{ padding: '10px 14px', background: 'rgba(254,176,25,0.08)', borderRadius: 10, border: '1px solid rgba(254,176,25,0.15)' }}>
                <div style={{ color: '#feb019', fontWeight: 700, marginBottom: 4 }}>🌡️ Temp</div>
                <div style={{ color: '#e8f4ff' }}>Min: {stats.temp?.min}°C / Max: {stats.temp?.max}°C</div>
                <div style={{ color: '#7fa6c9' }}>Avg: {stats.temp?.avg}°C</div>
              </div>
              <div style={{ padding: '10px 14px', background: 'rgba(168,85,247,0.08)', borderRadius: 10, border: '1px solid rgba(168,85,247,0.15)' }}>
                <div style={{ color: '#a855f7', fontWeight: 700, marginBottom: 4 }}>🚨 Alerts</div>
                <div style={{ color: '#e8f4ff', fontSize: '1.4rem', fontWeight: 800 }}>{stats.alerts}</div>
                <div style={{ color: '#7fa6c9' }}>in last 24h</div>
              </div>
            </div>
          </div>
        )}

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
              <h3>📈 Vitals History (Live + Stored)</h3>
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

          {/* Previous Data Table */}
          <div className="glass-card" style={{ display: 'flex', flexDirection: 'column' }}>
            <div className="chart-panel-header">
              <h3>🗄️ Previous Data Log (Patient 0{activePatient})</h3>
            </div>
            {history.length > 0 ? (
              <div style={{ flex: 1, overflowY: 'auto', maxHeight: '280px', fontSize: '0.8rem' }}>
                <table style={{ width: '100%', borderCollapse: 'collapse', textAlign: 'left' }}>
                  <thead style={{ position: 'sticky', top: 0, background: 'rgba(8,12,20,0.95)', color: '#7fa6c9' }}>
                    <tr>
                      <th style={{ padding: '8px 4px', borderBottom: '1px solid rgba(255,255,255,0.1)' }}>Time</th>
                      <th style={{ padding: '8px 4px', borderBottom: '1px solid rgba(255,255,255,0.1)' }}>BPM</th>
                      <th style={{ padding: '8px 4px', borderBottom: '1px solid rgba(255,255,255,0.1)' }}>SpO₂</th>
                      <th style={{ padding: '8px 4px', borderBottom: '1px solid rgba(255,255,255,0.1)' }}>Temp</th>
                    </tr>
                  </thead>
                  <tbody>
                    {[...history].reverse().map((row, i) => (
                      <tr key={i} style={{ borderBottom: '1px solid rgba(255,255,255,0.04)' }}>
                        <td style={{ padding: '8px 4px', color: '#e8f4ff' }}>{row.time}</td>
                        <td style={{ padding: '8px 4px', color: '#ff4560', fontWeight: 600 }}>{row.bpm}</td>
                        <td style={{ padding: '8px 4px', color: '#3d9eff' }}>{row.spo2}%</td>
                        <td style={{ padding: '8px 4px', color: '#feb019' }}>{row.temp}°C</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            ) : (
              <div style={{ flex: 1, display: 'flex', alignItems: 'center', justifyContent: 'center', color: '#4a6a8a', fontSize: '0.85rem' }}>
                No previous data available yet...
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
