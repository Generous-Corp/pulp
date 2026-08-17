"""Declarative installed agent capability records."""
from agent_capability_registry_types import binding, capability

EXPORTS = [
    capability(
        key="timebase.tick",
        contract_version={"major": 1, "minor": 1},
        domain="timebase",
        summary=(
            "Saturating integer musical position on the 705600-tick "
            "quarter-note grid."
        ),
        rt_class="any",
        lifecycle={
            "construction": "any",
            "prepare": "none",
            "process": "any",
            "reset": "value-initialization",
            "release": "none",
        },
        state_model="Value type with saturating arithmetic over signed 64-bit ticks.",
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "not_applicable",
            "platform_scope": "cross_platform",
            "transport_history": "irrelevant",
        },
        input_domain="document ticks",
        output_domain="document ticks",
        units=["ticks"],
        latency="zero",
        tail="none",
        scheduling="pure",
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_type",
                include="pulp/timebase/tick.hpp",
                qualified_name="pulp::timebase::TickPosition",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:e648c10386afc397349a341787aa4773326b551fb8f8f33e08bc9925aea42452"
                ),
            )
        ],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::timebase::TickPosition",
            "operation": "construct",
            "arguments": "1",
        }],
    ),
    capability(
        key="timebase.tempo-map",
        domain="timebase",
        summary=(
            "Validated sample-rate-specific tempo compilation with deterministic, "
            "allocation-free tick and sample lookup."
        ),
        rt_class="mixed",
        lifecycle={
            "construction": "control",
            "prepare": "factory-validation-on-control",
            "process": "audio",
            "reset": "replace-immutable-value",
            "release": "control-after-readers-quiesce",
        },
        state_model=(
            "Immutable owned compiled segments produced by a validating control-thread "
            "factory; published lookup values are allocation-free."
        ),
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "same_build",
            "transport_history": "irrelevant",
        },
        input_domain="authored tempo points, rational sample rate, ticks, and samples",
        output_domain=(
            "integer sample positions and nearest canonical ticks with explicit sample error"
        ),
        units=["ticks", "samples", "beats per minute", "rational sample rate"],
        latency="zero",
        tail="none",
        scheduling="pure coordinate conversion after control-thread compilation",
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_type",
                include="pulp/timebase/compiled_tempo_map.hpp",
                qualified_name="pulp::timebase::CompiledTempoMap",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:2e1451402b8e6301ac0773d5e4bb8bc6ed757ff9889ce61b978e188a8758f47c"
                ),
            ),
            binding(
                role="validated-factory",
                kind="cpp_function",
                include="pulp/timebase/compiled_tempo_map.hpp",
                qualified_name="pulp::timebase::CompiledTempoMap::compile",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:2e1451402b8e6301ac0773d5e4bb8bc6ed757ff9889ce61b978e188a8758f47c"
                ),
                address_expression=(
                    "static_cast<pulp::runtime::Result<pulp::timebase::CompiledTempoMap, "
                    "pulp::timebase::TempoMapError> (*)(std::span<const "
                    "pulp::timebase::TempoPoint>, pulp::timebase::RationalRate) noexcept>("
                    "&pulp::timebase::CompiledTempoMap::compile)"
                ),
            ),
            binding(
                role="ticks-to-samples",
                kind="cpp_function",
                include="pulp/timebase/compiled_tempo_map.hpp",
                qualified_name="pulp::timebase::CompiledTempoMap::ticks_to_samples",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:2e1451402b8e6301ac0773d5e4bb8bc6ed757ff9889ce61b978e188a8758f47c"
                ),
                address_expression=(
                    "static_cast<pulp::timebase::SamplePosition "
                    "(pulp::timebase::CompiledTempoMap::*)(pulp::timebase::TickPosition) "
                    "const noexcept>(&pulp::timebase::CompiledTempoMap::ticks_to_samples)"
                ),
            ),
            binding(
                role="resolve-sample",
                kind="cpp_function",
                include="pulp/timebase/compiled_tempo_map.hpp",
                qualified_name="pulp::timebase::CompiledTempoMap::resolve_sample",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:2e1451402b8e6301ac0773d5e4bb8bc6ed757ff9889ce61b978e188a8758f47c"
                ),
                address_expression=(
                    "static_cast<pulp::timebase::SampleToTickResult "
                    "(pulp::timebase::CompiledTempoMap::*)(pulp::timebase::SamplePosition) "
                    "const noexcept>(&pulp::timebase::CompiledTempoMap::resolve_sample)"
                ),
            ),
        ],
        _link_probes=[
            {
                "role": "entrypoint",
                "binding": "pulp::timebase::CompiledTempoMap",
                "operation": "construct",
                "arguments": (
                    "pulp::timebase::CompiledTempoMap::compile("
                    "pulp::timebase::TempoMap{}, "
                    "pulp::timebase::RationalRate{48000, 1}).value()"
                ),
            },
            {
                "role": "validated-factory",
                "binding": "pulp::timebase::CompiledTempoMap::compile",
                "operation": "function_call",
                "arguments": (
                    "pulp::timebase::TempoMap{}.points(), "
                    "pulp::timebase::RationalRate{48000, 1}"
                ),
            },
            {
                "role": "ticks-to-samples",
                "binding": "pulp::timebase::CompiledTempoMap::ticks_to_samples",
                "operation": "member_function_call",
                "object": (
                    "pulp::timebase::CompiledTempoMap::compile("
                    "pulp::timebase::TempoMap{}, "
                    "pulp::timebase::RationalRate{48000, 1}).value()"
                ),
                "arguments": "pulp::timebase::TickPosition{705600}",
            },
            {
                "role": "resolve-sample",
                "binding": "pulp::timebase::CompiledTempoMap::resolve_sample",
                "operation": "member_function_call",
                "object": (
                    "pulp::timebase::CompiledTempoMap::compile("
                    "pulp::timebase::TempoMap{}, "
                    "pulp::timebase::RationalRate{48000, 1}).value()"
                ),
                "arguments": "pulp::timebase::SamplePosition{24000}",
            },
        ],
    ),
    capability(
        key="timebase.swing",
        contract_version={"major": 1, "minor": 1},
        domain="timebase",
        summary=(
            "Exact rational swing projection with bounded-rounding recovery over "
            "integer document ticks."
        ),
        rt_class="any",
        lifecycle={
            "construction": "any",
            "prepare": "none",
            "process": "any",
            "reset": "value-initialization",
            "release": "none",
        },
        state_model=(
            "Pure value and free-function transforms; unswing recovers within the "
            "documented rounding bound, and invalid inputs leave positions unchanged."
        ),
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "not_applicable",
            "platform_scope": "cross_platform",
            "transport_history": "irrelevant",
        },
        input_domain="document ticks and rational swing",
        output_domain="document ticks",
        units=["ticks", "rational ratio"],
        latency="zero",
        tail="none",
        scheduling="pure",
        bindings=[
            binding(
                role="configuration",
                kind="cpp_type",
                include="pulp/timebase/quantize.hpp",
                qualified_name="pulp::timebase::SwingRatio",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:ca242cb57d4963cf48ce6bb41a4b9d90d0959b7a6aff28cdbdb76013ba39ac84"
                ),
            ),
            binding(
                role="forward-operation",
                kind="cpp_function",
                include="pulp/timebase/quantize.hpp",
                qualified_name="pulp::timebase::swing_position",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:ca242cb57d4963cf48ce6bb41a4b9d90d0959b7a6aff28cdbdb76013ba39ac84"
                ),
            ),
            binding(
                role="inverse-operation",
                kind="cpp_function",
                include="pulp/timebase/quantize.hpp",
                qualified_name="pulp::timebase::unswing_position",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:ca242cb57d4963cf48ce6bb41a4b9d90d0959b7a6aff28cdbdb76013ba39ac84"
                ),
            ),
        ],
        _link_probes=[
            {
                "role": "configuration",
                "binding": "pulp::timebase::SwingRatio",
                "operation": "construct",
                "arguments": "1, 2",
            },
            {
                "role": "forward-operation",
                "binding": "pulp::timebase::swing_position",
                "operation": "function_call",
                "arguments": (
                    "pulp::timebase::TickPosition{1}, "
                    "pulp::timebase::TickDuration{2}, pulp::timebase::kStraightSwing"
                ),
            },
            {
                "role": "inverse-operation",
                "binding": "pulp::timebase::unswing_position",
                "operation": "function_call",
                "arguments": (
                    "pulp::timebase::TickPosition{1}, "
                    "pulp::timebase::TickDuration{2}, pulp::timebase::kStraightSwing"
                ),
            },
        ],
    ),
    capability(
        key="timebase.beat-division",
        domain="timebase",
        summary="Canonical persisted beat-division vocabulary with exact tick conversion.",
        rt_class="any",
        lifecycle={
            "construction": "any",
            "prepare": "none",
            "process": "any",
            "reset": "value-initialization",
            "release": "none",
        },
        state_model="Pure enum, fraction, and tick-duration value conversion.",
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "not_applicable",
            "platform_scope": "cross_platform",
            "transport_history": "irrelevant",
        },
        input_domain="persisted beat-division ordinal",
        output_domain="exact quarter-note fraction and document tick duration",
        units=["quarter-note fraction", "ticks"],
        latency="zero",
        tail="none",
        scheduling="pure",
        bindings=[
            binding(
                role="vocabulary",
                kind="cpp_type",
                include="pulp/timebase/beat_division.hpp",
                qualified_name="pulp::timebase::BeatDivision",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:a6f8a5abd5184d33e08c38ed24565b8ed18df756ce63688594a7a3b4ca6ed570"
                ),
            ),
            binding(
                role="tick-conversion",
                kind="cpp_function",
                include="pulp/timebase/beat_division.hpp",
                qualified_name="pulp::timebase::division_ticks",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:a6f8a5abd5184d33e08c38ed24565b8ed18df756ce63688594a7a3b4ca6ed570"
                ),
            ),
        ],
        _link_probes=[
            {
                "role": "vocabulary",
                "binding": "pulp::timebase::BeatDivision",
                "operation": "construct",
                "arguments": "pulp::timebase::BeatDivision::Quarter",
            },
            {
                "role": "tick-conversion",
                "binding": "pulp::timebase::division_ticks",
                "operation": "function_call",
                "arguments": "pulp::timebase::BeatDivision::Quarter",
            },
        ],
    ),
    capability(
        key="timebase.coordinate-random",
        domain="timebase",
        summary="Stateless seeded probability keyed by stable musical coordinates.",
        rt_class="any",
        lifecycle={
            "construction": "any",
            "prepare": "none",
            "process": "any",
            "reset": "none",
            "release": "none",
        },
        state_model="Pure coordinate hash and exact integer-ratio predicate with no mutable RNG.",
        seed_model="caller-supplied 64-bit seed plus tick, lane, cycle, and stream coordinates",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "cross_platform",
            "transport_history": "irrelevant",
        },
        input_domain="seed, musical coordinate, and exact probability ratio",
        output_domain="deterministic probability selection",
        units=["ticks", "unsigned integer ratio"],
        latency="zero",
        tail="none",
        scheduling="event-synchronous",
        bindings=[
            binding(
                role="coordinate",
                kind="cpp_type",
                include="pulp/timebase/coordinate_random.hpp",
                qualified_name="pulp::timebase::RandomCoordinate",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:327487f1a49eb2f8729a6db7ba493ad220f634fec2cb4c7295a300f0a68ddae5"
                ),
            ),
            binding(
                role="probability-operation",
                kind="cpp_function",
                include="pulp/timebase/coordinate_random.hpp",
                qualified_name="pulp::timebase::coordinate_chance",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:327487f1a49eb2f8729a6db7ba493ad220f634fec2cb4c7295a300f0a68ddae5"
                ),
            ),
        ],
        _link_probes=[
            {
                "role": "coordinate",
                "binding": "pulp::timebase::RandomCoordinate",
                "operation": "construct",
                "arguments": "pulp::timebase::TickPosition{0}, 0, 0, 0",
            },
            {
                "role": "probability-operation",
                "binding": "pulp::timebase::coordinate_chance",
                "operation": "function_call",
                "arguments": "0, pulp::timebase::RandomCoordinate{}, 1, 2",
            },
        ],
    ),
    capability(
        key="timebase.grid-projection",
        domain="timebase",
        summary="Bounded projection of musical grid points through resolved transport ranges.",
        rt_class="audio",
        lifecycle={
            "construction": "control",
            "prepare": "control",
            "process": "audio",
            "reset": "caller-replaces-resolved-ranges",
            "release": "none",
        },
        state_model=(
            "Pure projection over caller-owned immutable compiled tempo and meter maps, "
            "resolved ranges, and fixed output storage."
        ),
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "same_build",
            "transport_history": "input",
        },
        input_domain="resolved half-open transport ranges and beat division",
        output_domain="bounded frame-offset grid events with timeline and monotonic coordinates",
        units=["frames", "samples", "ticks", "bars"],
        latency="zero",
        tail="none",
        scheduling="block-synchronous event projection",
        bindings=[
            binding(
                role="projection-operation",
                kind="cpp_function",
                include="pulp/timebase/grid_projection.hpp",
                qualified_name="pulp::timebase::project_grid",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:cf70650f506ab6b7ca648ca906219b4837a500e8aca8c56095b6acef626bf30c"
                ),
            )
        ],
        _link_probes=[{
            "role": "projection-operation",
            "binding": "pulp::timebase::project_grid",
            "operation": "function_call",
            "arguments": (
                "pulp::timebase::CompiledTempoMap::compile("
                "pulp::timebase::TempoMap{}, pulp::timebase::RationalRate{48000, 1}).value(), "
                "pulp::timebase::CompiledMeterMap::compile(pulp::timebase::MeterMap{}).value(), "
                "pulp::timebase::GridProjectionRequest{}, "
                "std::span<const pulp::timebase::GridProjectionRange>{}, "
                "std::span<pulp::timebase::GridProjectionPoint>{}"
            ),
        }],
    ),
    capability(
        key="timebase.groove-kernel",
        domain="timebase",
        summary="Fixed-capacity swing and groove projection that rejects event reordering.",
        rt_class="mixed",
        lifecycle={
            "construction": "control",
            "prepare": "factory-validation-on-control",
            "process": "audio",
            "reset": "replace-immutable-value",
            "release": "none",
        },
        state_model="Immutable copied groove table with bounded validation and allocation-free lookup.",
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "cross_platform",
            "transport_history": "irrelevant",
        },
        input_domain="authored event ticks, rational swing, and fixed-capacity groove steps",
        output_domain="order-preserving event ticks and velocity scale",
        units=["ticks", "rational ratio", "per-thousand scale"],
        latency="zero",
        tail="none",
        scheduling="event-synchronous",
        bindings=[
            binding(
                role="validated-factory",
                kind="cpp_function",
                include="pulp/timebase/groove_kernel.hpp",
                qualified_name="pulp::timebase::OrderPreservingGrooveKernel::create",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:169e0104d2ea6164d6924de224ac636eaf11d617f5b844b4a2facaf0cbaa0286"
                ),
            )
        ],
        _link_probes=[{
            "role": "validated-factory",
            "binding": "pulp::timebase::OrderPreservingGrooveKernel::create",
            "operation": "function_call",
            "arguments": "pulp::timebase::GrooveKernelInput{}",
        }],
    ),
    capability(
        key="timebase.trigger-grid",
        domain="timebase",
        summary="Fixed-capacity authored trigger grid with block-invariant window projection.",
        rt_class="mixed",
        lifecycle={
            "construction": "control",
            "prepare": "configure-and-author-on-control",
            "process": "audio",
            "reset": "control-or-audio-when-quiescent",
            "release": "none",
        },
        state_model="Inline fixed-capacity track-step cells; projection does not mutate the grid.",
        seed_model="caller supplies one stable 64-bit probability word per configured coordinate",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "cross_platform",
            "transport_history": "irrelevant",
        },
        input_domain="authored trigger cells, cycle origin, half-open tick window, and probability words",
        output_domain="bounded step-major trigger events",
        units=["ticks", "MIDI velocity", "unsigned integer ratio"],
        latency="zero",
        tail="none",
        scheduling="event-synchronous",
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_type",
                include="pulp/timebase/trigger_grid.hpp",
                qualified_name="pulp::timebase::TriggerGrid<>",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:6a5b7dd7f2b1185ed3a3f5c8ae1ffcf506415fe6083ec772f984010476b337ca"
                ),
            )
        ],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::timebase::TriggerGrid<>",
            "operation": "member_call",
            "member": "configure",
            "arguments": "1, 1, pulp::timebase::TickDuration{1}",
        }],
    ),
    capability(
        key="timebase.ratchet",
        domain="timebase",
        summary="Clock-locked bounded ratchet subdivision over half-open tick intervals.",
        rt_class="any",
        lifecycle={
            "construction": "any",
            "prepare": "none",
            "process": "any",
            "reset": "none",
            "release": "none",
        },
        state_model="Pure fixed-capacity integer subdivision with caller-owned output storage.",
        seed_model="none",
        determinism={
            "repeatability": "bit_exact",
            "block_partition": "invariant",
            "platform_scope": "cross_platform",
            "transport_history": "irrelevant",
        },
        input_domain="clock interval, hit count, and half-open projection window",
        output_domain="bounded tick-position ratchet onsets",
        units=["ticks", "event count"],
        latency="zero",
        tail="none",
        scheduling="event-synchronous",
        bindings=[
            binding(
                role="projection-operation",
                kind="cpp_function",
                include="pulp/timebase/ratchet.hpp",
                qualified_name="pulp::timebase::project_ratchet_interval<>",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:93f1ae1c80b5c5ad255180067adabcc331577e63a6f77b57b06255891926718d"
                ),
            )
        ],
        _link_probes=[{
            "role": "projection-operation",
            "binding": "pulp::timebase::project_ratchet_interval<>",
            "operation": "function_call",
            "arguments": (
                "pulp::timebase::TickPosition{0}, pulp::timebase::TickPosition{4}, 2, "
                "pulp::timebase::TickPosition{0}, pulp::timebase::TickPosition{4}, "
                "std::span<pulp::timebase::TickPosition>{}"
            ),
        }],
    ),
    capability(
        key="sequence.host-transport-projector",
        contract_version={"major": 1, "minor": 1},
        domain="sequence",
        summary=(
            "Prepared projection from host callback transport into Pulp playback "
            "snapshots."
        ),
        rt_class="mixed",
        lifecycle={
            "construction": "control",
            "prepare": "control",
            "process": "audio",
            "reset": "control-or-audio-when-quiescent",
            "release": "none",
        },
        state_model=(
            "Prepared tempo-map reference plus bounded callback history and playback "
            "epoch."
        ),
        seed_model="none",
        determinism={
            "repeatability": "not_promised",
            "block_partition": "fixed_partition_only",
            "platform_scope": "same_build",
            "transport_history": "input",
        },
        input_domain="host process context",
        output_domain="playback transport snapshot",
        units=["samples", "ticks", "beats per minute"],
        latency="zero",
        tail="none",
        scheduling="block-synchronous",
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_type",
                include="pulp/sequence/host_transport_projector.hpp",
                qualified_name="pulp::sequence::HostTransportProjector",
                target="Pulp::sequence",
                header_fingerprint=(
                    "sha256:3c0a31d541635c9339fadb13b584e9bdec7a4e9e274afd595450f707c8344051"
                ),
            )
        ],
        _link_probes=[{
            "role": "entrypoint",
            "binding": "pulp::sequence::HostTransportProjector",
            "operation": "member_call",
            "member": "reset",
            "arguments": "",
        }],
    ),
]
