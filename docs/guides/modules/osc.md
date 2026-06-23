# OSC Module

Open Sound Control (OSC 1.0) sender and receiver over UDP. Use for control surface integration, inter-application communication, and remote plugin control.

**Status**: experimental
**Dependencies**: runtime
**Headers**: `pulp/osc/osc.hpp`, `pulp/osc/bundle.hpp`, `pulp/osc/osc_channel.hpp`

## Sending OSC Messages

```cpp
#include <pulp/osc/osc.hpp>

pulp::osc::Sender sender;
sender.connect("127.0.0.1", 9000);

// Build and send a message
pulp::osc::Message msg("/synth/freq");
msg.add(440.0f);
sender.send(msg);

// Fluent API
sender.send(pulp::osc::Message("/mixer/volume").add(0.8f).add(1));
```

## Receiving OSC Messages

```cpp
pulp::osc::Receiver receiver;
receiver.listen(9000, [](const pulp::osc::Message& msg) {
    if (msg.address == "/synth/freq") {
        float freq = msg.get_float(0, 440.0f);
        set_frequency(freq);
    }
});

// ... later
receiver.stop();
```

The receiver runs on a background thread. The callback fires on that thread — use a lock-free queue if you need to forward to the audio thread.

For address-pattern routing, bundle callbacks, and malformed packet reporting, use `listen_with_options`:

```cpp
pulp::osc::ReceiverOptions options;
options.routes.push_back({
    "/synth/*",
    [](const pulp::osc::Message& msg) {
        handle_synth_message(msg);
    }
});
options.on_error = [](std::string_view error) {
    log_osc_error(error);
};

receiver.listen_with_options(9000, options);
```

## Message Types

OSC 1.0 argument types plus the optional RGBA colour tag:

| Type | C++ type | Tag |
|------|----------|-----|
| int32 | `int32_t` | `i` |
| float32 | `float` | `f` |
| string | `std::string` | `s` |
| blob | `std::vector<uint8_t>` | `b` |
| RGBA colour | `pulp::osc::ColourRgba` | `r` |

```cpp
pulp::osc::Message msg("/control");
msg.add(42);                    // int32
msg.add(3.14f);                 // float32
msg.add(std::string("hello"));  // string
msg.add(std::vector<uint8_t>{0x01, 0x02});  // blob
msg.add(pulp::osc::ColourRgba{255, 64, 0, 255});  // RGBA colour
```

## Bundles and Address Patterns

Bundles carry a timetag and a list of messages or nested bundles:

```cpp
#include <pulp/osc/bundle.hpp>

pulp::osc::Bundle bundle;
bundle.timetag = pulp::osc::TimeTag::immediate();
bundle.add(pulp::osc::Message("/synth/freq").add(440.0f));
bundle.add(pulp::osc::Message("/synth/gate").add(1));

sender.send(bundle);
```

Address routes use OSC address-pattern matching: `*`, `?`, `[...]`, and `{a,b}`.

## Low-Level Encoding/Decoding

For custom transport (TCP, WebSocket), use the encode/decode functions directly:

```cpp
auto bytes = pulp::osc::encode(msg);  // Message to binary
auto decoded = pulp::osc::decode(bytes.data(), bytes.size());  // binary to Message
```

Use `Sender::send_raw` when you already have a complete OSC datagram, such as a pre-encoded bundle.

## Runtime MessageChannel Adapter

`pulp::osc::OscChannel` adapts OSC UDP packets to the runtime `MessageChannel` interface. It is useful when higher-level code needs to swap OSC with another byte-oriented transport without branching on the transport type.

## Common Use Cases

- **TouchOSC / Lemur**: receive control surface input on port 9000
- **Max/MSP / SuperCollider**: send parameter values for live visualization
- **Remote control**: expose plugin parameters via OSC for hardware controllers
- **Inter-app**: communicate between standalone Pulp app and other audio software
