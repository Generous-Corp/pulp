# Creative Timeline Engine SDK

The Creative Timeline Engine is usable as three layered C++ libraries from an
installed Pulp SDK. It does not require Pulp's UI, GPU renderer, plugin-format
adapters, SignalGraph, standalone shell, or plugin host.

## Configure an external project

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyTimelineApp LANGUAGES CXX)

find_package(Pulp REQUIRED COMPONENTS timebase timeline playback)

add_executable(my_timeline_app main.cpp)
target_link_libraries(my_timeline_app PRIVATE
    Pulp::timebase
    Pulp::timeline
    Pulp::playback
)
```

The lowercase aliases `pulp::timebase`, `pulp::timeline`, and
`pulp::playback` are also available. Components validate that the installed SDK
contains each requested target; the package still defines its complete set of
installed targets.

## Dependency boundary

The three requested engine targets produce this installed Pulp link closure:

| Library | Why it is present |
| --- | --- |
| `Pulp::timebase` | Editable and compiled tempo/meter maps |
| `Pulp::timeline` | Immutable project model, commands, persistence |
| `Pulp::playback` | Transport, compilation, note/audio/automation rendering |
| `Pulp::platform` | Runtime's portable platform primitives |
| `Pulp::runtime` | Results, queues, slots, files, and worker primitives |
| `Pulp::audio` | Audio buffers, decoded assets, and file support |
| `Pulp::midi` | MIDI event types and scheduling |
| `Pulp::state` | Parameter-event and state primitives used by audio |
| `Pulp::signal` | Header-only signal utilities used by audio |
| `Pulp::events` | Static implementation dependency of `Pulp::state` listener dispatch |

That is ten first-party Pulp libraries. `Pulp::events` is a static
`LINK_ONLY` dependency of `Pulp::state`; omitting it from the count would not
describe the installed target graph accurately.

The complete static-link closure also contains six exported implementation
archives: `Pulp::hwy`, `Pulp::mbedcrypto`, `Pulp::mbedx509`, `Pulp::mbedtls`,
`Pulp::everest`, and `Pulp::p256m`. They support runtime SIMD and cryptography;
they are not additional timeline APIs. The external consumer smoke audits all
sixteen targets and fails if canvas, view, GPU, graph, format, standalone, or
host targets enter the closure.

Plugin hosting is deliberately outside the engine. A desktop integration
adapts its own instrument/effect ports; the caller owns audio-device I/O.

## Sample-rate conversion

Audio clips, take-comp segments, and frozen tracks may use a different sample
rate from the prepared tempo map. Playback compiles one shared 64-tap,
512-phase Kaiser-windowed sinc table for each distinct source/target rate pair.
That allocation happens during program compilation; rendering only reads the
immutable table. `AudioRendererLimits::max_sample_rate_converters` bounds the
number of distinct tables (64 by default), and compilation rejects excess
rates. Equal-rate audio bypasses the converter and retains its exact sample
path.

The deterministic offline quality gate covers both directions. For 96→48 kHz
conversion, a 20 kHz passband tone measures within 0.1 dB and a 30 kHz
stopband tone must fold below −60 dB. A deliberately unfiltered linear
decimator is the negative control and must expose that alias above −1 dB. For
44.1→48 kHz conversion, an 18 kHz tone must remain within 0.1 dB with residual
energy below −70 dB. Those named thresholds are the portable contract.

## Peek before loading

Project browsers and background media resolvers should inspect a snapshot with
`peek_project_summary()` before constructing an editable document:

```cpp
#include <pulp/timeline/serialize.hpp>

auto summary = pulp::timeline::peek_project_summary(snapshot_json);
if (!summary)
    return report(summary.error());

show_project(summary->name, summary->track_count, summary->clip_count);
```

The peek scans the complete JSON under the same depth, input-size, and authored
collection quotas as structural deserialization preflight, so malformed or
oversized structural arrays still fail closed. It decodes only the four root
scalar values needed for the summary and does not build the generic JSON DOM,
identity tree, clips, notes, or automation model.

Use `deserialize_project()` only when the project must become editable. Media
references may remain unresolved at that point; asset resolution belongs on a
background path.
