#include <catch2/catch_test_macros.hpp>

#include "../examples/g6-voice-bank-consumer/fixture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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
    CHECK(std::abs(scalar.left[3] + 0.125f) <= 2e-6f);
    // Independent oscillator/envelope reference, derived from MIDI note 69,
    // phase zero and the frozen four-frame release at absolute sample six.
    double phase = 0.0;
    for (std::size_t i = 3; i < 10; ++i) {
        const float triangle = phase < 0.5 ? static_cast<float>(4.0 * phase - 1.0)
                                           : static_cast<float>(3.0 - 4.0 * phase);
        const float release = i < 6 ? 1.0f : (4.0f - static_cast<float>(i - 6)) * 0.25f;
        CHECK(std::abs(scalar.left[i] - triangle * release * 0.125f) <= 2e-6f);
        phase += 440.0 / 48000.0;
    }
    for (std::size_t i = 10; i < scalar.left.size(); ++i)
        CHECK(scalar.left[i] == 0.0f);
    for (std::size_t i = 0; i < scalar.left.size(); ++i)
        CHECK(scalar.left[i] == scalar.right[i]);
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
            sum += (triangle * velocity) * 0.125f;
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
                CHECK(view.channel(0)[i] == view.channel(1)[i]);
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
    CHECK(baseline == -0.125f);
    CHECK(first_sample(1, 2.0f) == baseline);
    CHECK(first_sample(0, 2.0f) == baseline * 2.0f);
}
