// Voice bank retirement policy (HardCut default, opt-in DrainRetired) and the
// documented sustain rules it interacts with, through the public binding.
#include <catch2/catch_test_macros.hpp>

#include "../examples/g6-voice-bank-consumer/fixture.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

namespace {
using namespace pulp;
using namespace pulp::host;
using pulp::examples::g6::definition;
using pulp::examples::g6::event;
using pulp::examples::g6::schema;

// A voice whose output names its note: held it plays note/128 on the left and
// half that on the right; releasing it decays linearly. Notes below 100 have a
// release far longer than any retirement tail used here, so a drain can only
// end by being cut; notes from 100 up release in three frames.
class DrainVoice final : public midi::MpeSynthVoice {
  public:
    static constexpr unsigned kLongRelease = 4000;
    static constexpr unsigned kShortRelease = 3;
    void on_note_on(const midi::MpeNoteState& note) override {
        midi::MpeSynthVoice::on_note_on(note);
        release_frame_ = 0;
        set_smoothing(0.0f);
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
    void render_stereo(float* left, float* right, int frames) {
        for (int i = 0; i < frames; ++i) {
            float sample = 0.0f;
            if (active()) {
                advance_smoothers();
                const unsigned length = note_number() >= 100 ? kShortRelease : kLongRelease;
                const float level = static_cast<float>(note_number()) / 128.0f;
                const float factor = releasing() ? 1.0f - static_cast<float>(release_frame_) /
                                                              static_cast<float>(length)
                                                 : 1.0f;
                sample = level * factor;
                if (releasing() && ++release_frame_ == length)
                    finish_release();
            }
            left[i] = sample;
            if (right)
                right[i] = 0.5f * sample;
        }
    }

  private:
    unsigned release_frame_ = 0;
};

class DrainProcessor final : public format::Processor {
  public:
    explicit DrainProcessor(DrainVoice& voice) : voice_(voice) {}
    format::PluginDescriptor descriptor() const override {
        format::PluginDescriptor d;
        d.name = "Drain Voice";
        d.manufacturer = "Pulp Test";
        d.bundle_id = "com.pulp.test.drain-voice";
        d.version = "1.0.0";
        d.category = format::PluginCategory::Instrument;
        d.output_buses = {{"Stereo", 2, false}};
        return d;
    }
    void define_parameters(state::StateStore& store) override {
        store.add_parameter(
            {.id = 1, .name = "level", .range = state::ParamRange::linear(0.0f, 2.0f, 1.0f)});
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        voice_.render_stereo(output.channel_ptr(0), output.channel_ptr(1),
                             static_cast<int>(output.num_samples()));
    }

  private:
    DrainVoice& voice_;
};

using DrainOwner = MpeVoiceBankOwner<DrainVoice>;

std::unique_ptr<VoiceBankBinding> make_bank() {
    auto voices = std::make_unique<DrainOwner>(
        [](DrainVoice& voice, std::uint32_t) -> std::unique_ptr<format::Processor> {
            return std::make_unique<DrainProcessor>(voice);
        },
        std::array<VoiceBankInputEndpoint, 4>{
            {{"note_in", 0}, {"pitch", 1}, {"pressure", 2}, {"timbre", 3}}});
    VoiceBankBindingResult status;
    auto bank = VoiceBankBinding::prepare(schema(), definition(), std::move(voices), "audio_out",
                                          48000, 256, status);
    INFO("prepare code=" << static_cast<int>(status.code) << " detail=" << status.detail);
    REQUIRE(bank);
    return bank;
}

const DrainOwner& voices(const VoiceBankBinding& bank) {
    return static_cast<const DrainOwner&>(bank.owner());
}

VoiceBankRetirementPolicy drain(std::uint32_t capacity, std::uint32_t tail) {
    return {VoiceBankRetirement::DrainRetired, capacity, tail};
}

void set_policy(VoiceBankBinding& bank, const VoiceBankRetirementPolicy& policy) {
    VoiceBankBindingResult result;
    INFO("policy refusal: " << result.detail);
    REQUIRE(bank.set_retirement_policy(policy, result));
}

void reset(VoiceBankBinding& bank) {
    std::uint64_t epoch = 0;
    REQUIRE(bank.reserve_event_reset(epoch) == HostEventResult::Accepted);
    REQUIRE(bank.commit_event_reset(epoch) == HostEventResult::Accepted);
}

struct Timed {
    std::uint64_t sample;
    midi::MidiEvent message;
};

// Render [start, end) in blocks cut by the cyclic `pattern`, publishing each
// timed event in its block, and append the left channel to `left`.
void play(VoiceBankBinding& bank, std::uint64_t start, std::uint64_t end,
          const std::vector<std::size_t>& pattern, const std::vector<Timed>& events,
          std::vector<float>& left, std::vector<float>* right = nullptr) {
    std::size_t turn = 0;
    std::uint64_t sequence = 1;
    for (auto at = start; at < end;) {
        const auto frames = static_cast<std::size_t>(
            std::min<std::uint64_t>(pattern[turn++ % pattern.size()], end - at));
        REQUIRE(bank.begin_block(at, frames) == HostEventResult::Accepted);
        for (const auto& timed : events)
            if (timed.sample >= at && timed.sample < at + frames)
                REQUIRE(bank.publish(event(bank.epoch(), timed.sample, sequence++,
                                           timed.message)) == HostEventResult::Accepted);
        audio::Buffer<float> buffer(2, frames);
        auto view = buffer.view();
        REQUIRE(bank.render(view, at, frames) == VoiceBankBindingCode::Ok);
        left.insert(left.end(), view.channel(0).begin(), view.channel(0).end());
        if (right)
            right->insert(right->end(), view.channel(1).begin(), view.channel(1).end());
        at += frames;
    }
}

// Before the reset at kReset: a held note, an already releasing note, two
// pedal-held notes and a held short-release note. After it, in the new epoch:
// two notes and a pedal press and lift that must not touch retired voices.
constexpr std::uint64_t kReset = 64;
constexpr std::uint64_t kEnd = 240;
constexpr std::uint32_t kTail = 37;

const std::vector<Timed>& before_reset() {
    static const std::vector<Timed> events = {
        {3, midi::MidiEvent::note_on(1, 60, 100)},   {5, midi::MidiEvent::note_on(5, 66, 100)},
        {10, midi::MidiEvent::note_on(2, 62, 100)},  {15, midi::MidiEvent::note_off(5, 66)},
        {20, midi::MidiEvent::note_on(3, 64, 100)},  {25, midi::MidiEvent::cc(0, 64, 127)},
        {30, midi::MidiEvent::note_off(2, 62)},      {40, midi::MidiEvent::note_off(3, 64)},
        {50, midi::MidiEvent::note_on(4, 101, 100)},
    };
    return events;
}

const std::vector<Timed>& after_reset() {
    static const std::vector<Timed> events = {
        {70, midi::MidiEvent::note_on(6, 70, 100)},
        {80, midi::MidiEvent::cc(0, 64, 127)},
        {90, midi::MidiEvent::note_on(7, 72, 100)},
        {120, midi::MidiEvent::cc(0, 64, 0)},
    };
    return events;
}

struct Run {
    std::vector<float> left;
    VoiceBankRetirementStats stats;
};

Run run(const std::vector<std::size_t>& pattern, const VoiceBankRetirementPolicy* policy) {
    auto bank = make_bank();
    if (policy)
        set_policy(*bank, *policy);
    Run result;
    play(*bank, 0, kReset, pattern, before_reset(), result.left);
    reset(*bank);
    play(*bank, kReset, kEnd, pattern, after_reset(), result.left);
    result.stats = bank->retirement_stats();
    return result;
}

// Contribution of one retired long-release note: it was released at
// `released` (the reset, or its own note-off) and is heard until the cut.
float retired_long(std::uint8_t note, std::uint64_t released, std::uint64_t sample) {
    return static_cast<float>(note) / 128.0f *
           (1.0f -
            static_cast<float>(sample - released) / static_cast<float>(DrainVoice::kLongRelease));
}
} // namespace

TEST_CASE("Voice bank retirement defaults to HardCut", "[host][graph][voice-bank][retirement]") {
    auto bank = make_bank();
    const auto policy = bank->retirement_policy();
    CHECK(policy.mode == VoiceBankRetirement::HardCut);
    CHECK(policy.capacity == 0);
    CHECK(policy.tail_frames == 0);

    // The unconfigured bank and one explicitly set to HardCut render the same
    // samples, and every old-epoch voice is silent from the reset frame on.
    const auto implicit = run({16}, nullptr);
    const VoiceBankRetirementPolicy hard{};
    const auto explicit_hard = run({16}, &hard);
    REQUIRE(implicit.left.size() == kEnd);
    CHECK(implicit.left == explicit_hard.left);
    for (std::uint64_t i = kReset; i < 70; ++i)
        CHECK(implicit.left[i] == 0.0f);
    CHECK(implicit.left[70] == 70.0f / 128.0f);
    CHECK(implicit.stats.drained == 0);
    CHECK(implicit.stats.refused == 0);
    CHECK(implicit.stats.draining == 0);

    // Positive control: the same script before the reset is far from silent.
    CHECK(implicit.left[kReset - 1] > 1.0f);
}

TEST_CASE("DrainRetired drains retired voices for exactly the tail across partitions",
          "[host][graph][voice-bank][retirement][audio]") {
    const auto policy = drain(kVoiceBankSlots, kTail);
    const auto reference = run({1}, &policy);
    REQUIRE(reference.left.size() == kEnd);
    for (const auto& pattern :
         std::vector<std::vector<std::size_t>>{{7}, {16}, {32}, {64}, {13, 5, 29}, {3, 61, 2}}) {
        const auto candidate = run(pattern, &policy);
        REQUIRE(candidate.left.size() == reference.left.size());
        for (std::size_t i = 0; i < reference.left.size(); ++i) {
            INFO("pattern[0]=" << pattern[0] << " sample=" << i);
            CHECK(std::abs(candidate.left[i] - reference.left[i]) <= 1e-6f);
        }
        CHECK(candidate.stats.drained == reference.stats.drained);
        CHECK(candidate.stats.cut_at_tail == reference.stats.cut_at_tail);
        CHECK(candidate.stats.finished == reference.stats.finished);
    }

    // Independent expectation for the frames the drain covers, built from the
    // voice's own law: four long-release notes, each released at the reset
    // except note 66 (released at its own note-off), plus the short note 101,
    // which finishes in three frames.
    const auto hard = run({1}, nullptr);
    for (std::uint64_t i = kReset; i < kReset + kTail; ++i) {
        float expected = retired_long(60, kReset, i) + retired_long(62, kReset, i) +
                         retired_long(64, kReset, i) + retired_long(66, 15, i);
        if (i - kReset < DrainVoice::kShortRelease)
            expected += 101.0f / 128.0f *
                        (1.0f - static_cast<float>(i - kReset) /
                                    static_cast<float>(DrainVoice::kShortRelease));
        INFO("sample=" << i);
        CHECK(std::abs(reference.left[i] - (hard.left[i] + expected)) <= 1e-5f);
    }
    // From the deadline on, only the new epoch sounds: the drain run matches
    // HardCut, which never heard the retired voices.
    for (std::uint64_t i = kReset + kTail; i < kEnd; ++i) {
        INFO("sample=" << i);
        CHECK(std::abs(reference.left[i] - hard.left[i]) <= 1e-6f);
    }
    // Negative control: one frame before the deadline the drain is audible.
    CHECK(reference.left[kReset + kTail - 1] - hard.left[kReset + kTail - 1] > 1.0f);

    CHECK(reference.stats.drained == 5);
    CHECK(reference.stats.finished == 1);
    CHECK(reference.stats.cut_at_tail == 4);
    CHECK(reference.stats.refused == 0);
    CHECK(reference.stats.interrupted == 0);
    CHECK(reference.stats.draining == 0);
}

TEST_CASE("DrainRetired refuses drains beyond its capacity and cuts those voices",
          "[host][graph][voice-bank][retirement]") {
    // Five voices sound at the reset; capacity two drains slots 0 and 1 (notes
    // 60 and 66) and cuts the other three on the reset.
    const auto policy = drain(2, kTail);
    const auto limited = run({16}, &policy);
    const auto hard = run({16}, nullptr);
    CHECK(limited.stats.drained == 2);
    CHECK(limited.stats.refused == 3);
    for (std::uint64_t i = kReset; i < kReset + kTail; ++i) {
        const float expected = retired_long(60, kReset, i) + retired_long(66, 15, i);
        INFO("sample=" << i);
        CHECK(std::abs(limited.left[i] - (hard.left[i] + expected)) <= 1e-5f);
    }

    // A second reset while both drains run: they keep their first deadline
    // and fill the capacity, so the new epoch's sounding note is refused.
    auto bank = make_bank();
    set_policy(*bank, policy);
    std::vector<float> left;
    play(*bank, 0, kReset, {16}, before_reset(), left);
    reset(*bank);
    play(*bank, kReset, 80, {16}, {{70, midi::MidiEvent::note_on(6, 70, 100)}}, left);
    reset(*bank);
    CHECK(bank->retirement_stats().drained == 2);
    CHECK(bank->retirement_stats().refused == 4);
    CHECK(bank->retirement_stats().draining == 2);
    play(*bank, 80, kEnd, {16}, {}, left);
    CHECK(std::abs(left[kReset + kTail - 1] - (retired_long(60, kReset, kReset + kTail - 1) +
                                               retired_long(66, 15, kReset + kTail - 1))) <= 1e-5f);
    for (std::uint64_t i = kReset + kTail; i < kEnd; ++i)
        CHECK(left[i] == 0.0f);
    CHECK(bank->retirement_stats().cut_at_tail == 2);
}

TEST_CASE("Retirement policy refuses out-of-range settings and changes while draining",
          "[host][graph][voice-bank][retirement]") {
    auto bank = make_bank();
    VoiceBankBindingResult result;
    const VoiceBankRetirementPolicy refused[] = {
        drain(0, kTail),
        drain(kVoiceBankSlots + 1, kTail),
        drain(1, 0),
        drain(1, kVoiceBankMaxRetirementTailFrames + 1),
        {VoiceBankRetirement::HardCut, 1, 0},
        {VoiceBankRetirement::HardCut, 0, 1},
        {static_cast<VoiceBankRetirement>(7), 1, 1},
    };
    for (const auto& policy : refused) {
        CHECK_FALSE(bank->set_retirement_policy(policy, result));
        CHECK(result.code == VoiceBankBindingCode::InvalidState);
        CHECK(bank->retirement_policy().mode == VoiceBankRetirement::HardCut);
    }
    // Boundaries are accepted.
    CHECK(bank->set_retirement_policy(drain(kVoiceBankSlots, kVoiceBankMaxRetirementTailFrames),
                                      result));
    CHECK(bank->set_retirement_policy(drain(1, 1), result));

    REQUIRE(bank->begin_block(0, 4) == HostEventResult::Accepted);
    CHECK_FALSE(bank->set_retirement_policy({}, result));
    CHECK(result.code == VoiceBankBindingCode::InvalidBlock);
    audio::Buffer<float> buffer(2, 4);
    auto view = buffer.view();
    REQUIRE(bank->render(view, 0, 4) == VoiceBankBindingCode::Ok);

    set_policy(*bank, drain(4, kTail));
    std::vector<float> left;
    play(*bank, 4, 8, {4}, {{4, midi::MidiEvent::note_on(1, 60, 100)}}, left);
    reset(*bank);
    REQUIRE(bank->retirement_stats().draining == 1);
    CHECK_FALSE(bank->set_retirement_policy({}, result));
    CHECK(result.code == VoiceBankBindingCode::InvalidState);
    CHECK(bank->retirement_policy().mode == VoiceBankRetirement::DrainRetired);
    play(*bank, 8, 8 + kTail, {16}, {}, left);
    CHECK(bank->retirement_stats().draining == 0);
    CHECK(bank->set_retirement_policy({}, result));
    CHECK(bank->retirement_policy().mode == VoiceBankRetirement::HardCut);
}

TEST_CASE("A retired voice answers to no epoch's tokens, pedal or CC121",
          "[host][graph][voice-bank][retirement][pedal]") {
    auto bank = make_bank();
    set_policy(*bank, drain(kVoiceBankSlots, kTail));
    std::vector<float> left;
    play(*bank, 0, 4, {4},
         {{0, midi::MidiEvent::note_on(1, 60, 100)}, {1, midi::MidiEvent::pitch_bend(1, 12288)}},
         left);
    const auto old_epoch = bank->epoch();
    midi::MpeNoteState note{};
    bool releasing = false;
    REQUIRE(voices(*bank).slot_note(0, note, releasing));
    note.pitch_bend_semitones = 1.0f;
    // Positive control: before the reset the token addresses the voice.
    REQUIRE(bank->dispatch_deferred(old_epoch, 0, note.note_id,
                                    midi::MpeExpressionEvent::Kind::PitchBend,
                                    note) == VoiceBankBindingCode::Ok);
    reset(*bank);
    REQUIRE(bank->retirement_stats().draining == 1);
    CHECK(bank->dispatch_deferred(old_epoch, 0, note.note_id,
                                  midi::MpeExpressionEvent::Kind::PitchBend,
                                  note) == VoiceBankBindingCode::StaleEpoch);
    // The same generation re-stamped with the current epoch is still refused.
    CHECK(bank->dispatch_deferred(bank->epoch(), 0, note.note_id,
                                  midi::MpeExpressionEvent::Kind::PitchBend,
                                  note) == VoiceBankBindingCode::StaleGeneration);

    // A new-epoch note in another slot is addressable (positive control).
    play(*bank, 4, 5, {1}, {{4, midi::MidiEvent::note_on(2, 62, 100)}}, left);
    midi::MpeNoteState fresh{};
    REQUIRE(voices(*bank).slot_note(1, fresh, releasing));
    CHECK(bank->dispatch_deferred(bank->epoch(), 1, fresh.note_id,
                                  midi::MpeExpressionEvent::Kind::Pressure,
                                  fresh) == VoiceBankBindingCode::Ok);

    // CC121 on the retired voice's member channel resets the new epoch's
    // expression only; the retired voice keeps its bend.
    const float bend = voices(*bank).allocator().voice(0).pitch_bend();
    REQUIRE(bend > 0.0f);
    play(*bank, 5, 6, {1}, {{5, midi::MidiEvent::cc(1, 121, 0)}}, left);
    CHECK(voices(*bank).allocator().voice(0).pitch_bend() == bend);
    // A new-epoch pedal press and lift does not hold or release it either.
    play(*bank, 6, 8, {1},
         {{6, midi::MidiEvent::cc(0, 64, 127)}, {7, midi::MidiEvent::cc(0, 64, 0)}}, left);
    CHECK(voices(*bank).allocator().voice(0).releasing());
    CHECK(voices(*bank).allocator().voice(0).note_id() == note.note_id);
    CHECK(bank->retirement_stats().draining == 1);
}

TEST_CASE("All Sound Off and a steal end a drain early",
          "[host][graph][voice-bank][retirement][pedal]") {
    {
        auto bank = make_bank();
        set_policy(*bank, drain(kVoiceBankSlots, kTail));
        std::vector<float> left;
        play(*bank, 0, 4, {4}, {{0, midi::MidiEvent::note_on(1, 60, 100)}}, left);
        reset(*bank);
        play(*bank, 4, 6, {1}, {{5, midi::MidiEvent::cc(1, 120, 0)}}, left);
        CHECK(left[4] > 0.0f);
        CHECK(left[5] == 0.0f);
        CHECK_FALSE(voices(*bank).allocator().voice(0).active());
        CHECK(bank->retirement_stats().interrupted == 1);
        CHECK(bank->retirement_stats().draining == 0);
    }
    {
        auto bank = make_bank();
        set_policy(*bank, drain(kVoiceBankSlots, kTail));
        std::vector<Timed> chord;
        for (std::uint8_t i = 0; i < kVoiceBankSlots; ++i)
            chord.push_back({0, midi::MidiEvent::note_on(static_cast<std::uint8_t>(1 + i),
                                                         static_cast<std::uint8_t>(40 + i), 100)});
        std::vector<float> left;
        play(*bank, 0, 4, {4}, chord, left);
        reset(*bank);
        REQUIRE(bank->retirement_stats().draining == kVoiceBankSlots);
        // Every slot drains, so the new note steals the oldest (slot 0).
        play(*bank, 4, 5, {1}, {{4, midi::MidiEvent::note_on(9, 90, 100)}}, left);
        CHECK(bank->retirement_stats().interrupted == 1);
        CHECK(bank->retirement_stats().draining == kVoiceBankSlots - 1);
        CHECK(voices(*bank).allocator().voice(0).note_number() == 90);
        // The stolen slot's old deadline does not cut the note that replaced it.
        play(*bank, 5, 4 + kTail + 8, {16}, {}, left);
        CHECK(voices(*bank).allocator().voice(0).active());
        CHECK_FALSE(voices(*bank).allocator().voice(0).releasing());
        CHECK(bank->retirement_stats().cut_at_tail == kVoiceBankSlots - 1);
    }
}

TEST_CASE("CC121 after a same-sample steal resets the replacement's expression",
          "[host][graph][voice-bank][retirement][pedal]") {
    auto bank = make_bank();
    set_policy(*bank, drain(kVoiceBankSlots, kTail));
    std::vector<Timed> chord;
    for (std::uint8_t i = 0; i < kVoiceBankSlots; ++i)
        chord.push_back({0, midi::MidiEvent::note_on(static_cast<std::uint8_t>(1 + i),
                                                     static_cast<std::uint8_t>(40 + i), 100)});
    std::vector<float> left;
    play(*bank, 0, 4, {4}, chord, left);
    reset(*bank);
    REQUIRE(bank->retirement_stats().draining == kVoiceBankSlots);
    // Steal, bend, release and CC121 land on one sample, before any render
    // slice reconciles the stolen drain.
    play(*bank, 4, 6, {2},
         {{4, midi::MidiEvent::note_on(9, 90, 100)},
          {4, midi::MidiEvent::pitch_bend(9, 16000)},
          {4, midi::MidiEvent::note_off(9, 90)},
          {4, midi::MidiEvent::cc(9, 121, 0)}},
         left);
    const auto& voice = voices(*bank).allocator().voice(0);
    REQUIRE(voice.note_number() == 90);
    REQUIRE(voice.releasing());
    CHECK(voice.pitch_bend() == 0.0f);
    CHECK(bank->retirement_stats().interrupted == 1);
}

TEST_CASE("Activation retires voices under the same policy",
          "[host][graph][voice-bank][retirement]") {
    for (const bool drains : {false, true}) {
        auto bank = make_bank();
        if (drains)
            set_policy(*bank, drain(kVoiceBankSlots, kTail));
        std::vector<float> left;
        play(*bank, 0, 4, {4}, {{0, midi::MidiEvent::note_on(1, 60, 100)}}, left);
        VoiceBankBindingResult result;
        REQUIRE(bank->activate(result));
        play(*bank, 4, 4 + kTail + 2, {16}, {}, left);
        INFO("drains=" << drains);
        CHECK(((left[4] > 0.0f) == drains));
        CHECK(((left[4 + kTail - 1] > 0.0f) == drains));
        CHECK(left[4 + kTail] == 0.0f);
    }
}

TEST_CASE("Retired voices, deadlines and pedal holds are never persistent state",
          "[host][graph][voice-bank][retirement][state]") {
    auto bank = make_bank();
    set_policy(*bank, drain(kVoiceBankSlots, kTail));
    std::vector<float> left;
    play(*bank, 0, kReset, {16}, before_reset(), left);
    const auto before = bank->capture_state();
    reset(*bank);
    REQUIRE(bank->retirement_stats().draining == 5);
    const auto during = bank->capture_state();
    CHECK(during.manifest_sha256 == before.manifest_sha256);
    REQUIRE(during.parameters.size() == before.parameters.size());
    for (std::size_t i = 0; i < during.parameters.size(); ++i) {
        CHECK(during.parameters[i].slot_ordinal == before.parameters[i].slot_ordinal);
        CHECK(during.parameters[i].target == before.parameters[i].target);
        CHECK(during.parameters[i].value == before.parameters[i].value);
    }
    // Restoring onto a fresh bank revives nothing: no voice, no drain, and the
    // policy is the bank's own configuration, not part of the state.
    auto fresh = make_bank();
    VoiceBankBindingResult result;
    REQUIRE(fresh->restore_state(during, result));
    CHECK(fresh->retirement_stats().draining == 0);
    CHECK(fresh->retirement_policy().mode == VoiceBankRetirement::HardCut);
    CHECK(voices(*fresh).allocator().active_count() == 0);
}

TEST_CASE("Sustain holds a deferred note-off until the last covering pedal lifts",
          "[host][graph][voice-bank][pedal]") {
    for (const bool pedal : {false, true}) {
        auto bank = make_bank();
        std::vector<float> left;
        std::vector<Timed> start = {{0, midi::MidiEvent::note_on(1, 60, 100)}};
        if (pedal)
            start.push_back({0, midi::MidiEvent::cc(1, 64, 64)});
        play(*bank, 0, 2, {2}, start, left);
        midi::MpeNoteState note{};
        bool releasing = false;
        REQUIRE(voices(*bank).slot_note(0, note, releasing));
        REQUIRE(bank->dispatch_deferred(bank->epoch(), 0, note.note_id,
                                        midi::MpeExpressionEvent::Kind::NoteOff,
                                        note) == VoiceBankBindingCode::Ok);
        INFO("pedal=" << pedal);
        CHECK(voices(*bank).allocator().voice(0).releasing() != pedal);
        // A lift at 63 is below the threshold of 64, so it releases the hold.
        play(*bank, 2, 3, {1}, {{2, midi::MidiEvent::cc(1, 64, 63)}}, left);
        CHECK(voices(*bank).allocator().voice(0).releasing());
        CHECK(voices(*bank).allocator().voice(0).note_id() == note.note_id);
    }
}

TEST_CASE("Draining and cutting retired voices does not allocate",
          "[host][graph][voice-bank][retirement][rt-safety]") {
    REQUIRE(pulp::test::RtAllocationProbe::self_check());
    auto bank = make_bank();
    set_policy(*bank, drain(4, 11));
    std::vector<Timed> chord;
    for (std::uint8_t i = 0; i < 6; ++i)
        chord.push_back({static_cast<std::uint64_t>(i),
                         midi::MidiEvent::note_on(static_cast<std::uint8_t>(1 + i),
                                                  static_cast<std::uint8_t>(50 + i), 100)});
    audio::Buffer<float> buffer(2, 8);
    std::size_t allocations = 0;
    bool ok = true;
    {
        pulp::test::RtAllocationProbe probe;
        std::uint64_t sequence = 1;
        for (std::uint64_t block = 0; block < 12; ++block) {
            const auto at = block * 8;
            if (block == 2 || block == 3 || block == 7) {
                std::uint64_t epoch = 0;
                ok = bank->reserve_event_reset(epoch) == HostEventResult::Accepted && ok;
                ok = bank->commit_event_reset(epoch) == HostEventResult::Accepted && ok;
            }
            ok = bank->begin_block(at, 8) == HostEventResult::Accepted && ok;
            for (const auto& timed : chord)
                if (block == 0 || block == 4)
                    ok = bank->publish(event(bank->epoch(), at + timed.sample, sequence++,
                                             timed.message)) == HostEventResult::Accepted &&
                         ok;
            auto view = buffer.view();
            ok = bank->render(view, at, 8) == VoiceBankBindingCode::Ok && ok;
        }
        allocations = probe.allocation_count();
    }
    CHECK(ok);
    CHECK(allocations == 0);
    const auto stats = bank->retirement_stats();
    CHECK(stats.drained > 0);
    CHECK(stats.refused > 0);
    CHECK(stats.cut_at_tail > 0);
}
