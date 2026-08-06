#include <catch2/catch_test_macros.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/value_channel_set.hpp>
#include <pulp/view/widgets.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace pulp::state;
using namespace pulp::view;
namespace fs = std::filesystem;

namespace {

fs::path make_temp_dir(const char* stem) {
    const auto unique =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto dir = fs::temp_directory_path() / (std::string(stem) + "-" + unique);
    fs::create_directories(dir);
    return dir;
}

void write_text(const fs::path& path, const std::string& content) {
    std::ofstream file(path);
    file << content;
}

} // namespace

TEST_CASE("ScriptedUiSession uses bounded cleanup grace after evaluation timeout",
          "[view][scripted-ui][inspector][runtime-eval][deadline]") {
    const auto temp = make_temp_dir("pulp-scripted-ui-eval-reset-deadline");
    const auto script = temp / "ui.js";
    write_text(script, "enableInspectClick(); createLabel('status', 'ready', '');");

    View root;
    StateStore store;
    ScriptedUiSession session(root, store, {
        .script_path = script,
        .granted_capabilities = CapabilitySet{},
    });
    REQUIRE(session.load());
    REQUIRE(root.on_global_click);

    const auto result = session.script_inspector()->evaluate(
        "for (;;) {}", std::chrono::milliseconds::zero());
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.timed_out);
    REQUIRE(result.error.find("evaluation timed out") != std::string::npos);
    REQUIRE_FALSE(session.script_inspector()->capabilities().engine.empty());
    REQUIRE(session.bridge() != nullptr);

    // The reset owed by the timed-out evaluation is discharged at the frame
    // boundary and keeps its own fixed grace there — the runaway loop must not
    // be able to stretch the cleanup that follows it.
    const auto reset_started = std::chrono::steady_clock::now();
    std::string poll_error;
    session.poll(&poll_error);
    REQUIRE(poll_error.empty());
    REQUIRE(std::chrono::steady_clock::now() - reset_started
            < std::chrono::seconds(1));
    REQUIRE(session.bridge() != nullptr);
    REQUIRE(root.on_global_click);

    const auto recovered = session.script_inspector()->evaluate("40 + 2");
    REQUIRE(recovered.ok);
    REQUIRE(recovered.json == "42");

    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

TEST_CASE("Runtime evaluation cannot wedge cleanup or retain theme mutations",
          "[view][scripted-ui][inspector][runtime-eval][reset]") {
    const auto temp = make_temp_dir("pulp-scripted-ui-eval-hostile-cleanup");
    const auto script = temp / "ui.js";
    write_text(script, "enableInspectClick(); createLabel('status', 'ready', '');");

    View root;
    root.set_theme(Theme::dark());
    StateStore store;
    ScriptedUiSession session(root, store, {
        .script_path = script,
        .granted_capabilities = CapabilitySet{},
    });
    REQUIRE(session.load());
    REQUIRE(root.on_global_click);
    const auto last_good_background = root.theme().color("bg.primary");

    View other_root;
    auto other_combo_owner = std::make_unique<ComboBox>();
    auto* other_combo = other_combo_owner.get();
    other_combo->set_items({"other"});
    other_root.add_child(std::move(other_combo_owner));
    MouseEvent other_open_click;
    other_open_click.position = {1.0f, 1.0f};
    other_open_click.is_down = true;
    other_combo->on_mouse_event(other_open_click);
    REQUIRE(other_combo->is_open());

    const auto result = session.script_inspector()->evaluate(R"(
        setTheme('light');
        __forgetWidgetCallbacks__ = function () { for (;;) {} };
        40 + 2;
    )");
    REQUIRE(result.ok);
    REQUIRE(result.json == "42");

    // Cleanup runs at the frame boundary, and the hostile hook must not be able
    // to wedge it there: the theme mutation is reverted within the reset's own
    // bounded grace rather than whenever the evaluated code decides to yield.
    const auto started = std::chrono::steady_clock::now();
    session.poll();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(elapsed < std::chrono::seconds(1));
    REQUIRE(root.theme().color("bg.primary") == last_good_background);
    REQUIRE(session.bridge() != nullptr);
    REQUIRE(session.bridge()->widget("status") != nullptr);
    REQUIRE(root.on_global_click);
    REQUIRE(other_combo->is_open());

    const auto recovered = session.script_inspector()->evaluate("6 * 7");
    REQUIRE(recovered.ok);
    REQUIRE(recovered.json == "42");

    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

TEST_CASE("Runtime evaluation retires its realm no earlier than the next poll",
          "[view][scripted-ui][inspector][runtime-eval][reset][lifetime]") {
    struct DestructorProbe final : View {
        explicit DestructorProbe(int& calls) : calls_(calls) {}
        ~DestructorProbe() override { ++calls_; }
        int& calls_;
    };

    const auto temp = make_temp_dir("pulp-scripted-ui-eval-retired-realms");
    const auto script = temp / "ui.js";
    write_text(script, "createLabel('status', 'ready', '');");

    View root;
    StateStore store;
    ScriptedUiSession session(root, store, {
        .script_path = script,
        .granted_capabilities = CapabilitySet{},
    });
    REQUIRE(session.load());

    // Evaluations between two frames share one realm, so they owe one
    // reconstruction — and neither the evaluation nor the debt may destroy a
    // View inside the response fence.
    int destructor_calls = 0;
    root.add_child(std::make_unique<DestructorProbe>(destructor_calls));
    REQUIRE(session.script_inspector()->evaluate("1").ok);
    REQUIRE(destructor_calls == 0);

    root.add_child(std::make_unique<DestructorProbe>(destructor_calls));
    REQUIRE(session.script_inspector()->evaluate("2").ok);
    REQUIRE(destructor_calls == 0);

    session.poll();
    REQUIRE(destructor_calls == 2);

    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

TEST_CASE("retired realm destruction can reenter evaluation without corrupting its queue",
          "[view][scripted-ui][inspector][runtime-eval][reset][lifetime][reentrant]") {
    struct DestructorProbe final : View {
        explicit DestructorProbe(int& calls) : calls_(calls) {}
        ~DestructorProbe() override { ++calls_; }
        int& calls_;
    };
    struct ReentrantProbe final : View {
        explicit ReentrantProbe(std::function<void()> callback)
            : callback_(std::move(callback)) {}
        ~ReentrantProbe() override { callback_(); }
        std::function<void()> callback_;
    };

    const auto temp = make_temp_dir("pulp-scripted-ui-eval-reentrant-retirement");
    const auto script = temp / "ui.js";
    write_text(script, "createLabel('status', 'ready', '');");

    View root;
    StateStore store;
    ScriptedUiSession session(root, store, {
        .script_path = script,
        .granted_capabilities = CapabilitySet{},
    });
    REQUIRE(session.load());

    bool reentrant_ok = false;
    root.add_child(std::make_unique<ReentrantProbe>([&] {
        reentrant_ok = session.script_inspector()->evaluate("3").ok;
    }));
    REQUIRE(session.script_inspector()->evaluate("1").ok);

    // This poll discharges the debt: it rebuilds the realm, retires the old one
    // and destroys it — which re-enters evaluation from a widget releaser.
    session.poll();
    REQUIRE(reentrant_ok);
    // The re-entrant evaluation owes its own reset. The pass it ran inside had
    // already discharged the earlier debt, so this one must survive to the next
    // frame rather than be swallowed by the pass in progress.
    REQUIRE(session.script_inspector()->post_evaluation_reset_pending());

    int second_batch_destructions = 0;
    root.add_child(
        std::make_unique<DestructorProbe>(second_batch_destructions));
    session.poll();
    REQUIRE_FALSE(session.script_inspector()->post_evaluation_reset_pending());
    REQUIRE(second_batch_destructions == 1);

    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

TEST_CASE("Runtime reset reapplies cached theme override after source theme mutation",
          "[view][scripted-ui][inspector][runtime-eval][reset][theme]") {
    const auto temp = make_temp_dir("pulp-scripted-ui-eval-reset-theme-override");
    const auto script = temp / "ui.js";
    write_text(script, "setTheme('light'); createLabel('status', 'ready', '');");
    write_text(temp / "theme.json", R"({
        "colors": {
            "bg.primary": "#112233"
        }
    })");

    View root;
    root.set_theme(Theme::dark());
    StateStore store;
    ScriptedUiSession session(root, store, {
        .script_path = script,
        .granted_capabilities = CapabilitySet{},
    });
    REQUIRE(session.load());

    const auto overridden_background = root.theme().color("bg.primary");
    REQUIRE(overridden_background.has_value());
    REQUIRE(overridden_background->r8() == 0x11);
    REQUIRE(overridden_background->g8() == 0x22);
    REQUIRE(overridden_background->b8() == 0x33);

    // Deadline recovery must use the cached effective theme without touching
    // the filesystem; a newer on-disk value belongs to normal reload polling.
    // Both now run inside the same poll(), so pin the file's write time to what
    // the session cached at load: the theme poll then has nothing to report and
    // any #abcdef in the live theme can only have come from the reset reading a
    // file it must not read.
    const auto pinned_write_time = fs::last_write_time(temp / "theme.json");
    write_text(temp / "theme.json", R"({
        "colors": {
            "bg.primary": "#abcdef"
        }
    })");
    fs::last_write_time(temp / "theme.json", pinned_write_time);

    const auto result = session.script_inspector()->evaluate("40 + 2");
    REQUIRE(result.ok);
    REQUIRE(result.json == "42");
    session.poll();

    const auto reset_background = root.theme().color("bg.primary");
    REQUIRE(reset_background.has_value());
    REQUIRE(reset_background->r8() == 0x11);
    REQUIRE(reset_background->g8() == 0x22);
    REQUIRE(reset_background->b8() == 0x33);

    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

TEST_CASE("Runtime reset quarantines a partially built replacement until owner teardown",
          "[view][scripted-ui][inspector][runtime-eval][reset][quarantine]") {
    const auto temp = make_temp_dir("pulp-scripted-ui-eval-partial-realm");
    const auto script = temp / "ui.js";
    write_text(script, R"(
        createLabel('partial', 'first', '');
        createLabel('partial', 'second', '');
        createLabel('', 'empty-first', '');
        createLabel('', 'empty-second', '');
        if (listValueChannels().length) throw new Error('live-only failure');
    )");

    View root;
    root.set_visible(true);
    StateStore store;
    ValueChannelSet channels;
    REQUIRE(channels.declare_scalar("meter") != nullptr);
    int live_channel_visits = 0;
    {
        auto session = std::make_unique<ScriptedUiSession>(root, store, ScriptedUiOptions{
            .script_path = script,
            .value_channel_access = [&](const ValueChannelVisitor& visitor) {
                ++live_channel_visits;
                visitor(live_channel_visits == 1 ? nullptr : &channels);
            },
            .granted_capabilities = CapabilitySet{},
        });
        REQUIRE(session->load());
        REQUIRE(session->bridge() != nullptr);
        int repaint_requests = 0;
        session->set_repaint_callback([&] { ++repaint_requests; });

        // The evaluation itself succeeds and the realm it ran in stays live
        // until the frame boundary; the reset that cannot rebuild it fails
        // there, and poll() is what reports that failure now.
        const auto result = session->script_inspector()->evaluate("1");
        REQUIRE(result.ok);
        REQUIRE(session->bridge() != nullptr);

        std::string poll_error;
        REQUIRE_FALSE(session->poll(&poll_error));
        REQUIRE(poll_error.find("evaluated realm reset failed") != std::string::npos);
        REQUIRE(session->bridge() == nullptr);
        REQUIRE_FALSE(root.visible());
        REQUIRE(repaint_requests == 1);
        REQUIRE_FALSE(session->poll());
        session->attach_gpu_surface(nullptr);

        const auto replacement_script = temp / "replacement.js";
        write_text(replacement_script,
                   "enableInspectClick();"
                   "createCombo('partial', '');"
                   "setItems('partial', ['ready']);");
        auto replacement = std::make_unique<ScriptedUiSession>(
            root, store, ScriptedUiOptions{
                .script_path = replacement_script,
                .granted_capabilities = CapabilitySet{},
            });
        REQUIRE(replacement->load());
        REQUIRE(root.child_count() == 5);
        REQUIRE(static_cast<bool>(root.on_global_click));
        auto* replacement_combo =
            dynamic_cast<ComboBox*>(replacement->bridge()->widget("partial"));
        REQUIRE(replacement_combo != nullptr);
        MouseEvent open_click;
        open_click.position = {1.0f, 1.0f};
        open_click.is_down = true;
        replacement_combo->on_mouse_event(open_click);
        REQUIRE(replacement_combo->is_open());

        session.reset();
        REQUIRE(root.visible());
        REQUIRE(root.child_count() == 1);
        REQUIRE(replacement->bridge()->widget("partial") == replacement_combo);
        REQUIRE(static_cast<bool>(root.on_global_click));
        REQUIRE(replacement_combo->is_open());

        replacement->bridge()->clear_for_realm_replacement();
        replacement.reset();
    }

    REQUIRE(root.visible());
    REQUIRE(root.child_count() == 0);
    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

TEST_CASE("Runtime reset pairs last-good source with its original script base",
          "[view][scripted-ui][inspector][runtime-eval][reset][assets]") {
    const auto temp = make_temp_dir("pulp-scripted-ui-eval-last-good-base");
    const auto good_dir = temp / "good";
    const auto failed_dir = temp / "failed";
    fs::create_directories(good_dir);
    fs::create_directories(failed_dir);
    write_text(good_dir / "ui.js", "createLabel('status', 'ready', '');\n");

    View root;
    StateStore store;
    ScriptedUiSession session(
        root, store,
        {.script_path = good_dir / "ui.js",
         .granted_capabilities = CapabilitySet{}});
    std::string error;
    REQUIRE(session.load(&error));

    REQUIRE_FALSE(session.reload_from(failed_dir / "missing.js", &error));
    REQUIRE(session.script_path() == failed_dir / "missing.js");
    const auto result = session.script_inspector()->evaluate("1");
    REQUIRE(result.ok);
    session.poll();

    REQUIRE(session.bridge()->widget("status") != nullptr);
    REQUIRE(session.bridge()->script_base_dir() == fs::absolute(good_dir));

    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

TEST_CASE("Ordinary reload retries after a live-only provisional failure",
          "[view][scripted-ui][reload][retry][value-channels]") {
    const auto temp = make_temp_dir("pulp-scripted-ui-reload-live-only-retry");
    const auto script = temp / "ui.js";
    write_text(script, "createLabel('status', 'initial', '');");

    View root;
    StateStore store;
    ValueChannelSet channels;
    REQUIRE(channels.declare_scalar("meter") != nullptr);
    ScriptedUiSession session(root, store, ScriptedUiOptions{
        .script_path = script,
        .value_channel_access = [&](const ValueChannelVisitor& visitor) {
            visitor(&channels);
        },
        .granted_capabilities = CapabilitySet{},
    });
    REQUIRE(session.load());

    write_text(script, R"(
        createLabel('partial', 'failed', '');
        __forgetWidgetCallbacks__ = function () { for (;;) {} };
        if (listValueChannels().length) throw new Error('live-only failure');
    )");
    std::string error;
    REQUIRE_FALSE(session.reload(&error));
    REQUIRE(error.find("live-only failure") != std::string::npos);
    REQUIRE(session.bridge() == nullptr);

    write_text(script, "createLabel('status', 'recovered', '');");
    error.clear();
    REQUIRE(session.reload(&error));
    REQUIRE(error.empty());
    REQUIRE(session.bridge() != nullptr);
    REQUIRE(session.bridge()->widget("status") != nullptr);

    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

TEST_CASE("Ordinary reload detaches views before retaining their provider lifetime",
          "[view][scripted-ui][reload][lifetime][detach]") {
    struct DetachProbe final : View {
        DetachProbe(int& detaches, int& destructions)
            : detaches_(detaches), destructions_(destructions) {}
        ~DetachProbe() override { ++destructions_; }
        void on_detached() override { ++detaches_; }
        int& detaches_;
        int& destructions_;
    };

    const auto temp = make_temp_dir("pulp-scripted-ui-reload-detach");
    const auto script = temp / "ui.js";
    write_text(script, "createLabel('status', 'initial', '');");

    View root;
    StateStore store;
    ScriptedUiSession session(root, store, {
        .script_path = script,
        .granted_capabilities = CapabilitySet{},
    });
    REQUIRE(session.load());

    int detaches = 0;
    int destructions = 0;
    root.add_child(std::make_unique<DetachProbe>(detaches, destructions));
    write_text(script, "createLabel('status', 'replacement', '');");
    REQUIRE(session.reload());
    CHECK(detaches == 1);
    CHECK(destructions == 0);

    session.poll();
    CHECK(destructions == 1);

    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

namespace {

// A slider bound to a parameter through a script-registered change callback —
// the shape a generated plugin panel uses, and the shape a caller holds a raw
// widget pointer to across an inspector evaluation.
constexpr const char* kParamBoundSliderScript =
    "createRangeSlider('scrub', '');"
    "setMin('scrub', 0);"
    "setMax('scrub', 1);"
    "bindWidgetToParam('scrub', 'gain');"
    "on('scrub', 'change', function (v) { setParam('gain', v); });";

void click_slider(RangeSlider& slider, float x) {
    MouseEvent event;
    event.position = {x, 12.0f};
    event.is_down = true;
    slider.on_mouse_event(event);
}

} // namespace

TEST_CASE("an inspector evaluation leaves a registered change binding live",
          "[view][scripted-ui][inspector][runtime-eval][binding]") {
    const auto temp = make_temp_dir("pulp-scripted-ui-eval-live-binding");
    const auto script = temp / "ui.js";
    write_text(script, kParamBoundSliderScript);

    View root;
    root.set_bounds({0.0f, 0.0f, 400.0f, 300.0f});
    StateStore store;
    store.add_parameter({.id = 1, .name = "gain", .range = {0.0f, 1.0f, 0.0f}});

    ScriptedUiSession session(root, store, {
        .script_path = script,
        .granted_capabilities = CapabilitySet{},
    });
    REQUIRE(session.load());
    root.layout_children();

    auto* slider = dynamic_cast<RangeSlider*>(session.bridge()->widget("scrub"));
    REQUIRE(slider != nullptr);
    // A script-created widget lays out with no height under a headless root, so
    // a click at its centre would land outside it and prove nothing.
    slider->set_bounds({0.0f, 0.0f, 200.0f, 24.0f});

    // POSITIVE CONTROL. Without it this case would also pass on a harness that
    // could never move a parameter at all.
    const float before_eval = store.get_value(1);
    click_slider(*slider, 150.0f);
    const float after_first_click = store.get_value(1);
    INFO("first click " << before_eval << " -> " << after_first_click);
    REQUIRE(after_first_click != before_eval);

    // Any evaluation at all. The claim under test is about what the realm reset
    // does to the caller's pointer, not about what the expression computes.
    REQUIRE(session.script_inspector()->evaluate("1").ok);

    click_slider(*slider, 50.0f);
    INFO("after eval " << after_first_click << " -> " << store.get_value(1));
    CHECK(store.get_value(1) != after_first_click);

    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

TEST_CASE("The evaluated realm is discarded at the frame boundary",
          "[view][scripted-ui][inspector][runtime-eval][reset][containment]") {
    const auto temp = make_temp_dir("pulp-scripted-ui-eval-frame-boundary");
    const auto script = temp / "ui.js";
    write_text(script, kParamBoundSliderScript);

    View root;
    root.set_bounds({0.0f, 0.0f, 400.0f, 300.0f});
    StateStore store;
    store.add_parameter({.id = 1, .name = "gain", .range = {0.0f, 1.0f, 0.0f}});

    ScriptedUiSession session(root, store, {
        .script_path = script,
        .granted_capabilities = CapabilitySet{},
    });
    REQUIRE(session.load());
    auto* before_eval = session.bridge()->widget("scrub");
    REQUIRE(before_eval != nullptr);

    REQUIRE(session.script_inspector()->evaluate("globalThis.planted = 7; 1").ok);
    REQUIRE(session.script_inspector()->post_evaluation_reset_pending());
    // Still the caller's realm: the pointer it holds is the live one, and a
    // global the evaluation planted is still visible to the next evaluation,
    // because no frame has run to discard the realm they share.
    REQUIRE(session.bridge()->widget("scrub") == before_eval);
    const auto shared_realm =
        session.script_inspector()->evaluate("typeof globalThis.planted");
    REQUIRE(shared_realm.ok);
    CHECK(shared_realm.json == "\"number\"");

    session.poll();

    // The frame boundary discarded the realm that ran the evaluations, so
    // nothing they planted survives into the frame that follows.
    REQUIRE_FALSE(session.script_inspector()->post_evaluation_reset_pending());
    REQUIRE(session.bridge() != nullptr);
    auto* after_poll = session.bridge()->widget("scrub");
    REQUIRE(after_poll != nullptr);
    CHECK(after_poll != before_eval);
    const auto planted = session.script_inspector()->evaluate("typeof globalThis.planted");
    REQUIRE(planted.ok);
    CHECK(planted.json == "\"undefined\"");

    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

TEST_CASE("ScriptedUiSession destruction contains throwing descendant detach hooks",
          "[view][scripted-ui][lifetime][exception-safety]") {
    struct ThrowingDetach final : View {
        void on_detached() override {
            throw std::runtime_error("detach failed");
        }
    };

    const auto temp = make_temp_dir("pulp-scripted-ui-destroy-detach");
    const auto script = temp / "ui.js";
    write_text(script, "createLabel('status', 'ready', '');");

    View root;
    StateStore store;
    {
        ScriptedUiSession session(root, store, {
            .script_path = script,
            .granted_capabilities = CapabilitySet{},
        });
        REQUIRE(session.load());
        auto* status = session.bridge()->widget("status");
        REQUIRE(status != nullptr);
        status->add_child(std::make_unique<ThrowingDetach>());
    }

    CHECK(root.child_count() == 0);
    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
}

