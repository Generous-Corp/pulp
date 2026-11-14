#include <catch2/catch_test_macros.hpp>

#include "../examples/g6-voice-bank-consumer/fixture.hpp"
#include "harness/paced_timing_cell.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
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

// ---------------------------------------------------------------------------
// Idle voice skipping
// ---------------------------------------------------------------------------
namespace {

// What an inactive voice writes. Everything except Silent and DelayedClick
// breaks the idle-skip contract in some way the tests below pin down.
enum class IdleOutput { Silent, Tiny, NegativeZero, FreeRunning, DelayedClick };

struct IdleKnobs {
    int tail_samples = 0;
    int latency_samples = 0;
    bool declare_input = false;
    unsigned release_frames = 4;
    IdleOutput idle_output = IdleOutput::Silent;
    unsigned click_delay = 200;
    // process() calls per slot, counted by the processor.
    std::array<int, 8> calls{};
};

class IdleVoice final : public midi::MpeSynthVoice {
  public:
    void bind(IdleKnobs& knobs) noexcept {
        knobs_ = &knobs;
    }
    void on_note_on(const midi::MpeNoteState& note) override {
        midi::MpeSynthVoice::on_note_on(note);
        set_smoothing(0.0f);
        release_frame_ = 0;
        idle_frames_ = 0;
        was_active_ = true;
        // A voice that starts its phase from a free-running counter depends on
        // how many blocks it was rendered while inactive.
        phase_ = knobs_->idle_output == IdleOutput::FreeRunning
                     ? std::fmod(static_cast<double>(free_counter_) * 0.0137, 1.0)
                     : 0.0;
    }
    void on_note_off() override {
        midi::MpeSynthVoice::on_note_off();
        release_frame_ = 0;
    }
    void reset() override {
        midi::MpeSynthVoice::reset();
        release_frame_ = 0;
    }
    void render(float* out, int frames) override {
        render_stereo(out, nullptr, frames);
    }
    void render_stereo(float* left, float* right, int frames) noexcept {
        for (int i = 0; i < frames; ++i) {
            float l = 0.0f;
            float r = 0.0f;
            if (active()) {
                advance_smoothers();
                const double frequency =
                    440.0 *
                    std::exp2((static_cast<double>(note_number()) - 69.0 + pitch_bend()) / 12.0);
                const float triangle = phase_ < 0.5 ? static_cast<float>(4.0 * phase_ - 1.0)
                                                    : static_cast<float>(3.0 - 4.0 * phase_);
                const float fade =
                    releasing() ? (static_cast<float>(knobs_->release_frames - release_frame_)) /
                                      static_cast<float>(knobs_->release_frames)
                                : 1.0f;
                const float shaped = triangle * (static_cast<float>(velocity()) / 127.0f) * fade;
                l = shaped * (1.0f + 0.25f * pressure());
                r = shaped * (0.5f + 0.25f * timbre());
                phase_ += frequency / 48000.0;
                if (phase_ >= 1.0)
                    phase_ -= 1.0;
                if (releasing() && ++release_frame_ == knobs_->release_frames)
                    finish_release();
            } else {
                switch (knobs_->idle_output) {
                case IdleOutput::Silent:
                case IdleOutput::FreeRunning:
                    break;
                case IdleOutput::Tiny:
                    l = r = 1e-30f;
                    break;
                case IdleOutput::NegativeZero:
                    l = r = -0.0f;
                    break;
                case IdleOutput::DelayedClick:
                    if (was_active_ && ++idle_frames_ == knobs_->click_delay)
                        l = r = 0.5f;
                    break;
                }
            }
            ++free_counter_;
            left[i] = l;
            if (right)
                right[i] = r;
        }
    }

  private:
    IdleKnobs* knobs_ = nullptr;
    double phase_ = 0.0;
    unsigned release_frame_ = 0;
    unsigned idle_frames_ = 0;
    bool was_active_ = false;
    std::uint64_t free_counter_ = 0;
};

class IdleProcessor final : public format::Processor {
  public:
    IdleProcessor(IdleVoice& voice, IdleKnobs& knobs, std::uint32_t slot)
        : voice_(voice), knobs_(knobs), slot_(slot) {}
    format::PluginDescriptor descriptor() const override {
        format::PluginDescriptor d;
        d.name = "Idle Skip Voice";
        d.manufacturer = "Pulp Test";
        d.bundle_id = "com.pulp.test.idle-skip-voice";
        d.version = "1.0.0";
        d.category = format::PluginCategory::Instrument;
        d.output_buses = {{"Stereo", 2, false}};
        // PluginDescriptor declares a stereo input by default; an idle-skipped
        // voice has none.
        d.input_buses.clear();
        if (knobs_.declare_input)
            d.input_buses = {{"Input", 2, false}};
        d.tail_samples = knobs_.tail_samples;
        return d;
    }
    int latency_samples() const override {
        return knobs_.latency_samples;
    }
    void define_parameters(state::StateStore& store) override {
        store.add_parameter(
            {.id = 1, .name = "level", .range = state::ParamRange::linear(0.0f, 2.0f, 1.0f)});
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        ++knobs_.calls[slot_];
        auto* left = output.channel_ptr(0);
        auto* right = output.channel_ptr(1);
        voice_.render_stereo(left, right, static_cast<int>(output.num_samples()));
        const float gain = state().get_value(1) * kBankGain;
        for (std::size_t i = 0; i < output.num_samples(); ++i) {
            left[i] *= gain;
            right[i] *= gain;
        }
    }

  private:
    IdleVoice& voice_;
    IdleKnobs& knobs_;
    std::uint32_t slot_;
};

std::unique_ptr<VoiceBankBinding> make_idle_bank(IdleKnobs& knobs, VoiceBankIdleSkip mode,
                                                 VoiceBankBindingResult& status) {
    auto bank_owner = std::make_unique<MpeVoiceBankOwner<IdleVoice>>(
        [&knobs](IdleVoice& voice, std::uint32_t slot) -> std::unique_ptr<format::Processor> {
            voice.bind(knobs);
            return std::make_unique<IdleProcessor>(voice, knobs, slot);
        },
        std::array<VoiceBankInputEndpoint, 4>{
            {{"note_in", 0}, {"pitch", 1}, {"pressure", 2}, {"timbre", 3}}});
    return VoiceBankBinding::prepare(schema(), definition(), std::move(bank_owner), "audio_out",
                                     48000, 256, status, midi::MpeConfig::standard_lower(), mode);
}

struct ScriptedEvent {
    std::uint64_t sample;
    midi::MidiEvent message;
};

struct IdleRender {
    Stereo audio;
    std::uint64_t skipped = 0;
    std::vector<std::uint64_t> skipped_after_block;
    std::vector<std::array<int, 8>> calls_after_block;
};

using BlockHook = std::function<void(std::size_t block, VoiceBankBinding&)>;

IdleRender render_idle(IdleKnobs& knobs, const std::vector<ScriptedEvent>& script,
                       std::size_t total, std::size_t partition, VoiceBankIdleSkip mode,
                       midi::MpeVoiceStealMode steal = midi::MpeVoiceStealMode::Oldest,
                       const BlockHook& hook = {}) {
    VoiceBankBindingResult status;
    auto bank = make_idle_bank(knobs, mode, status);
    INFO("prepare code=" << static_cast<int>(status.code) << " detail=" << status.detail);
    REQUIRE(bank);
    bank->set_steal_mode(steal);
    IdleRender result;
    std::uint64_t sequence = 1;
    std::size_t block = 0;
    for (std::size_t start = 0; start < total; start += partition, ++block) {
        const auto frames = std::min(partition, total - start);
        if (hook)
            hook(block, *bank);
        REQUIRE(bank->begin_block(start, frames) == HostEventResult::Accepted);
        for (const auto& scripted : script)
            if (scripted.sample >= start && scripted.sample < start + frames)
                REQUIRE(bank->publish(event(bank->epoch(), scripted.sample, sequence++,
                                            scripted.message)) == HostEventResult::Accepted);
        audio::Buffer<float> buffer(2, frames);
        auto view = buffer.view();
        REQUIRE(bank->render(view, start, frames) == VoiceBankBindingCode::Ok);
        result.audio.left.insert(result.audio.left.end(), view.channel(0).begin(),
                                 view.channel(0).end());
        result.audio.right.insert(result.audio.right.end(), view.channel(1).begin(),
                                  view.channel(1).end());
        result.skipped_after_block.push_back(bank->idle_skipped_blocks());
        result.calls_after_block.push_back(knobs.calls);
    }
    result.skipped = bank->idle_skipped_blocks();
    return result;
}

bool same_bytes(const Stereo& a, const Stereo& b) {
    return a.left.size() == b.left.size() && a.right.size() == b.right.size() &&
           std::memcmp(a.left.data(), b.left.data(), a.left.size() * sizeof(float)) == 0 &&
           std::memcmp(a.right.data(), b.right.data(), a.right.size() * sizeof(float)) == 0;
}

ScriptedEvent on(std::uint64_t sample, std::uint8_t channel, std::uint8_t note,
                 std::uint8_t velocity = 100) {
    return {sample, midi::MidiEvent::note_on(channel, note, velocity)};
}
ScriptedEvent off(std::uint64_t sample, std::uint8_t channel, std::uint8_t note) {
    return {sample, midi::MidiEvent::note_off(channel, note)};
}
ScriptedEvent cc(std::uint64_t sample, std::uint8_t channel, std::uint8_t controller,
                 std::uint8_t value) {
    return {sample, midi::MidiEvent::cc(channel, controller, value)};
}

void require_on_off_identical(const std::vector<ScriptedEvent>& script, std::size_t total,
                              IdleKnobs knobs, midi::MpeVoiceStealMode steal) {
    auto reference_knobs = knobs;
    const auto reference =
        render_idle(reference_knobs, script, total, 1, VoiceBankIdleSkip::Off, steal);
    std::uint64_t skipped_somewhere = 0;
    for (const std::size_t partition : {1u, 32u, 64u, 127u, 256u}) {
        auto off_knobs = knobs;
        auto skip_knobs = knobs;
        const auto without =
            render_idle(off_knobs, script, total, partition, VoiceBankIdleSkip::Off, steal);
        const auto with = render_idle(skip_knobs, script, total, partition,
                                      VoiceBankIdleSkip::SkipInactiveSlots, steal);
        INFO("partition=" << partition);
        CHECK(same_bytes(without.audio, with.audio));
        CHECK(without.skipped == 0);
        skipped_somewhere += with.skipped;
        REQUIRE(without.audio.left.size() == reference.audio.left.size());
        for (std::size_t i = 0; i < reference.audio.left.size(); ++i) {
            CHECK(std::abs(without.audio.left[i] - reference.audio.left[i]) <= 2e-6f);
            CHECK(std::abs(without.audio.right[i] - reference.audio.right[i]) <= 2e-6f);
        }
    }
    // The comparison is only meaningful if skipping actually happened.
    CHECK(skipped_somewhere > 0);
}

} // namespace

TEST_CASE("Voice bank idle skip renders identical audio with skipping on and off",
          "[host][graph][voice-bank][idle-skip]") {
    IdleKnobs knobs;
    knobs.release_frames = 150;
    // A release that crosses block boundaries (2550 + 150 spans 2560 and 2667),
    // a retrigger of the same key, a sustain pedal hold and release, and the
    // scoped all-sound-off. Long gaps between them let slots go idle.
    const std::vector<ScriptedEvent> basic{
        on(3, 1, 69, 127), off(600, 1, 69),     on(1500, 1, 69),    on(1700, 1, 69, 90),
        off(1900, 1, 69),  on(2400, 2, 60),     off(2550, 2, 60),   cc(3000, 1, 64, 127),
        on(3010, 3, 62),   off(3100, 3, 62),    cc(3500, 1, 64, 0), on(4000, 4, 62),
        on(4010, 5, 64),   cc(4500, 4, 120, 0), cc(4800, 5, 120, 0)};
    require_on_off_identical(basic, 6000, knobs, midi::MpeVoiceStealMode::Oldest);

    // Expression and channel-mode events: pitch bend, pressure, timbre, the
    // scoped and manager all-notes-off and the reset-all-controllers message,
    // each arriving while a voice sounds, is held, or is releasing.
    const midi::MidiEvent pressure{choc::midi::ShortMessage(0xd1, 127, 0), 0, 0.0};
    const std::vector<ScriptedEvent> expression{
        on(10, 1, 69, 127),  {100, midi::MidiEvent::pitch_bend(1, 12288)},
        {150, pressure},     cc(200, 1, 74, 127),
        cc(300, 1, 64, 127), off(400, 1, 69),
        cc(600, 1, 121, 0),  cc(900, 1, 64, 0),
        on(1100, 2, 62),     on(1110, 3, 64),
        cc(1300, 2, 123, 0), cc(1600, 0, 123, 0),
        on(2000, 4, 65),     cc(2200, 0, 121, 0),
        cc(2400, 4, 120, 0), on(2600, 5, 67),
        off(2700, 5, 67)};
    require_on_off_identical(expression, 4000, knobs, midi::MpeVoiceStealMode::Oldest);

    // Nine notes force a steal; repeat for every steal policy.
    std::vector<ScriptedEvent> steal;
    for (std::uint8_t k = 0; k < 9; ++k)
        steal.push_back(on(100 + 10 * k, k + 1, 60 + k, 120 - k * 9));
    for (std::uint8_t k = 0; k < 9; ++k)
        steal.push_back(off(2000 + 5 * k, k + 1, 60 + k));
    for (const auto mode :
         {midi::MpeVoiceStealMode::Oldest, midi::MpeVoiceStealMode::LowestVelocity,
          midi::MpeVoiceStealMode::LowestPitch, midi::MpeVoiceStealMode::HighestPitch}) {
        INFO("steal mode=" << static_cast<int>(mode));
        require_on_off_identical(steal, 4500, knobs, mode);
    }
}

TEST_CASE("A note arriving mid-block wakes a skipped voice on its exact sample",
          "[host][graph][voice-bank][idle-skip]") {
    constexpr std::size_t kBlock = 64;
    constexpr std::size_t kNoteBlock = 6;
    const std::vector<ScriptedEvent> script{on(kNoteBlock * kBlock + 3, 1, 69, 127)};
    IdleKnobs off_knobs;
    IdleKnobs skip_knobs;
    const auto without = render_idle(off_knobs, script, 8 * kBlock, kBlock, VoiceBankIdleSkip::Off);
    const auto with =
        render_idle(skip_knobs, script, 8 * kBlock, kBlock, VoiceBankIdleSkip::SkipInactiveSlots);
    REQUIRE(with.skipped_after_block[kNoteBlock - 1] > 0);
    const auto base = kNoteBlock * kBlock;
    for (std::size_t i = 0; i < 3; ++i)
        CHECK(with.audio.left[base + i] == 0.0f);
    CHECK(with.audio.left[base + 3] != 0.0f);
    CHECK(same_bytes(without.audio, with.audio));
}

TEST_CASE("Idle skip stops calling inactive voices and counts the skipped blocks",
          "[host][graph][voice-bank][idle-skip]") {
    constexpr std::size_t kBlock = 64;
    constexpr std::size_t kBlocks = 10;
    IdleKnobs knobs;
    const auto result = render_idle(knobs, {on(0, 1, 69, 127)}, kBlocks * kBlock, kBlock,
                                    VoiceBankIdleSkip::SkipInactiveSlots);
    std::size_t active_slots = 0;
    for (const auto calls : knobs.calls) {
        if (calls == static_cast<int>(kBlocks))
            ++active_slots;
        else
            CHECK(calls == 1);
    }
    CHECK(active_slots == 1);
    CHECK(result.skipped == 7 * (kBlocks - 1));
}

TEST_CASE("Idle skip never engages during a release tail", "[host][graph][voice-bank][idle-skip]") {
    constexpr std::size_t kBlock = 64;
    IdleKnobs knobs;
    knobs.release_frames = 200;
    // Note off at sample 64 starts a 200 sample release that ends in block 4
    // (samples 256 to 319). Blocks 0 to 4 start with the voice active; block 5
    // starts idle and runs once to confirm silence; later blocks are skipped.
    const auto result = render_idle(knobs, {on(0, 1, 69, 127), off(64, 1, 69)}, 16 * kBlock, kBlock,
                                    VoiceBankIdleSkip::SkipInactiveSlots);
    int previous = 0;
    for (std::size_t block = 0; block < result.calls_after_block.size(); ++block) {
        // Slot 0 holds the note.
        const int calls = result.calls_after_block[block][0];
        INFO("block=" << block);
        CHECK(calls == (block <= 5 ? static_cast<int>(block) + 1 : 6));
        CHECK(calls >= previous);
        previous = calls;
    }
}

TEST_CASE("Idle skip waits out declared latency and tail", "[host][graph][voice-bank][idle-skip]") {
    constexpr std::size_t kBlock = 64;
    constexpr std::size_t kTotal = 24 * kBlock;
    // The voice goes inactive around sample 100 and then emits a click 200
    // samples later, after blocks 2 and 3 that are entirely silent.
    const std::vector<ScriptedEvent> script{on(0, 1, 69, 127), off(100, 1, 69)};
    IdleKnobs off_knobs;
    off_knobs.tail_samples = 300;
    off_knobs.idle_output = IdleOutput::DelayedClick;
    auto skip_knobs = off_knobs;
    const auto without = render_idle(off_knobs, script, kTotal, kBlock, VoiceBankIdleSkip::Off);
    const auto with =
        render_idle(skip_knobs, script, kTotal, kBlock, VoiceBankIdleSkip::SkipInactiveSlots);
    bool click = false;
    for (std::size_t i = 256; i < 320; ++i)
        click = click || with.audio.left[i] != 0.0f;
    CHECK(click);
    CHECK(same_bytes(without.audio, with.audio));
    CHECK(with.skipped > 0);

    // Without the declared tail the same voice would be skipped before its
    // click, which is what the hold exists to prevent.
    auto short_knobs = off_knobs;
    short_knobs.tail_samples = 0;
    const auto unprotected =
        render_idle(short_knobs, script, kTotal, kBlock, VoiceBankIdleSkip::SkipInactiveSlots);
    CHECK_FALSE(same_bytes(without.audio, unprotected.audio));
}

TEST_CASE("Idle skip is refused for an unbounded tail or an audio input",
          "[host][graph][voice-bank][idle-skip]") {
    for (const bool unbounded : {true, false}) {
        IdleKnobs knobs;
        if (unbounded)
            knobs.tail_samples = -1;
        else
            knobs.declare_input = true;
        VoiceBankBindingResult status;
        CHECK_FALSE(make_idle_bank(knobs, VoiceBankIdleSkip::SkipInactiveSlots, status));
        CHECK(status.code == VoiceBankBindingCode::InvalidManifest);
        INFO("unbounded=" << unbounded << " detail=" << status.detail);
        VoiceBankBindingResult plain;
        CHECK(make_idle_bank(knobs, VoiceBankIdleSkip::Off, plain));
    }
}

TEST_CASE("A voice that is not silent when inactive is never skipped",
          "[host][graph][voice-bank][idle-skip]") {
    for (const auto idle : {IdleOutput::Tiny, IdleOutput::NegativeZero}) {
        IdleKnobs off_knobs;
        off_knobs.idle_output = idle;
        auto skip_knobs = off_knobs;
        const std::vector<ScriptedEvent> script{on(0, 1, 69, 127), off(100, 1, 69)};
        const auto without = render_idle(off_knobs, script, 2000, 64, VoiceBankIdleSkip::Off);
        const auto with =
            render_idle(skip_knobs, script, 2000, 64, VoiceBankIdleSkip::SkipInactiveSlots);
        CHECK(with.skipped == 0);
        CHECK(same_bytes(without.audio, with.audio));
    }
}

TEST_CASE("Changing a slot parameter clears the idle confirmation",
          "[host][graph][voice-bank][idle-skip]") {
    IdleKnobs knobs;
    const auto result =
        render_idle(knobs, {}, 10 * 64, 64, VoiceBankIdleSkip::SkipInactiveSlots,
                    midi::MpeVoiceStealMode::Oldest, [](std::size_t block, VoiceBankBinding& bank) {
                        if (block == 5)
                            REQUIRE(bank.set_slot_parameter(3, "level", 0.5f));
                    });
    // Every slot ran once to confirm silence. Slot 3 ran once more after its
    // parameter changed, and then went back to being skipped.
    for (std::size_t slot = 0; slot < 8; ++slot)
        CHECK(knobs.calls[slot] == (slot == 3 ? 2 : 1));
    CHECK(result.calls_after_block[4][3] == 1);
    CHECK(result.calls_after_block[5][3] == 2);
}

TEST_CASE("Voice bank idle skip is off by default", "[host][graph][voice-bank][idle-skip]") {
    IdleKnobs knobs;
    VoiceBankBindingResult status;
    auto bank = make_idle_bank(knobs, VoiceBankIdleSkip::Off, status);
    REQUIRE(bank);
    for (std::size_t block = 0; block < 6; ++block) {
        REQUIRE(bank->begin_block(block * 64, 64) == HostEventResult::Accepted);
        audio::Buffer<float> buffer(2, 64);
        auto view = buffer.view();
        REQUIRE(bank->render(view, block * 64, 64) == VoiceBankBindingCode::Ok);
    }
    CHECK(bank->idle_skipped_blocks() == 0);
    for (const auto calls : knobs.calls)
        CHECK(calls == 6);
    // A prepare that does not mention the option behaves the same way.
    IdleKnobs defaults;
    auto owner_default = std::make_unique<MpeVoiceBankOwner<IdleVoice>>(
        [&defaults](IdleVoice& voice, std::uint32_t slot) -> std::unique_ptr<format::Processor> {
            voice.bind(defaults);
            return std::make_unique<IdleProcessor>(voice, defaults, slot);
        },
        std::array<VoiceBankInputEndpoint, 4>{
            {{"note_in", 0}, {"pitch", 1}, {"pressure", 2}, {"timbre", 3}}});
    VoiceBankBindingResult default_status;
    auto default_bank = VoiceBankBinding::prepare(schema(), definition(), std::move(owner_default),
                                                  "audio_out", 48000, 256, default_status);
    REQUIRE(default_bank);
    for (std::size_t block = 0; block < 3; ++block) {
        REQUIRE(default_bank->begin_block(block * 64, 64) == HostEventResult::Accepted);
        audio::Buffer<float> buffer(2, 64);
        auto view = buffer.view();
        REQUIRE(default_bank->render(view, block * 64, 64) == VoiceBankBindingCode::Ok);
    }
    CHECK(default_bank->idle_skipped_blocks() == 0);
    for (const auto calls : defaults.calls)
        CHECK(calls == 3);
}

TEST_CASE("Idle skip changes a voice that keeps free-running state, which is why it is opt-in",
          "[host][graph][voice-bank][idle-skip]") {
    // The voice starts its phase from a counter that advances on every rendered
    // sample, active or not. Skipping stops the counter, so the second note
    // sounds different. This is the contract authors must check for.
    IdleKnobs off_knobs;
    off_knobs.idle_output = IdleOutput::FreeRunning;
    auto skip_knobs = off_knobs;
    const std::vector<ScriptedEvent> script{on(0, 1, 69, 127), off(100, 1, 69),
                                            on(3000, 1, 69, 127)};
    const auto without = render_idle(off_knobs, script, 4000, 64, VoiceBankIdleSkip::Off);
    const auto with =
        render_idle(skip_knobs, script, 4000, 64, VoiceBankIdleSkip::SkipInactiveSlots);
    CHECK(with.skipped > 0);
    CHECK_FALSE(same_bytes(without.audio, with.audio));
}

TEST_CASE("Idle skip waits for the declared latency plus tail and skips on the exact next block",
          "[host][graph][voice-bank][idle-skip]") {
    constexpr std::size_t kBlock = 64;
    IdleKnobs knobs;
    knobs.latency_samples = 96;
    knobs.tail_samples = 96;
    // Hold is 192 frames: silent blocks 0, 1 and 2 reach 192 and confirm, so
    // block 3 is the first one skipped (eight slots, nothing playing).
    const auto result =
        render_idle(knobs, {}, 8 * kBlock, kBlock, VoiceBankIdleSkip::SkipInactiveSlots);
    for (std::size_t block = 0; block < 8; ++block) {
        INFO("block=" << block);
        CHECK(result.skipped_after_block[block] == (block < 3 ? 0u : 8u * (block - 2)));
    }
    for (const auto calls : knobs.calls)
        CHECK(calls == 3);
}

TEST_CASE("Idle skip confirmation restarts when a click lands in its last block",
          "[host][graph][voice-bank][idle-skip]") {
    constexpr std::size_t kBlock = 64;
    constexpr std::size_t kTotal = 20 * kBlock;
    // The voice is inactive from sample 68 and, with a 300 sample tail, would be
    // confirmed silent at the end of block 6 (samples 384 to 447). Its click
    // arrives on sample 447, the very last sample of that window. The count
    // restarts, the click is rendered, and the slot is first skipped in block 12.
    // A confirmation that ended one block early would skip block 6 and lose it.
    const std::vector<ScriptedEvent> script{on(0, 1, 69, 127), off(64, 1, 69)};
    IdleKnobs off_knobs;
    off_knobs.tail_samples = 300;
    off_knobs.idle_output = IdleOutput::DelayedClick;
    off_knobs.click_delay = 380;
    auto skip_knobs = off_knobs;
    const auto without = render_idle(off_knobs, script, kTotal, kBlock, VoiceBankIdleSkip::Off);
    const auto with =
        render_idle(skip_knobs, script, kTotal, kBlock, VoiceBankIdleSkip::SkipInactiveSlots);
    CHECK(with.audio.left[447] != 0.0f);
    CHECK(same_bytes(without.audio, with.audio));
    // The seven slots that never played are skipped from block 5. Slot 0 is
    // processed through block 11 and skipped from block 12.
    CHECK(with.skipped_after_block[11] == 7 * 7);
    CHECK(with.skipped_after_block[12] == 7 * 8 + 1);
    CHECK(with.calls_after_block[11][0] == 12);
    CHECK(with.calls_after_block[19][0] == 12);
}

TEST_CASE("Idle skip stays identical on one bank across changing block sizes and activation",
          "[host][graph][voice-bank][idle-skip]") {
    const std::array<std::size_t, 9> cycle{64, 1, 37, 256, 5, 128, 3, 200, 17};
    const std::vector<ScriptedEvent> script{
        on(100, 1, 69, 127), off(900, 1, 69), on(1500, 2, 60), on(1510, 3, 64),
        // activate() runs at sample 2000 while these are still sounding.
        on(2400, 4, 62), off(3000, 4, 62), on(3500, 5, 67)};
    constexpr std::size_t kTotal = 5000;
    constexpr std::size_t kActivateAt = 2000;
    std::array<IdleRender, 2> renders;
    for (const bool skip : {false, true}) {
        IdleKnobs knobs;
        knobs.release_frames = 150;
        VoiceBankBindingResult status;
        auto bank = make_idle_bank(
            knobs, skip ? VoiceBankIdleSkip::SkipInactiveSlots : VoiceBankIdleSkip::Off, status);
        REQUIRE(bank);
        auto& result = renders[skip ? 1 : 0];
        std::uint64_t sequence = 1;
        std::size_t index = 0;
        for (std::size_t start = 0; start < kTotal;) {
            if (start == kActivateAt) {
                VoiceBankBindingResult activation;
                REQUIRE(bank->activate(activation));
            }
            auto frames = std::min(cycle[index++ % cycle.size()], kTotal - start);
            if (start < kActivateAt)
                frames = std::min(frames, kActivateAt - start);
            REQUIRE(bank->begin_block(start, frames) == HostEventResult::Accepted);
            for (const auto& scripted : script)
                if (scripted.sample >= start && scripted.sample < start + frames)
                    REQUIRE(bank->publish(event(bank->epoch(), scripted.sample, sequence++,
                                                scripted.message)) == HostEventResult::Accepted);
            audio::Buffer<float> buffer(2, frames);
            auto view = buffer.view();
            REQUIRE(bank->render(view, start, frames) == VoiceBankBindingCode::Ok);
            result.audio.left.insert(result.audio.left.end(), view.channel(0).begin(),
                                     view.channel(0).end());
            result.audio.right.insert(result.audio.right.end(), view.channel(1).begin(),
                                      view.channel(1).end());
            start += frames;
        }
        result.skipped = bank->idle_skipped_blocks();
    }
    CHECK(renders[0].skipped == 0);
    CHECK(renders[1].skipped > 0);
    CHECK(same_bytes(renders[0].audio, renders[1].audio));
}

TEST_CASE("Activating a bank whose slots are all idle makes each slot render once before skipping",
          "[host][graph][voice-bank][idle-skip]") {
    constexpr std::size_t kBlock = 64;
    IdleKnobs knobs;
    const std::vector<ScriptedEvent> script{on(0, 1, 69, 127), on(0, 2, 64, 100), off(64, 1, 69),
                                            off(64, 2, 64)};
    // Blocks 0 to 9 let every slot go idle and be skipped; the bank is
    // reactivated before block 10.
    const auto result =
        render_idle(knobs, script, 12 * kBlock, kBlock, VoiceBankIdleSkip::SkipInactiveSlots,
                    midi::MpeVoiceStealMode::Oldest, [](std::size_t block, VoiceBankBinding& bank) {
                        if (block == 10) {
                            VoiceBankBindingResult activation;
                            REQUIRE(bank.activate(activation));
                        }
                    });
    // Before activation every slot is being skipped.
    CHECK(result.skipped_after_block[9] == result.skipped_after_block[8] + 8);
    // The reactivated block renders each slot once and skips none.
    CHECK(result.skipped_after_block[10] == result.skipped_after_block[9]);
    for (std::size_t slot = 0; slot < 8; ++slot) {
        INFO("slot=" << slot);
        CHECK(result.calls_after_block[10][slot] == result.calls_after_block[9][slot] + 1);
        // The next block is skipped again.
        CHECK(result.calls_after_block[11][slot] == result.calls_after_block[10][slot]);
    }
    CHECK(result.skipped_after_block[11] == result.skipped_after_block[10] + 8);
}
