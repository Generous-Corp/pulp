#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/host/graph_event_contract.hpp>
#include <pulp/host/signal_graph_subgraph.hpp>
#include <pulp/host/voice_template.hpp>
#include <pulp/midi/mpe_synth_voice.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

namespace pulp::host {

class VoiceBankOwner {
  public:
    virtual ~VoiceBankOwner() = default;
    virtual std::unique_ptr<format::Processor> make_processor(std::uint32_t slot) = 0;
    virtual void dispatch(const midi::MpeExpressionEvent& event) noexcept = 0;
    virtual bool live(std::uint32_t slot, midi::MpeNoteGeneration generation,
                      midi::MpeExpressionEvent::Kind kind,
                      const midi::MpeNoteState& state) const noexcept = 0;
    virtual void reset() noexcept = 0;
    virtual void set_steal_mode(midi::MpeVoiceStealMode mode) noexcept = 0;
    // Opaque identity of the mutable voice state borrowed by the Processor
    // factory. Preparation refuses null or repeated identities across slots.
    virtual const void* slot_state_identity(std::uint32_t slot) const noexcept = 0;
    enum class InputSemantic : std::uint8_t { Unknown, Note, Pitch, Pressure, Timbre };
    virtual InputSemantic resolve_input(const VoiceBindingSpec& binding) const noexcept = 0;
};

struct VoiceBankInputEndpoint {
    std::string target;
    std::uint32_t ordinal = 0;
};

// The developer supplies a Processor that borrows the corresponding voice.
// This adapter retains exactly one existing MPE allocator for all eight slots.
template <class Voice> class MpeVoiceBankOwner final : public VoiceBankOwner {
    static_assert(std::is_base_of_v<midi::MpeSynthVoice, Voice>);

  public:
    using ProcessorFactory =
        std::function<std::unique_ptr<format::Processor>(Voice&, std::uint32_t)>;
    explicit MpeVoiceBankOwner(ProcessorFactory factory,
                               std::array<VoiceBankInputEndpoint, 4> endpoints)
        : allocator_(8), factory_(std::move(factory)), endpoints_(std::move(endpoints)) {}
    std::unique_ptr<format::Processor> make_processor(std::uint32_t slot) override {
        return slot < 8 && factory_ ? factory_(allocator_.voice(slot), slot) : nullptr;
    }
    void dispatch(const midi::MpeExpressionEvent& event) noexcept override {
        allocator_.dispatch(event);
    }
    bool live(std::uint32_t slot, midi::MpeNoteGeneration generation,
              midi::MpeExpressionEvent::Kind kind,
              const midi::MpeNoteState& state) const noexcept override {
        return slot < 8 && generation != 0 && allocator_.voice(slot).active() &&
               allocator_.voice(slot).note_id() == generation && state.note_id == generation &&
               state.channel == allocator_.voice(slot).channel() &&
               state.note == allocator_.voice(slot).note_number() &&
               state.velocity == allocator_.voice(slot).velocity() &&
               kind != midi::MpeExpressionEvent::Kind::NoteOn &&
               (kind != midi::MpeExpressionEvent::Kind::NoteOff ||
                !allocator_.voice(slot).releasing());
    }
    void reset() noexcept override {
        allocator_.reset_all();
    }
    void set_steal_mode(midi::MpeVoiceStealMode mode) noexcept override {
        allocator_.set_steal_mode(mode);
    }
    const void* slot_state_identity(std::uint32_t slot) const noexcept override {
        return slot < 8 ? static_cast<const void*>(&allocator_.voice(slot)) : nullptr;
    }
    InputSemantic resolve_input(const VoiceBindingSpec& binding) const noexcept override {
        if (binding.target_kind != VoiceBindingTarget::Port ||
            binding.direction != VoiceBindingDirection::Input)
            return InputSemantic::Unknown;
        for (std::size_t i = 0; i < endpoints_.size(); ++i) {
            if (endpoints_[i].target != binding.target || endpoints_[i].ordinal != binding.ordinal)
                continue;
            const bool note = i == 0;
            if (binding.type != (note ? VoiceBindingType::Event : VoiceBindingType::Control) ||
                binding.shape != VoiceBindingShape::Scalar ||
                binding.rate != (note ? VoiceBindingRate::Event : VoiceBindingRate::Sample))
                return InputSemantic::Unknown;
            return static_cast<InputSemantic>(i + 1);
        }
        return InputSemantic::Unknown;
    }
    midi::MpeVoiceAllocator<Voice>& allocator() noexcept {
        return allocator_;
    }
    const midi::MpeVoiceAllocator<Voice>& allocator() const noexcept {
        return allocator_;
    }

  private:
    midi::MpeVoiceAllocator<Voice> allocator_;
    ProcessorFactory factory_;
    std::array<VoiceBankInputEndpoint, 4> endpoints_;
};

enum class VoiceBankBindingCode : std::uint8_t {
    Ok,
    InvalidManifest,
    FactoryFailure,
    GraphPrepareFailure,
    InvalidBlock,
    IngressFailure,
    GraphExecutionFailure,
    StaleEpoch,
    StaleGeneration,
    EpochExhausted,
};

struct VoiceBankBindingResult {
    VoiceBankBindingCode code = VoiceBankBindingCode::Ok;
    std::uint32_t failing_slot = 0;
    std::string failing_path;
    std::string detail;
    explicit operator bool() const noexcept {
        return code == VoiceBankBindingCode::Ok;
    }
};

// One prepared graph and one eight-voice allocator. Construction, activation,
// and parameter edits are control-thread operations while audio is stopped.
// begin_block/publish/render and deferred dispatch belong to one audio owner.
class VoiceBankBinding final {
  public:
    static std::unique_ptr<VoiceBankBinding>
    prepare(const VoiceTemplateSchema& schema, SubgraphTemplate definition,
            std::unique_ptr<VoiceBankOwner> owner, std::string stereo_port, double sample_rate,
            int max_frames, VoiceBankBindingResult& result);
    ~VoiceBankBinding();
    VoiceBankBinding(const VoiceBankBinding&) = delete;
    VoiceBankBinding& operator=(const VoiceBankBinding&) = delete;

    const VoiceBankManifest& manifest() const noexcept;
    std::uint64_t epoch() const noexcept;
    bool activate(VoiceBankBindingResult& result) noexcept;
    void set_steal_mode(midi::MpeVoiceStealMode mode) noexcept;
    const VoiceBankOwner& owner() const noexcept;

    HostEventResult begin_block(std::uint64_t first_sample, std::size_t frames) noexcept;
    HostEventResult publish(const HostEventRecord& event) noexcept;
    VoiceBankBindingCode render(audio::BufferView<float>& stereo, std::uint64_t first_sample,
                                std::size_t frames) noexcept;
    VoiceBankBindingCode dispatch_deferred(std::uint64_t epoch, std::uint32_t slot,
                                           midi::MpeNoteGeneration generation,
                                           midi::MpeExpressionEvent::Kind kind,
                                           const midi::MpeNoteState& state) noexcept;
    bool set_slot_parameter(std::uint32_t slot, std::string_view path, float value);

  private:
    VoiceBankBinding();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::host
