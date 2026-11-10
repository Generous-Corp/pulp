#include "fixture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
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
    check(bank->render(cc120_view, 131, 1) == VoiceBankBindingCode::Ok,
          "capacity-cc120-reuse");
    check(bank->begin_block(132, 1) == HostEventResult::Accepted, "capacity-cc123-begin");
    check(bank->publish(event(bank->epoch(), 132, 1, midi::MidiEvent::cc(0, 123, 0))) ==
              HostEventResult::Accepted,
          "capacity-cc123-publish");
    check(bank->publish(event(bank->epoch(), 132, 2, midi::MidiEvent::note_on(1, 0, 127))) ==
              HostEventResult::Accepted,
          "capacity-cc123-restart");
    audio::Buffer<float> cc123_audio(2, 1);
    auto cc123_view = cc123_audio.view();
    check(bank->render(cc123_view, 132, 1) == VoiceBankBindingCode::Ok,
          "capacity-cc123-reuse");

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

Stereo render_event_trace(std::size_t partition,
                          const std::vector<std::pair<std::uint64_t, midi::MidiEvent>>& events,
                          std::size_t total_frames) {
    VoiceBankBindingResult status;
    auto bank = make_bank(schema(), status);
    check(static_cast<bool>(bank), "trace-prepare");
    Stereo output;
    std::uint64_t sequence = 1;
    for (std::size_t start = 0; start < total_frames; start += partition) {
        const auto frames = std::min(partition, total_frames - start);
        check(bank->begin_block(start, frames) == HostEventResult::Accepted, "trace-begin");
        for (const auto& [sample, message] : events)
            if (start <= sample && sample < start + frames)
                check(bank->publish(event(bank->epoch(), sample, sequence++, message)) ==
                          HostEventResult::Accepted,
                      "trace-publish");
        audio::Buffer<float> buffer(2, frames);
        auto view = buffer.view();
        check(bank->render(view, start, frames) == VoiceBankBindingCode::Ok, "trace-render");
        for (std::size_t frame = 0; frame < frames; ++frame) {
            output.left.push_back(view.channel(0)[frame]);
            output.right.push_back(view.channel(1)[frame]);
        }
    }
    return output;
}

void check_trace_matches(const Stereo& actual, const Stereo& expected, const char* rule) {
    check(actual.left.size() == expected.left.size() &&
              actual.right.size() == expected.right.size(),
          rule);
    for (std::size_t frame = 0; frame < expected.left.size(); ++frame)
        check(near(actual.left[frame], expected.left[frame]) &&
                  near(actual.right[frame], expected.right[frame]),
              rule);
}

void check_pedal_expression_audio_partitions() {
    const auto pedal_events = [](bool channel_mode, bool manager_reset, bool sustain) {
        std::vector<std::pair<std::uint64_t, midi::MidiEvent>> events{
            {0, midi::MidiEvent::note_on(1, 69, 127)},
        };
        if (sustain) {
            events.emplace_back(1, midi::MidiEvent::cc(1, 64, 127));
            events.emplace_back(2, midi::MidiEvent::cc(0, 64, 127));
        }
        events.emplace_back(3, channel_mode ? midi::MidiEvent::cc(1, 123, 0)
                                            : midi::MidiEvent::note_off(1, 69));
        events.emplace_back(4, midi::MidiEvent::cc(1, 64, 0));
        events.emplace_back(5, midi::MidiEvent::cc(0, manager_reset ? 121 : 64, 0));
        events.emplace_back(10, midi::MidiEvent::note_on(1, 72, 127));
        events.emplace_back(11, midi::MidiEvent::cc(0, 120, 0));
        return events;
    };
    const auto held = render_event_trace(1, pedal_events(false, false, true), 12);
    const auto no_pedal = render_event_trace(1, pedal_events(false, false, false), 12);
    check(std::abs(held.left[7]) > 1e-4f && near(no_pedal.left[7], 0.0f),
          "pedal-defers-audible-tail");
    check(near(held.left[9], 0.0f) && near(held.right[9], 0.0f), "pedal-release-tail-bounded");
    check(std::abs(held.left[10]) > 1e-4f && near(held.left[11], 0.0f) &&
              near(held.right[11], 0.0f),
          "manager-panic-silences-audio");
    for (const auto partition : {2u, 3u, 7u, 16u}) {
        check_trace_matches(render_event_trace(partition, pedal_events(false, false, true), 12),
                            held, "pedal-partition-invariant");
        check_trace_matches(render_event_trace(partition, pedal_events(true, true, true), 12), held,
                            "channel-mode-audio-equivalent");
    }

    const midi::MidiEvent pressure{choc::midi::ShortMessage(0xd1, 127, 0), 0, 0.0};
    const std::vector<std::pair<std::uint64_t, midi::MidiEvent>> expression_events{
        {0, midi::MidiEvent::note_on(1, 69, 127)},
        {2, midi::MidiEvent::pitch_bend(1, 16383)},
        {3, pressure},
        {4, midi::MidiEvent::cc(1, 74, 127)},
        {6, midi::MidiEvent::note_off(1, 69)},
    };
    const auto expression = render_event_trace(1, expression_events, 12);
    check(near(expression.left[10], 0.0f) && near(expression.right[10], 0.0f),
          "expression-release-tail-bounded");
    for (const auto partition : {2u, 3u, 7u, 16u})
        check_trace_matches(render_event_trace(partition, expression_events, 12), expression,
                            "expression-partition-invariant");
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

void check_frozen_a1_vectors() {
    for (const auto& fixture : a1::kCases) {
        for (const auto partition : {1u, 32u, 64u, 127u, 256u}) {
            VoiceBankBindingResult status;
            auto bank = make_a1_bank(fixture, status);
            check(static_cast<bool>(bank), "a1-prepare");
            for (std::uint32_t slot = 0; slot < 8; ++slot)
                check(bank->set_slot_parameter(slot, "level", fixture.gain[slot]),
                      "a1-authored-gain");
            std::vector<float> left;
            std::vector<float> right;
            const auto total_frames = fixture.capture_start + fixture.frames;
            for (std::size_t start = 0; start < total_frames; start += partition) {
                const auto frames = std::min<std::size_t>(partition, total_frames - start);
                check(bank->begin_block(start, frames) == HostEventResult::Accepted, "a1-begin");
                if (start == 0) {
                    std::uint64_t sequence = 1;
                    for (std::uint8_t slot = 0; slot < 8; ++slot) {
                        if (!fixture.active[slot] && fixture.capture_start == 0 &&
                            fixture.kind != a1::Kind::Steal)
                            continue;
                        const auto note = fixture.kind == a1::Kind::Steal && slot == 0
                                              ? 69
                                              : static_cast<int>(60 + slot);
                        check(bank->publish(event(bank->epoch(), 0, sequence++,
                                                  midi::MidiEvent::note_on(slot + 1, note, 127))) ==
                                  HostEventResult::Accepted,
                              "a1-start");
                    }
                    if (fixture.capture_start != 0) {
                        for (std::uint8_t slot = 0; slot < 8; ++slot)
                            if (!fixture.active[slot])
                                check(bank->publish(
                                          event(bank->epoch(), 0, sequence++,
                                                midi::MidiEvent::note_off(slot + 1, 60 + slot))) ==
                                          HostEventResult::Accepted,
                                      "a1-sparse-retire");
                    }
                }
                if (fixture.kind == a1::Kind::Release && start <= fixture.note_off_sample &&
                    fixture.note_off_sample < start + frames)
                    check(bank->publish(event(bank->epoch(), fixture.note_off_sample, 9,
                                              midi::MidiEvent::note_off(1, 60))) ==
                              HostEventResult::Accepted,
                          "a1-release");
                if (fixture.kind == a1::Kind::Steal && start <= fixture.steal_sample &&
                    fixture.steal_sample < start + frames)
                    check(bank->publish(event(bank->epoch(), fixture.steal_sample, 9,
                                              midi::MidiEvent::note_on(9, 70, 127))) ==
                              HostEventResult::Accepted,
                          "a1-steal");
                audio::Buffer<float> buffer(2, frames);
                auto view = buffer.view();
                check(bank->render(view, start, frames) == VoiceBankBindingCode::Ok, "a1-render");
                for (std::size_t frame = 0; frame < frames; ++frame)
                    if (start + frame >= fixture.capture_start) {
                        left.push_back(view.channel(0)[frame]);
                        right.push_back(view.channel(1)[frame]);
                    }
            }
            check(left.size() == fixture.frames && right.size() == fixture.frames,
                  "a1-vector-length");
            for (std::size_t frame = 0; frame < fixture.frames; ++frame) {
                if (!near(left[frame], fixture.expected_left[frame]))
                    throw std::runtime_error(
                        std::string(fixture.id) + " left frame " + std::to_string(frame) +
                        " actual=" + std::to_string(left[frame]) +
                        " expected=" + std::to_string(fixture.expected_left[frame]));
                if (!near(right[frame], fixture.expected_right[frame]))
                    throw std::runtime_error(
                        std::string(fixture.id) + " right frame " + std::to_string(frame) +
                        " actual=" + std::to_string(right[frame]) +
                        " expected=" + std::to_string(fixture.expected_right[frame]));
            }
            if (fixture.kind == a1::Kind::Release)
                check(static_cast<const MpeVoiceBankOwner<A1ScriptedVoice>&>(bank->owner())
                              .allocator()
                              .active_count() == 0,
                      "a1-bounded-tail");
            if (fixture.kind == a1::Kind::Steal)
                check(static_cast<const MpeVoiceBankOwner<A1ScriptedVoice>&>(bank->owner())
                              .allocator()
                              .steal_count() == 1,
                      "a1-hard-steal");
        }
    }
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
        check_capacity_and_poly_pressure();
        check_eight_voice_sum();
        check_expression_audio();
        check_pedals_and_modes();
        check_pedal_expression_audio_partitions();
        check_frozen_a1_vectors();
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
