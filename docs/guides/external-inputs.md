# External Inputs

Pulp's experimental external-input contract lets an application connect a
camera, sensor, controller, analysis worker, network service, or third-party
application without adding that provider to the Pulp SDK. The application
selects and owns the provider. Pulp receives bounded observations and applies
validation and expiry on the audio side.

## Boundary and ownership

The provider owns capture, permissions, inference, timestamps, and lifecycle.
The application owns mapping values to parameters or controls. Pulp owns only
the allocation-free frame contract and its gate:

```cpp
#include <pulp/runtime/external_input.hpp>

pulp::runtime::ExternalInputGate<1> gate;
pulp::runtime::ExternalInputPolicy policy{
    .source_id = 7,
    .minimum_confidence = 0.6f,
    .max_age_samples = 12000,
    .expected_count = 1,
};
```

`ExternalInputFrame<N>` contains a source id, restart epoch, provider sequence,
provider timestamp, status, confidence, count, and up to `N` application-defined
float values. `N` is bounded to 1 through 64. The frame is trivially copyable
and can travel through an existing `TripleBuffer`, SPSC queue, OSC/IPC/MIDI
ingress, or another application-owned seam.

## Lifecycle

Submit frames from the provider or its application handoff. A higher epoch is
accepted as a restart and resets age. Sequences and provider timestamps must be
monotonic within an epoch. `valid` frames must meet the configured confidence
and count policy; `invalid`, `unavailable`, `denied`, and `paused` statuses
expire the accepted value. Advance the gate once per audio block using the
block's sample count. The gate never compares a provider clock with the audio
clock.

Providers should emit an explicit status frame when permission is denied, a
sensor is paused or unavailable, and during teardown. Permission APIs stay in
the application or platform adapter; they are not part of Pulp core.

## Transport and mapping

Use an existing transport or handoff. Do not add a provider-specific queue,
transport, framing format, landmark schema, or SDK dependency to Pulp core.
After the gate accepts a frame, application code maps its values to the
desired parameter or control. The external-input contract does not write
`StateStore` directly and does not assign meaning to a value's dimensions.

The replay and receipt helpers in `test/support` provide deterministic fixture
coverage for lifecycle behavior. They are test infrastructure, not a runtime
provider API.
