"use client";

import { useEffect, useRef, useState } from "react";

export default function AudioControlPanel() {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const [drive, setDrive] = useState(0.42);
  const [tone, setTone] = useState(0.58);
  const [armed, setArmed] = useState(true);

  useEffect(() => {
    let active = true;

    const draw = () => {
      const canvas = canvasRef.current;
      if (!canvas) {
        if (active) requestAnimationFrame(draw);
        return;
      }

      const ctx = canvas.getContext("2d");
      if (!ctx) {
        if (active) requestAnimationFrame(draw);
        return;
      }

      const width = canvas.width;
      const height = canvas.height;
      const time = performance.now() / 1000;
      const level = armed ? 0.52 + Math.sin(time * 2.4) * 0.18 + drive * 0.2 : 0.12;
      const peak = Math.min(1, Math.max(0, level));
      const bars = 22;
      const gap = 3;
      const barWidth = (width - gap * (bars - 1)) / bars;

      ctx.clearRect(0, 0, width, height);
      ctx.fillStyle = "#111827";
      ctx.fillRect(0, 0, width, height);

      for (let i = 0; i < bars; i++) {
        const x = i * (barWidth + gap);
        const ratio = (i + 1) / bars;
        const barHeight = Math.max(6, height * ratio * 0.86);
        const y = height - barHeight;
        ctx.fillStyle = ratio <= peak ? (ratio > 0.82 ? "#f97316" : "#22c55e") : "#263244";
        ctx.fillRect(x, y, barWidth, barHeight);
      }

      ctx.fillStyle = "#d1d5db";
      ctx.font = "12px Inter, sans-serif";
      ctx.fillText("OUT", 8, 18);

      if (active) requestAnimationFrame(draw);
    };

    draw();
    return () => {
      active = false;
    };
  }, [armed, drive]);

  const panelStyle = {
    width: 420,
    minHeight: 300,
    display: "flex",
    flexDirection: "column" as const,
    gap: 16,
    padding: 18,
    backgroundColor: "#0b1020",
    color: "#f9fafb",
    border: "1px solid #334155",
    borderRadius: 8,
    fontFamily: "Inter, system-ui, sans-serif",
  };

  const rowStyle = {
    display: "flex",
    flexDirection: "row" as const,
    gap: 12,
    alignItems: "center",
  };

  const controlStyle = {
    display: "flex",
    flexDirection: "column" as const,
    gap: 6,
    flexGrow: 1,
  };

  return (
    <div id="v0-audio-control-panel" style={panelStyle}>
      <div style={{ ...rowStyle, justifyContent: "space-between" }}>
        <div>
          <h2 style={{ margin: 0, fontSize: 22, lineHeight: 1.1 }}>Pulse Channel</h2>
          <p style={{ margin: "6px 0 0", color: "#94a3b8", fontSize: 13 }}>
            Drive, tone, and output meter
          </p>
        </div>
        <button
          type="button"
          onClick={() => setArmed(!armed)}
          style={{
            minWidth: 88,
            minHeight: 36,
            borderRadius: 6,
            border: "1px solid #475569",
            backgroundColor: armed ? "#14532d" : "#1f2937",
            color: "#f8fafc",
            cursor: "pointer",
          }}
        >
          {armed ? "Armed" : "Bypass"}
        </button>
      </div>

      <canvas
        ref={canvasRef}
        width={384}
        height={112}
        style={{
          width: "100%",
          height: 112,
          borderRadius: 6,
          border: "1px solid #1f2937",
          backgroundColor: "#111827",
        }}
      />

      <div style={rowStyle}>
        <div style={controlStyle}>
          <span style={{ fontSize: 12, color: "#cbd5e1" }}>Drive</span>
          <input
            type="range"
            min={0}
            max={1}
            step={0.01}
            value={drive}
            onChange={(event) => setDrive(Number(event.currentTarget.value))}
          />
          <span style={{ fontSize: 12, color: "#94a3b8" }}>{Math.round(drive * 100)}%</span>
        </div>
        <div style={controlStyle}>
          <span style={{ fontSize: 12, color: "#cbd5e1" }}>Tone</span>
          <input
            type="range"
            min={0}
            max={1}
            step={0.01}
            value={tone}
            onChange={(event) => setTone(Number(event.currentTarget.value))}
          />
          <span style={{ fontSize: 12, color: "#94a3b8" }}>{Math.round(tone * 100)}%</span>
        </div>
      </div>
    </div>
  );
}
