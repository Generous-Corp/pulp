// fmt.mjs — tiny shared formatters for harness output.
export function fmtBudget(pct) {
  if (!Number.isFinite(pct)) return "n/a";
  return pct.toFixed(2) + "%";
}
export function fmtDb(x) {
  if (x === -Infinity) return "-inf dBFS";
  if (!Number.isFinite(x)) return "n/a";
  return x.toFixed(2) + " dBFS";
}
export function fmtDelta(x) {
  if (x === -Infinity) return "-inf dB";
  if (!Number.isFinite(x)) return "n/a";
  return (x >= 0 ? "+" : "") + x.toFixed(2) + " dB";
}
