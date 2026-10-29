// Browser twin of cv_control.hpp. It operates on one coherent result from one
// inference pass and never runs inside the audio worklet.
export class FaceMapping {
  constructor(confidenceGate = 0.6, smoothing = 0.25) {
    this.confidenceGate = confidenceGate;
    this.smoothing = smoothing;
    this.value = 0;
    this.tracked = false;
    this.sequence = 0;
  }

  submit(frame) {
    const valid = Boolean(frame?.tracked) && Number.isFinite(frame?.mouthOpen) &&
      Number.isFinite(frame?.confidence) && frame.confidence >= this.confidenceGate;
    const target = valid ? Math.max(0, Math.min(1, frame.mouthOpen)) : 0;
    this.value += this.smoothing * (target - this.value);
    this.value = Math.max(0, Math.min(1, this.value));
    this.tracked = valid;
    this.sequence = Number.isInteger(frame?.sequence) ? frame.sequence : this.sequence;
    return this.value;
  }

  expire() {
    this.value = 0;
    this.tracked = false;
  }
}
