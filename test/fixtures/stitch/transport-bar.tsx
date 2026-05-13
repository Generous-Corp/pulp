import { useEffect, useRef, useState } from "react";

export default function TransportBar() {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const [playing, setPlaying] = useState(true);
  const [position, setPosition] = useState(0.34);
  const [gain, setGain] = useState(0.62);

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
      const level = playing ? 0.38 + Math.sin(time * 2.1) * 0.16 + gain * 0.28 : 0.1;
      const peak = Math.max(0, Math.min(1, level));
      const bars = 20;
      const gap = 3;
      const barWidth = (width - gap * (bars - 1)) / bars;

      ctx.clearRect(0, 0, width, height);
      ctx.fillStyle = "#101826";
      ctx.fillRect(0, 0, width, height);

      for (let index = 0; index < bars; index++) {
        const ratio = (index + 1) / bars;
        const x = index * (barWidth + gap);
        const barHeight = Math.max(8, height * ratio * 0.82);
        const y = height - barHeight;
        ctx.fillStyle = ratio <= peak ? (ratio > 0.82 ? "#f59e0b" : "#2dd4bf") : "#243244";
        ctx.fillRect(x, y, barWidth, barHeight);
      }

      ctx.fillStyle = "#cbd5e1";
      ctx.font = "12px Inter, sans-serif";
      ctx.fillText("VU", 8, 18);

      if (active) requestAnimationFrame(draw);
    };

    draw();
    return () => {
      active = false;
    };
  }, [gain, playing]);

  const panelStyle = {
    width: 460,
    minHeight: 278,
    display: "flex",
    flexDirection: "column" as const,
    gap: 16,
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

  const controlStyle = {
    display: "flex",
    flexDirection: "column" as const,
    gap: 6,
    flexGrow: 1,
  };

  const buttonStyle = {
    minWidth: 82,
    minHeight: 36,
    borderRadius: 6,
    border: "1px solid #475569",
    backgroundColor: playing ? "#115e59" : "#1f2937",
    color: "#f8fafc",
    cursor: "pointer",
  };

  return (
    <div id="stitch-transport-bar" data-stitch-screen="transport-bar" style={panelStyle}>
      <div style={{ ...rowStyle, justifyContent: "space-between" }}>
        <div>
          <h2 style={{ margin: 0, fontSize: 22, lineHeight: 1.1 }}>Transport</h2>
          <p style={{ margin: "6px 0 0", color: "#94a3b8", fontSize: 13 }}>
            Level and playback
          </p>
        </div>
        <div style={rowStyle}>
          <button type="button" style={buttonStyle}>
            Play
          </button>
          <button type="button" style={buttonStyle}>
            Stop
          </button>
        </div>
      </div>

      <canvas
        ref={canvasRef}
        width={420}
        height={104}
        style={{
          width: "100%",
          height: 104,
          borderRadius: 6,
          border: "1px solid #1f2937",
          backgroundColor: "#101826",
        }}
      />

      <div style={rowStyle}>
        <div style={controlStyle}>
          <span style={{ color: "#cbd5e1", fontSize: 12 }}>Position</span>
          <input
            type="range"
            min={0}
            max={1}
            step={0.01}
            value={position}
            onChange={(event) => setPosition(Number(event.currentTarget.value))}
          />
          <span style={{ color: "#94a3b8", fontSize: 12 }}>{Math.round(position * 100)}%</span>
        </div>
        <div style={controlStyle}>
          <span style={{ color: "#cbd5e1", fontSize: 12 }}>Gain</span>
          <input
            type="range"
            min={0}
            max={1}
            step={0.01}
            value={gain}
            onChange={(event) => setGain(Number(event.currentTarget.value))}
          />
          <span style={{ color: "#94a3b8", fontSize: 12 }}>{Math.round(gain * 100)}%</span>
        </div>
      </div>
    </div>
  );
}
