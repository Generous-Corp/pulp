"use client";

import { useEffect, useRef, useState } from "react";

export default function TransportMeter() {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const [playing, setPlaying] = useState(false);
  const [rate, setRate] = useState(0.5);

  useEffect(() => {
    let active = true;

    const draw = () => {
      const canvas = canvasRef.current;
      const ctx = canvas?.getContext("2d");
      if (!canvas || !ctx) {
        if (active) requestAnimationFrame(draw);
        return;
      }

      const width = canvas.width;
      const height = canvas.height;
      const time = performance.now() / 1000;
      const phase = playing ? (time * (0.8 + rate * 2.2)) % 1 : rate;

      ctx.clearRect(0, 0, width, height);
      ctx.fillStyle = "#0f172a";
      ctx.fillRect(0, 0, width, height);

      ctx.fillStyle = "#1e293b";
      ctx.fillRect(12, 20, width - 24, 18);
      ctx.fillStyle = "#38bdf8";
      ctx.fillRect(12, 20, (width - 24) * phase, 18);

      ctx.fillStyle = "#f8fafc";
      ctx.font = "12px Inter, sans-serif";
      ctx.fillText(playing ? "PLAYING" : "STOPPED", 12, 64);

      if (active) requestAnimationFrame(draw);
    };

    draw();
    return () => {
      active = false;
    };
  }, [playing, rate]);

  const shellStyle = {
    width: 460,
    minHeight: 230,
    display: "flex",
    flexDirection: "column" as const,
    gap: 14,
    padding: 18,
    backgroundColor: "#171717",
    color: "#fafafa",
    border: "1px solid #3f3f46",
    borderRadius: 8,
    fontFamily: "Inter, system-ui, sans-serif",
  };

  const rowStyle = {
    display: "flex",
    flexDirection: "row" as const,
    gap: 10,
    alignItems: "center",
  };

  const buttonStyle = (active: boolean) => ({
    minWidth: 86,
    minHeight: 36,
    borderRadius: 6,
    border: "1px solid #52525b",
    backgroundColor: active ? "#2563eb" : "#27272a",
    color: "#fafafa",
    cursor: "pointer",
  });

  return (
    <div id="v0-transport-meter" style={shellStyle}>
      <div style={{ ...rowStyle, justifyContent: "space-between" }}>
        <div>
          <h3 style={{ margin: 0, fontSize: 18, lineHeight: 1.1 }}>Transport</h3>
          <p style={{ margin: "5px 0 0", color: "#a1a1aa", fontSize: 13 }}>
            Clock and activity meter
          </p>
        </div>
        <div style={rowStyle}>
          <button type="button" style={buttonStyle(playing)} onClick={() => setPlaying(!playing)}>
            {playing ? "Pause" : "Play"}
          </button>
          <button type="button" style={buttonStyle(!playing)} onClick={() => setPlaying(false)}>
            Stop
          </button>
        </div>
      </div>

      <canvas
        ref={canvasRef}
        width={424}
        height={84}
        style={{
          width: "100%",
          height: 84,
          borderRadius: 6,
          border: "1px solid #27272a",
          backgroundColor: "#0f172a",
        }}
      />

      <div style={{ display: "flex", flexDirection: "column", gap: 6 }}>
        <span style={{ fontSize: 12, color: "#d4d4d8" }}>Rate</span>
        <input
          type="range"
          min={0}
          max={1}
          step={0.01}
          value={rate}
          onChange={(event) => setRate(Number(event.currentTarget.value))}
        />
      </div>
    </div>
  );
}
