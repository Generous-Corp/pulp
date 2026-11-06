// PanicButton: the stock panic control, alone over a scripted target and wired
// to a Processor through format::panic_target and a HeadlessHost.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/headless.hpp>
#include <pulp/format/panic_target.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/panic.hpp>
#include <pulp/view/panic_button.hpp>
#include <pulp/view/view.hpp>

#include <memory>
#include <vector>

using namespace pulp;
using view::PanicAvailability;
using view::PanicButton;

namespace {

struct ScriptedTarget {
    PanicAvailability availability = PanicAvailability::voices_and_midi;
    std::vector<bool> requests; // release_only per request

    view::PanicTarget target() {
        return {[this](bool release_only) { requests.push_back(release_only); },
                [this] { return availability; }};
    }
};

// Click through the view tree, the way a pointer does: the button sits inside a
// root so hit-testing and visibility both apply.
struct Tree {
    explicit Tree(view::PanicTarget target) {
        root.set_bounds({0, 0, 200, 100});
        auto owned = std::make_unique<PanicButton>(std::move(target));
        owned->set_bounds({10, 10, 80, 30});
        button = owned.get();
        root.add_child(std::move(owned));
    }
    void click() {
        root.simulate_click({20, 20});
    }

    view::View root;
    PanicButton* button = nullptr;
};

class Synth : public format::Processor {
  public:
    explicit Synth(bool produces_midi) : produces_midi_(produces_midi) {}

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PanicButtonSynth",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.panic-button-synth",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Out", 2}},
            .accepts_midi = true,
            .produces_midi = produces_midi_,
        };
    }
    void define_parameters(state::StateStore&) override {}
    void prepare(const format::PrepareContext&) override {}
    void on_panic(format::PanicMode mode) override {
        modes.push_back(mode);
    }
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        for (std::size_t c = 0; c < output.num_channels(); ++c)
            for (std::size_t i = 0; i < output.num_samples(); ++i)
                output.channel_ptr(c)[i] = 0.0f;
    }

    std::vector<format::PanicMode> modes;

  private:
    bool produces_midi_;
};

Synth* g_synth = nullptr;

std::unique_ptr<format::Processor> make_with_midi() {
    auto p = std::make_unique<Synth>(true);
    g_synth = p.get();
    return p;
}
std::unique_ptr<format::Processor> make_without_midi() {
    auto p = std::make_unique<Synth>(false);
    g_synth = p.get();
    return p;
}

struct Rig {
    explicit Rig(format::ProcessorFactory factory) : host(factory), out(2, kBlock) {
        host.prepare(48000.0, kBlock, 0, 2);
        midi_out.reserve(1024);
    }
    void run() {
        midi_out.clear();
        auto buffers = out.view();
        audio::BufferView<const float> no_input;
        midi::MidiBuffer none;
        host.process(buffers, no_input, none, midi_out);
    }
    static constexpr std::size_t kBlock = 128;
    format::HeadlessHost host;
    audio::Buffer<float> out;
    midi::MidiBuffer midi_out;
};

} // namespace

TEST_CASE("a click raises exactly one hard-stop request", "[view][panic]") {
    ScriptedTarget scripted;
    Tree tree(scripted.target());
    REQUIRE(tree.button->visible());

    tree.click();
    REQUIRE(scripted.requests.size() == 1);
    CHECK_FALSE(scripted.requests[0]);
    CHECK(tree.button->request_count() == 1);
}

TEST_CASE("release-only mode asks for the release panic", "[view][panic]") {
    ScriptedTarget scripted;
    Tree tree(scripted.target());
    tree.button->set_release_only(true);
    tree.click();
    REQUIRE(scripted.requests.size() == 1);
    CHECK(scripted.requests[0]);
}

TEST_CASE("an unserviced host hides the button and a click raises nothing", "[view][panic]") {
    ScriptedTarget scripted;
    scripted.availability = PanicAvailability::unavailable;
    Tree tree(scripted.target());
    CHECK_FALSE(tree.button->visible());
    CHECK_FALSE(tree.button->is_enabled());

    tree.click();
    CHECK(scripted.requests.empty());
    // Calling it directly is refused too, not just unreachable by the pointer.
    CHECK_FALSE(tree.button->trigger());
    CHECK(scripted.requests.empty());
}

TEST_CASE("the button follows availability on refresh and on click", "[view][panic]") {
    ScriptedTarget scripted;
    Tree tree(scripted.target());

    // The host stops servicing between frames: the next click re-reads
    // availability and raises nothing.
    scripted.availability = PanicAvailability::unavailable;
    tree.button->trigger();
    CHECK(scripted.requests.empty());
    CHECK_FALSE(tree.button->visible());

    scripted.availability = PanicAvailability::voices_only;
    CHECK(tree.button->refresh() == PanicAvailability::voices_only);
    CHECK(tree.button->visible());
    tree.click();
    CHECK(scripted.requests.size() == 1);
}

TEST_CASE("disabled-not-hidden mode keeps the button visible but inert", "[view][panic]") {
    ScriptedTarget scripted;
    scripted.availability = PanicAvailability::unavailable;
    Tree tree(scripted.target());
    tree.button->set_hide_when_unavailable(false);
    CHECK(tree.button->visible());
    CHECK_FALSE(tree.button->is_enabled());
    tree.click();
    CHECK(scripted.requests.empty());
}

TEST_CASE("the accessible label says what a click will do", "[view][panic]") {
    ScriptedTarget scripted;
    Tree tree(scripted.target());
    CHECK(tree.button->access_label() == "Panic: stop all notes and send MIDI panic");

    scripted.availability = PanicAvailability::voices_only;
    tree.button->refresh();
    CHECK(tree.button->access_label() == "Panic: stop all notes");

    tree.button->set_release_only(true);
    CHECK(tree.button->access_label() == "Panic: release all notes");
}

TEST_CASE("panic_availability maps processor capabilities", "[format][panic]") {
    CHECK(format::panic_availability({false, false}) == PanicAvailability::unavailable);
    CHECK(format::panic_availability({false, true}) == PanicAvailability::unavailable);
    CHECK(format::panic_availability({true, false}) == PanicAvailability::voices_only);
    CHECK(format::panic_availability({true, true}) == PanicAvailability::voices_and_midi);
}

TEST_CASE("a processor with no servicing host hides the button", "[format][panic]") {
    Synth bare(true);
    PanicButton button(format::panic_target(bare));
    CHECK(button.availability() == PanicAvailability::unavailable);
    CHECK_FALSE(button.visible());
    CHECK_FALSE(button.trigger());
    CHECK_FALSE(bare.panic_state()->pending());
}

TEST_CASE("clicking the bound button panics the processor at the next block", "[format][panic]") {
    Rig rig(make_with_midi);
    Tree tree(format::panic_target(*g_synth));
    CHECK(tree.button->availability() == PanicAvailability::voices_and_midi);

    rig.run();
    CHECK(g_synth->modes.empty());

    tree.click();
    rig.run();
    REQUIRE(g_synth->modes.size() == 1);
    CHECK(g_synth->modes[0] == format::PanicMode::immediate);
    CHECK(rig.midi_out.size() == midi::panic_message_count(midi::PanicScope{}));

    // Consumed once.
    rig.run();
    CHECK(g_synth->modes.size() == 1);
    CHECK(rig.midi_out.empty());
}

TEST_CASE("the bound button's release mode reaches the processor", "[format][panic]") {
    Rig rig(make_without_midi);
    Tree tree(format::panic_target(*g_synth));
    CHECK(tree.button->availability() == PanicAvailability::voices_only);
    tree.button->set_release_only(true);

    tree.click();
    rig.run();
    REQUIRE(g_synth->modes.size() == 1);
    CHECK(g_synth->modes[0] == format::PanicMode::release);
    CHECK(rig.midi_out.empty());
}
