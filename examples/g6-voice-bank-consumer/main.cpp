#include "fixture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace pulp;
using namespace pulp::host;
using namespace pulp::examples::g6;

namespace {
void check(bool value, const char* rule) {
    if (!value) throw std::runtime_error(rule);
}

std::vector<float> render(std::size_t partition) {
    VoiceBankBindingResult status;
    auto bank = make_bank(schema(), status);
    check(static_cast<bool>(bank), "prepare-eight-processors");
    std::vector<float> samples;
    for (std::size_t start = 0; start < 800; start += partition) {
        const auto frames = std::min(partition, std::size_t{800} - start);
        check(bank->begin_block(start, frames) == HostEventResult::Accepted, "begin-block");
        if (start <= 3 && 3 < start + frames)
            check(bank->publish(event(bank->epoch(), 3, 1,
                                      midi::MidiEvent::note_on(1, 69, 127))) ==
                      HostEventResult::Accepted, "note-on");
        if (start <= 6 && 6 < start + frames)
            check(bank->publish(event(bank->epoch(), 6, 2,
                                      midi::MidiEvent::note_off(1, 69))) ==
                      HostEventResult::Accepted, "note-off");
        audio::Buffer<float> buffer(2, frames);
        auto view = buffer.view();
        const auto render_code = bank->render(view, start, frames);
        if (render_code != VoiceBankBindingCode::Ok)
            throw std::runtime_error("render-code=" +
                                     std::to_string(static_cast<unsigned>(render_code)));
        for (std::size_t i = 0; i < frames; ++i) {
            check(std::isfinite(view.channel(0)[i]), "finite-audio");
            check(view.channel(0)[i] == view.channel(1)[i], "stereo-mono-duplicate");
            samples.push_back(view.channel(0)[i]);
        }
    }
    check(owner(*bank).allocator().active_count() == 0, "finite-release-reclaimed");
    return samples;
}

void check_steal_policies() {
    const std::array modes{midi::MpeVoiceStealMode::Oldest,
                           midi::MpeVoiceStealMode::LowestVelocity,
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
            check(bank->publish(event(bank->epoch(), 0, slot + 1,
                                      midi::MidiEvent::note_on(slot + 1, 60 + slot,
                                                               100 - slot * 10))) ==
                      HostEventResult::Accepted, "fill-bank");
        check(bank->publish(event(bank->epoch(), 1, 9,
                                  midi::MidiEvent::note_on(9, 90, 127))) ==
                  HostEventResult::Accepted, "ninth-note");
        audio::Buffer<float> buffer(2, 2);
        auto view = buffer.view();
        check(bank->render(view, 0, 2) == VoiceBankBindingCode::Ok, "steal-render");
        check(owner(*bank).allocator().voice(victims[policy]).note_number() == 90,
              "steal-victim");
        check(owner(*bank).allocator().steal_count() == 1, "steal-count");
    }
}

void check_state_and_generation() {
    VoiceBankBindingResult status;
    auto bank = make_bank(schema(), status);
    check(static_cast<bool>(bank), "state-prepare");
    check(bank->set_slot_parameter(0, "level", 2.0f), "named-parameter-bind");
    check(!bank->set_slot_parameter(0, "missing", 1.0f), "unknown-parameter-refusal");
    check(bank->begin_block(0, 1) == HostEventResult::Accepted, "state-begin");
    check(bank->publish(event(bank->epoch(), 0, 1,
                              midi::MidiEvent::note_on(1, 69, 127))) ==
              HostEventResult::Accepted, "state-note-on");
    audio::Buffer<float> buffer(2, 1);
    auto view = buffer.view();
    check(bank->render(view, 0, 1) == VoiceBankBindingCode::Ok, "state-render");
    check(std::abs(view.channel(0)[0] + 0.25f) <= 2e-6f, "slot-zero-level-audio");
    const auto generation = owner(*bank).allocator().voice(0).note_id();
    midi::MpeNoteState note{};
    note.note_id = generation;
    note.channel = 1;
    note.note = 69;
    note.velocity = 127;
    check(bank->dispatch_deferred(bank->epoch(), 0, generation + 1,
                                  midi::MpeExpressionEvent::Kind::NoteOff, note) ==
              VoiceBankBindingCode::StaleGeneration, "stale-generation-refusal");
    auto forged = note;
    forged.channel = 2;
    check(bank->dispatch_deferred(bank->epoch(), 0, generation,
                                  midi::MpeExpressionEvent::Kind::NoteOff, forged) ==
              VoiceBankBindingCode::StaleGeneration, "forged-channel-refusal");
    check(bank->activate(status), "state-activate");
    check(bank->dispatch_deferred(bank->epoch() - 1, 0, generation,
                                  midi::MpeExpressionEvent::Kind::NoteOff, note) ==
              VoiceBankBindingCode::StaleEpoch, "stale-epoch-refusal");
}

class AliasedOwner final : public VoiceBankOwner {
  public:
    AliasedOwner()
        : backing_([](ProbeVoice& voice, std::uint32_t)
                       -> std::unique_ptr<format::Processor> {
              return std::make_unique<ProbeProcessor>(voice);
          }, std::array<VoiceBankInputEndpoint, 4>{{{"note_in", 0}, {"pitch", 1},
                                                    {"pressure", 2}, {"timbre", 3}}}) {}
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
    void reset() noexcept override { backing_.reset(); }
    void set_steal_mode(midi::MpeVoiceStealMode mode) noexcept override {
        backing_.set_steal_mode(mode);
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
        check(scalar.size() == 800, "scalar-length");
        double phase = 0.0;
        for (std::size_t i = 0; i < scalar.size(); ++i) {
            float expected = 0.0f;
            if (i >= 3 && i < 10) {
                const float triangle = phase < 0.5 ? static_cast<float>(4.0 * phase - 1.0) :
                                                     static_cast<float>(3.0 - 4.0 * phase);
                const float release = i < 6 ? 1.0f :
                    (4.0f - static_cast<float>(i - 6)) * 0.25f;
                expected = triangle * release * 0.125f;
                phase += 440.0 / 48000.0;
            }
            check(std::abs(scalar[i] - expected) <= 2e-6f, "independent-audio-oracle");
        }
        for (const auto partition : {32u, 64u, 127u, 256u}) {
            const auto candidate = render(partition);
            check(candidate.size() == scalar.size(), "partition-length");
            for (std::size_t i = 0; i < scalar.size(); ++i)
                check(std::abs(candidate[i] - scalar[i]) <= 2e-6f, "partition-audio");
        }
        check_steal_policies();
        check_state_and_generation();
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
        check(!VoiceBankBinding::prepare(schema(), definition(),
                                         std::make_unique<AliasedOwner>(), "audio_out",
                                         48000, 256, status), "aliased-state-refusal");
        std::cout << "g6-voice-bank-public-consumer PASS partitions=1,32,64,127,256"
                     " samples=800 sample3=" << scalar[3] << " sample6=" << scalar[6]
                  << " sample10=" << scalar[10] << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "g6-voice-bank-public-consumer FAIL rule=" << error.what() << '\n';
        return 1;
    }
}
