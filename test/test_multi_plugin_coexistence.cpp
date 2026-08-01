// Three Pulp plugins alive in ONE process, at the same time.
//
// On macOS a DAW loads several Audio Units into a single shared
// `AUHostingServiceXPC` process, so a product family shipped as a MIDI effect +
// an instrument + an audio effect is three instances of the SAME binary sharing
// one address space, one main thread, and every process-global static that
// binary owns. A user assembling a channel strip does this on the first day.
//
// The companion suites each cover one slice of that world and deliberately stop
// short of this one:
//   - test_interaction_multiinstance.cpp pins the per-root interaction STATE
//     (focus / overlay / popup slots) for two bare view trees;
//   - test_foreign_framework_coexistence.mm pins AppKit-level coexistence with
//     a non-Pulp window.
// Neither stands up real `Processor`s, and neither exercises the ROUTING verbs
// the hosts call — which is exactly where the process-global shim mirrors were
// still being read without a root scope.
//
// What is asserted here, all with three instances live simultaneously:
//   1. Parameter state, prepared state, and rendered audio stay per-instance,
//      including while all three render concurrently on their own threads.
//   2. Three editors coexist: focus, open dropdown, and painted content each
//      belong to exactly one editor.
//   3. Pointer routing is scoped to the editor that received the event — the
//      regression guard for the process-wide `ComboBox::active_popup_` mirror.
//   4. Tearing one editor down and rebuilding it leaves the other two intact.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/window_host.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace pulp::view;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;
using pulp::format::PluginCategory;
using pulp::format::PluginDescriptor;
using pulp::format::PrepareContext;
using pulp::format::ProcessContext;
using pulp::format::Processor;

namespace {

constexpr pulp::state::ParamID kAmount = 1;

// ── The three product shapes ────────────────────────────────────────────────
//
// One base carries everything the three share (a parameter, a prepare latch, an
// editor tree with a ComboBox + a ScrollView); each subclass differs only in the
// descriptor it advertises and the mark it writes, so a mixed-up instance is
// visible in the assertion rather than hidden behind identical behavior.
class FamilyMember : public Processor {
public:
    FamilyMember(std::string name, float mark) : name_(std::move(name)), mark_(mark) {}

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({.id = kAmount, .name = "Amount", .range = {0.0f, 1.0f, 0.5f}});
        store_ = &store;
    }
    void prepare(const PrepareContext& ctx) override {
        prepared_rate_ = ctx.sample_rate;
        prepared_block_ = ctx.max_buffer_size;
        ++prepare_count_;
    }

    // Writes this instance's own mark scaled by its own parameter, so a buffer
    // filled by the wrong instance is arithmetically distinguishable.
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const ProcessContext&) override {
        const float amount = store_ ? store_->get_value(kAmount) : 0.0f;
        for (std::size_t ch = 0; ch < out.num_channels(); ++ch) {
            auto samples = out.channel(ch);
            for (auto& s : samples) s = mark_ * amount;
        }
        midi_seen_ += static_cast<int>(midi_in.size());
        if (produces_midi_) {
            // A MIDI effect's whole output surface. `mark_` doubles as the note
            // number so a swapped instance shows up as a wrong note.
            midi_out.add(pulp::midi::MidiEvent::note_on(
                0, static_cast<uint8_t>(mark_), 100));
        }
    }

    // An editor built from real widgets: a ComboBox (the popup-routing surface),
    // a focusable text-accepting field (the keyboard surface), and a scrollable
    // pane (the wheel surface). Every editor is laid out identically ON PURPOSE
    // — overlapping geometry across editors is precisely what lets a
    // process-global routing slot deliver one editor's pointer to another.
    std::unique_ptr<View> create_view() override {
        auto root = std::make_unique<View>();
        root->set_bounds({0, 0, 400, 300});

        auto combo = std::make_unique<ComboBox>();
        combo->set_bounds({0, 0, 160, 28});
        combo->set_items({name_ + " 1", name_ + " 2", name_ + " 3", name_ + " 4"});
        root->add_child(std::move(combo));

        auto field = std::make_unique<TextEditor>();
        field->set_bounds({0, 200, 160, 28});
        root->add_child(std::move(field));

        auto scroll = std::make_unique<ScrollView>();
        scroll->set_bounds({200, 0, 200, 300});
        scroll->set_content_size({200, 2000});
        root->add_child(std::move(scroll));

        // A second scroll pane placed directly UNDER where the ComboBox's open
        // menu hangs. That overlap is the whole point: it makes "who consumed
        // this wheel" observable without asking the popup-routing helper — the
        // pane moves only if this editor's own tree handled the event.
        auto under_menu = std::make_unique<ScrollView>();
        under_menu->set_bounds({0, 40, 160, 120});
        under_menu->set_content_size({160, 2000});
        root->add_child(std::move(under_menu));

        // A label whose text names this instance, so a paint recording proves
        // WHICH editor produced it.
        auto label = std::make_unique<Label>(name_);
        label->set_bounds({0, 100, 200, 24});
        root->add_child(std::move(label));

        return root;
    }

    double prepared_rate() const { return prepared_rate_; }
    int prepared_block() const { return prepared_block_; }
    int prepare_count() const { return prepare_count_; }
    int midi_seen() const { return midi_seen_; }
    float mark() const { return mark_; }
    const std::string& display_name() const { return name_; }

protected:
    std::string name_;
    float mark_;
    bool produces_midi_ = false;
    pulp::state::StateStore* store_ = nullptr;
    double prepared_rate_ = 0.0;
    int prepared_block_ = 0;
    int prepare_count_ = 0;
    int midi_seen_ = 0;
};

class MidiFx : public FamilyMember {
public:
    MidiFx() : FamilyMember("Arp", 60.0f) { produces_midi_ = true; }
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "Family Arp";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.test.family.arp";
        d.category = PluginCategory::MidiEffect;
        d.accepts_midi = true;
        d.produces_midi = true;
        return d;
    }
};

class Instrument : public FamilyMember {
public:
    Instrument() : FamilyMember("Synth", 0.25f) {}
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "Family Synth";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.test.family.synth";
        d.category = PluginCategory::Instrument;
        d.accepts_midi = true;
        d.input_buses = {};
        return d;
    }
};

class AudioFx : public FamilyMember {
public:
    AudioFx() : FamilyMember("Drive", 0.75f) {}
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "Family Drive";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.test.family.drive";
        d.category = PluginCategory::Effect;
        return d;
    }
};

// One loaded plugin: the processor, its own StateStore, and (once opened) its
// own editor tree — the shape a format adapter hands a host.
struct LoadedPlugin {
    explicit LoadedPlugin(std::unique_ptr<FamilyMember> p) : processor(std::move(p)) {
        processor->define_parameters(store);
    }

    void open_editor() {
        editor = processor->create_view();
        REQUIRE(editor != nullptr);
        processor->on_view_opened(*editor);
        // The accessors below index create_view()'s fixed child order; a
        // reordered editor must fail here, not by dereferencing a null cast
        // three cases later.
        REQUIRE(combo() != nullptr);
        REQUIRE(field() != nullptr);
        REQUIRE(scroll() != nullptr);
        REQUIRE(under_menu() != nullptr);
    }
    void close_editor() {
        if (!editor) return;
        processor->on_view_closed(*editor);
        editor.reset();
    }

    // First child of each kind — the editor layout is fixed by create_view().
    ComboBox* combo() const { return dynamic_cast<ComboBox*>(editor->child_at(0)); }
    TextEditor* field() const { return dynamic_cast<TextEditor*>(editor->child_at(1)); }
    ScrollView* scroll() const { return dynamic_cast<ScrollView*>(editor->child_at(2)); }
    ScrollView* under_menu() const { return dynamic_cast<ScrollView*>(editor->child_at(3)); }

    std::unique_ptr<FamilyMember> processor;
    pulp::state::StateStore store;
    std::unique_ptr<View> editor;
};

// A host that owns no window — `paint_root` is the headless paint entry point.
class HeadlessPaintHost : public WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return true; }
    void repaint() override {}
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}
};

// Deliver a left press through the real mouse path (how a host opens a combo).
void press(View& v, Point local) {
    MouseEvent e;
    e.position = local;
    e.window_position = local;
    e.button = MouseButton::left;
    e.is_down = true;
    e.phase = MousePhase::press;
    v.on_mouse_event(e);
}

// Every text command a paint recording emitted, concatenated. Enough to prove a
// recording carries THIS editor's label and not a sibling's.
std::string recorded_text(const RecordingCanvas& rec) {
    std::string all;
    for (const auto& c : rec.commands())
        if (c.type == DrawCommand::Type::fill_text ||
            c.type == DrawCommand::Type::stroke_text)
            all += c.text + "\n";
    return all;
}

// The process-global shim mirrors outlive any one case's local roots, so clear
// them between cases exactly as test_interaction_multiinstance.cpp does.
void reset_mirrors() {
    ComboBox::close_active_popup();
    View::focused_input_ = nullptr;
    View::active_overlay_ = nullptr;
}

// Build the family. Returned by value so each case gets three fresh instances.
std::array<std::unique_ptr<LoadedPlugin>, 3> load_family() {
    return {std::make_unique<LoadedPlugin>(std::make_unique<MidiFx>()),
            std::make_unique<LoadedPlugin>(std::make_unique<Instrument>()),
            std::make_unique<LoadedPlugin>(std::make_unique<AudioFx>())};
}

}  // namespace

// ── 1. DSP-side coexistence ─────────────────────────────────────────────────

TEST_CASE("three plugin instances in one process keep separate identity and state",
          "[format][view][multiinstance]") {
    auto family = load_family();

    // Distinct host-facing identity. A host that cannot tell the three apart
    // will hand one instance's state to another.
    std::vector<std::string> ids, names;
    for (auto& p : family) {
        ids.push_back(p->processor->descriptor().bundle_id);
        names.push_back(p->processor->descriptor().name);
    }
    REQUIRE(ids[0] != ids[1]);
    REQUIRE(ids[1] != ids[2]);
    REQUIRE(ids[0] != ids[2]);
    REQUIRE(names[0] != names[1]);
    REQUIRE(names[1] != names[2]);
    REQUIRE(names[0] != names[2]);
    REQUIRE(family[0]->processor->descriptor().category == PluginCategory::MidiEffect);
    REQUIRE(family[1]->processor->descriptor().category == PluginCategory::Instrument);
    REQUIRE(family[2]->processor->descriptor().category == PluginCategory::Effect);

    // Each instance owns its parameter value; writing one never moves another.
    family[0]->store.set_value(kAmount, 0.1f);
    family[1]->store.set_value(kAmount, 0.6f);
    family[2]->store.set_value(kAmount, 1.0f);
    CHECK(family[0]->store.get_value(kAmount) == 0.1f);
    CHECK(family[1]->store.get_value(kAmount) == 0.6f);
    CHECK(family[2]->store.get_value(kAmount) == 1.0f);

    // Hosts prepare each instance separately, often at different block sizes.
    family[0]->processor->prepare({.sample_rate = 44100.0, .max_buffer_size = 64});
    family[1]->processor->prepare({.sample_rate = 48000.0, .max_buffer_size = 256});
    family[2]->processor->prepare({.sample_rate = 96000.0, .max_buffer_size = 512});
    CHECK(family[0]->processor->prepared_rate() == 44100.0);
    CHECK(family[1]->processor->prepared_rate() == 48000.0);
    CHECK(family[2]->processor->prepared_rate() == 96000.0);
    CHECK(family[0]->processor->prepared_block() == 64);
    CHECK(family[1]->processor->prepared_block() == 256);
    CHECK(family[2]->processor->prepared_block() == 512);
    for (auto& p : family) CHECK(p->processor->prepare_count() == 1);
}

TEST_CASE("three plugin instances render concurrently without crossing streams",
          "[format][multiinstance]") {
    auto family = load_family();
    constexpr std::size_t kFrames = 64;
    constexpr int kBlocks = 200;

    const std::array<float, 3> amounts{0.1f, 0.6f, 1.0f};
    for (std::size_t i = 0; i < family.size(); ++i) {
        family[i]->store.set_value(kAmount, amounts[i]);
        family[i]->processor->prepare({.sample_rate = 48000.0,
                                       .max_buffer_size = static_cast<int>(kFrames)});
    }

    // Three audio threads, one per instance — the DAW's real shape. Each thread
    // owns its own buffers; the only thing they share is the process.
    std::atomic<int> mismatches{0};
    std::atomic<int> midi_faults{0};
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < family.size(); ++i) {
        threads.emplace_back([&, i] {
            std::array<float, kFrames> l{}, r{};
            std::array<float, kFrames> in_l{}, in_r{};
            std::array<float*, 2> out_ptrs{l.data(), r.data()};
            std::array<const float*, 2> in_ptrs{in_l.data(), in_r.data()};
            pulp::midi::MidiBuffer midi_in, midi_out;
            // Read the amount back rather than reusing the literal: the
            // assertion is about WHICH instance's mark and WHICH instance's
            // parameter reached the buffer, not about the store's rounding.
            const float expected =
                family[i]->processor->mark() * family[i]->store.get_value(kAmount);
            for (int b = 0; b < kBlocks; ++b) {
                pulp::audio::BufferView<float> out(out_ptrs.data(), 2, kFrames);
                const pulp::audio::BufferView<const float> in(in_ptrs.data(), 2, kFrames);
                midi_out.clear();
                family[i]->processor->process(out, in, midi_in, midi_out, ProcessContext{});
                for (std::size_t n = 0; n < kFrames; ++n)
                    if (l[n] != expected || r[n] != expected) ++mismatches;
                // Only the MIDI effect emits, and it emits its own note number.
                const bool is_midi_fx = i == 0;
                if (midi_out.size() != (is_midi_fx ? 1u : 0u)) ++midi_faults;
                if (is_midi_fx && !midi_out.empty() &&
                    midi_out.begin()->message.getNoteNumber() !=
                        static_cast<int>(family[i]->processor->mark()))
                    ++midi_faults;
            }
        });
    }
    for (auto& t : threads) t.join();

    // Every sample of every block carried the rendering instance's own mark and
    // its own parameter — no instance ever wrote another's buffer.
    CHECK(mismatches.load() == 0);
    // ...and the MIDI stream stayed with the one instance that produces one.
    CHECK(midi_faults.load() == 0);
}

// ── 2. Editor-side coexistence ──────────────────────────────────────────────

TEST_CASE("three editors open at once each paint their own content",
          "[view][multiinstance]") {
    reset_mirrors();
    auto family = load_family();
    for (auto& p : family) p->open_editor();

    HeadlessPaintHost host;
    std::array<RecordingCanvas, 3> recs;
    for (std::size_t i = 0; i < family.size(); ++i)
        host.paint_root(recs[i], *family[i]->editor);

    for (std::size_t i = 0; i < family.size(); ++i) {
        const std::string text = recorded_text(recs[i]);
        const std::string mine = family[i]->processor->display_name();
        INFO("editor " << i << " recorded: " << text);
        REQUIRE(text.find(mine) != std::string::npos);   // painted its own name
        for (std::size_t j = 0; j < family.size(); ++j)  // and nobody else's
            if (j != i)
                CHECK(text.find(family[j]->processor->display_name()) == std::string::npos);
    }
    reset_mirrors();
}

TEST_CASE("three editors open at once hold independent keyboard focus",
          "[view][input][multiinstance]") {
    reset_mirrors();
    auto family = load_family();
    for (auto& p : family) p->open_editor();

    // Typing into the middle editor must leave the outer two with no focused
    // input — the condition every macOS plugin host gates first-responder on.
    family[1]->field()->claim_input_focus();
    CHECK(focused_input_under_root(*family[0]->editor) == nullptr);
    CHECK(focused_input_under_root(*family[1]->editor) == family[1]->field());
    CHECK(focused_input_under_root(*family[2]->editor) == nullptr);

    // A second editor taking focus does not release the first's slot behind its
    // back; each root answers for itself.
    family[2]->field()->claim_input_focus();
    CHECK(focused_input_under_root(*family[1]->editor) == family[1]->field());
    CHECK(focused_input_under_root(*family[2]->editor) == family[2]->field());
    CHECK(focused_input_under_root(*family[0]->editor) == nullptr);

    reset_mirrors();
}

// ── 3. Pointer routing is scoped to the editor that got the event ───────────
//
// The regression guard. `ComboBox::active_popup_` is a process-global mirror
// naming whichever editor opened a dropdown MOST RECENTLY. The routing verbs
// used to read it directly, so with three editors live the mirror named one
// editor while the event belonged to another — and since all three editors have
// identical geometry (they are the same binary), the dropdown rect test passed
// and the event was delivered into a view tree the host did not own.

TEST_CASE("a wheel in one editor never scrolls another editor's open dropdown",
          "[view][input][multiinstance]") {
    reset_mirrors();
    auto family = load_family();
    for (auto& p : family) p->open_editor();

    // The MIDI effect opens a dropdown; its mirror now names that combo.
    press(*family[0]->combo(), {10, 14});
    REQUIRE(family[0]->combo()->is_open());
    REQUIRE(ComboBox::active_popup_ == family[0]->combo());

    // Pick a point that is inside editor 0's open menu AND over editor 2's
    // under-menu scroll pane. Because all three editors are the same binary
    // with the same layout, this is the everyday overlap, not a contrived one.
    const Point p{80, 90};
    float mx = 0, my = 0, mw = 0, mh = 0;
    REQUIRE(family[0]->combo()->dropdown_window_rect(mx, my, mw, mh));
    REQUIRE(p.x >= mx);
    REQUIRE(p.x <= mx + mw);
    REQUIRE(p.y >= my);
    REQUIRE(p.y <= my + mh);  // the premise: p really is inside editor 0's menu

    // Editor 2 receives the wheel, so editor 2's own pane must scroll. This is
    // the load-bearing assertion and it observes the TREE, not the routing
    // helper: reading the process-wide mirror sends the wheel into editor 0's
    // menu instead and editor 2's pane never moves.
    REQUIRE(family[2]->under_menu()->target_scroll_y() == 0.0f);
    deliver_mouse_wheel(*family[2]->editor, p, 0.0f, 40.0f, WheelHost{});
    CHECK(family[2]->under_menu()->target_scroll_y() > 0.0f);

    // Editor 0 is untouched: its popup is still open and its own pane, which
    // sits at the same coordinates in its own tree, never scrolled.
    CHECK(family[0]->combo()->is_open());
    CHECK(family[0]->under_menu()->target_scroll_y() == 0.0f);
    CHECK(ComboBox::active_popup_in(*family[0]->editor) == family[0]->combo());
    CHECK(ComboBox::active_popup_in(*family[2]->editor) == nullptr);

    reset_mirrors();
}

TEST_CASE("a wheel over one editor's own scroll pane still reaches it while another "
          "editor has a dropdown open",
          "[view][input][multiinstance]") {
    reset_mirrors();
    auto family = load_family();
    for (auto& p : family) p->open_editor();

    press(*family[0]->combo(), {10, 14});
    REQUIRE(family[0]->combo()->is_open());

    // Editor 2's scroll pane sits at x=200..400 — outside editor 0's menu rect,
    // which hangs below the combo at x=0..160. Wheel there must scroll editor 2.
    REQUIRE(family[2]->scroll()->target_scroll_y() == 0.0f);
    deliver_mouse_wheel(*family[2]->editor, {300, 150}, 0.0f, 40.0f, WheelHost{});
    CHECK(family[2]->scroll()->target_scroll_y() > 0.0f);

    // ...and scrolling editor 2 does NOT dismiss editor 0's open dropdown. A
    // process-wide close here is how a user's menu vanished when they touched
    // a different plugin.
    CHECK(family[0]->combo()->is_open());

    reset_mirrors();
}

TEST_CASE("scrolling one editor's pane closes only that editor's dropdown",
          "[view][input][multiinstance]") {
    reset_mirrors();
    auto family = load_family();
    for (auto& p : family) p->open_editor();

    // Open the SCROLLING editor's dropdown FIRST, so the process-global mirror
    // ends up naming the OTHER editor. Without that ordering the mirror happens
    // to name the scrolling editor's own popup and a process-wide close shuts
    // the right one by accident — the assertions would pass against the bug.
    press(*family[1]->combo(), {10, 14});
    press(*family[0]->combo(), {10, 14});
    REQUIRE(family[0]->combo()->is_open());
    REQUIRE(family[1]->combo()->is_open());
    REQUIRE(ComboBox::active_popup_ == family[0]->combo());

    // A scroll gesture dismisses the dropdown in ITS OWN editor only.
    family[1]->scroll()->scroll_by(0.0f, 30.0f, false);
    CHECK_FALSE(family[1]->combo()->is_open());
    CHECK(family[0]->combo()->is_open());

    reset_mirrors();
}

// ── 4. Editor churn with siblings live ──────────────────────────────────────

TEST_CASE("closing and reopening one editor leaves its two siblings intact",
          "[view][multiinstance]") {
    reset_mirrors();
    auto family = load_family();
    for (auto& p : family) p->open_editor();

    family[0]->field()->claim_input_focus();
    press(*family[2]->combo(), {10, 14});
    REQUIRE(family[2]->combo()->is_open());

    // Tear the middle editor down and rebuild it three times while the other
    // two stay open — the "user reopens the synth's window" loop. A slot that
    // held a pointer into the destroyed tree would surface here.
    for (int cycle = 0; cycle < 3; ++cycle) {
        family[1]->close_editor();
        CHECK(focused_input_under_root(*family[0]->editor) == family[0]->field());
        CHECK(family[2]->combo()->is_open());
        CHECK(ComboBox::active_popup_in(*family[2]->editor) == family[2]->combo());

        family[1]->open_editor();
        // The rebuilt editor starts clean: no focus, no popup.
        CHECK(focused_input_under_root(*family[1]->editor) == nullptr);
        CHECK(ComboBox::active_popup_in(*family[1]->editor) == nullptr);
        // ...and its siblings are still exactly where the user left them.
        CHECK(focused_input_under_root(*family[0]->editor) == family[0]->field());
        CHECK(family[2]->combo()->is_open());
    }

    // A rebuilt editor routes its own pointer normally.
    press(*family[1]->combo(), {10, 14});
    CHECK(family[1]->combo()->is_open());
    CHECK(family[2]->combo()->is_open());
    CHECK(ComboBox::active_popup_in(*family[1]->editor) == family[1]->combo());

    reset_mirrors();
}

TEST_CASE("destroying an editor with an open dropdown clears only its own slot",
          "[view][multiinstance]") {
    reset_mirrors();
    auto family = load_family();
    for (auto& p : family) p->open_editor();

    press(*family[0]->combo(), {10, 14});
    press(*family[1]->combo(), {10, 14});
    REQUIRE(ComboBox::active_popup_ == family[1]->combo());

    // Editor 1 dies while holding the process-global mirror. ~ComboBox clears
    // the mirror; editor 0's root slot must survive, and a later routing read
    // for editor 0 must still find editor 0's popup rather than a dangling one.
    family[1]->close_editor();
    CHECK(ComboBox::active_popup_ == nullptr);
    CHECK(family[0]->combo()->is_open());
    CHECK(ComboBox::active_popup_in(*family[0]->editor) == family[0]->combo());

    deliver_mouse_wheel(*family[0]->editor, {10, 40}, 0.0f, 40.0f, WheelHost{});
    CHECK(family[0]->combo()->is_open());

    reset_mirrors();
}
