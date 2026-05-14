// Source: Pencil JSX export
// Sanitized: tokens and utility classes expanded to inline styles.
import React, { useEffect, useRef, useState } from 'react';

export default function GainStageCard() {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const [armed, setArmed] = useState(true);
  const [gain, setGain] = useState(0.62);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.fillStyle = '#111827';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    for (let i = 0; i < 18; i += 1) {
      const ratio = (i + 1) / 18;
      const height = Math.max(8, ratio * 92);
      ctx.fillStyle = ratio < gain ? '#22c55e' : '#263244';
      if (ratio > 0.78 && armed) ctx.fillStyle = '#f97316';
      ctx.fillRect(i * 12, canvas.height - height - 8, 8, height);
    }
  }, [armed, gain]);

  return (
    <div id="pencil-gain-stage-card" data-pencil-export="tailwind-jsx-sanitized" style={styles.panel}>
      <div style={styles.header}>
        <div style={styles.titleBlock}>
          <p style={styles.eyebrow}>Pencil export</p>
          <h2 style={styles.title}>Gain Stage Card</h2>
        </div>
        <button type="button" onClick={() => setArmed(!armed)} style={armed ? styles.armed : styles.bypass}>
          {armed ? 'Armed' : 'Bypass'}
        </button>
      </div>

      <canvas ref={canvasRef} width={224} height={116} style={styles.meter} />

      <div style={styles.controlRow}>
        <span style={styles.label}>Input gain</span>
        <input
          type="range"
          min={0}
          max={1}
          step={0.01}
          value={gain}
          onChange={(event) => setGain(Number(event.currentTarget.value))}
          style={styles.slider}
        />
        <span style={styles.value}>{Math.round(gain * 100)}%</span>
      </div>
    </div>
  );
}

const styles = {
  panel: {
    width: 420,
    minHeight: 320,
    display: 'flex',
    flexDirection: 'column',
    gap: 18,
    padding: 20,
    backgroundColor: 'rgb(11, 16, 32)',
    color: '#f8fafc',
    borderRadius: 8,
    border: '1px solid #334155',
    fontFamily: 'Inter, system-ui, sans-serif',
    boxShadow: '0 18px 45px rgba(0, 0, 0, 0.35)',
  },
  header: {
    display: 'flex',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: 16,
  },
  titleBlock: {
    display: 'flex',
    flexDirection: 'column',
    gap: 4,
  },
  eyebrow: {
    margin: 0,
    color: '#93c5fd',
    fontSize: 12,
    fontWeight: 600,
  },
  title: {
    margin: 0,
    color: '#f8fafc',
    fontSize: 24,
    fontWeight: 700,
  },
  armed: {
    minWidth: 88,
    minHeight: 36,
    border: '1px solid #166534',
    borderRadius: 6,
    backgroundColor: '#14532d',
    color: '#dcfce7',
    fontWeight: 700,
  },
  bypass: {
    minWidth: 88,
    minHeight: 36,
    border: '1px solid #475569',
    borderRadius: 6,
    backgroundColor: '#1f2937',
    color: '#e5e7eb',
    fontWeight: 700,
  },
  meter: {
    width: '100%',
    height: 116,
    borderRadius: 6,
    border: '1px solid #1f2937',
    backgroundColor: '#111827',
  },
  controlRow: {
    display: 'flex',
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
  },
  label: {
    minWidth: 76,
    color: '#cbd5e1',
    fontSize: 13,
    fontWeight: 600,
  },
  slider: {
    flex: 1,
  },
  value: {
    minWidth: 40,
    color: '#94a3b8',
    fontSize: 12,
    textAlign: 'right',
  },
};
