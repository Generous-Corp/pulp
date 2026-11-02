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

// Bounded application-owned multi-value packet for the existing WAM SysEx
// ingress. Values have no Pulp-defined meaning; the consumer chooses the
// mapping. Returns false instead of sending malformed or oversized packets.
export function sendMultiValueSample(adapter, values, {
  sourceId = 1, epoch = 1, sequence = 1, providerTimeNs = 0,
  status = 0, confidence = 1
} = {}) {
  if (!Array.isArray(values) || values.length < 1 || values.length > 16 ||
      !Number.isInteger(sourceId) || sourceId < 0 || sourceId > 0xffffffff ||
      !Number.isInteger(epoch) || epoch < 0 || epoch > 0xffffffff ||
      !Number.isInteger(sequence) || sequence < 0 ||
      !Number.isInteger(providerTimeNs) || providerTimeNs < 0 ||
      !Number.isInteger(status) || status < 0 || status > 4 ||
      !Number.isFinite(confidence) || values.some((v) => !Number.isFinite(v))) return false;
  const bytes = new Uint8Array(36 + values.length * 4);
  const dv = new DataView(bytes.buffer);
  bytes.set([0xf0, 0x7d, 0x50, 0x49, 1], 0);
  dv.setUint32(5, sourceId, true); dv.setUint32(9, epoch, true);
  dv.setBigUint64(13, BigInt(sequence), true); dv.setBigUint64(21, BigInt(providerTimeNs), true);
  bytes[29] = status; dv.setFloat32(30, confidence, true); bytes[34] = values.length;
  values.forEach((v, i) => dv.setFloat32(35 + i * 4, v, true));
  bytes[35 + values.length * 4] = 0xf7;
  adapter.sendSysex(bytes);
  return true;
}
