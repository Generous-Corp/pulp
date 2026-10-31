# Real-world inputs

Pulp applications can map camera, sensor, controller, analysis, or network
data to parameters and application behavior without adding those providers to
Pulp core. The provider owns capture, permissions, inference, and lifecycle;
the application owns the result type and mapping policy.

## The portable recipe

Use one coherent application-owned result for values produced by one provider
inference or sensor update. Keep related values together so a hand pose, face
pose, or IMU orientation cannot be assembled from different updates.

```cpp
struct MyProviderFrame {
    std::uint64_t sequence = 0;
    float control = 0.0f;       // provider-normalized value
    float confidence = 0.0f;
    bool valid = false;
};

// Provider thread / external adapter: capture and inference happen here.
void on_provider_frame(MyProviderFrame frame) {
    // Use an application-owned handoff, such as TripleBuffer, or send an
    // existing OSC/IPC/MIDI message to the consumer.
    published.write({frame.sequence, frame.control, frame.confidence, frame.valid});
}

// Audio callback: read the latest complete result and apply an ephemeral
// modulation offset. Do not perform capture, inference, blocking I/O, or
// parameter persistence here.
const auto frame = published.read();
const float effective = base_parameter + map(frame.control, frame.confidence);
```

`examples/cv-control` is the executable reference. Its `ControlSample` and
`SignalMapping` intentionally remain application-owned. The example supports
replay, confidence gating, smoothing, malformed input rejection, provider
restart, sample-count stale expiry, native OSC, and a browser short-MIDI
packet. It does not define a universal face, hand, body, sensor, or tensor
schema.

## Choosing a handoff

| Source and consumer | Recommended existing path |
|---|---|
| Native provider to the same application or plugin process | Application-owned `TripleBuffer<T>` or another existing bounded runtime primitive. |
| External process to a desktop/plugin consumer | Existing OSC or IPC. Keep the external process responsible for camera/device permissions and inference. |
| Browser provider to a WAM processor | Existing WAM MIDI ingress; the example reserves channel-16 pitch bend for one compact control packet. |
| Host automation or musical control | Existing MIDI/MPE and StateStore/modulation paths. |
| Mobile app to a mobile plugin or companion process | Application-owned platform handoff or an optional external transport selected by the host. |

The handoff must have one clear writer and one audio-side reader. If several
providers can be selected, the application owns arbitration and provider
lifecycle; do not concurrently write the same `TripleBuffer` from multiple
providers.

## Mapping and failure behavior

Keep the saved or host-automated parameter as the base. Add or otherwise
explicitly combine the ephemeral input value, and expose the effective value
through the existing modulation/display path. Define these cases in the
application and test them with replay:

- confidence below the application gate;
- malformed or non-finite values;
- no provider or denied permission;
- provider restart and sequence reset;
- stale input after a provider stops;
- lifecycle pause/resume and unavailable hardware.

Capture and inference must stop before the application tears down their
provider resources. Audio processing must remain allocation-free and
non-blocking after preparation.

## Surface and host boundaries

- **Web/WASM:** camera and sensor APIs require browser permission and an
  allowed origin. Web Bluetooth, HID, Serial, DeviceMotion, and inference
  availability vary by browser. The mapping can remain shared with native code
  while the provider is browser-specific.
- **Standalone desktop:** native camera, IMU, BLE, serial, USB, HID, or an
  external provider may own the device. The provider must publish off the audio
  thread.
- **Audio plugins:** assume the host may deny camera/device access. Prefer a
  companion process, OSC/IPC, host MIDI, or host automation when direct access
  is unavailable.
- **iOS and Android:** Core Motion, platform sensors, camera, BLE, and mobile
  ML APIs belong to the application adapter. Permissions and pause/resume are
  platform-owned. AUv3 or other mobile plugin sandboxes may require an app or
  companion process to provide the input.

OBS Studio, DaVinci Resolve/Fairlight, Max/MSP, TouchDesigner, and similar
hosts are compatibility forcing functions. They should consume existing
plugin, OSC, MIDI, IPC, WebSocket, or external-process boundaries through
optional recipes. Pulp core must not link host SDKs or copy host code. A host
adapter becomes implementation work only when a named application requires a
live acceptance receipt.

## Optional provider and model dependencies

MediaPipe, OpenCV, OpenSeeFace, ONNX Runtime, LiteRT, Core ML, platform sensor
APIs, and device SDKs remain application-selected dependencies. Audit model
weights, task bundles, codecs, and SDK terms separately from runtime licenses.
Do not add any of them to Pulp core for an application that does not use them.

