// The browser provider sends one complete scalar through WAM's existing MIDI
// ingress. Channel-16 pitch bend is reserved by this example; the C++ processor
// owns confidence gating, smoothing, loss, and parameter modulation.
export function sendControlSample(adapter, value, tracked) {
  const valid = Boolean(tracked) && Number.isFinite(value);
  const normalized = valid ? Math.max(0, Math.min(1, value)) : 0;
  const quantized = Math.round(normalized * 8191);
  adapter.scheduleMidi(0xEF, quantized & 0x7F,
    (quantized >> 7) | (valid ? 0x40 : 0));
}
