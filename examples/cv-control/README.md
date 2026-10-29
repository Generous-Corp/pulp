# CV Face Tone vertical slice

This example keeps camera capture and inference outside Pulp. It demonstrates one
coherent face result mapped to a synth parameter while preserving the saved base
value in native/plugin builds:

- `cv_control.hpp` is the application-owned frame/sample, smoothing, confidence,
  and loss policy. `SignalMapping` is deliberately input-neutral: an IMU,
  proximity sensor, MIDI bridge, or CV provider submits the same normalized
  sample without a new Pulp subsystem.
- `main.cpp` is a native standalone host. Set `PULP_CV_OSC_PORT=9001` and send
  `/pulp/cv/face` with `mouth_open`, `confidence`, `tracked`, and optional
  `sequence` arguments, or `/pulp/control` with normalized `value`,
  `confidence`, `valid`, and optional `sequence` for motion/proximity/other sensors. OpenSeeFace, FaceOSC, or another provider can send the
  message; no camera library is linked into Pulp.
- `CvFaceTone` is also a VST3/CLAP/standalone plugin. The optional OSC receiver
  is started in `prepare()` and stopped in `release()`; `process()` only reads
  atomics and StateStore values. Host automation and the saved Brightness base
  remain separate from the ephemeral face offset.
- `web/index.html` uses browser camera permission and MediaPipe Face Landmarker,
  then the ordinary WAM adapter. The provider and capture code differ, but the
  frame mapping algorithm and loss rules are mirrored by `face_mapping.mjs`.

Build the native target with `pulp build --target pulp-cv-control-standalone` and
run `pulp-cv-control-test`. Build the WAM target using the existing web-demo
instructions; it emits `CvFaceToneWorklet.js` for the page. Serve this directory
through HTTP(S), since camera APIs and AudioWorklet are not available from
`file://`.

A browser WAM parameter write cannot currently express a separate modulation
lane, so the page retains the base value in its application closure and sends an
effective value. Native/plugin builds use the additive internal modulation path.
That is a documented limitation, not a new web-specific Pulp abstraction.

## Other physical inputs

A motion adapter can read an iPhone/IMU, BLE device, serial sensor, or game
controller on its own thread and call `mapping.submit({sequence, normalized,
confidence, true})`. The synth, smoothing, replay, loss behavior, and audio
boundary do not change. Browser adapters can use Web Bluetooth, WebHID, Web
Serial, DeviceMotion, or camera APIs when the origin and permission policy allows
it; desktop adapters can use native SDKs or an external process. No universal
hardware layer is introduced.
