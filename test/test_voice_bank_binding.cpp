#include <catch2/catch_test_macros.hpp>

#include "../examples/g6-voice-bank-consumer/fixture.hpp"
#include "harness/paced_timing_cell.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {
using namespace pulp;
using namespace pulp::host;

using namespace pulp::examples::g6;

struct Stereo {
    std::vector<float> left;
    std::vector<float> right;
};

Stereo render(std::size_t partition) {
    VoiceBankBindingResult status;
    auto bank = make_bank(schema(), status);
    INFO("prepare code=" << static_cast<int>(status.code) << " path=" << status.failing_path
                         << " detail=" << status.detail);
    REQUIRE(bank);
    Stereo audio;
    for (std::size_t start = 0; start < 800; start += partition) {
        const auto frames = std::min(partition, std::size_t{800} - start);
        REQUIRE(bank->begin_block(start, frames) == HostEventResult::Accepted);
        if (start <= 3 && 3 < start + frames)
            REQUIRE(
                bank->publish(event(bank->epoch(), 3, 1, midi::MidiEvent::note_on(1, 69, 127))) ==
                HostEventResult::Accepted);
        if (start <= 6 && 6 < start + frames)
            REQUIRE(bank->publish(event(bank->epoch(), 6, 2, midi::MidiEvent::note_off(1, 69))) ==
                    HostEventResult::Accepted);
        audio::Buffer<float> buffer(2, frames);
        auto view = buffer.view();
        REQUIRE(bank->render(view, start, frames) == VoiceBankBindingCode::Ok);
        audio.left.insert(audio.left.end(), view.channel(0).begin(), view.channel(0).end());
        audio.right.insert(audio.right.end(), view.channel(1).begin(), view.channel(1).end());
    }
    return audio;
}

VoiceBankBindingCode step(VoiceBankBinding& bank, std::uint64_t sample,
                          std::initializer_list<midi::MidiEvent> messages) {
    REQUIRE(bank.begin_block(sample, 1) == HostEventResult::Accepted);
    std::uint64_t sequence = 1;
    for (const auto& message : messages)
        REQUIRE(bank.publish(event(bank.epoch(), sample, sequence++, message)) ==
                HostEventResult::Accepted);
    audio::Buffer<float> buffer(2, 1);
    auto view = buffer.view();
    return bank.render(view, sample, 1);
}
// Paced-cell oracle for the authored eight-slot chord: both channels finite,
// the left mix bounded by eight full-gain slots, the authored stereo law
// (right == 0.5 * left), and non-silence once the chord is sounding. Each
// channel is checked for finiteness on its own: a NaN right sample makes the
// stereo-law difference NaN, and a NaN comparison is false, not a failure.
bool eight_slot_block_ok(const float* left, const float* right, std::size_t frames) {
    float energy = 0.0f;
    for (std::size_t i = 0; i < frames; ++i) {
        if (!std::isfinite(left[i]) || !std::isfinite(right[i]) ||
            std::abs(left[i]) > 8.0f * kBankGain || std::abs(right[i] - 0.5f * left[i]) > 2e-6f)
            return false;
        energy += left[i] * left[i];
    }
    return energy > 0.0f;
}
} // namespace

TEST_CASE("G6 authored eight-slot bank renders partition-invariant stereo audio",
          "[host][graph][g6][audio]") {
    const auto scalar = render(1);
    REQUIRE(scalar.left.size() == 800);
    for (const auto partition : {32u, 64u, 127u, 256u}) {
        const auto candidate = render(partition);
        REQUIRE(candidate.left.size() == scalar.left.size());
        for (std::size_t i = 0; i < scalar.left.size(); ++i) {
            INFO("partition=" << partition << " sample=" << i);
            CHECK(std::abs(candidate.left[i] - scalar.left[i]) <= 2e-6f);
            CHECK(std::abs(candidate.right[i] - scalar.right[i]) <= 2e-6f);
        }
    }
    for (std::size_t i = 0; i < 3; ++i)
        CHECK(scalar.left[i] == 0.0f);
    CHECK(std::abs(scalar.left[3] + kBankGain) <= 2e-6f);
    // Independent oscillator/envelope reference, derived from MIDI note 69,
    // phase zero and the frozen four-frame release at absolute sample six.
    double phase = 0.0;
    for (std::size_t i = 3; i < 10; ++i) {
        const float triangle = phase < 0.5 ? static_cast<float>(4.0 * phase - 1.0)
                                           : static_cast<float>(3.0 - 4.0 * phase);
        const float release = i < 6 ? 1.0f : (4.0f - static_cast<float>(i - 6)) * 0.25f;
        CHECK(std::abs(scalar.left[i] - triangle * release * kBankGain) <= 2e-6f);
        CHECK(std::abs(scalar.right[i] - triangle * release * (0.5f * kBankGain)) <= 2e-6f);
        phase += 440.0 / 48000.0;
    }
    for (std::size_t i = 10; i < scalar.left.size(); ++i)
        CHECK(scalar.left[i] == 0.0f);
    CHECK(scalar.left[3] != scalar.right[3]);
}

TEST_CASE("G6 ninth note uses the existing four MPE steal policies and first-slot ties",
          "[host][graph][g6][mpe]") {
    const std::array modes{midi::MpeVoiceStealMode::Oldest, midi::MpeVoiceStealMode::LowestVelocity,
                           midi::MpeVoiceStealMode::LowestPitch,
                           midi::MpeVoiceStealMode::HighestPitch};
    const std::array expected{0u, 7u, 0u, 7u};
    for (std::size_t policy = 0; policy < modes.size(); ++policy) {
        VoiceBankBindingResult status;
        auto bank = make_bank(schema(), status);
        INFO("prepare code=" << static_cast<int>(status.code) << " path=" << status.failing_path
                             << " detail=" << status.detail);
        REQUIRE(bank);
        bank->set_steal_mode(modes[policy]);
        REQUIRE(bank->begin_block(0, 2) == HostEventResult::Accepted);
        for (std::uint8_t slot = 0; slot < 8; ++slot)
            REQUIRE(bank->publish(
                        event(bank->epoch(), 0, slot + 1,
                              midi::MidiEvent::note_on(slot + 1, 60 + slot, 100 - slot * 10))) ==
                    HostEventResult::Accepted);
        REQUIRE(bank->publish(event(bank->epoch(), 1, 9, midi::MidiEvent::note_on(9, 90, 127))) ==
                HostEventResult::Accepted);
        audio::Buffer<float> buffer(2, 2);
        auto view = buffer.view();
        REQUIRE(bank->render(view, 0, 2) == VoiceBankBindingCode::Ok);
        CHECK(owner(*bank).allocator().voice(expected[policy]).note_number() == 90);
        CHECK(owner(*bank).allocator().steal_count() == 1);
    }
    for (const auto mode :
         {midi::MpeVoiceStealMode::LowestVelocity, midi::MpeVoiceStealMode::LowestPitch,
          midi::MpeVoiceStealMode::HighestPitch}) {
        VoiceBankBindingResult status;
        auto bank = make_bank(schema(), status);
        REQUIRE(bank);
        bank->set_steal_mode(mode);
        REQUIRE(bank->begin_block(0, 2) == HostEventResult::Accepted);
        for (std::uint8_t slot = 0; slot < 8; ++slot)
            REQUIRE(bank->publish(event(bank->epoch(), 0, slot + 1,
                                        midi::MidiEvent::note_on(slot + 1, 60, 100))) ==
                    HostEventResult::Accepted);
        REQUIRE(bank->publish(event(bank->epoch(), 1, 9, midi::MidiEvent::note_on(9, 90, 127))) ==
                HostEventResult::Accepted);
        audio::Buffer<float> buffer(2, 2);
        auto view = buffer.view();
        REQUIRE(bank->render(view, 0, 2) == VoiceBankBindingCode::Ok);
        CHECK(owner(*bank).allocator().voice(0).note_number() == 90);
    }
}

TEST_CASE("G6 eight authored slots sum in ordinal order across block partitions",
          "[host][graph][g6][audio]") {
    std::array<double, 8> phases{};
    std::vector<float> reference(512);
    for (std::size_t frame = 0; frame < reference.size(); ++frame) {
        float sum = 0.0f;
        for (std::size_t slot = 0; slot < 8; ++slot) {
            const float triangle = phases[slot] < 0.5
                                       ? static_cast<float>(4.0 * phases[slot] - 1.0)
                                       : static_cast<float>(3.0 - 4.0 * phases[slot]);
            const float velocity = static_cast<float>(100 - slot * 10) / 127.0f;
            sum += (triangle * velocity) * kBankGain;
            phases[slot] +=
                440.0 * std::exp2((static_cast<double>(60 + slot) - 69.0) / 12.0) / 48000.0;
            if (phases[slot] >= 1.0)
                phases[slot] -= 1.0;
        }
        reference[frame] = sum;
    }
    for (const auto partition : {1u, 32u, 64u, 127u, 256u}) {
        VoiceBankBindingResult status;
        auto bank = make_bank(schema(), status);
        REQUIRE(bank);
        std::vector<float> rendered;
        for (std::size_t start = 0; start < reference.size(); start += partition) {
            const auto frames = std::min<std::size_t>(partition, reference.size() - start);
            REQUIRE(bank->begin_block(start, frames) == HostEventResult::Accepted);
            if (start == 0)
                for (std::uint8_t slot = 0; slot < 8; ++slot)
                    REQUIRE(bank->publish(event(
                                bank->epoch(), 0, slot + 1,
                                midi::MidiEvent::note_on(slot + 1, 60 + slot, 100 - slot * 10))) ==
                            HostEventResult::Accepted);
            audio::Buffer<float> buffer(2, frames);
            auto view = buffer.view();
            REQUIRE(bank->render(view, start, frames) == VoiceBankBindingCode::Ok);
            rendered.insert(rendered.end(), view.channel(0).begin(), view.channel(0).end());
            for (std::size_t i = 0; i < frames; ++i)
                CHECK(std::abs(view.channel(1)[i] - view.channel(0)[i] * 0.5f) <= 2e-6f);
        }
        REQUIRE(rendered.size() == reference.size());
        for (std::size_t i = 0; i < rendered.size(); ++i) {
            INFO("partition=" << partition << " sample=" << i);
            CHECK(std::abs(rendered[i] - reference[i]) <= 2e-6f);
        }
        for (std::size_t slot = 0; slot < 8; ++slot) {
            CHECK(owner(*bank).allocator().voice(slot).note_id() == slot + 1);
            CHECK(owner(*bank).allocator().voice(slot).note_number() == 60 + slot);
        }
    }
}

TEST_CASE("G6 binding refuses wrong shape and stale deferred generations",
          "[host][graph][g6][binding]") {
    VoiceBankBindingResult status;
    auto malformed = schema();
    malformed.bindings[4].shape = VoiceBindingShape::Mono;
    CHECK_FALSE(make_bank(malformed, status));
    CHECK(status.code == VoiceBankBindingCode::InvalidManifest);
    malformed = schema();
    malformed.bindings[0].target = "nonexistent";
    CHECK_FALSE(make_bank(malformed, status));
    CHECK(status.code == VoiceBankBindingCode::InvalidManifest);
    malformed = schema();
    malformed.bindings[1].target = "wrong-expression";
    CHECK_FALSE(make_bank(malformed, status));
    CHECK(status.code == VoiceBankBindingCode::InvalidManifest);
    malformed = schema();
    malformed.bindings[2].ordinal = 99;
    CHECK_FALSE(make_bank(malformed, status));
    CHECK(status.code == VoiceBankBindingCode::InvalidManifest);
    auto disconnected = definition();
    disconnected.exposed_ports.pop_back();
    CHECK_FALSE(make_bank(schema(), status, std::move(disconnected)));
    CHECK(status.code == VoiceBankBindingCode::InvalidManifest);
    auto swapped = definition();
    swapped.exposed_ports[0].port = 1;
    swapped.exposed_ports[1].port = 0;
    CHECK_FALSE(make_bank(schema(), status, std::move(swapped)));
    CHECK(status.code == VoiceBankBindingCode::InvalidManifest);
    class AliasedOwner final : public VoiceBankOwner {
      public:
        AliasedOwner()
            : backing_(
                  [](ProbeVoice& voice, std::uint32_t) -> std::unique_ptr<format::Processor> {
                      return std::make_unique<ProbeProcessor>(voice);
                  },
                  std::array<VoiceBankInputEndpoint, 4>{
                      {{"note_in", 0}, {"pitch", 1}, {"pressure", 2}, {"timbre", 3}}}) {}
        std::unique_ptr<format::Processor> make_processor(std::uint32_t slot) override {
            return backing_.make_processor(slot);
        }
        void dispatch(const midi::MpeExpressionEvent& e) noexcept override {
            backing_.dispatch(e);
        }
        bool live(std::uint32_t slot, midi::MpeNoteGeneration generation,
                  midi::MpeExpressionEvent::Kind kind,
                  const midi::MpeNoteState& state) const noexcept override {
            return backing_.live(slot, generation, kind, state);
        }
        void reset() noexcept override {
            backing_.reset();
        }
        void set_steal_mode(midi::MpeVoiceStealMode mode) noexcept override {
            backing_.set_steal_mode(mode);
        }
        bool slot_note(std::uint32_t slot, midi::MpeNoteState& note,
                       bool& releasing) const noexcept override {
            return backing_.slot_note(slot, note, releasing);
        }
        bool retire_slot(std::uint32_t slot, midi::MpeNoteGeneration generation) noexcept override {
            return backing_.retire_slot(slot, generation);
        }
        InputSemantic resolve_input(const VoiceBindingSpec& binding) const noexcept override {
            return backing_.resolve_input(binding);
        }
        const void* slot_state_identity(std::uint32_t) const noexcept override {
            return backing_.slot_state_identity(0);
        }

      private:
        MpeVoiceBankOwner<ProbeVoice> backing_;
    };
    CHECK_FALSE(VoiceBankBinding::prepare(schema(), definition(), std::make_unique<AliasedOwner>(),
                                          "audio_out", 48000, 64, status));
    CHECK(status.code == VoiceBankBindingCode::InvalidManifest);
    auto bank = make_bank(schema(), status);
    INFO("prepare code=" << static_cast<int>(status.code) << " path=" << status.failing_path
                         << " detail=" << status.detail);
    REQUIRE(bank);
    REQUIRE(bank->begin_block(0, 1) == HostEventResult::Accepted);
    REQUIRE(bank->publish(event(bank->epoch(), 0, 1, midi::MidiEvent::note_on(1, 69, 127))) ==
            HostEventResult::Accepted);
    audio::Buffer<float> buffer(2, 1);
    auto view = buffer.view();
    REQUIRE(bank->render(view, 0, 1) == VoiceBankBindingCode::Ok);
    const auto generation = owner(*bank).allocator().voice(0).note_id();
    REQUIRE(generation != 0);
    midi::MpeNoteState state{};
    state.note_id = generation;
    CHECK(bank->dispatch_deferred(bank->epoch(), 0, generation + 1,
                                  midi::MpeExpressionEvent::Kind::NoteOff,
                                  state) == VoiceBankBindingCode::StaleGeneration);
    state.channel = 1;
    state.note = 69;
    state.velocity = 127;
    auto forged = state;
    forged.channel = 2;
    CHECK(bank->dispatch_deferred(bank->epoch(), 0, generation,
                                  midi::MpeExpressionEvent::Kind::NoteOff,
                                  forged) == VoiceBankBindingCode::StaleGeneration);
    CHECK(bank->dispatch_deferred(bank->epoch(), 0, generation,
                                  midi::MpeExpressionEvent::Kind::NoteOff,
                                  state) == VoiceBankBindingCode::Ok);
    CHECK(bank->dispatch_deferred(bank->epoch(), 0, generation,
                                  midi::MpeExpressionEvent::Kind::NoteOff,
                                  state) == VoiceBankBindingCode::StaleGeneration);
    REQUIRE(bank->activate(status));
    CHECK(bank->dispatch_deferred(bank->epoch() - 1, 0, generation,
                                  midi::MpeExpressionEvent::Kind::NoteOff,
                                  state) == VoiceBankBindingCode::StaleEpoch);
}

TEST_CASE("G6 retired bank's deferred token is a stale epoch on a replacement bank",
          "[host][graph][g6][swap]") {
    VoiceBankBindingResult status;
    auto retired = make_bank(schema(), status);
    REQUIRE(retired);
    REQUIRE(step(*retired, 0, {midi::MidiEvent::note_on(1, 69, 127)}) == VoiceBankBindingCode::Ok);
    midi::MpeNoteState note{};
    bool releasing = true;
    REQUIRE(retired->owner().slot_note(0, note, releasing));
    const auto retired_epoch = retired->epoch();

    // The replacement plays the same note, so its tracker hands out the same
    // generation in the same slot: only the epoch can tell the tokens apart.
    auto replacement = make_bank(schema(), status);
    REQUIRE(replacement);
    CHECK(replacement->epoch() != retired_epoch);
    REQUIRE(step(*replacement, 0, {midi::MidiEvent::note_on(1, 69, 127)}) ==
            VoiceBankBindingCode::Ok);
    midi::MpeNoteState replacement_note{};
    REQUIRE(replacement->owner().slot_note(0, replacement_note, releasing));
    REQUIRE(replacement_note.note_id == note.note_id);
    CHECK(replacement->dispatch_deferred(retired_epoch, 0, note.note_id,
                                         midi::MpeExpressionEvent::Kind::NoteOff,
                                         note) == VoiceBankBindingCode::StaleEpoch);

    // Activation also draws from the shared sequence: the retired bank's next
    // epoch cannot land on the replacement's live one.
    REQUIRE(retired->activate(status));
    CHECK(retired->epoch() != replacement->epoch());
    CHECK(replacement->dispatch_deferred(retired->epoch(), 0, note.note_id,
                                         midi::MpeExpressionEvent::Kind::NoteOff,
                                         note) == VoiceBankBindingCode::StaleEpoch);
    CHECK(replacement->dispatch_deferred(replacement->epoch(), 0, replacement_note.note_id,
                                         midi::MpeExpressionEvent::Kind::NoteOff,
                                         replacement_note) == VoiceBankBindingCode::Ok);
}

TEST_CASE("G6 deferred expression rejects non-finite and out-of-range state before dispatch",
          "[host][graph][g6][expression][validation]") {
    struct Case {
        midi::MpeExpressionEvent::Kind kind;
        float value;
        bool valid;
    };
    const auto cases = std::array{
        Case{midi::MpeExpressionEvent::Kind::PitchBend, 1.0f, true},
        // The default MPE member range is finite and inclusive: both edges
        // must reach the deferred voice dispatch, while the adjacent values
        // must be rejected before they can mutate voice state.
        Case{midi::MpeExpressionEvent::Kind::PitchBend, 48.0f, true},
        Case{midi::MpeExpressionEvent::Kind::PitchBend, -48.0f, true},
        Case{midi::MpeExpressionEvent::Kind::PitchBend, 48.001f, false},
        Case{midi::MpeExpressionEvent::Kind::PitchBend, -48.001f, false},
        Case{midi::MpeExpressionEvent::Kind::Pressure, 0.5f, true},
        Case{midi::MpeExpressionEvent::Kind::Timbre, 0.5f, true},
        Case{midi::MpeExpressionEvent::Kind::PitchBend, std::numeric_limits<float>::quiet_NaN(),
             false},
        Case{midi::MpeExpressionEvent::Kind::PitchBend, std::numeric_limits<float>::infinity(),
             false},
        Case{midi::MpeExpressionEvent::Kind::Pressure, std::numeric_limits<float>::quiet_NaN(),
             false},
        Case{midi::MpeExpressionEvent::Kind::Pressure, -std::numeric_limits<float>::infinity(),
             false},
        Case{midi::MpeExpressionEvent::Kind::Timbre, std::numeric_limits<float>::quiet_NaN(),
             false},
        Case{midi::MpeExpressionEvent::Kind::Timbre, std::numeric_limits<float>::infinity(), false},
        Case{midi::MpeExpressionEvent::Kind::Pressure, -0.01f, false},
        Case{midi::MpeExpressionEvent::Kind::Timbre, 1.01f, false},
    };
    for (const auto& test : cases) {
        VoiceBankBindingResult status;
        auto bank = make_bank(schema(), status);
        REQUIRE(bank);
        REQUIRE(step(*bank, 0, {midi::MidiEvent::note_on(1, 69, 127)}) == VoiceBankBindingCode::Ok);
        const auto generation = owner(*bank).allocator().voice(0).note_id();
        midi::MpeNoteState state{};
        state.note_id = generation;
        state.channel = 1;
        state.note = 69;
        state.velocity = 127;
        if (test.kind == midi::MpeExpressionEvent::Kind::PitchBend)
            state.pitch_bend_semitones = test.value;
        else if (test.kind == midi::MpeExpressionEvent::Kind::Pressure)
            state.pressure = test.value;
        else
            state.timbre = test.value;
        const auto result = bank->dispatch_deferred(bank->epoch(), 0, generation, test.kind, state);
        CHECK(result ==
              (test.valid ? VoiceBankBindingCode::Ok : VoiceBankBindingCode::IngressFailure));
        REQUIRE(step(*bank, 1, {}) == VoiceBankBindingCode::Ok);
        const auto& voice = owner(*bank).allocator().voice(0);
        if (test.valid) {
            CHECK(std::isfinite(voice.pitch_bend()));
            CHECK(std::isfinite(voice.pressure()));
            CHECK(std::isfinite(voice.timbre()));
        } else {
            CHECK(voice.pitch_bend() == 0.0f);
            CHECK(voice.pressure() == 0.0f);
            CHECK(voice.timbre() == 0.0f);
        }
    }
}

TEST_CASE("G6 malformed supported-kind MIDI refuses before rendering or voice dispatch",
          "[host][graph][g6][ingress]") {
    VoiceBankBindingResult status;
    auto bank = make_bank(schema(), status);
    REQUIRE(bank);
    REQUIRE(bank->begin_block(0, 4) == HostEventResult::Accepted);
    REQUIRE(bank->publish(event(bank->epoch(), 0, 1, midi::MidiEvent::note_on(1, 69, 127))) ==
            HostEventResult::Accepted);
    midi::MidiEvent malformed{choc::midi::ShortMessage(0x91, 0x80, 0x7f), 0, 0.0};
    REQUIRE(bank->publish(event(bank->epoch(), 1, 2, malformed)) == HostEventResult::Accepted);
    audio::Buffer<float> buffer(2, 4);
    for (auto channel : {0u, 1u})
        std::fill(buffer.channel(channel).begin(), buffer.channel(channel).end(), 7.0f);
    auto view = buffer.view();
    REQUIRE(bank->render(view, 0, 4) == VoiceBankBindingCode::IngressFailure);
    CHECK(owner(*bank).allocator().active_count() == 0);
    for (auto channel : {0u, 1u})
        for (const auto sample : buffer.channel(channel))
            CHECK(sample == 7.0f);
}

TEST_CASE("G6 named Processor parameters remain independent between slots",
          "[host][graph][g6][parameter]") {
    auto first_sample = [](std::uint32_t changed_slot, float level) {
        VoiceBankBindingResult status;
        auto bank = make_bank(schema(), status);
        REQUIRE(bank);
        REQUIRE(bank->set_slot_parameter(changed_slot, "level", level));
        REQUIRE(bank->begin_block(0, 1) == HostEventResult::Accepted);
        REQUIRE(bank->publish(event(bank->epoch(), 0, 1, midi::MidiEvent::note_on(1, 69, 127))) ==
                HostEventResult::Accepted);
        audio::Buffer<float> buffer(2, 1);
        auto view = buffer.view();
        REQUIRE(bank->render(view, 0, 1) == VoiceBankBindingCode::Ok);
        return view.channel(0)[0];
    };
    const auto baseline = first_sample(0, 1.0f);
    CHECK(baseline == -kBankGain);
    CHECK(first_sample(1, 2.0f) == baseline);
    CHECK(first_sample(0, 2.0f) == baseline * 2.0f);
}

TEST_CASE("G6 tracker capacity refuses 129th distinct start atomically and recovers",
          "[host][graph][g6][ingress][capacity]") {
    VoiceBankBindingResult status;
    auto bank = make_bank(schema(), status);
    REQUIRE(bank);
    for (std::uint32_t slot = 1; slot < 8; ++slot)
        REQUIRE(bank->set_slot_parameter(slot, "level", 0.0f));
    for (std::uint64_t sample = 0; sample < 128; ++sample) {
        REQUIRE(bank->begin_block(sample, 1) == HostEventResult::Accepted);
        REQUIRE(bank->publish(event(bank->epoch(), sample, 1,
                                    midi::MidiEvent::note_on(1, static_cast<std::uint8_t>(sample),
                                                             127))) == HostEventResult::Accepted);
        audio::Buffer<float> buffer(2, 1);
        auto view = buffer.view();
        REQUIRE(bank->render(view, sample, 1) == VoiceBankBindingCode::Ok);
    }
    CHECK(owner(*bank).allocator().steal_count() == 120);
    const auto generation = owner(*bank).allocator().voice(0).note_id();
    REQUIRE(bank->begin_block(128, 1) == HostEventResult::Accepted);
    REQUIRE(bank->publish(event(bank->epoch(), 128, 1, midi::MidiEvent::note_on(2, 0, 127))) ==
            HostEventResult::Accepted);
    audio::Buffer<float> refused(2, 1);
    refused.channel(0)[0] = 7.0f;
    refused.channel(1)[0] = 7.0f;
    auto view = refused.view();
    CHECK(bank->render(view, 128, 1) == VoiceBankBindingCode::TrackerCapacity);
    CHECK(refused.channel(0)[0] == 7.0f);
    CHECK(refused.channel(1)[0] == 7.0f);
    CHECK(owner(*bank).allocator().voice(0).note_id() == generation);
    CHECK(owner(*bank).allocator().steal_count() == 120);
    REQUIRE(bank->begin_block(129, 1) == HostEventResult::Accepted);
    REQUIRE(bank->publish(event(bank->epoch(), 129, 1, midi::MidiEvent::note_off(1, 0))) ==
            HostEventResult::Accepted);
    audio::Buffer<float> released(2, 1);
    auto released_view = released.view();
    REQUIRE(bank->render(released_view, 129, 1) == VoiceBankBindingCode::Ok);
    REQUIRE(bank->begin_block(130, 1) == HostEventResult::Accepted);
    REQUIRE(bank->publish(event(bank->epoch(), 130, 1, midi::MidiEvent::note_on(2, 0, 127))) ==
            HostEventResult::Accepted);
    audio::Buffer<float> resumed(2, 1);
    auto resumed_view = resumed.view();
    REQUIRE(bank->render(resumed_view, 130, 1) == VoiceBankBindingCode::Ok);
    CHECK(owner(*bank).allocator().voice(0).note_id() != generation);
    CHECK(owner(*bank).allocator().voice(0).note_id() == 129);
    CHECK(owner(*bank).allocator().voice(0).channel() == 2);
    CHECK(owner(*bank).allocator().voice(0).note_number() == 0);
    CHECK(resumed.channel(0)[0] == -kBankGain);
}

TEST_CASE("G6 capacity preview respects retrigger and equal-sample event order",
          "[host][graph][g6][ingress][capacity]") {
    auto filled = [] {
        VoiceBankBindingResult status;
        auto bank = make_bank(schema(), status);
        REQUIRE(bank);
        for (std::uint64_t sample = 0; sample < 128; ++sample) {
            REQUIRE(bank->begin_block(sample, 1) == HostEventResult::Accepted);
            REQUIRE(bank->publish(event(
                        bank->epoch(), sample, 1,
                        midi::MidiEvent::note_on(1, static_cast<std::uint8_t>(sample), 127))) ==
                    HostEventResult::Accepted);
            audio::Buffer<float> buffer(2, 1);
            auto view = buffer.view();
            REQUIRE(bank->render(view, sample, 1) == VoiceBankBindingCode::Ok);
        }
        return bank;
    };
    auto retrigger = filled();
    REQUIRE(retrigger->begin_block(128, 1) == HostEventResult::Accepted);
    REQUIRE(retrigger->publish(
                event(retrigger->epoch(), 128, 1, midi::MidiEvent::note_on(1, 127, 100))) ==
            HostEventResult::Accepted);
    audio::Buffer<float> retrigger_audio(2, 1);
    auto retrigger_view = retrigger_audio.view();
    CHECK(retrigger->render(retrigger_view, 128, 1) == VoiceBankBindingCode::Ok);

    auto off_on = filled();
    REQUIRE(off_on->begin_block(128, 1) == HostEventResult::Accepted);
    REQUIRE(off_on->publish(event(off_on->epoch(), 128, 1, midi::MidiEvent::note_off(1, 0))) ==
            HostEventResult::Accepted);
    REQUIRE(off_on->publish(event(off_on->epoch(), 128, 2, midi::MidiEvent::note_on(2, 0, 127))) ==
            HostEventResult::Accepted);
    audio::Buffer<float> off_on_audio(2, 1);
    auto off_on_view = off_on_audio.view();
    CHECK(off_on->render(off_on_view, 128, 1) == VoiceBankBindingCode::Ok);

    auto on_off = filled();
    const auto generation = owner(*on_off).allocator().voice(0).note_id();
    REQUIRE(on_off->begin_block(128, 1) == HostEventResult::Accepted);
    REQUIRE(on_off->publish(event(on_off->epoch(), 128, 1, midi::MidiEvent::note_on(2, 0, 127))) ==
            HostEventResult::Accepted);
    REQUIRE(on_off->publish(event(on_off->epoch(), 128, 2, midi::MidiEvent::note_off(1, 0))) ==
            HostEventResult::Accepted);
    audio::Buffer<float> on_off_audio(2, 1);
    on_off_audio.channel(0)[0] = 7.0f;
    auto on_off_view = on_off_audio.view();
    CHECK(on_off->render(on_off_view, 128, 1) == VoiceBankBindingCode::TrackerCapacity);
    CHECK(on_off_audio.channel(0)[0] == 7.0f);
    CHECK(owner(*on_off).allocator().voice(0).note_id() == generation);

    for (const auto control : {120, 123}) {
        auto channel_mode = filled();
        REQUIRE(channel_mode->begin_block(128, 1) == HostEventResult::Accepted);
        REQUIRE(channel_mode->publish(event(channel_mode->epoch(), 128, 1,
                                            midi::MidiEvent::cc(control == 120 ? 1 : 0, control,
                                                                0))) == HostEventResult::Accepted);
        REQUIRE(channel_mode->publish(
                    event(channel_mode->epoch(), 128, 2, midi::MidiEvent::note_on(2, 0, 127))) ==
                HostEventResult::Accepted);
        audio::Buffer<float> mode_audio(2, 1);
        auto mode_view = mode_audio.view();
        CHECK(channel_mode->render(mode_view, 128, 1) == VoiceBankBindingCode::Ok);
        CHECK(owner(*channel_mode).allocator().active_count() > 0);
    }
}

TEST_CASE("G6 polyphonic pressure refuses before note or output mutation",
          "[host][graph][g6][ingress]") {
    VoiceBankBindingResult status;
    auto bank = make_bank(schema(), status);
    REQUIRE(bank);
    REQUIRE(bank->begin_block(0, 1) == HostEventResult::Accepted);
    REQUIRE(bank->publish(event(bank->epoch(), 0, 1, midi::MidiEvent::note_on(1, 69, 127))) ==
            HostEventResult::Accepted);
    REQUIRE(bank->publish(event(bank->epoch(), 0, 2, midi::MidiEvent::poly_pressure(1, 69, 127))) ==
            HostEventResult::Accepted);
    audio::Buffer<float> buffer(2, 1);
    buffer.channel(0)[0] = 7.0f;
    buffer.channel(1)[0] = 7.0f;
    auto view = buffer.view();
    CHECK(bank->render(view, 0, 1) == VoiceBankBindingCode::IngressFailure);
    CHECK(owner(*bank).allocator().active_count() == 0);
    CHECK(buffer.channel(0)[0] == 7.0f);
    CHECK(buffer.channel(1)[0] == 7.0f);
    REQUIRE(bank->begin_block(1, 1) == HostEventResult::Accepted);
    REQUIRE(bank->publish(event(bank->epoch(), 1, 1, midi::MidiEvent::note_on(1, 69, 127))) ==
            HostEventResult::Accepted);
    REQUIRE(bank->publish(event(bank->epoch(), 1, 2,
                                midi::MidiEvent{choc::midi::ShortMessage(0xd1, 127, 0), 0, 0.0})) ==
            HostEventResult::Accepted);
    audio::Buffer<float> positive(2, 1);
    auto positive_view = positive.view();
    REQUIRE(bank->render(positive_view, 1, 1) == VoiceBankBindingCode::Ok);
    CHECK(owner(*bank).allocator().voice(0).pressure() == 1.0f);
}

TEST_CASE("G6 member and manager pedals release once on the last applicable rise",
          "[host][graph][g6][pedal]") {
    for (const std::uint8_t down : {64, 127}) {
        VoiceBankBindingResult status;
        auto bank = make_bank(schema(), status);
        REQUIRE(bank);
        REQUIRE(step(*bank, 0,
                     {midi::MidiEvent::note_on(1, 69, 127), midi::MidiEvent::cc(1, 64, down),
                      midi::MidiEvent::cc(0, 64, down)}) == VoiceBankBindingCode::Ok);
        const auto generation = owner(*bank).allocator().voice(0).note_id();
        REQUIRE(
            step(*bank, 1, {midi::MidiEvent::note_off(1, 69), midi::MidiEvent::note_off(1, 69)}) ==
            VoiceBankBindingCode::Ok);
        CHECK(owner(*bank).allocator().voice(0).active());
        CHECK_FALSE(owner(*bank).allocator().voice(0).releasing());
        REQUIRE(step(*bank, 2, {midi::MidiEvent::cc(1, 64, 63)}) == VoiceBankBindingCode::Ok);
        CHECK_FALSE(owner(*bank).allocator().voice(0).releasing());
        REQUIRE(step(*bank, 3, {midi::MidiEvent::cc(0, 64, 63)}) == VoiceBankBindingCode::Ok);
        CHECK(owner(*bank).allocator().voice(0).releasing());
        CHECK(owner(*bank).allocator().voice(0).note_id() == generation);
        REQUIRE(step(*bank, 4, {midi::MidiEvent::cc(0, 64, 0)}) == VoiceBankBindingCode::Ok);
        CHECK(owner(*bank).allocator().voice(0).releasing());
        REQUIRE(step(*bank, 5, {}) == VoiceBankBindingCode::Ok);
        REQUIRE(step(*bank, 6, {}) == VoiceBankBindingCode::Ok);
        CHECK_FALSE(owner(*bank).allocator().voice(0).active());
    }
    VoiceBankBindingResult status;
    auto bank = make_bank(schema(), status);
    REQUIRE(bank);
    REQUIRE(
        step(*bank, 0, {midi::MidiEvent::note_on(1, 69, 127), midi::MidiEvent::cc(1, 64, 63)}) ==
        VoiceBankBindingCode::Ok);
    REQUIRE(step(*bank, 1, {midi::MidiEvent::note_off(1, 69)}) == VoiceBankBindingCode::Ok);
    CHECK(owner(*bank).allocator().voice(0).releasing());
}

TEST_CASE("G6 scoped channel modes preserve unrelated voices and refuse outside zones",
          "[host][graph][g6][pedal]") {
    VoiceBankBindingResult status;
    auto bank = make_bank(schema(), status, definition(), midi::MpeConfig::dual(2, 2));
    REQUIRE(bank);
    REQUIRE(step(*bank, 0,
                 {midi::MidiEvent::note_on(1, 60, 100), midi::MidiEvent::note_on(2, 62, 100),
                  midi::MidiEvent::note_on(14, 64, 100), midi::MidiEvent::cc(0, 64, 127)}) ==
            VoiceBankBindingCode::Ok);
    REQUIRE(step(*bank, 1, {midi::MidiEvent::cc(1, 123, 0)}) == VoiceBankBindingCode::Ok);
    CHECK_FALSE(owner(*bank).allocator().voice(0).releasing());
    CHECK_FALSE(owner(*bank).allocator().voice(1).releasing());
    REQUIRE(step(*bank, 2, {midi::MidiEvent::cc(1, 121, 0)}) == VoiceBankBindingCode::Ok);
    CHECK_FALSE(owner(*bank).allocator().voice(0).releasing());
    REQUIRE(step(*bank, 3, {midi::MidiEvent::cc(0, 121, 0)}) == VoiceBankBindingCode::Ok);
    CHECK(owner(*bank).allocator().voice(0).releasing());
    CHECK_FALSE(owner(*bank).allocator().voice(1).releasing());
    REQUIRE(step(*bank, 4, {midi::MidiEvent::cc(2, 120, 0)}) == VoiceBankBindingCode::Ok);
    CHECK_FALSE(owner(*bank).allocator().voice(1).active());
    CHECK(owner(*bank).allocator().voice(2).active());
    REQUIRE(step(*bank, 5, {midi::MidiEvent::cc(3, 64, 127)}) == VoiceBankBindingCode::OutOfZone);
    REQUIRE(step(*bank, 6, {midi::MidiEvent::cc(14, 66, 0)}) ==
            VoiceBankBindingCode::UnsupportedControl);
    REQUIRE(step(*bank, 7, {midi::MidiEvent::cc(15, 120, 0)}) == VoiceBankBindingCode::Ok);
    CHECK_FALSE(owner(*bank).allocator().voice(2).active());
}

TEST_CASE("G6 stolen held generation cannot release its replacement", "[host][graph][g6][pedal]") {
    VoiceBankBindingResult status;
    auto bank = make_bank(schema(), status);
    REQUIRE(bank);
    REQUIRE(step(*bank, 0,
                 {midi::MidiEvent::cc(0, 64, 127), midi::MidiEvent::note_on(1, 60, 100),
                  midi::MidiEvent::note_on(2, 61, 100), midi::MidiEvent::note_on(3, 62, 100),
                  midi::MidiEvent::note_on(4, 63, 100), midi::MidiEvent::note_on(5, 64, 100),
                  midi::MidiEvent::note_on(6, 65, 100), midi::MidiEvent::note_on(7, 66, 100),
                  midi::MidiEvent::note_on(8, 67, 100)}) == VoiceBankBindingCode::Ok);
    const auto stale = owner(*bank).allocator().voice(0).note_id();
    REQUIRE(step(*bank, 1, {midi::MidiEvent::note_off(1, 60)}) == VoiceBankBindingCode::Ok);
    REQUIRE(step(*bank, 2, {midi::MidiEvent::note_on(9, 90, 100)}) == VoiceBankBindingCode::Ok);
    const auto replacement = owner(*bank).allocator().voice(0).note_id();
    REQUIRE(replacement != stale);
    midi::MpeNoteState old{};
    old.note_id = stale;
    old.channel = 1;
    old.note = 60;
    old.velocity = 100;
    CHECK(bank->dispatch_deferred(bank->epoch(), 0, stale, midi::MpeExpressionEvent::Kind::NoteOff,
                                  old) == VoiceBankBindingCode::StaleGeneration);
    REQUIRE(step(*bank, 3, {midi::MidiEvent::cc(0, 64, 0)}) == VoiceBankBindingCode::Ok);
    CHECK(owner(*bank).allocator().voice(0).active());
    CHECK_FALSE(owner(*bank).allocator().voice(0).releasing());
    CHECK(owner(*bank).allocator().voice(0).note_id() == replacement);
}

TEST_CASE("G6 CC121 resets expression on held and releasing voices",
          "[host][graph][g6][pedal][expression]") {
    for (const bool held : {true, false}) {
        VoiceBankBindingResult status;
        auto bank = make_bank(schema(), status);
        REQUIRE(bank);
        const midi::MidiEvent pressure{choc::midi::ShortMessage(0xd1, 127, 0), 0, 0.0};
        REQUIRE(step(*bank, 0,
                     {midi::MidiEvent::note_on(1, 69, 127), midi::MidiEvent::pitch_bend(1, 12288),
                      pressure, midi::MidiEvent::cc(1, 74, 127)}) == VoiceBankBindingCode::Ok);
        if (held)
            REQUIRE(step(*bank, 1,
                         {midi::MidiEvent::cc(1, 64, 127), midi::MidiEvent::note_off(1, 69)}) ==
                    VoiceBankBindingCode::Ok);
        else
            REQUIRE(step(*bank, 1, {midi::MidiEvent::note_off(1, 69)}) == VoiceBankBindingCode::Ok);
        const auto& before = owner(*bank).allocator().voice(0);
        CHECK(before.pitch_bend() > 0.0f);
        CHECK(before.pressure() > 0.0f);
        CHECK(before.timbre() > 0.0f);
        REQUIRE(step(*bank, 2, {midi::MidiEvent::cc(1, 121, 0)}) == VoiceBankBindingCode::Ok);
        const auto& after = owner(*bank).allocator().voice(0);
        CHECK(after.pitch_bend() == 0.0f);
        CHECK(after.pressure() == 0.0f);
        CHECK(after.timbre() == 0.0f);
        CHECK(after.releasing());
    }
}

TEST_CASE("G6 held expression stays generation-scoped when its member channel is reused",
          "[host][graph][g6][pedal][expression][audio]") {
    struct Observation {
        float left = 0.0f;
        float old_pressure = 0.0f;
        float new_pressure = 0.0f;
    };
    const auto pressure = [](std::uint8_t value) {
        return midi::MidiEvent{choc::midi::ShortMessage(0xd1, value, 0), 0, 0.0};
    };
    const auto observe = [&](std::uint8_t old_value, std::uint8_t new_value) {
        VoiceBankBindingResult status;
        auto bank = make_bank(schema(), status);
        REQUIRE(bank);
        for (std::uint64_t sample = 0; sample < 4; ++sample) {
            REQUIRE(bank->begin_block(sample, 1) == HostEventResult::Accepted);
            if (sample == 0) {
                REQUIRE(bank->publish(event(bank->epoch(), sample, 1,
                                            midi::MidiEvent::note_on(1, 69, 127))) ==
                        HostEventResult::Accepted);
                REQUIRE(bank->publish(event(bank->epoch(), sample, 2, pressure(old_value))) ==
                        HostEventResult::Accepted);
            } else if (sample == 1) {
                REQUIRE(bank->publish(
                            event(bank->epoch(), sample, 1, midi::MidiEvent::cc(1, 64, 127))) ==
                        HostEventResult::Accepted);
                REQUIRE(bank->publish(
                            event(bank->epoch(), sample, 2, midi::MidiEvent::note_off(1, 69))) ==
                        HostEventResult::Accepted);
            } else if (sample == 2) {
                REQUIRE(bank->publish(event(bank->epoch(), sample, 1,
                                            midi::MidiEvent::note_on(1, 72, 127))) ==
                        HostEventResult::Accepted);
                REQUIRE(bank->publish(event(bank->epoch(), sample, 2, pressure(new_value))) ==
                        HostEventResult::Accepted);
            }
            audio::Buffer<float> buffer(2, 1);
            auto view = buffer.view();
            REQUIRE(bank->render(view, sample, 1) == VoiceBankBindingCode::Ok);
            if (sample == 3)
                return Observation{view.channel(0)[0], owner(*bank).allocator().voice(0).pressure(),
                                   owner(*bank).allocator().voice(1).pressure()};
        }
        return Observation{};
    };
    const auto baseline = observe(0, 0);
    const auto held = observe(127, 0);
    const auto newcomer = observe(0, 127);
    CHECK(held.old_pressure == 1.0f);
    CHECK(held.new_pressure == 0.0f);
    CHECK(newcomer.old_pressure == 0.0f);
    CHECK(newcomer.new_pressure == 1.0f);
    CHECK(std::abs(held.left - baseline.left) > 1e-3f);
    CHECK(std::abs(newcomer.left - baseline.left) > 1e-3f);
}

TEST_CASE("G6 manager expression refuses before dispatch until scoped audio semantics exist",
          "[host][graph][g6][ingress][expression]") {
    const std::array<midi::MidiEvent, 3> unsupported{
        midi::MidiEvent::pitch_bend(0, 12288),
        midi::MidiEvent{choc::midi::ShortMessage(0xd0, 64, 0), 0, 0.0},
        midi::MidiEvent::cc(0, 74, 64),
    };
    for (const auto& message : unsupported) {
        VoiceBankBindingResult status;
        auto bank = make_bank(schema(), status);
        REQUIRE(bank);
        REQUIRE(bank->begin_block(0, 1) == HostEventResult::Accepted);
        REQUIRE(bank->publish(event(bank->epoch(), 0, 1, midi::MidiEvent::note_on(1, 69, 127))) ==
                HostEventResult::Accepted);
        REQUIRE(bank->publish(event(bank->epoch(), 0, 2, message)) == HostEventResult::Accepted);
        audio::Buffer<float> buffer(2, 1);
        std::fill(buffer.channel(0).begin(), buffer.channel(0).end(), 7.0f);
        std::fill(buffer.channel(1).begin(), buffer.channel(1).end(), 7.0f);
        auto view = buffer.view();
        CHECK(bank->render(view, 0, 1) == VoiceBankBindingCode::UnsupportedExpression);
        CHECK(owner(*bank).allocator().active_count() == 0);
        CHECK(buffer.channel(0)[0] == 7.0f);
        CHECK(buffer.channel(1)[0] == 7.0f);
    }
}

// Paced-timing matrix cell: the authored eight-slot bank with all eight slots
// held from the first callback, so every paced block renders full polyphony.
// The per-block oracle is the authored stereo law (right == 0.5 * left), a
// finite bounded mix, and non-silence once the chord is sounding.
TEST_CASE("G6 voice bank paced matrix cell", "[.][host][graph][g6][paced-matrix]") {
    const auto config = pulp::test::paced::read_config("eight-slot", 128);
    REQUIRE(config.variant == "eight-slot");
    REQUIRE(config.rate == 48000);
    REQUIRE(config.frames <= 256);
    VoiceBankBindingResult status;
    auto bank = make_bank(schema(), status);
    REQUIRE(bank);
    audio::Buffer<float> buffer(2, config.frames);
    const auto frames = static_cast<std::size_t>(config.frames);
    bool dispatched = false;
    const auto rows = pulp::test::paced::run_cell(
        config,
        [&](std::size_t index, std::uint64_t sample) {
            dispatched = false;
            if (bank->begin_block(sample, frames) != HostEventResult::Accepted)
                return;
            if (index == 0)
                for (std::uint8_t slot = 0; slot < 8; ++slot)
                    if (bank->publish(event(
                            bank->epoch(), 0, slot + 1,
                            midi::MidiEvent::note_on(slot + 1, 60 + slot, 100 - slot * 10))) !=
                        HostEventResult::Accepted)
                        return;
            auto view = buffer.view();
            dispatched = bank->render(view, sample, frames) == VoiceBankBindingCode::Ok;
        },
        [&](std::size_t, std::uint64_t) {
            if (!dispatched)
                return false;
            auto view = buffer.view();
            return eight_slot_block_ok(view.channel(0).data(), view.channel(1).data(), frames);
        });
    CHECK(pulp::test::paced::print_cell("g6-voice-bank", config, rows) == 0);
}

TEST_CASE("G6 paced cell oracle rejects a non-finite right channel beside a passing control",
          "[host][graph][g6][paced-matrix]") {
    std::array<float, 4> left{0.25f, -0.5f, 0.125f, 0.0f};
    std::array<float, 4> right{};
    for (std::size_t i = 0; i < left.size(); ++i)
        right[i] = 0.5f * left[i];
    CHECK(eight_slot_block_ok(left.data(), right.data(), left.size()));
    for (const float planted :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        auto broken = right;
        broken[1] = planted;
        CHECK_FALSE(eight_slot_block_ok(left.data(), broken.data(), left.size()));
    }
    auto broken_left = left;
    broken_left[2] = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(eight_slot_block_ok(broken_left.data(), right.data(), left.size()));
}
