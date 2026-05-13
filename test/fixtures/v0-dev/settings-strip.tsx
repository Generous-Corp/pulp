"use client";

import { useState } from "react";

export default function SettingsStrip() {
  const [inputGain, setInputGain] = useState(0.34);
  const [mix, setMix] = useState(0.72);
  const [wide, setWide] = useState(false);
  const [monitor, setMonitor] = useState(true);

  const shellStyle = {
    width: 520,
    minHeight: 190,
    display: "flex",
    flexDirection: "column" as const,
    gap: 14,
    padding: 18,
    backgroundColor: "#111827",
    color: "#f8fafc",
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

  const sliderStyle = {
    display: "flex",
    flexDirection: "column" as const,
    gap: 6,
    flexGrow: 1,
  };

  const toggleStyle = (active: boolean) => ({
    minWidth: 92,
    minHeight: 34,
    borderRadius: 6,
    border: "1px solid #475569",
    backgroundColor: active ? "#0f766e" : "#1f2937",
    color: "#f8fafc",
    cursor: "pointer",
  });

  return (
    <div id="v0-settings-strip" style={shellStyle}>
      <div style={{ ...rowStyle, justifyContent: "space-between" }}>
        <div>
          <h3 style={{ margin: 0, fontSize: 18, lineHeight: 1.1 }}>Input Settings</h3>
          <p style={{ margin: "5px 0 0", color: "#94a3b8", fontSize: 13 }}>
            Gain staging and monitor options
          </p>
        </div>
        <div style={rowStyle}>
          <button type="button" style={toggleStyle(wide)} onClick={() => setWide(!wide)}>
            {wide ? "Wide" : "Narrow"}
          </button>
          <button type="button" style={toggleStyle(monitor)} onClick={() => setMonitor(!monitor)}>
            {monitor ? "Monitor" : "Mute"}
          </button>
        </div>
      </div>

      <div style={rowStyle}>
        <div style={sliderStyle}>
          <span style={{ fontSize: 12, color: "#cbd5e1" }}>Input</span>
          <input
            type="range"
            min={0}
            max={1}
            step={0.01}
            value={inputGain}
            onChange={(event) => setInputGain(Number(event.currentTarget.value))}
          />
          <span style={{ fontSize: 12, color: "#94a3b8" }}>{Math.round(inputGain * 100)}%</span>
        </div>
        <div style={sliderStyle}>
          <span style={{ fontSize: 12, color: "#cbd5e1" }}>Mix</span>
          <input
            type="range"
            min={0}
            max={1}
            step={0.01}
            value={mix}
            onChange={(event) => setMix(Number(event.currentTarget.value))}
          />
          <span style={{ fontSize: 12, color: "#94a3b8" }}>{Math.round(mix * 100)}%</span>
        </div>
      </div>
    </div>
  );
}
