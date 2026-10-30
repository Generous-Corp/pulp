# CV Face Tone vertical slice

This example keeps capture and inference outside Pulp. The same C++ processor,
mapping, and additive modulation run in native standalone, VST3/CLAP/AU, and
the browser WAM. Brightness remains the saved and automatable base value.

- `cv_control.hpp` is the application-owned sample, smoothing, confidence,
  and loss policy. An IMU, proximity sensor, or CV provider can reduce its
  result to the same normalized sample. The audio thread owns this mapping.
- `main.cpp` is a native standalone host. Set `PULP_CV_OSC_PORT=9001` and send
  `/pulp/cv/face` with `mouth_open`, `confidence`, `tracked`, and optional
  `sequence` arguments, or `/pulp/control` with normalized `value`,
  `confidence`, `valid`, and optional `sequence` for other inputs. The first
  three OSC arguments must be float, float, int. An external provider can send
  these messages; no camera library is linked into Pulp.
- `CvFaceTone` is also a VST3/CLAP/standalone plugin. The optional OSC receiver
  is started in `prepare()` and stopped in `release()`. Pulp's TripleBuffer
  hands one complete provider sample to the audio callback without blocking.
  The callback applies a 250 ms sample-count timeout and reads the saved base
  from StateStore. It never writes tracking into the saved parameter.
- `web/index.html` uses browser camera permission and MediaPipe Face Landmarker,
  then the existing WAM adapter. It sends one channel-16 pitch-bend packet per
  frame through WAM's short MIDI path. The same C++ mapping and modulation run
  in the worklet. The packet carries 13-bit normalized value plus one validity
  bit; this example reserves channel 16. The page treats MediaPipe tracking
  presence as confidence 1 and absence as confidence 0. It does not claim a
  graded confidence score. The page's Start button also starts a demo note.

Build the native target with `pulp build --target pulp-cv-control-standalone` and
run `pulp-cv-control-test`. Build the WAM target using the existing web-demo
instructions; it emits `CvFaceToneWorklet.js` for the page. Serve this directory
through HTTP(S), since camera APIs and AudioWorklet are not available from
`file://`.

The browser requires a secure context for camera access and a user gesture for
audio. Stop releases camera tracks and audio resources. If the provider stops
without sending a loss packet, the processor expires its last value after 250
ms of processed audio. No CV dependency is added to Pulp or its native builds.

## Other physical inputs

A motion adapter can read an iPhone/IMU, BLE device, serial sensor, or game
controller on its own thread and publish one `ControlSample` through the
TripleBuffer (or send `/pulp/control` to the plugin). The synth, smoothing,
replay, loss behavior, and audio boundary do not change. Browser adapters can
send the example's short MIDI packet. They may use Web Bluetooth, WebHID, Web
Serial, DeviceMotion, or camera APIs when the origin and permission policy allows
it; desktop adapters can use native SDKs or an external process. No universal
hardware layer is introduced.
