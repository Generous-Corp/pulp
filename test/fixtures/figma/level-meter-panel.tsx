// Source: Figma Make export (sanitized for Pulp runtime import)

import { useEffect, useRef, useState } from "react";

export default function LevelMeterPanel() {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const [input, setInput] = useState(0.64);
  const [output, setOutput] = useState(0.48);
  const [monitoring, setMonitoring] = useState(true);

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
      const now = performance.now() / 1000;
      const inputPeak = monitoring ? Math.min(1, Math.max(0, input + Math.sin(now * 2.2) * 0.12)) : 0.1;
      const outputPeak = monitoring ? Math.min(1, Math.max(0, output + Math.cos(now * 1.7) * 0.1)) : 0.08;
      const bars = 18;
      const gap = 4;
      const laneHeight = Math.floor((height - 18) / 2);
      const barWidth = (width - gap * (bars - 1)) / bars;

      ctx.clearRect(0, 0, width, height);
      ctx.fillStyle = "#10131c";
      ctx.fillRect(0, 0, width, height);

      for (let lane = 0; lane < 2; lane++) {
        const peak = lane === 0 ? inputPeak : outputPeak;
        const yBase = lane === 0 ? laneHeight : height - 4;
        for (let i = 0; i < bars; i++) {
          const ratio = (i + 1) / bars;
          const h = Math.max(5, laneHeight * ratio);
          ctx.fillStyle = ratio <= peak ? (ratio > 0.84 ? "#fb7185" : "#38bdf8") : "#263244";
          ctx.fillRect(i * (barWidth + gap), yBase - h, barWidth, h);
        }
      }

      ctx.fillStyle = "#dbeafe";
      ctx.font = "12px Inter, sans-serif";
      ctx.fillText("FIG", 8, 16);

      if (active) requestAnimationFrame(draw);
    };

    draw();
    return () => {
      active = false;
    };
  }, [input, output, monitoring]);

  const panelStyle = {
    width: 428,
    minHeight: 304,
    display: "flex",
    flexDirection: "column" as const,
    gap: 16,
    padding: 18,
    backgroundColor: "#0f172a",
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

  const controlStyle = {
    display: "flex",
    flexDirection: "column" as const,
    gap: 6,
    flexGrow: 1,
  };

  return (
    <div id="figma-level-meter-panel" style={panelStyle}>
      <div style={{ ...rowStyle, justifyContent: "space-between" }}>
        <div>
          <h2 style={{ margin: 0, fontSize: 22, lineHeight: 1.1 }}>Level Matrix</h2>
          <p style={{ margin: "6px 0 0", color: "#a5b4fc", fontSize: 13 }}>
            Input and output monitor from a sanitized Figma Make panel
          </p>
        </div>
        <button
          type="button"
          onClick={() => setMonitoring(!monitoring)}
          style={{
            minWidth: 92,
            minHeight: 36,
            borderRadius: 6,
            border: "1px solid #475569",
            backgroundColor: monitoring ? "#075985" : "#1f2937",
            color: "#f8fafc",
            cursor: "pointer",
          }}
        >
          {monitoring ? "Live" : "Hold"}
        </button>
      </div>

      <canvas
        ref={canvasRef}
        width={392}
        height={118}
        style={{
          width: "100%",
          height: 118,
          borderRadius: 6,
          border: "1px solid #1f2937",
          backgroundColor: "#10131c",
        }}
      />

      <div style={rowStyle}>
        <div style={controlStyle}>
          <span style={{ fontSize: 12, color: "#cbd5e1" }}>Input</span>
          <input
            type="range"
            min={0}
            max={1}
            step={0.01}
            value={input}
            onChange={(event) => setInput(Number(event.currentTarget.value))}
          />
          <span style={{ fontSize: 12, color: "#94a3b8" }}>{Math.round(input * 100)}%</span>
        </div>
        <div style={controlStyle}>
          <span style={{ fontSize: 12, color: "#cbd5e1" }}>Output</span>
          <input
            type="range"
            min={0}
            max={1}
            step={0.01}
            value={output}
            onChange={(event) => setOutput(Number(event.currentTarget.value))}
          />
          <span style={{ fontSize: 12, color: "#94a3b8" }}>{Math.round(output * 100)}%</span>
        </div>
      </div>
    </div>
  );
}
