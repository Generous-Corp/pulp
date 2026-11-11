#include "fixture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace pulp;
using namespace pulp::host;
using namespace pulp::examples::g6;

namespace {
void check(bool value, const char* rule) {
    if (!value)
        throw std::runtime_error(rule);
}

bool near(float actual, float expected) {
    return std::isfinite(actual) &&
           std::abs(actual - expected) <= 2e-6f + 2e-6f * std::abs(expected);
}

struct Stereo {
    std::vector<float> left;
    std::vector<float> right;
};

VoiceBankBindingCode step(VoiceBankBinding& bank, std::uint64_t sample,
                          std::initializer_list<midi::MidiEvent> messages);

Stereo render(std::size_t partition) {
    VoiceBankBindingResult status;
    auto bank = make_bank(schema(), status);
    check(static_cast<bool>(bank), "prepare-eight-processors");
    Stereo samples;
    for (std::size_t start = 0; start < 800; start += partition) {
        const auto frames = std::min(partition, std::size_t{800} - start);
        check(bank->begin_block(start, frames) == HostEventResult::Accepted, "begin-block");
        if (start <= 3 && 3 < start + frames)
            check(bank->publish(event(bank->epoch(), 3, 1, midi::MidiEvent::note_on(1, 69, 127))) ==
                      HostEventResult::Accepted,
                  "note-on");
        if (start <= 6 && 6 < start + frames)
            check(bank->publish(event(bank->epoch(), 6, 2, midi::MidiEvent::note_off(1, 69))) ==
                      HostEventResult::Accepted,
                  "note-off");
        audio::Buffer<float> buffer(2, frames);
        auto view = buffer.view();
        const auto render_code = bank->render(view, start, frames);
        if (render_code != VoiceBankBindingCode::Ok)
            throw std::runtime_error("render-code=" +
                                     std::to_string(static_cast<unsigned>(render_code)));
        for (std::size_t i = 0; i < frames; ++i) {
            check(std::isfinite(view.channel(0)[i]) && std::isfinite(view.channel(1)[i]),
                  "finite-audio");
            samples.left.push_back(view.channel(0)[i]);
            samples.right.push_back(view.channel(1)[i]);
        }
    }
    check(owner(*bank).allocator().active_count() == 0, "finite-release-reclaimed");
    return samples;
}

void check_steal_policies() {
    const std::array modes{midi::MpeVoiceStealMode::Oldest, midi::MpeVoiceStealMode::LowestVelocity,
                           midi::MpeVoiceStealMode::LowestPitch,
                           midi::MpeVoiceStealMode::HighestPitch};
    const std::array victims{0u, 7u, 0u, 7u};
    for (std::size_t policy = 0; policy < modes.size(); ++policy) {
        VoiceBankBindingResult status;
        auto bank = make_bank(schema(), status);
        check(static_cast<bool>(bank), "steal-prepare");
        bank->set_steal_mode(modes[policy]);
        check(bank->begin_block(0, 2) == HostEventResult::Accepted, "steal-begin");
        for (std::uint8_t slot = 0; slot < 8; ++slot)
            check(bank->publish(
                      event(bank->epoch(), 0, slot + 1,
                            midi::MidiEvent::note_on(slot + 1, 60 + slot, 100 - slot * 10))) ==
                      HostEventResult::Accepted,
                  "fill-bank");
        check(bank->publish(event(bank->epoch(), 1, 9, midi::MidiEvent::note_on(9, 90, 127))) ==
                  HostEventResult::Accepted,
              "ninth-note");
        audio::Buffer<float> buffer(2, 2);
        auto view = buffer.view();
        check(bank->render(view, 0, 2) == VoiceBankBindingCode::Ok, "steal-render");
        check(owner(*bank).allocator().voice(victims[policy]).note_number() == 90, "steal-victim");
        check(owner(*bank).allocator().steal_count() == 1, "steal-count");
        float expected_left = 0.0f;
        float expected_right = 0.0f;
        for (std::size_t slot = 0; slot < 8; ++slot) {
            float source = -1.0f;
            if (slot != victims[policy]) {
                const double phase =
                    440.0 * std::exp2((static_cast<double>(60 + slot) - 69.0) / 12.0) / 48000.0;
                source = static_cast<float>(4.0 * phase - 1.0) *
                         (static_cast<float>(100 - slot * 10) / 127.0f);
            }
            expected_left += source * kBankGain;
            expected_right += (source * 0.5f) * kBankGain;
        }
        check(near(view.channel(0)[1], expected_left), "steal-hard-no-tail-left");
        check(near(view.channel(1)[1], expected_right), "steal-hard-no-tail-right");
    }
}

void check_state_and_generation() {
    VoiceBankBindingResult status;
    auto bank = make_bank(schema(), status);
    check(static_cast<bool>(bank), "state-prepare");
    check(bank->set_slot_parameter(0, "level", 2.0f), "named-parameter-bind");
    check(!bank->set_slot_parameter(0, "missing", 1.0f), "unknown-parameter-refusal");
    check(bank->begin_block(0, 1) == HostEventResult::Accepted, "state-begin");
    check(bank->publish(event(bank->epoch(), 0, 1, midi::MidiEvent::note_on(1, 69, 127))) ==
              HostEventResult::Accepted,
          "state-note-on");
    audio::Buffer<float> buffer(2, 1);
    auto view = buffer.view();
    check(bank->render(view, 0, 1) == VoiceBankBindingCode::Ok, "state-render");
    check(std::abs(view.channel(0)[0] + 2.0f * kBankGain) <= 2e-6f, "slot-zero-level-audio");
    const auto generation = owner(*bank).allocator().voice(0).note_id();
    midi::MpeNoteState note{};
    note.note_id = generation;
    note.channel = 1;
    note.note = 69;
    note.velocity = 127;
    check(bank->dispatch_deferred(bank->epoch(), 0, generation + 1,
                                  midi::MpeExpressionEvent::Kind::NoteOff,
                                  note) == VoiceBankBindingCode::StaleGeneration,
          "stale-generation-refusal");
    auto forged = note;
    forged.channel = 2;
    check(bank->dispatch_deferred(bank->epoch(), 0, generation,
                                  midi::MpeExpressionEvent::Kind::NoteOff,
                                  forged) == VoiceBankBindingCode::StaleGeneration,
          "forged-channel-refusal");
    check(bank->activate(status), "state-activate");
    check(bank->dispatch_deferred(bank->epoch() - 1, 0, generation,
                                  midi::MpeExpressionEvent::Kind::NoteOff,
                                  note) == VoiceBankBindingCode::StaleEpoch,
          "stale-epoch-refusal");
}

void check_deferred_expression_validation() {
    struct Case {
        midi::MpeExpressionEvent::Kind kind;
        float value;
        bool valid;
    };
    const auto cases = std::array{
        Case{midi::MpeExpressionEvent::Kind::PitchBend, 1.0f, true},
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
        check(static_cast<bool>(bank), "deferred-expression-prepare");
        check(step(*bank, 0, {midi::MidiEvent::note_on(1, 69, 127)}) == VoiceBankBindingCode::Ok,
              "deferred-expression-note");
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
        check(result ==
                  (test.valid ? VoiceBankBindingCode::Ok : VoiceBankBindingCode::IngressFailure),
              test.valid ? "deferred-expression-accepted" : "deferred-expression-refusal");
        check(step(*bank, 1, {}) == VoiceBankBindingCode::Ok, "deferred-expression-render");
        const auto& voice = owner(*bank).allocator().voice(0);
        if (test.valid) {
            check(std::isfinite(voice.pitch_bend()) && std::isfinite(voice.pressure()) &&
                      std::isfinite(voice.timbre()),
                  "deferred-expression-finite");
        } else {
            check(voice.pitch_bend() == 0.0f && voice.pressure() == 0.0f && voice.timbre() == 0.0f,
                  "deferred-expression-atomic-refusal");
        }
    }
}

void check_capacity_and_poly_pressure() {
    VoiceBankBindingResult status;
    auto bank = make_bank(schema(), status);
    check(static_cast<bool>(bank), "capacity-prepare");
    for (std::uint32_t slot = 1; slot < 8; ++slot)
        check(bank->set_slot_parameter(slot, "level", 0.0f), "capacity-silent-slot");
    for (std::uint64_t sample = 0; sample < 128; ++sample) {
        check(bank->begin_block(sample, 1) == HostEventResult::Accepted, "capacity-begin");
        check(bank->publish(event(bank->epoch(), sample, 1,
                                  midi::MidiEvent::note_on(1, static_cast<std::uint8_t>(sample),
                                                           127))) == HostEventResult::Accepted,
              "capacity-fill");
        audio::Buffer<float> buffer(2, 1);
        auto view = buffer.view();
        check(bank->render(view, sample, 1) == VoiceBankBindingCode::Ok, "capacity-fill-render");
    }
    const auto generation = owner(*bank).allocator().voice(0).note_id();
    check(bank->begin_block(128, 1) == HostEventResult::Accepted, "capacity-overflow-begin");
    check(bank->publish(event(bank->epoch(), 128, 1, midi::MidiEvent::note_on(2, 0, 127))) ==
              HostEventResult::Accepted,
          "capacity-overflow-publish");
    audio::Buffer<float> overflow(2, 1);
    overflow.channel(0)[0] = overflow.channel(1)[0] = 7.0f;
    auto overflow_view = overflow.view();
    check(bank->render(overflow_view, 128, 1) == VoiceBankBindingCode::TrackerCapacity,
          "capacity-typed-refusal");
    check(overflow.channel(0)[0] == 7.0f && overflow.channel(1)[0] == 7.0f,
          "capacity-output-untouched");
    check(owner(*bank).allocator().voice(0).note_id() == generation,
          "capacity-generation-untouched");
    check(bank->begin_block(129, 1) == HostEventResult::Accepted, "capacity-release-begin");
    check(bank->publish(event(bank->epoch(), 129, 1, midi::MidiEvent::note_off(1, 0))) ==
              HostEventResult::Accepted,
          "capacity-release");
    audio::Buffer<float> released(2, 1);
    auto released_view = released.view();
    check(bank->render(released_view, 129, 1) == VoiceBankBindingCode::Ok,
          "capacity-release-render");
    check(bank->begin_block(130, 1) == HostEventResult::Accepted, "capacity-retry-begin");
    check(bank->publish(event(bank->epoch(), 130, 1, midi::MidiEvent::note_on(2, 0, 127))) ==
              HostEventResult::Accepted,
          "capacity-retry-publish");
    audio::Buffer<float> resumed(2, 1);
    auto resumed_view = resumed.view();
    check(bank->render(resumed_view, 130, 1) == VoiceBankBindingCode::Ok, "capacity-retry-render");
    check(owner(*bank).allocator().voice(0).channel() == 2 &&
              owner(*bank).allocator().voice(0).note_number() == 0 &&
              owner(*bank).allocator().voice(0).note_id() == 129 &&
              resumed.channel(0)[0] == -kBankGain,
          "capacity-recovered-audio");
    check(bank->begin_block(131, 1) == HostEventResult::Accepted, "capacity-cc120-begin");
    check(bank->publish(event(bank->epoch(), 131, 1, midi::MidiEvent::cc(2, 120, 0))) ==
              HostEventResult::Accepted,
          "capacity-cc120-publish");
    check(bank->publish(event(bank->epoch(), 131, 2, midi::MidiEvent::note_on(2, 1, 127))) ==
              HostEventResult::Accepted,
          "capacity-cc120-restart");
    audio::Buffer<float> cc120_audio(2, 1);
    auto cc120_view = cc120_audio.view();
    check(bank->render(cc120_view, 131, 1) == VoiceBankBindingCode::Ok, "capacity-cc120-reuse");
    check(bank->begin_block(132, 1) == HostEventResult::Accepted, "capacity-cc123-begin");
    check(bank->publish(event(bank->epoch(), 132, 1, midi::MidiEvent::cc(0, 123, 0))) ==
              HostEventResult::Accepted,
          "capacity-cc123-publish");
    check(bank->publish(event(bank->epoch(), 132, 2, midi::MidiEvent::note_on(1, 0, 127))) ==
              HostEventResult::Accepted,
          "capacity-cc123-restart");
    audio::Buffer<float> cc123_audio(2, 1);
    auto cc123_view = cc123_audio.view();
    check(bank->render(cc123_view, 132, 1) == VoiceBankBindingCode::Ok, "capacity-cc123-reuse");

    auto pressure = make_bank(schema(), status);
    check(static_cast<bool>(pressure), "pressure-prepare");
    check(pressure->begin_block(0, 1) == HostEventResult::Accepted, "pressure-begin");
    check(pressure->publish(event(pressure->epoch(), 0, 1, midi::MidiEvent::note_on(1, 69, 127))) ==
              HostEventResult::Accepted,
          "pressure-note");
    check(pressure->publish(
              event(pressure->epoch(), 0, 2, midi::MidiEvent::poly_pressure(1, 69, 127))) ==
              HostEventResult::Accepted,
          "poly-pressure-publish");
    audio::Buffer<float> refused(2, 1);
    refused.channel(0)[0] = refused.channel(1)[0] = 7.0f;
    auto refused_view = refused.view();
    check(pressure->render(refused_view, 0, 1) == VoiceBankBindingCode::IngressFailure,
          "poly-pressure-typed-refusal");
    check(owner(*pressure).allocator().active_count() == 0 && refused.channel(0)[0] == 7.0f &&
              refused.channel(1)[0] == 7.0f,
          "poly-pressure-atomic-refusal");
}

void check_eight_voice_sum() {
    for (const auto partition : {1u, 32u, 64u, 127u, 256u}) {
        VoiceBankBindingResult status;
        auto bank = make_bank(schema(), status);
        check(static_cast<bool>(bank), "sum-prepare");
        std::array<double, 8> phase{};
        std::size_t sample = 0;
        while (sample < 512) {
            const auto frames = std::min<std::size_t>(partition, 512 - sample);
            check(bank->begin_block(sample, frames) == HostEventResult::Accepted, "sum-begin");
            if (sample == 0)
                for (std::uint8_t slot = 0; slot < 8; ++slot)
                    check(bank->publish(event(
                              bank->epoch(), 0, slot + 1,
                              midi::MidiEvent::note_on(slot + 1, 60 + slot, 100 - slot * 10))) ==
                              HostEventResult::Accepted,
                          "sum-note-on");
            audio::Buffer<float> buffer(2, frames);
            auto view = buffer.view();
            check(bank->render(view, sample, frames) == VoiceBankBindingCode::Ok, "sum-render");
            for (std::size_t frame = 0; frame < frames; ++frame) {
                float expected_left = 0.0f;
                float expected_right = 0.0f;
                for (std::size_t slot = 0; slot < 8; ++slot) {
                    const float triangle = phase[slot] < 0.5
                                               ? static_cast<float>(4.0 * phase[slot] - 1.0)
                                               : static_cast<float>(3.0 - 4.0 * phase[slot]);
                    const float source = triangle * (static_cast<float>(100 - slot * 10) / 127.0f);
                    expected_left += source * kBankGain;
                    expected_right += (source * 0.5f) * kBankGain;
                    phase[slot] +=
                        440.0 * std::exp2((static_cast<double>(60 + slot) - 69.0) / 12.0) / 48000.0;
                    if (phase[slot] >= 1.0)
                        phase[slot] -= 1.0;
                }
                check(near(view.channel(0)[frame], expected_left), "ordered-left-sum");
                check(near(view.channel(1)[frame], expected_right), "ordered-right-sum");
            }
            sample += frames;
        }
    }
}

std::array<float, 2> expression_sample(std::optional<midi::MidiEvent> change) {
    VoiceBankBindingResult status;
    auto bank = make_bank(schema(), status);
    check(static_cast<bool>(bank), "expression-prepare");
    std::array<float, 2> observed{};
    for (std::uint64_t sample = 0; sample < 3; ++sample) {
        check(bank->begin_block(sample, 1) == HostEventResult::Accepted, "expression-begin");
        if (sample == 0)
            check(bank->publish(
                      event(bank->epoch(), sample, 1, midi::MidiEvent::note_on(1, 69, 127))) ==
                      HostEventResult::Accepted,
                  "expression-note");
        if (sample == 1 && change)
            check(bank->publish(event(bank->epoch(), sample, 1, *change)) ==
                      HostEventResult::Accepted,
                  "expression-change");
        audio::Buffer<float> buffer(2, 1);
        auto view = buffer.view();
        check(bank->render(view, sample, 1) == VoiceBankBindingCode::Ok, "expression-render");
        if (sample == 2)
            observed = {view.channel(0)[0], view.channel(1)[0]};
    }
    return observed;
}

void check_expression_audio() {
    const auto baseline = expression_sample(std::nullopt);
    const auto bend = expression_sample(midi::MidiEvent::pitch_bend(1, 16383));
    const auto pressure =
        expression_sample(midi::MidiEvent{choc::midi::ShortMessage(0xd1, 127, 0), 0, 0.0});
    const auto timbre = expression_sample(midi::MidiEvent::cc(1, 74, 127));
    check(std::abs(bend[0] - baseline[0]) > 1e-4f, "audible-pitch");
    check(std::abs(pressure[0] - baseline[0]) > 1e-3f, "audible-pressure");
    check(std::abs(timbre[1] - baseline[1]) > 1e-3f, "audible-timbre");
    check(near(pressure[1], baseline[1]), "pressure-channel-independence");
    check(near(timbre[0], baseline[0]), "timbre-channel-independence");
}

VoiceBankBindingCode step(VoiceBankBinding& bank, std::uint64_t sample,
                          std::initializer_list<midi::MidiEvent> messages) {
    check(bank.begin_block(sample, 1) == HostEventResult::Accepted, "pedal-begin");
    std::uint64_t sequence = 1;
    for (const auto& message : messages)
        check(bank.publish(event(bank.epoch(), sample, sequence++, message)) ==
                  HostEventResult::Accepted,
              "pedal-publish");
    audio::Buffer<float> buffer(2, 1);
    auto view = buffer.view();
    return bank.render(view, sample, 1);
}

void check_pedals_and_modes() {
    VoiceBankBindingResult status;
    auto boundary = make_bank(schema(), status);
    check(static_cast<bool>(boundary), "boundary-prepare");
    check(step(*boundary, 0,
               {midi::MidiEvent::note_on(1, 69, 127), midi::MidiEvent::cc(1, 64, 64)}) ==
              VoiceBankBindingCode::Ok,
          "boundary-down-render");
    check(step(*boundary, 1, {midi::MidiEvent::note_off(1, 69)}) == VoiceBankBindingCode::Ok,
          "boundary-note-off-render");
    check(!owner(*boundary).allocator().voice(0).releasing(), "pedal-64-down");
    check(step(*boundary, 2, {midi::MidiEvent::cc(1, 64, 63)}) == VoiceBankBindingCode::Ok,
          "boundary-up-render");
    check(owner(*boundary).allocator().voice(0).releasing(), "pedal-63-up");

    auto bank = make_bank(schema(), status);
    check(static_cast<bool>(bank), "pedal-prepare");
    check(step(*bank, 0,
               {midi::MidiEvent::note_on(1, 69, 127), midi::MidiEvent::cc(1, 64, 64),
                midi::MidiEvent::cc(0, 64, 127)}) == VoiceBankBindingCode::Ok,
          "pedal-down-render");
    check(step(*bank, 1, {midi::MidiEvent::note_off(1, 69), midi::MidiEvent::note_off(1, 69)}) ==
              VoiceBankBindingCode::Ok,
          "pedal-held-note-off");
    check(!owner(*bank).allocator().voice(0).releasing(), "pedal-64-down");
    check(step(*bank, 2, {midi::MidiEvent::cc(1, 64, 63)}) == VoiceBankBindingCode::Ok,
          "member-pedal-up");
    check(!owner(*bank).allocator().voice(0).releasing(), "manager-hold-persists");
    check(step(*bank, 3, {midi::MidiEvent::cc(0, 121, 0)}) == VoiceBankBindingCode::Ok,
          "manager-cc121");
    check(owner(*bank).allocator().voice(0).releasing(), "cc121-drains-last-hold");
    check(step(*bank, 4, {midi::MidiEvent::cc(0, 64, 0)}) == VoiceBankBindingCode::Ok,
          "duplicate-pedal-up");
    check(owner(*bank).allocator().voice(0).releasing(), "single-release");

    auto scoped = make_bank(schema(), status, definition(), midi::MpeConfig::dual(2, 2));
    check(static_cast<bool>(scoped), "scoped-prepare");
    check(step(*scoped, 0,
               {midi::MidiEvent::note_on(1, 60, 100), midi::MidiEvent::note_on(2, 62, 100),
                midi::MidiEvent::note_on(14, 64, 100), midi::MidiEvent::cc(1, 64, 127)}) ==
              VoiceBankBindingCode::Ok,
          "scoped-notes");
    check(step(*scoped, 1, {midi::MidiEvent::cc(1, 123, 0)}) == VoiceBankBindingCode::Ok,
          "member-cc123");
    check(!owner(*scoped).allocator().voice(0).releasing(), "cc123-held");
    check(step(*scoped, 2, {midi::MidiEvent::cc(1, 64, 0)}) == VoiceBankBindingCode::Ok,
          "member-final-rise");
    check(owner(*scoped).allocator().voice(0).releasing(), "cc123-dispatches-release");
    check(step(*scoped, 3, {midi::MidiEvent::cc(2, 120, 0)}) == VoiceBankBindingCode::Ok,
          "member-panic");
    check(!owner(*scoped).allocator().voice(1).active(), "member-panic-silences-self");
    check(owner(*scoped).allocator().voice(2).active(), "member-panic-preserves-other");
    check(step(*scoped, 4, {midi::MidiEvent::cc(3, 64, 127)}) == VoiceBankBindingCode::OutOfZone,
          "out-of-zone-typed-refusal");
    check(step(*scoped, 5, {midi::MidiEvent::cc(14, 66, 0)}) ==
              VoiceBankBindingCode::UnsupportedControl,
          "unsupported-control-typed-refusal");
    check(step(*scoped, 6, {midi::MidiEvent::cc(15, 120, 0)}) == VoiceBankBindingCode::Ok,
          "manager-panic");
    check(!owner(*scoped).allocator().voice(2).active(), "manager-panic-zone-scope");
    check(step(*scoped, 7, {midi::MidiEvent::pitch_bend(15, 12288)}) ==
              VoiceBankBindingCode::UnsupportedExpression,
          "manager-expression-typed-refusal");
}

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
    void dispatch(const midi::MpeExpressionEvent& event) noexcept override {
        backing_.dispatch(event);
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
    const void* slot_state_identity(std::uint32_t) const noexcept override {
        return backing_.slot_state_identity(0);
    }
    InputSemantic resolve_input(const VoiceBindingSpec& binding) const noexcept override {
        return backing_.resolve_input(binding);
    }

  private:
    MpeVoiceBankOwner<ProbeVoice> backing_;
};
} // namespace

int main() {
    try {
        const auto scalar = render(1);
        check(scalar.left.size() == 800 && scalar.right.size() == 800, "scalar-length");
        double phase = 0.0;
        for (std::size_t i = 0; i < scalar.left.size(); ++i) {
            float expected_left = 0.0f;
            float expected_right = 0.0f;
            if (i >= 3 && i < 10) {
                const float triangle = phase < 0.5 ? static_cast<float>(4.0 * phase - 1.0)
                                                   : static_cast<float>(3.0 - 4.0 * phase);
                const float release = i < 6 ? 1.0f : (4.0f - static_cast<float>(i - 6)) * 0.25f;
                expected_left = triangle * release * kBankGain;
                expected_right = triangle * release * (0.5f * kBankGain);
                phase += 440.0 / 48000.0;
            }
            check(near(scalar.left[i], expected_left), "independent-left-oracle");
            check(near(scalar.right[i], expected_right), "independent-right-oracle");
        }
        for (const auto partition : {32u, 64u, 127u, 256u}) {
            const auto candidate = render(partition);
            check(candidate.left.size() == scalar.left.size(), "partition-length");
            for (std::size_t i = 0; i < scalar.left.size(); ++i) {
                check(near(candidate.left[i], scalar.left[i]), "partition-left-audio");
                check(near(candidate.right[i], scalar.right[i]), "partition-right-audio");
            }
        }
        check_steal_policies();
        check_state_and_generation();
        check_deferred_expression_validation();
        check_capacity_and_poly_pressure();
        check_eight_voice_sum();
        check_expression_audio();
        check_pedals_and_modes();
        VoiceBankBindingResult status;
        auto malformed = schema();
        malformed.bindings[0].target = "unbound-note-port";
        check(!make_bank(malformed, status), "unbound-note-refusal");
        auto disconnected = definition();
        disconnected.exposed_ports.pop_back();
        check(!make_bank(schema(), status, std::move(disconnected)), "disconnected-stereo-refusal");
        auto swapped = definition();
        swapped.exposed_ports[0].port = 1;
        swapped.exposed_ports[1].port = 0;
        check(!make_bank(schema(), status, std::move(swapped)), "swapped-stereo-refusal");
        auto expression = schema();
        expression.bindings[1].target = "wrong-expression";
        check(!make_bank(expression, status), "unknown-expression-refusal");
        auto wrong_path = schema();
        wrong_path.bindings[0].path = {"other"};
        check(!make_bank(wrong_path, status), "wrong-path-refusal");
        check(!VoiceBankBinding::prepare(schema(), definition(), std::make_unique<AliasedOwner>(),
                                         "audio_out", 48000, 256, status),
              "aliased-state-refusal");
        std::cout << "g6-voice-bank-public-consumer PASS partitions=1,32,64,127,256"
                     " samples=800 sample3="
                  << scalar.left[3] << " sample6=" << scalar.left[6]
                  << " sample10=" << scalar.left[10] << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "g6-voice-bank-public-consumer FAIL rule=" << error.what() << '\n';
        return 1;
    }
}
