// pulp-ui-frames-observe — replay a fixed frame schedule against a native
// view tree and record what every frame looked like.
//
// A final-frame comparison cannot see timing: an animation that runs at the
// wrong rate, a document that mounts a frame late, or a parameter that reaches
// a second editor one frame after the first all converge on the same last
// frame. This observer drives the tree the way a host render loop does
// (HostFramePump + begin_host_frame / advance_host_frame), but from host
// timestamps the schedule supplies, so the clock is fixed and two runs see the
// same dt sequence. Every frame records the dispatch decision, the measured dt,
// readiness, focus, control and animation state, open menus, parameter values,
// and digests of the laid-out tree and the rendered pixels.
//
// Inputs: a serialized DesignIR (materialized and bound to a StateStore by
// parameter key) or a scripted UI (one ScriptedUiSession per attached view,
// loaded view-first like a hosted editor or immediately like a standalone
// one). The schedule declares the host timestamps, the parameters, the initial
// view count, timed input/host events, and which frames to write as PNGs.
//
// Exit codes: 0 observed, 1 the input could not be observed, 2 usage error,
// 77 this build has no raster backend (a skip, never a pass).

#include <pulp/runtime/crypto.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/context_menu.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/host_frame_pump.hpp>
#include <pulp/view/layout_snapshot.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/widgets.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace pulp;

constexpr int kExitSkipped = 77;
constexpr const char* kSchema = "pulp-ui-frames-v1";
constexpr const char* kScheduleSchema = "pulp-ui-frame-schedule-v1";

std::string read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool write_text(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text << '\n';
    return static_cast<bool>(output);
}

void usage() {
    std::cerr << "Usage: pulp-ui-frames-observe (--design-ir <file> | --script <ui.js>) "
                 "--schedule <schedule.json> --width <px> --height <px> [--scale <factor>] "
                 "[--timer-clock schedule|wall] --out-dir <dir>\n";
}

std::optional<std::pair<view::KeyCode, std::uint16_t>> parse_key(std::string_view text) {
    using view::KeyCode;
    std::uint16_t modifiers = 0;
    constexpr std::string_view shift_prefix = "shift+";
    if (text.substr(0, shift_prefix.size()) == shift_prefix) {
        modifiers = view::kModShift;
        text.remove_prefix(shift_prefix.size());
    }
    static const std::pair<std::string_view, KeyCode> named[] = {
        {"tab", KeyCode::tab},     {"enter", KeyCode::enter},     {"escape", KeyCode::escape},
        {"space", KeyCode::space}, {"left", KeyCode::left},       {"right", KeyCode::right},
        {"up", KeyCode::up},       {"down", KeyCode::down},       {"home", KeyCode::home},
        {"end", KeyCode::end_},    {"page_up", KeyCode::page_up}, {"page_down", KeyCode::page_down},
    };
    for (const auto& [name, code] : named)
        if (text == name)
            return std::pair{code, modifiers};
    if (text.size() == 1 &&
        ((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= '0' && text[0] <= '9')))
        return std::pair{static_cast<KeyCode>(text[0]), modifiers};
    return std::nullopt;
}

// A view's addressing handle: the DesignIR anchor when it has one, otherwise
// the scripted widget id.
std::string label_of(const view::View& v) {
    return v.anchor_id().empty() ? v.id() : v.anchor_id();
}

std::string labelled_ancestor(const view::View* v) {
    for (; v; v = v->parent())
        if (!label_of(*v).empty())
            return label_of(*v);
    return {};
}

view::View* find_focused(view::View& root) {
    if (root.has_focus())
        return &root;
    for (std::size_t i = 0; i < root.child_count(); ++i)
        if (auto* found = find_focused(*root.child_at(i)))
            return found;
    return nullptr;
}

// ── parameters: one StateStore shared by every attached view ──────────────

state::ParamID param_id(const state::StateStore& store, std::string_view name) {
    for (const auto& info : store.all_params())
        if (info.name == name)
            return info.id;
    return 0;
}

// Binds materialized DesignIR controls to the store by parameter key and keeps
// them in step with it, the way a host editor re-asserts parameter values every
// frame. A control mid-gesture is left alone: the gesture wins.
class StoreBinding final : public view::NativeImportBindingContext,
                           public view::NativeImportComboBoxBindingContext {
  public:
    explicit StoreBinding(state::StateStore& store) : store_(store) {}

    void bind_knob(view::Knob& knob, const view::NativeImportBindingDescriptor& d) override {
        attach_scalar(knob, d.param_key);
    }
    void bind_fader(view::Fader& fader, const view::NativeImportBindingDescriptor& d) override {
        attach_scalar(fader, d.param_key);
    }
    void bind_combo_box(view::ComboBox& combo,
                        const view::NativeImportSegmentedBindingDescriptor& d) override {
        const auto id = resolve(d.param_key);
        if (id == 0)
            return;
        const int count = d.segment_count;
        combo.on_change = [this, id, count](int index) {
            store_.set_normalized(id, view::selector_segment_value(index, count));
        };
        sync_.push_back([this, &combo, id, count] {
            if (!combo.is_open())
                combo.set_selected_silent(
                    view::selector_segment_index(store_.get_normalized(id), count));
        });
    }

    void sync() {
        for (auto& f : sync_)
            f();
    }
    const std::vector<std::pair<std::string, bool>>& resolutions() const {
        return resolutions_;
    }

  private:
    state::ParamID resolve(std::string_view key) {
        const auto id = param_id(store_, key);
        resolutions_.emplace_back(std::string(key), id != 0);
        return id;
    }

    template <typename Control> void attach_scalar(Control& control, std::string_view key) {
        const auto id = resolve(key);
        if (id == 0)
            return;
        control.on_change = [this, id](float v) { store_.set_normalized(id, v); };
        sync_.push_back([this, &control, id] {
            if (!control.is_gesture_active())
                control.set_value(store_.get_normalized(id));
        });
    }

    state::StateStore& store_;
    std::vector<std::function<void()>> sync_;
    std::vector<std::pair<std::string, bool>> resolutions_;
};

// ── observed views ─────────────────────────────────────────────────────────

struct ObservedView {
    int index = 0;
    bool attached = false;
    int attached_frame = 0;
    std::optional<int> detached_frame;
    float width = 0.0f;
    float height = 0.0f;
    std::unique_ptr<view::View> root;
    std::unique_ptr<view::FrameClock> clock;
    std::unique_ptr<view::HostFramePump> pump;
    std::unique_ptr<view::ScriptedUiSession> session;
    std::unique_ptr<StoreBinding> binding;
    bool dirty = true;
    bool last_continuous = false;
    std::uint64_t damage_seen = 0;
    int loaded_callbacks = 0;
    std::optional<int> loaded_frame;
    bool loaded_ok = false;
    std::string loaded_error;
    std::string poll_error;
};

struct Options {
    fs::path design_ir;
    fs::path script;
    fs::path schedule;
    fs::path out_dir;
    float width = 0.0f;
    float height = 0.0f;
    float scale = 2.0f;
    // "schedule": script timers and performance.now() read the schedule's host
    // clock, so a timer fires on the frame whose timestamp passes its deadline.
    // "wall": they read the steady clock, as in a real host.
    std::string timer_clock = "schedule";
};

struct Event {
    int frame = 0;
    int view = 0;
    std::string kind;
    choc::value::Value spec;
};

struct Schedule {
    std::vector<double> timestamps;
    float nominal_dt = view::HostFramePump::kDefaultNominalDt;
    std::string readiness = "deferred";
    int initial_views = 1;
    std::vector<Event> events;
    std::vector<int> capture;
};

std::optional<Schedule> parse_schedule(const std::string& text, state::StateStore& store,
                                       std::string& error);

class Observer {
  public:
    Observer(Options options, view::DesignIR ir)
        : options_(std::move(options)), ir_(std::move(ir)) {}

    bool load_schedule(const std::string& text, std::string& error) {
        auto parsed = parse_schedule(text, store_, error);
        if (!parsed)
            return false;
        schedule_ = std::move(*parsed);
        return true;
    }

    bool attach_view(int frame, std::string& error) {
        auto ov = std::make_unique<ObservedView>();
        ov->index = static_cast<int>(views_.size());
        ov->attached = true;
        ov->attached_frame = frame;
        ov->width = options_.width;
        ov->height = options_.height;
        ov->clock = std::make_unique<view::FrameClock>();
        ov->pump = std::make_unique<view::HostFramePump>();
        ov->pump->set_nominal_dt(schedule_.nominal_dt);
        if (!options_.script.empty()) {
            ov->root = std::make_unique<view::View>();
            ov->root->set_theme(view::Theme::dark());
            ov->root->set_bounds({0.0f, 0.0f, ov->width, ov->height});
            ov->root->set_frame_clock(ov->clock.get());
            ov->session = std::make_unique<view::ScriptedUiSession>(
                *ov->root, store_,
                view::ScriptedUiOptions{.script_path = options_.script,
                                        .enable_hot_reload = false,
                                        .enable_theme_reload = false});
            if (options_.timer_clock == "schedule") {
                // A fixed origin, offset by the scheduled host time: two runs
                // of one schedule see identical script time.
                ov->session->set_time_source([this] {
                    return std::chrono::steady_clock::time_point{} + std::chrono::hours(1) +
                           std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                               std::chrono::duration<double>(current_timestamp_ -
                                                             schedule_.timestamps.front()));
                });
            }
            auto* raw = ov.get();
            ov->session->set_document_loaded_callback(
                [this, raw](bool loaded, const std::string& why) {
                    ++raw->loaded_callbacks;
                    if (!raw->loaded_frame)
                        raw->loaded_frame = current_frame_;
                    raw->loaded_ok = loaded;
                    raw->loaded_error = why;
                });
            bool started = false;
            if (schedule_.readiness == "deferred") {
                // A hosted editor's view-creation call: the document mounts
                // from a later poll(), and the first frames precede it.
                view::ScopedDeferredDocumentLoad guard;
                started = ov->session->load_deferrable(&error);
            } else {
                started = ov->session->load(&error);
            }
            if (!started)
                return false;
        } else {
            ov->root = view::build_native_view_tree(
                ir_, ir_.asset_manifest,
                {.asset_base_directory = options_.design_ir.parent_path()});
            if (!ov->root) {
                error = "could not materialize DesignIR";
                return false;
            }
            ov->root->set_bounds({0.0f, 0.0f, ov->width, ov->height});
            ov->root->set_frame_clock(ov->clock.get());
            ov->binding = std::make_unique<StoreBinding>(store_);
            view::bind_native_view_tree(*ov->root, ir_, *ov->binding);
        }
        views_.push_back(std::move(ov));
        return true;
    }

    choc::value::Value apply(const Event& event, std::string& error) {
        auto record = choc::value::createObject("");
        record.addMember("kind", choc::value::createString(event.kind));
        record.addMember("view", choc::value::createInt64(event.view));
        if (event.kind == "attach_view") {
            const bool ok = attach_view(current_frame_, error);
            record.addMember("attached", choc::value::createBool(ok));
            record.addMember(
                "new_view", choc::value::createInt64(static_cast<std::int64_t>(views_.size()) - 1));
            return record;
        }
        if (event.kind == "set_param") {
            // Host automation: the value arrives through the store, not a view.
            const auto name = event.spec["name"].getWithDefault<std::string>("");
            const auto id = param_id(store_, name);
            record.addMember("name", choc::value::createString(name));
            record.addMember("resolved", choc::value::createBool(id != 0));
            if (id != 0)
                store_.set_normalized(id,
                                      static_cast<float>(event.spec["value"].getWithDefault(0.0)));
            for (auto& v : views_)
                v->dirty = true;
            return record;
        }
        if (event.view < 0 || event.view >= static_cast<int>(views_.size()) ||
            !views_[event.view]->attached) {
            error = "event targets a view that is not attached: " + event.kind;
            return record;
        }
        auto& ov = *views_[event.view];
        auto& root = *ov.root;
        ov.dirty = true;
        const auto point = [&](const char* x, const char* y) {
            return view::Point{static_cast<float>(event.spec[x].getWithDefault(0.0)),
                               static_cast<float>(event.spec[y].getWithDefault(0.0))};
        };
        const view::View* target = nullptr;
        bool handled = false;
        if (event.kind == "detach_view") {
            if (ov.session)
                ov.session->release_root();
            ov.session.reset();
            ov.binding.reset();
            ov.root.reset();
            ov.attached = false;
            ov.detached_frame = current_frame_;
            record.addMember("detached", choc::value::createBool(true));
            return record;
        } else if (event.kind == "resize") {
            ov.width = static_cast<float>(event.spec["width"].getWithDefault(ov.width));
            ov.height = static_cast<float>(event.spec["height"].getWithDefault(ov.height));
            root.set_bounds({0.0f, 0.0f, ov.width, ov.height});
            handled = true;
        } else if (event.kind == "hover") {
            const auto p = point("x", "y");
            target = root.hit_test(p);
            root.simulate_hover(p);
            handled = target != nullptr && target != &root;
        } else if (event.kind == "click") {
            const auto p = point("x", "y");
            target = root.hit_test(p);
            root.simulate_click(p);
            handled = target != nullptr && target != &root;
        } else if (event.kind == "drag") {
            const auto from = point("x", "y");
            target = root.hit_test(from);
            root.simulate_drag(from, point("to_x", "to_y"),
                               static_cast<int>(event.spec["steps"].getWithDefault<int64_t>(10)));
            handled = target != nullptr && target != &root;
        } else if (event.kind == "context_click") {
            const auto p = point("x", "y");
            target = root.hit_test(p);
            handled = root.simulate_context_click(p);
        } else if (event.kind == "key") {
            const auto name = event.spec["key"].getWithDefault<std::string>("");
            record.addMember("key", choc::value::createString(name));
            const auto key = parse_key(name);
            if (!key) {
                error = "unsupported key: " + name;
                return record;
            }
            auto* focused = find_focused(root);
            if (key->first == view::KeyCode::tab) {
                auto* next = (key->second & view::kModShift)
                                 ? view::View::focus_prev(root, focused)
                                 : view::View::focus_next(root, focused);
                if (next)
                    next->on_focus_changed(true);
                if (focused && focused != next)
                    focused->on_focus_changed(false);
                handled = next != nullptr;
                target = next;
            } else if (focused) {
                view::KeyEvent down;
                down.key = key->first;
                down.modifiers = key->second;
                handled = focused->on_key_event(down);
                view::KeyEvent up = down;
                up.is_down = false;
                focused->on_key_event(up);
                target = focused;
            }
        } else {
            error = "unknown event kind: " + event.kind;
            return record;
        }
        record.addMember("target", choc::value::createString(labelled_ancestor(target)));
        record.addMember("handled", choc::value::createBool(handled));
        return record;
    }

    // One host tick for one view: poll the session, re-assert bound values,
    // then the HostFramePump contract — skip a vsync that has nothing to do
    // (noting it on the pump, as the real hosts do), otherwise measure dt and
    // advance every time-integrating consumer by it.
    choc::value::Value tick(ObservedView& ov, double timestamp) {
        auto record = choc::value::createObject("");
        record.addMember("view", choc::value::createInt64(ov.index));
        if (ov.session && !ov.session->poll(&ov.poll_error))
            record.addMember("poll_error", choc::value::createString(ov.poll_error));
        if (ov.binding)
            ov.binding->sync();
        ov.root->layout_children();
        // Repaint requests are how a host learns its tree changed (a bound
        // control picking up another editor's write, a script setter). With
        // no window host attached they only bump the process-wide damage
        // counter, so any movement since this view's last tick marks it
        // dirty. Over-approximate across views, never under.
        const auto damage =
            view::View::damage_request_count() + view::View::unmarked_paint_mutation_count();
        if (damage != ov.damage_seen)
            ov.dirty = true;
        const bool idle_callback = ov.session != nullptr;
        const bool dispatched =
            view::should_dispatch_host_frame(*ov.pump, ov.dirty, ov.last_continuous, idle_callback);
        record.addMember("dispatched", choc::value::createBool(dispatched));
        if (dispatched) {
            const auto t =
                view::begin_host_frame(ov.root.get(), *ov.clock, *ov.pump, timestamp, ov.dirty);
            if (t.should_render)
                view::advance_host_frame(ov.root.get(), *ov.clock, t.dt);
            // The hosts ask again after advancing: an animation the advance
            // just started (or just finished) decides whether the NEXT vsync
            // is dispatched, so the value from the top of the frame is stale.
            ov.last_continuous =
                ov.clock->has_active_subscribers() || view::needs_continuous_frames(ov.root.get());
            record.addMember("dt", choc::value::createFloat64(t.dt));
            record.addMember("resumed", choc::value::createBool(t.resumed));
            record.addMember("rendered", choc::value::createBool(t.should_render));
            record.addMember("continuous", choc::value::createBool(t.continuous));
            record.addMember("continuous_after", choc::value::createBool(ov.last_continuous));
        }
        ov.dirty = false;
        ov.damage_seen =
            view::View::damage_request_count() + view::View::unmarked_paint_mutation_count();
        record.addMember("clock_time", choc::value::createFloat64(ov.clock->time()));
        record.addMember("readiness", readiness(ov));
        const auto* focused = find_focused(*ov.root);
        record.addMember("focused", choc::value::createString(labelled_ancestor(focused)));
        auto state = choc::value::createEmptyArray();
        collect_state(*ov.root, state);
        record.addMember("state", state);
        record.addMember("layout_sha256",
                         choc::value::createString(runtime::sha256_hex(
                             view::dump_layout_tree(*ov.root, {.surface = "ui-frames",
                                                               .viewport_width = ov.width,
                                                               .viewport_height = ov.height}))));
        std::uint32_t w = 0;
        std::uint32_t h = 0;
        const auto pixels =
            view::render_to_rgba(*ov.root, static_cast<std::uint32_t>(ov.width),
                                 static_cast<std::uint32_t>(ov.height), options_.scale, &w, &h);
        record.addMember("pixels_sha256",
                         choc::value::createString(
                             pixels.empty() ? std::string{}
                                            : runtime::sha256_hex(pixels.data(), pixels.size())));
        record.addMember("pixel_size",
                         choc::value::createString(std::to_string(w) + "x" + std::to_string(h)));
        return record;
    }

    static choc::value::Value readiness(const ObservedView& ov) {
        auto r = choc::value::createObject("");
        if (!ov.session) {
            r.addMember("mode", choc::value::createString("design-ir"));
            r.addMember("ready", choc::value::createBool(true));
            return r;
        }
        r.addMember("mode", choc::value::createString("script"));
        r.addMember("document_load_pending",
                    choc::value::createBool(ov.session->document_load_pending()));
        r.addMember("settling", choc::value::createBool(ov.session->settling()));
        r.addMember("loaded_callbacks", choc::value::createInt64(ov.loaded_callbacks));
        r.addMember("loaded_frame", ov.loaded_frame ? choc::value::createInt64(*ov.loaded_frame)
                                                    : choc::value::Value{});
        r.addMember("loaded_ok", choc::value::createBool(ov.loaded_ok));
        r.addMember("loaded_error", choc::value::createString(ov.loaded_error));
        r.addMember("ready", choc::value::createBool(ov.loaded_callbacks > 0 && ov.loaded_ok &&
                                                     !ov.session->document_load_pending() &&
                                                     !ov.session->settling()));
        return r;
    }

    // Observable state in tree order: control values, the scalars the built-in
    // widget animations integrate, menu state, and label text (which is how a
    // late-mounting document shows up).
    static void collect_state(const view::View& v, choc::value::Value& out) {
        auto entry = choc::value::createObject("");
        bool keep = true;
        if (auto* knob = dynamic_cast<const view::Knob*>(&v)) {
            entry.addMember("kind", choc::value::createString("knob"));
            entry.addMember("value", choc::value::createFloat64(knob->value()));
            entry.addMember("hover_glow", choc::value::createFloat64(knob->hover_glow()));
        } else if (auto* fader = dynamic_cast<const view::Fader*>(&v)) {
            entry.addMember("kind", choc::value::createString("fader"));
            entry.addMember("value", choc::value::createFloat64(fader->value()));
            entry.addMember("hover_scale", choc::value::createFloat64(fader->hover_scale()));
        } else if (auto* toggle = dynamic_cast<const view::Toggle*>(&v)) {
            entry.addMember("kind", choc::value::createString("toggle"));
            entry.addMember("on", choc::value::createBool(toggle->is_on()));
            entry.addMember("thumb_position", choc::value::createFloat64(toggle->thumb_position()));
        } else if (auto* button = dynamic_cast<const view::ToggleButton*>(&v)) {
            entry.addMember("kind", choc::value::createString("toggle_button"));
            entry.addMember("on", choc::value::createBool(button->is_on()));
        } else if (auto* box = dynamic_cast<const view::Checkbox*>(&v)) {
            entry.addMember("kind", choc::value::createString("checkbox"));
            entry.addMember("checked", choc::value::createBool(box->is_checked()));
        } else if (auto* combo = dynamic_cast<const view::ComboBox*>(&v)) {
            entry.addMember("kind", choc::value::createString("combo"));
            entry.addMember("selected", choc::value::createInt64(combo->selected()));
            entry.addMember("open", choc::value::createBool(combo->is_open()));
            entry.addMember("hovered", choc::value::createInt64(combo->hovered_index()));
            entry.addMember("items", choc::value::createInt64(
                                         static_cast<std::int64_t>(combo->items().size())));
        } else if (auto* menu = dynamic_cast<const view::ContextMenu*>(&v)) {
            entry.addMember("kind", choc::value::createString("context_menu"));
            auto items = choc::value::createEmptyArray();
            for (const auto& item : menu->items())
                items.addArrayElement(choc::value::createString(item.label));
            entry.addMember("items", items);
            entry.addMember("hovered", choc::value::createInt64(menu->hovered_index()));
        } else if (auto* label = dynamic_cast<const view::Label*>(&v)) {
            entry.addMember("kind", choc::value::createString("label"));
            entry.addMember("text", choc::value::createString(label->text()));
        } else {
            keep = false;
        }
        if (keep) {
            entry.addMember("id", choc::value::createString(label_of(v)));
            entry.addMember("visible", choc::value::createBool(v.visible()));
            if (v.active_animation_count() > 0)
                entry.addMember("animations", choc::value::createInt64(v.active_animation_count()));
            out.addArrayElement(entry);
        }
        for (std::size_t i = 0; i < v.child_count(); ++i)
            collect_state(*v.child_at(i), out);
    }

    int run(choc::value::Value& log) {
        std::string error;
        current_timestamp_ = schedule_.timestamps.front();
        for (int i = 0; i < schedule_.initial_views; ++i) {
            if (!attach_view(0, error)) {
                std::cerr << "Error: could not open view " << i << ": " << error << "\n";
                return 1;
            }
        }
        auto frames = choc::value::createEmptyArray();
        for (std::size_t f = 0; f < schedule_.timestamps.size(); ++f) {
            current_frame_ = static_cast<int>(f);
            current_timestamp_ = schedule_.timestamps[f];
            auto frame = choc::value::createObject("");
            frame.addMember("frame", choc::value::createInt64(current_frame_));
            frame.addMember("timestamp", choc::value::createFloat64(schedule_.timestamps[f]));
            auto applied = choc::value::createEmptyArray();
            for (const auto& event : schedule_.events) {
                if (event.frame != current_frame_)
                    continue;
                applied.addArrayElement(apply(event, error));
                if (!error.empty()) {
                    std::cerr << "Error: frame " << f << ": " << error << "\n";
                    return 1;
                }
            }
            frame.addMember("events", applied);
            auto view_records = choc::value::createEmptyArray();
            for (auto& ov : views_) {
                if (!ov->attached)
                    continue;
                view_records.addArrayElement(tick(*ov, schedule_.timestamps[f]));
                if (std::find(schedule_.capture.begin(), schedule_.capture.end(), current_frame_) !=
                    schedule_.capture.end()) {
                    std::ostringstream name;
                    name << "frame-v" << ov->index << "-f" << std::setw(4) << std::setfill('0')
                         << current_frame_ << ".png";
                    if (!view::render_to_file(*ov->root, static_cast<std::uint32_t>(ov->width),
                                              static_cast<std::uint32_t>(ov->height),
                                              (options_.out_dir / name.str()).string(),
                                              options_.scale, view::ScreenshotBackend::skia)) {
                        std::cerr << "Error: could not render " << name.str() << "\n";
                        return 1;
                    }
                }
            }
            frame.addMember("views", view_records);
            auto params = choc::value::createObject("");
            for (const auto& info : store_.all_params())
                params.addMember(info.name,
                                 choc::value::createFloat64(store_.get_normalized(info.id)));
            frame.addMember("params", params);
            frames.addArrayElement(frame);
        }
        log.addMember("frames", frames);
        auto bindings = choc::value::createEmptyArray();
        for (const auto& ov : views_) {
            if (!ov->binding)
                continue;
            for (const auto& [key, resolved] : ov->binding->resolutions()) {
                auto b = choc::value::createObject("");
                b.addMember("view", choc::value::createInt64(ov->index));
                b.addMember("param_key", choc::value::createString(key));
                b.addMember("resolved", choc::value::createBool(resolved));
                bindings.addArrayElement(b);
            }
        }
        log.addMember("bindings", bindings);
        auto lifetimes = choc::value::createEmptyArray();
        for (const auto& ov : views_) {
            auto l = choc::value::createObject("");
            l.addMember("view", choc::value::createInt64(ov->index));
            l.addMember("attached_frame", choc::value::createInt64(ov->attached_frame));
            l.addMember("detached_frame", ov->detached_frame
                                              ? choc::value::createInt64(*ov->detached_frame)
                                              : choc::value::Value{});
            lifetimes.addArrayElement(l);
        }
        log.addMember("views", lifetimes);
        // Release scripted realms while their roots still exist.
        for (auto& ov : views_)
            if (ov->session)
                ov->session->release_root();
        return 0;
    }

  private:
    Options options_;
    Schedule schedule_;
    view::DesignIR ir_;
    state::StateStore store_;
    std::vector<std::unique_ptr<ObservedView>> views_;
    int current_frame_ = 0;
    double current_timestamp_ = 0.0;
};

std::optional<Schedule> parse_schedule(const std::string& text, state::StateStore& store,
                                       std::string& error) {
    choc::value::Value doc;
    try {
        doc = choc::json::parse(text);
    } catch (const std::exception& e) {
        error = std::string("schedule is not JSON: ") + e.what();
        return std::nullopt;
    }
    if (!doc.isObject() || doc["schema"].getWithDefault<std::string>("") != kScheduleSchema) {
        error = std::string("schedule schema must be ") + kScheduleSchema;
        return std::nullopt;
    }
    Schedule s;
    const auto stamps = doc["timestamps"];
    if (!stamps.isArray() || stamps.size() == 0) {
        error = "schedule needs a non-empty timestamps array";
        return std::nullopt;
    }
    for (uint32_t i = 0; i < stamps.size(); ++i)
        s.timestamps.push_back(stamps[i].getWithDefault(0.0));
    if (doc.hasObjectMember("nominal_dt"))
        s.nominal_dt = static_cast<float>(doc["nominal_dt"].getWithDefault(0.0));
    if (doc.hasObjectMember("readiness"))
        s.readiness = doc["readiness"].getWithDefault<std::string>("deferred");
    if (s.readiness != "deferred" && s.readiness != "immediate") {
        error = "readiness must be deferred or immediate";
        return std::nullopt;
    }
    if (doc.hasObjectMember("views"))
        s.initial_views = static_cast<int>(doc["views"].getWithDefault<int64_t>(1));
    if (s.initial_views < 1) {
        error = "views must be at least 1";
        return std::nullopt;
    }
    if (doc.hasObjectMember("params")) {
        const auto params = doc["params"];
        for (uint32_t i = 0; i < params.size(); ++i) {
            const auto p = params[i];
            state::ParamInfo info;
            info.id = static_cast<state::ParamID>(i + 1);
            info.name = p["name"].getWithDefault<std::string>("");
            info.range = {static_cast<float>(p["min"].getWithDefault(0.0)),
                          static_cast<float>(p["max"].getWithDefault(1.0)),
                          static_cast<float>(p["default"].getWithDefault(0.0))};
            if (info.name.empty()) {
                error = "every schedule param needs a name";
                return std::nullopt;
            }
            store.add_parameter(info);
        }
    }
    if (doc.hasObjectMember("events")) {
        const auto events = doc["events"];
        for (uint32_t i = 0; i < events.size(); ++i) {
            const auto e = events[i];
            Event ev;
            ev.frame = static_cast<int>(e["frame"].getWithDefault<int64_t>(-1));
            ev.view = static_cast<int>(e["view"].getWithDefault<int64_t>(0));
            ev.kind = e["kind"].getWithDefault<std::string>("");
            ev.spec = e;
            if (ev.frame < 0 || ev.frame >= static_cast<int>(s.timestamps.size()) ||
                ev.kind.empty()) {
                error =
                    "event " + std::to_string(i) + " needs a kind and a frame inside the schedule";
                return std::nullopt;
            }
            s.events.push_back(std::move(ev));
        }
    }
    if (doc.hasObjectMember("capture")) {
        const auto capture = doc["capture"];
        for (uint32_t i = 0; i < capture.size(); ++i)
            s.capture.push_back(static_cast<int>(capture[i].getWithDefault<int64_t>(-1)));
    }
    return s;
}

bool parse_positive(const char* value, float& result) {
    try {
        result = std::stof(value);
        return result > 0.0f;
    } catch (...) {
        return false;
    }
}

const char* platform_name() {
#if defined(__APPLE__)
    return "macos";
#elif defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

const char* arch_name() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "unknown";
#endif
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) {
            usage();
            return 2;
        }
        const char* value = argv[++i];
        if (arg == "--design-ir")
            options.design_ir = value;
        else if (arg == "--script")
            options.script = value;
        else if (arg == "--schedule")
            options.schedule = value;
        else if (arg == "--out-dir")
            options.out_dir = value;
        else if (arg == "--width" && parse_positive(value, options.width)) {
        } else if (arg == "--height" && parse_positive(value, options.height)) {
        } else if (arg == "--scale" && parse_positive(value, options.scale)) {
        } else if (arg == "--timer-clock" &&
                   (std::string_view{value} == "schedule" || std::string_view{value} == "wall")) {
            options.timer_clock = value;
        } else {
            usage();
            return 2;
        }
    }
    if (options.design_ir.empty() == options.script.empty() || options.schedule.empty() ||
        options.out_dir.empty() || options.width <= 0.0f || options.height <= 0.0f) {
        usage();
        return 2;
    }
    if (!view::raw_rgba_render_available()) {
        std::cerr << "Skipped: this build has no raster backend, so no frame can be observed\n";
        return kExitSkipped;
    }
    std::error_code ec;
    fs::create_directories(options.out_dir, ec);
    if (ec) {
        std::cerr << "Error: could not create " << options.out_dir << "\n";
        return 1;
    }
    const auto input_path = options.script.empty() ? options.design_ir : options.script;
    const auto input_bytes = read_bytes(input_path);
    const auto schedule_bytes = read_bytes(options.schedule);
    if (input_bytes.empty() || schedule_bytes.empty()) {
        std::cerr << "Error: could not read the input or the schedule\n";
        return 1;
    }
    view::DesignIR ir;
    if (!options.design_ir.empty()) {
        try {
            ir = view::parse_design_ir_json(input_bytes);
        } catch (const view::DesignIrParseError& error) {
            std::cerr << "Error: invalid DesignIR: " << error.what() << "\n";
            return 1;
        }
    }

    Observer observer(options, std::move(ir));
    std::string error;
    if (!observer.load_schedule(schedule_bytes, error)) {
        std::cerr << "Error: " << error << "\n";
        return 2;
    }
    auto log = choc::value::createObject("");
    log.addMember("schema", choc::value::createString(kSchema));
    auto environment = choc::value::createObject("");
    environment.addMember("platform", choc::value::createString(platform_name()));
    environment.addMember("arch", choc::value::createString(arch_name()));
    environment.addMember("backend", choc::value::createString("skia-raster"));
    environment.addMember("timer_clock", choc::value::createString(options.timer_clock));
    environment.addMember("scale", choc::value::createFloat64(options.scale));
    environment.addMember("width", choc::value::createFloat64(options.width));
    environment.addMember("height", choc::value::createFloat64(options.height));
    log.addMember("environment", environment);
    auto inputs = choc::value::createObject("");
    inputs.addMember("mode",
                     choc::value::createString(options.script.empty() ? "design-ir" : "script"));
    inputs.addMember("input", choc::value::createString(input_path.filename().string()));
    inputs.addMember("input_sha256", choc::value::createString(runtime::sha256_hex(input_bytes)));
    inputs.addMember("schedule_sha256",
                     choc::value::createString(runtime::sha256_hex(schedule_bytes)));
    log.addMember("inputs", inputs);
    const int status = observer.run(log);
    if (status != 0)
        return status;
    if (!write_text(options.out_dir / "frames.json", choc::json::toString(log, true))) {
        std::cerr << "Error: could not write frames.json\n";
        return 1;
    }
    return 0;
}
