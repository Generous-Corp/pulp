#include <pulp/host/voice_bank_binding.hpp>

#include <pulp/format/processor_node_adapter.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/host/signal_graph_subgraph.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace pulp::host {
namespace {

constexpr std::uint32_t kSlots = 8;

bool valid_schema(const VoiceBankManifest& manifest, const SubgraphTemplate& definition,
                  const VoiceBankOwner& owner, std::string_view stereo_port,
                  VoiceBankBindingResult& result) {
    if (stereo_port.empty() || definition.identity.empty() || definition.nodes.size() != 1 ||
        definition.nodes[0].type != NodeType::Plugin || !definition.children.empty() ||
        definition.binding_path.empty()) {
        result = {VoiceBankBindingCode::InvalidManifest,
                  0,
                  {},
                  "bounded bank requires one authored Processor template"};
        return false;
    }
    const auto has_exposed = [&](std::string_view name, PortIndex index) {
        return std::any_of(definition.exposed_ports.begin(), definition.exposed_ports.end(),
                           [&](const SubgraphPortSpec& port) {
                               return port.name == name && port.node == definition.nodes[0].key &&
                                      port.port == index && port.output &&
                                      port.kind == PortKind::Audio;
                           });
    };
    if (!has_exposed(std::string(stereo_port) + ".left", 0) ||
        !has_exposed(std::string(stereo_port) + ".right", 1)) {
        result = {VoiceBankBindingCode::InvalidManifest, 0, std::string(stereo_port),
                  "stereo output endpoints are disconnected or mismatched"};
        return false;
    }
    bool audio = false;
    std::array<unsigned, 4> inputs{};
    std::size_t parameters = 0;
    for (const auto& spec : manifest.schema.bindings) {
        if (spec.path != definition.binding_path) {
            result = {VoiceBankBindingCode::InvalidManifest, 0, spec.target,
                      "authored slot path does not match the template"};
            return false;
        }
        if (spec.target_kind == VoiceBindingTarget::Parameter) {
            const auto found =
                std::find_if(definition.parameters.begin(), definition.parameters.end(),
                             [&](const SubgraphParameterSpec& parameter) {
                                 return parameter.path == spec.target &&
                                        parameter.node == definition.nodes[0].key &&
                                        parameter.processor_parameter_id != 0 &&
                                        !parameter.processor_parameter_name.empty();
                             });
            if (found != definition.parameters.end() && spec.type == VoiceBindingType::Control &&
                spec.direction == VoiceBindingDirection::Input &&
                spec.shape == VoiceBindingShape::Scalar && spec.rate == VoiceBindingRate::Block) {
                ++parameters;
                continue;
            }
        } else if (spec.target == stereo_port && spec.type == VoiceBindingType::Audio &&
                   spec.direction == VoiceBindingDirection::Output &&
                   spec.shape == VoiceBindingShape::Stereo &&
                   spec.rate == VoiceBindingRate::Sample) {
            audio = true;
            continue;
        } else {
            const auto semantic = owner.resolve_input(spec);
            if (semantic != VoiceBankOwner::InputSemantic::Unknown) {
                ++inputs[static_cast<std::size_t>(semantic) - 1];
                continue;
            }
        }
        result = {VoiceBankBindingCode::InvalidManifest, 0, spec.target,
                  "unsupported or mismatched authored binding"};
        return false;
    }
    if (!audio || parameters != definition.parameters.size() ||
        std::any_of(inputs.begin(), inputs.end(), [](unsigned count) { return count != 1; })) {
        result = {VoiceBankBindingCode::InvalidManifest, 0, std::string(stereo_port),
                  "audio/input/parameter declarations do not match the template"};
        return false;
    }
    return true;
}

} // namespace

struct VoiceBankBinding::Impl {
    std::unique_ptr<VoiceBankOwner> owner;
    SignalGraph graph;
    VoiceBankManifest manifest;
    std::array<SubgraphInstance, kSlots> slots{};
    midi::MpeVoiceTracker tracker;
    HostEventIngress<SignalGraph::kHostEventIngressCapacity> ingress;
    std::array<HostEventRecord, SignalGraph::kHostEventIngressCapacity> projected{};
    std::uint64_t epoch = 1;
    std::size_t max_frames = 0;
    std::uint64_t block_start = 0;
    std::size_t block_frames = 0;
    bool block_open = false;
    std::string stereo_port;

    Impl() {
        tracker.on_note_on = [this](const midi::MpeNoteState& note) {
            owner->dispatch({0, midi::MpeExpressionEvent::Kind::NoteOn, note});
        };
        tracker.on_note_off = [this](const midi::MpeNoteState& note) {
            owner->dispatch({0, midi::MpeExpressionEvent::Kind::NoteOff, note});
        };
        tracker.on_pitch_bend = [this](const midi::MpeNoteState& note) {
            owner->dispatch({0, midi::MpeExpressionEvent::Kind::PitchBend, note});
        };
        tracker.on_pressure = [this](const midi::MpeNoteState& note) {
            owner->dispatch({0, midi::MpeExpressionEvent::Kind::Pressure, note});
        };
        tracker.on_timbre = [this](const midi::MpeNoteState& note) {
            owner->dispatch({0, midi::MpeExpressionEvent::Kind::Timbre, note});
        };
    }
};

VoiceBankBinding::VoiceBankBinding() : impl_(std::make_unique<Impl>()) {}
VoiceBankBinding::~VoiceBankBinding() = default;

std::unique_ptr<VoiceBankBinding>
VoiceBankBinding::prepare(const VoiceTemplateSchema& schema, SubgraphTemplate definition,
                          std::unique_ptr<VoiceBankOwner> owner, std::string stereo_port,
                          double sample_rate, int max_frames, VoiceBankBindingResult& result) {
    result = {};
    auto prepared = prepare_voice_bank(schema);
    if (!owner) {
        result = {VoiceBankBindingCode::FactoryFailure, 0, {}, "missing MPE owner"};
        return {};
    }
    if (!prepared.valid ||
        !valid_schema(prepared.manifest, definition, *owner, stereo_port, result)) {
        if (result.code == VoiceBankBindingCode::Ok)
            result = {VoiceBankBindingCode::InvalidManifest, prepared.failing_slot,
                      prepared.failing_path, prepared.error};
        return {};
    }
    if (!std::isfinite(sample_rate) || sample_rate <= 0 || max_frames <= 0 || max_frames > 4096) {
        result = {VoiceBankBindingCode::GraphPrepareFailure, 0, {}, "invalid audio preparation"};
        return {};
    }
    for (std::uint32_t slot = 0; slot < kSlots; ++slot) {
        const auto* identity = owner->slot_state_identity(slot);
        if (identity == nullptr) {
            result = {VoiceBankBindingCode::InvalidManifest, slot, "voice",
                      "missing mutable slot identity"};
            return {};
        }
        for (std::uint32_t earlier = 0; earlier < slot; ++earlier)
            if (identity == owner->slot_state_identity(earlier)) {
                result = {VoiceBankBindingCode::InvalidManifest, slot, "voice",
                          "cross-slot mutable voice state is aliased"};
                return {};
            }
    }
    auto bank = std::unique_ptr<VoiceBankBinding>(new VoiceBankBinding);
    bank->impl_->owner = std::move(owner);
    bank->impl_->manifest = std::move(prepared.manifest);
    bank->impl_->max_frames = static_cast<std::size_t>(max_frames);
    bank->impl_->stereo_port = std::move(stereo_port);
    definition.nodes[0].processor_factory = [owner = bank->impl_->owner.get()](std::uint32_t slot) {
        return owner->make_processor(slot);
    };
    const auto midi_input = bank->impl_->graph.add_midi_input_node("Bank MIDI ingress");
    const auto output = bank->impl_->graph.add_output_node(2, "Bank Stereo");
    if (midi_input == 0 || output == 0) {
        result = {VoiceBankBindingCode::FactoryFailure, 0, bank->impl_->stereo_port,
                  "output node rejected"};
        return {};
    }
    for (std::uint32_t slot = 0; slot < kSlots; ++slot) {
        auto instance = instantiate_subgraph(bank->impl_->graph, definition,
                                             "voice[" + std::to_string(slot) + "]", slot);
        if (!instance) {
            result = {VoiceBankBindingCode::FactoryFailure, slot, bank->impl_->stereo_port,
                      instance.result.message()};
            return {};
        }
        if (instance.instance.nodes.size() != 1 ||
            !bank->impl_->graph.connect_midi(midi_input, instance.instance.nodes.front())) {
            result = {VoiceBankBindingCode::FactoryFailure, slot, "midi",
                      "bank ingress is disconnected from the authored Processor"};
            return {};
        }
        const auto left_exposed = instance.instance.ports.at(bank->impl_->stereo_port + ".left");
        const auto right_exposed = instance.instance.ports.at(bank->impl_->stereo_port + ".right");
        const auto* left_node = bank->impl_->graph.node(left_exposed.node);
        const auto* right_node = bank->impl_->graph.node(right_exposed.node);
        if (!left_node || !right_node || left_exposed.index >= left_node->output_ports.size() ||
            right_exposed.index >= right_node->output_ports.size()) {
            result = {VoiceBankBindingCode::FactoryFailure, slot, bank->impl_->stereo_port,
                      "authored stereo endpoint is unavailable"};
            return {};
        }
        const auto left =
            bank->impl_->graph.connect(left_node->output_ports[left_exposed.index],
                                       bank->impl_->graph.node(output)->input_ports[0]);
        const auto right =
            bank->impl_->graph.connect(right_node->output_ports[right_exposed.index],
                                       bank->impl_->graph.node(output)->input_ports[1]);
        if (!left || !right) {
            result = {VoiceBankBindingCode::FactoryFailure, slot, bank->impl_->stereo_port,
                      !left ? left.message() : right.message()};
            return {};
        }
        bank->impl_->slots[slot] = std::move(instance.instance);
    }
    if (!bank->impl_->graph.prepare(sample_rate, max_frames)) {
        result = {VoiceBankBindingCode::GraphPrepareFailure, 0, "voice",
                  "prepared graph rejected the eight-slot candidate"};
        return {};
    }
    return bank;
}

const VoiceBankManifest& VoiceBankBinding::manifest() const noexcept {
    return impl_->manifest;
}
std::uint64_t VoiceBankBinding::epoch() const noexcept {
    return impl_->epoch;
}
const VoiceBankOwner& VoiceBankBinding::owner() const noexcept {
    return *impl_->owner;
}
void VoiceBankBinding::set_steal_mode(midi::MpeVoiceStealMode mode) noexcept {
    impl_->owner->set_steal_mode(mode);
}
bool VoiceBankBinding::activate(VoiceBankBindingResult& result) noexcept {
    if (impl_->block_open) {
        result = {VoiceBankBindingCode::InvalidBlock, 0, {}, "cannot activate inside a block"};
        return false;
    }
    if (impl_->epoch == std::numeric_limits<std::uint64_t>::max()) {
        result = {VoiceBankBindingCode::EpochExhausted, 0, {}, "epoch exhausted"};
        return false;
    }
    impl_->tracker.reset();
    impl_->owner->reset();
    ++impl_->epoch;
    impl_->ingress.reset(impl_->epoch);
    result = {};
    return true;
}
HostEventResult VoiceBankBinding::begin_block(std::uint64_t first_sample,
                                              std::size_t frames) noexcept {
    if (impl_->block_open || frames == 0 || frames > impl_->max_frames)
        return HostEventResult::OutOfRange;
    const auto status = impl_->ingress.begin_block(impl_->epoch, first_sample, frames);
    if (status == HostEventResult::Accepted) {
        impl_->block_start = first_sample;
        impl_->block_frames = frames;
        impl_->block_open = true;
    }
    return status;
}
HostEventResult VoiceBankBinding::publish(const HostEventRecord& event) noexcept {
    if (!impl_->block_open)
        return HostEventResult::Busy;
    if (event.has_ump || event.midi.size() == 0)
        return HostEventResult::OutOfRange;
    return impl_->ingress.publish(event);
}
VoiceBankBindingCode VoiceBankBinding::render(audio::BufferView<float>& stereo,
                                              std::uint64_t first_sample,
                                              std::size_t frames) noexcept {
    if (!impl_->block_open || first_sample != impl_->block_start || frames != impl_->block_frames ||
        stereo.num_channels() != 2 || stereo.num_samples() != frames ||
        !stereo.has_valid_channels())
        return VoiceBankBindingCode::InvalidBlock;
    std::size_t count = 0;
    if (impl_->ingress.project(impl_->projected, count) != HostEventResult::Accepted)
        return VoiceBankBindingCode::IngressFailure;
    const auto refuse = [&](VoiceBankBindingCode code) {
        impl_->ingress.rollback_projection();
        (void)impl_->ingress.end_block();
        impl_->block_open = false;
        return code;
    };
    // Refuse unsupported MIDI before any slot or output is changed. Pedal and
    // channel-mode semantics need the separate G5 sustain bridge and are not
    // silently interpreted as ordinary tracker input here.
    for (std::size_t index = 0; index < count; ++index) {
        const auto& record = impl_->projected[index];
        const auto* bytes = record.midi.data();
        const auto size = record.midi.size();
        const auto status = size == 0 ? 0 : bytes[0];
        const auto kind = status & 0xf0;
        const auto channel = status & 0x0f;
        const auto& config = impl_->tracker.config();
        const bool in_zone = config.zone_for_channel(channel) ||
                             (channel == 0 && config.lower_zone.member_channels != 0) ||
                             (channel == 15 && config.upper_zone.member_channels != 0);
        const bool supported = kind == 0x80 || kind == 0x90 || kind == 0xd0 ||
                               kind == 0xe0 || (kind == 0xb0 && size == 3 && bytes[1] == 74);
        const std::size_t expected_size = kind == 0xd0 ? 2 : 3;
        const bool valid_bytes = size == expected_size && (bytes[1] & 0x80) == 0 &&
                                 (size == 2 || (bytes[2] & 0x80) == 0);
        if (record.has_ump || !in_zone || !supported || !valid_bytes ||
            ((kind == 0x80 || kind == 0x90) &&
             ((channel == 0 && config.lower_zone.member_channels != 0) ||
              (channel == 15 && config.upper_zone.member_channels != 0))))
            return refuse(VoiceBankBindingCode::IngressFailure);
    }
    // The tracker can hold 128 distinct physical keys even while the audio
    // allocator keeps only eight sounding voices. Preview the complete block
    // in event order so a full table refuses before any output or voice changes.
    std::array<std::uint16_t, midi::MpeVoiceTracker::kMaxNotes> membership{};
    for (std::size_t i = 0; i < membership.size(); ++i) {
        const auto& note = impl_->tracker.slots()[i];
        if (note.active)
            membership[i] = static_cast<std::uint16_t>(1u + (note.channel << 7) + note.note);
    }
    for (std::size_t index = 0; index < count; ++index) {
        const auto& message = impl_->projected[index].midi;
        if (!message.is_note_on() && !message.is_note_off()) continue;
        const auto key = static_cast<std::uint16_t>(1u + (message.channel() << 7) +
                                                    message.note());
        const auto matching = std::find(membership.begin(), membership.end(), key);
        if (message.is_note_off() || message.velocity() == 0) {
            if (matching != membership.end()) *matching = 0;
        } else if (matching == membership.end()) {
            const auto free = std::find(membership.begin(), membership.end(), 0);
            if (free == membership.end()) return refuse(VoiceBankBindingCode::TrackerCapacity);
            *free = key;
        }
    }
    const auto failures_before = impl_->graph.routed_only_execution_failures();
    std::size_t offset = 0;
    for (std::size_t index = 0; index < count;) {
        const auto at = static_cast<std::size_t>(impl_->projected[index].absolute_sample -
                                                 static_cast<std::int64_t>(first_sample));
        if (at > offset) {
            auto slice = stereo.slice(offset, at - offset);
            audio::BufferView<const float> input(nullptr, 0, at - offset);
            impl_->graph.process(slice, input, static_cast<int>(at - offset));
            if (impl_->graph.routed_only_execution_failures() != failures_before) {
                (void)impl_->ingress.end_block();
                impl_->block_open = false;
                return VoiceBankBindingCode::GraphExecutionFailure;
            }
            offset = at;
        }
        do {
            const auto& record = impl_->projected[index];
            if (record.midi.size() != 0 && !impl_->tracker.process(record.midi)) {
                impl_->ingress.rollback_projection();
                (void)impl_->ingress.end_block();
                impl_->block_open = false;
                return VoiceBankBindingCode::IngressFailure;
            }
            ++index;
        } while (index < count &&
                 static_cast<std::uint64_t>(impl_->projected[index].absolute_sample) ==
                     first_sample + at);
    }
    if (offset < frames) {
        auto slice = stereo.slice(offset, frames - offset);
        audio::BufferView<const float> input(nullptr, 0, frames - offset);
        impl_->graph.process(slice, input, static_cast<int>(frames - offset));
        if (impl_->graph.routed_only_execution_failures() != failures_before) {
            (void)impl_->ingress.end_block();
            impl_->block_open = false;
            return VoiceBankBindingCode::GraphExecutionFailure;
        }
    }
    (void)impl_->ingress.end_block();
    impl_->block_open = false;
    return VoiceBankBindingCode::Ok;
}
VoiceBankBindingCode VoiceBankBinding::dispatch_deferred(std::uint64_t epoch, std::uint32_t slot,
                                                         midi::MpeNoteGeneration generation,
                                                         midi::MpeExpressionEvent::Kind kind,
                                                         const midi::MpeNoteState& state) noexcept {
    if (epoch != impl_->epoch)
        return VoiceBankBindingCode::StaleEpoch;
    if (slot >= kSlots || generation == 0 || !impl_->owner->live(slot, generation, kind, state) ||
        state.note_id != generation)
        return VoiceBankBindingCode::StaleGeneration;
    impl_->owner->dispatch({0, kind, state});
    return VoiceBankBindingCode::Ok;
}
bool VoiceBankBinding::set_slot_parameter(std::uint32_t slot, std::string_view path, float value) {
    if (slot >= kSlots || !std::isfinite(value) || impl_->block_open)
        return false;
    const auto key = std::string(path);
    const auto target = impl_->slots[slot].parameter_nodes.find(key);
    const auto bounds = impl_->slots[slot].parameter_bounds.find(key);
    const auto id = impl_->slots[slot].processor_parameter_ids.find(key);
    if (target == impl_->slots[slot].parameter_nodes.end() ||
        bounds == impl_->slots[slot].parameter_bounds.end() ||
        id == impl_->slots[slot].processor_parameter_ids.end() || value < bounds->second.minimum ||
        value > bounds->second.maximum)
        return false;
    const auto node = target->second;
    const auto processor = impl_->graph.processor_node_instance(node);
    if (!processor || !processor->parameter(id->second))
        return false;
    processor->state_store().set_value(id->second, value);
    impl_->slots[slot].parameters.at(key) = value;
    return true;
}

} // namespace pulp::host
