// CLAP hosted-editor negotiation (core/host/src/plugin_slot_clap.cpp).
//
// Drives ClapSlot against a fake clap_plugin_t whose gui extension records
// every call, so the negotiation is pinned as pure C-ABI call ordering with no
// dlopen and no bundle on disk.
//
// ObjC++ because create_hosted_editor() hands the plugin a real parent view and
// the slot's container is a real NSView. Allocating an NSView needs AppKit but
// not a window server, so these still run headless. The file is Apple-only for
// the same reason the feature is: no other platform has a WindowHost that
// implements the native-child seam, so there is nothing to parent an editor
// into.

#include <catch2/catch_test_macros.hpp>

#include "../core/host/src/plugin_slot_clap_internal.hpp"
#include <pulp/host/hosted_editor_container.hpp>
#include <pulp/host/plugin_slot.hpp>

#import <AppKit/AppKit.h>

#include <string>
#include <vector>

using namespace pulp::host;

namespace {

/// A fake CLAP plugin whose gui extension logs its call sequence. Each knob
/// makes one negotiation step fail so the unwind path can be pinned.
struct FakeClapPlugin {
    std::vector<std::string> calls;

    bool api_supported = true;
    bool create_ok = true;
    bool get_size_ok = true;
    bool set_parent_ok = true;
    bool show_ok = true;
    bool resizable = true;

    uint32_t width = 640;
    uint32_t height = 480;

    // What the plugin snaps an adjust_size() request to.
    uint32_t adjust_to_width = 0;
    uint32_t adjust_to_height = 0;

    const clap_host_t* host = nullptr;
    clap_plugin_t plugin{};
    clap_plugin_gui_t gui{};

    bool logged(const std::string& name) const {
        for (const auto& c : calls) {
            if (c == name) return true;
        }
        return false;
    }

    /// Index of `name` in the call log, or -1. Lets a test assert ordering
    /// rather than mere presence.
    int index_of(const std::string& name) const {
        for (size_t i = 0; i < calls.size(); ++i) {
            if (calls[i] == name) return static_cast<int>(i);
        }
        return -1;
    }
};

FakeClapPlugin* g_fake = nullptr;

FakeClapPlugin* fake_from(const clap_plugin_t*) { return g_fake; }

// ── gui extension ────────────────────────────────────────────────────────────

bool CLAP_ABI fake_gui_is_api_supported(const clap_plugin_t* p, const char* api, bool floating) {
    auto* f = fake_from(p);
    f->calls.push_back("is_api_supported");
    if (floating) return false;
    (void) api;
    return f->api_supported;
}

bool CLAP_ABI fake_gui_get_preferred_api(const clap_plugin_t*, const char**, bool*) { return false; }

bool CLAP_ABI fake_gui_create(const clap_plugin_t* p, const char*, bool) {
    auto* f = fake_from(p);
    f->calls.push_back("create");
    return f->create_ok;
}

void CLAP_ABI fake_gui_destroy(const clap_plugin_t* p) { fake_from(p)->calls.push_back("destroy"); }

bool CLAP_ABI fake_gui_set_scale(const clap_plugin_t* p, double) {
    fake_from(p)->calls.push_back("set_scale");
    return true;
}

bool CLAP_ABI fake_gui_get_size(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
    auto* f = fake_from(p);
    f->calls.push_back("get_size");
    if (!f->get_size_ok) return false;
    *w = f->width;
    *h = f->height;
    return true;
}

bool CLAP_ABI fake_gui_can_resize(const clap_plugin_t* p) {
    auto* f = fake_from(p);
    f->calls.push_back("can_resize");
    return f->resizable;
}

bool CLAP_ABI fake_gui_get_resize_hints(const clap_plugin_t*, clap_gui_resize_hints_t*) {
    return false;
}

bool CLAP_ABI fake_gui_adjust_size(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
    auto* f = fake_from(p);
    f->calls.push_back("adjust_size");
    if (f->adjust_to_width != 0) *w = f->adjust_to_width;
    if (f->adjust_to_height != 0) *h = f->adjust_to_height;
    return true;
}

bool CLAP_ABI fake_gui_set_size(const clap_plugin_t* p, uint32_t w, uint32_t h) {
    auto* f = fake_from(p);
    f->calls.push_back("set_size");
    f->width = w;
    f->height = h;
    return true;
}

bool CLAP_ABI fake_gui_set_parent(const clap_plugin_t* p, const clap_window_t* w) {
    auto* f = fake_from(p);
    f->calls.push_back("set_parent");
    // The host must hand over a populated window of the API it negotiated.
    if (!w || !w->api) return false;
    return f->set_parent_ok;
}

bool CLAP_ABI fake_gui_set_transient(const clap_plugin_t*, const clap_window_t*) { return false; }
void CLAP_ABI fake_gui_suggest_title(const clap_plugin_t*, const char*) {}

bool CLAP_ABI fake_gui_show(const clap_plugin_t* p) {
    auto* f = fake_from(p);
    f->calls.push_back("show");
    return f->show_ok;
}

bool CLAP_ABI fake_gui_hide(const clap_plugin_t* p) {
    fake_from(p)->calls.push_back("hide");
    return true;
}

// ── plugin ───────────────────────────────────────────────────────────────────

bool CLAP_ABI fake_plugin_init(const clap_plugin_t*) { return true; }
void CLAP_ABI fake_plugin_destroy(const clap_plugin_t*) {}
bool CLAP_ABI fake_plugin_activate(const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
void CLAP_ABI fake_plugin_deactivate(const clap_plugin_t*) {}
bool CLAP_ABI fake_plugin_start_processing(const clap_plugin_t*) { return true; }
void CLAP_ABI fake_plugin_stop_processing(const clap_plugin_t*) {}
void CLAP_ABI fake_plugin_reset(const clap_plugin_t*) {}
clap_process_status CLAP_ABI fake_plugin_process(const clap_plugin_t*, const clap_process_t*) {
    return CLAP_PROCESS_CONTINUE;
}
void CLAP_ABI fake_plugin_on_main_thread(const clap_plugin_t*) {}

const void* CLAP_ABI fake_plugin_get_extension(const clap_plugin_t* p, const char* id) {
    auto* f = fake_from(p);
    if (std::string(id) == CLAP_EXT_GUI) return &f->gui;
    return nullptr;
}

clap_plugin_descriptor_t g_desc{};

/// Wire a fake into a slot. The slot owns nothing here — `fake` must outlive it.
std::unique_ptr<PluginSlot> make_slot(FakeClapPlugin& fake) {
    g_fake = &fake;

    g_desc.clap_version = CLAP_VERSION_INIT;
    g_desc.id = "pulp.test.fake";
    g_desc.name = "Fake";

    fake.gui.is_api_supported = &fake_gui_is_api_supported;
    fake.gui.get_preferred_api = &fake_gui_get_preferred_api;
    fake.gui.create = &fake_gui_create;
    fake.gui.destroy = &fake_gui_destroy;
    fake.gui.set_scale = &fake_gui_set_scale;
    fake.gui.get_size = &fake_gui_get_size;
    fake.gui.can_resize = &fake_gui_can_resize;
    fake.gui.get_resize_hints = &fake_gui_get_resize_hints;
    fake.gui.adjust_size = &fake_gui_adjust_size;
    fake.gui.set_size = &fake_gui_set_size;
    fake.gui.set_parent = &fake_gui_set_parent;
    fake.gui.set_transient = &fake_gui_set_transient;
    fake.gui.suggest_title = &fake_gui_suggest_title;
    fake.gui.show = &fake_gui_show;
    fake.gui.hide = &fake_gui_hide;

    fake.plugin.desc = &g_desc;
    fake.plugin.plugin_data = &fake;
    fake.plugin.init = &fake_plugin_init;
    fake.plugin.destroy = &fake_plugin_destroy;
    fake.plugin.activate = &fake_plugin_activate;
    fake.plugin.deactivate = &fake_plugin_deactivate;
    fake.plugin.start_processing = &fake_plugin_start_processing;
    fake.plugin.stop_processing = &fake_plugin_stop_processing;
    fake.plugin.reset = &fake_plugin_reset;
    fake.plugin.process = &fake_plugin_process;
    fake.plugin.get_extension = &fake_plugin_get_extension;
    fake.plugin.on_main_thread = &fake_plugin_on_main_thread;

    PluginInfo info;
    info.name = "Fake";
    info.format = PluginFormat::CLAP;

    return make_clap_slot(info, [&fake](const clap_host_t* host) -> const clap_plugin_t* {
        fake.host = host;
        return &fake.plugin;
    });
}

/// A real parent view for the slot's container to be inserted into. An NSView
/// with no window is enough for the container helper, which only walks up to a
/// window to read the backing scale.
class ParentView {
public:
    ParentView() : view_([[NSView alloc] initWithFrame:NSMakeRect(0, 0, 1024, 768)]) {}
    ~ParentView() { [view_ release]; }
    ParentView(const ParentView&) = delete;
    ParentView& operator=(const ParentView&) = delete;

    void* handle() const { return (__bridge void*) view_; }
    NSUInteger subview_count() const { return [[view_ subviews] count]; }

private:
    NSView* view_ = nil;
};

} // namespace

TEST_CASE("CLAP slot reports an editor when the gui extension embeds", "[clap][editor]") {
    FakeClapPlugin fake;
    auto slot = make_slot(fake);
    REQUIRE(slot != nullptr);

    // has_editor() is resolved once at attach time, so the API query must
    // already have happened — not be deferred to the first create call.
    REQUIRE(fake.logged("is_api_supported"));
    REQUIRE(slot->has_editor());
    // The scanned descriptor must carry the resolved answer, not stay default.
    REQUIRE(slot->info().has_editor);
}

TEST_CASE("CLAP slot reports no editor when the window API is unsupported", "[clap][editor]") {
    FakeClapPlugin fake;
    fake.api_supported = false;
    auto slot = make_slot(fake);
    REQUIRE(slot != nullptr);

    REQUIRE_FALSE(slot->has_editor());
    REQUIRE_FALSE(slot->info().has_editor);

    // A slot with no usable editor must not begin negotiating one.
    ParentView parent;
    REQUIRE(slot->create_hosted_editor(parent.handle()) == nullptr);
    REQUIRE_FALSE(fake.logged("create"));
}

TEST_CASE("CLAP editor create refuses a null parent without touching the plugin",
          "[clap][editor]") {
    FakeClapPlugin fake;
    auto slot = make_slot(fake);
    REQUIRE(slot != nullptr);

    REQUIRE(slot->create_hosted_editor(nullptr) == nullptr);
    REQUIRE_FALSE(fake.logged("create"));
}

TEST_CASE("CLAP editor unwinds when gui->create fails", "[clap][editor]") {
    FakeClapPlugin fake;
    fake.create_ok = false;
    auto slot = make_slot(fake);
    REQUIRE(slot != nullptr);

    ParentView parent;
    REQUIRE(slot->create_hosted_editor(parent.handle()) == nullptr);
    REQUIRE(fake.logged("create"));
    // create() failing means there is no gui to tear down; destroy() here would
    // be a call against a gui that was never created.
    REQUIRE_FALSE(fake.logged("destroy"));
    REQUIRE_FALSE(fake.logged("set_parent"));
}

TEST_CASE("CLAP editor unwinds with destroy when gui->get_size fails", "[clap][editor]") {
    FakeClapPlugin fake;
    fake.get_size_ok = false;
    auto slot = make_slot(fake);
    REQUIRE(slot != nullptr);

    ParentView parent;
    REQUIRE(slot->create_hosted_editor(parent.handle()) == nullptr);
    REQUIRE(fake.logged("create"));
    REQUIRE(fake.logged("get_size"));
    // The gui WAS created, so it must be destroyed on the way out.
    REQUIRE(fake.logged("destroy"));
    REQUIRE_FALSE(fake.logged("set_parent"));
}

TEST_CASE("CLAP editor never calls set_scale on a logical-size window API",
          "[clap][editor]") {
    // clap/ext/gui.h: cocoa and uikit "use logical size, don't call set_scale()".
    // Physical-size APIs (win32, x11) do want it.
    FakeClapPlugin fake;
    fake.get_size_ok = false;  // stop early; set_scale would already have run
    auto slot = make_slot(fake);
    REQUIRE(slot != nullptr);
    ParentView parent;
    slot->create_hosted_editor(parent.handle());

    REQUIRE_FALSE(fake.logged("set_scale"));
}

TEST_CASE("CLAP editor resize request is denied without a handler", "[clap][editor]") {
    FakeClapPlugin fake;
    auto slot = make_slot(fake);
    REQUIRE(slot != nullptr);
    REQUIRE(fake.host != nullptr);

    auto* host_gui =
        static_cast<const clap_host_gui_t*>(fake.host->get_extension(fake.host, CLAP_EXT_GUI));
    REQUIRE(host_gui != nullptr);
    REQUIRE(host_gui->request_resize != nullptr);

    // No editor open and no handler installed: denial is the spec-legal answer.
    REQUIRE_FALSE(host_gui->request_resize(fake.host, 800, 600));
}

TEST_CASE("CLAP host exposes only the gui extension it implements", "[clap][editor]") {
    FakeClapPlugin fake;
    auto slot = make_slot(fake);
    REQUIRE(slot != nullptr);
    REQUIRE(fake.host != nullptr);

    REQUIRE(fake.host->get_extension(fake.host, CLAP_EXT_GUI) != nullptr);
    // Claiming an extension the host does not implement would invite the plugin
    // to call into null function pointers.
    REQUIRE(fake.host->get_extension(fake.host, "clap.params") == nullptr);
    REQUIRE(fake.host->get_extension(fake.host, "definitely.not.an.extension") == nullptr);
}

TEST_CASE("CLAP set_hosted_editor_size refuses when no editor is open", "[clap][editor]") {
    FakeClapPlugin fake;
    auto slot = make_slot(fake);
    REQUIRE(slot != nullptr);

    uint32_t w = 800;
    uint32_t h = 600;
    REQUIRE_FALSE(slot->set_hosted_editor_size(w, h));
    REQUIRE_FALSE(fake.logged("set_size"));
}

TEST_CASE("CLAP editor negotiates in the order clap/ext/gui.h specifies", "[clap][editor]") {
    FakeClapPlugin fake;
    auto slot = make_slot(fake);
    REQUIRE(slot != nullptr);

    ParentView parent;
    auto ed = slot->create_hosted_editor(parent.handle());
    REQUIRE(ed != nullptr);

    // clap/ext/gui.h documents the sequence: create → [set_scale] → can_resize
    // → get_size → set_parent → show. set_parent before show is the load-bearing
    // pair — showing an unparented editor would surface a stray window.
    REQUIRE(fake.index_of("create") < fake.index_of("get_size"));
    REQUIRE(fake.index_of("get_size") < fake.index_of("set_parent"));
    REQUIRE(fake.index_of("set_parent") < fake.index_of("show"));

    // The reported size is the plugin's, and the handle is the slot's container.
    REQUIRE(ed->width == 640);
    REQUIRE(ed->height == 480);
    REQUIRE(ed->resizable);
    REQUIRE(ed->native_handle != nullptr);

    // The container must really be in the parent's view tree — that is what
    // makes the plugin's view visible rather than orphaned.
    REQUIRE(parent.subview_count() == 1);

    slot->destroy_hosted_editor(std::move(ed));
    REQUIRE(parent.subview_count() == 0);
}

TEST_CASE("CLAP editor teardown hides before destroying", "[clap][editor]") {
    FakeClapPlugin fake;
    auto slot = make_slot(fake);
    REQUIRE(slot != nullptr);

    ParentView parent;
    auto ed = slot->create_hosted_editor(parent.handle());
    REQUIRE(ed != nullptr);

    slot->destroy_hosted_editor(std::move(ed));
    REQUIRE(fake.logged("hide"));
    REQUIRE(fake.logged("destroy"));
    REQUIRE(fake.index_of("hide") < fake.index_of("destroy"));
}

TEST_CASE("CLAP editor unwinds the created gui when set_parent fails", "[clap][editor]") {
    FakeClapPlugin fake;
    fake.set_parent_ok = false;
    auto slot = make_slot(fake);
    REQUIRE(slot != nullptr);

    ParentView parent;
    REQUIRE(slot->create_hosted_editor(parent.handle()) == nullptr);
    REQUIRE(fake.logged("set_parent"));
    REQUIRE(fake.logged("destroy"));
    // A failed parenting must not leave the container in the view tree.
    REQUIRE(parent.subview_count() == 0);
    REQUIRE_FALSE(fake.logged("show"));
}

TEST_CASE("CLAP editor unwinds the created gui when show fails", "[clap][editor]") {
    FakeClapPlugin fake;
    fake.show_ok = false;
    auto slot = make_slot(fake);
    REQUIRE(slot != nullptr);

    ParentView parent;
    REQUIRE(slot->create_hosted_editor(parent.handle()) == nullptr);
    REQUIRE(fake.logged("show"));
    REQUIRE(fake.logged("destroy"));
    REQUIRE(parent.subview_count() == 0);
}

TEST_CASE("CLAP editor refuses a second editor while one is open", "[clap][editor]") {
    FakeClapPlugin fake;
    auto slot = make_slot(fake);
    REQUIRE(slot != nullptr);

    ParentView parent;
    auto first = slot->create_hosted_editor(parent.handle());
    REQUIRE(first != nullptr);

    // CLAP allows exactly one gui per plugin instance; a second create() would
    // clobber the first.
    REQUIRE(slot->create_hosted_editor(parent.handle()) == nullptr);
    REQUIRE(parent.subview_count() == 1);

    slot->destroy_hosted_editor(std::move(first));
}

TEST_CASE("CLAP host-initiated resize lets the plugin snap the size", "[clap][editor]") {
    FakeClapPlugin fake;
    // The plugin only accepts sizes on a 100px grid.
    fake.adjust_to_width = 800;
    fake.adjust_to_height = 600;
    auto slot = make_slot(fake);
    REQUIRE(slot != nullptr);

    ParentView parent;
    auto ed = slot->create_hosted_editor(parent.handle());
    REQUIRE(ed != nullptr);

    uint32_t w = 823;
    uint32_t h = 617;
    REQUIRE(slot->set_hosted_editor_size(w, h));

    // adjust_size must run before set_size, and the caller must be told the
    // size the plugin actually took — not the one it asked for.
    REQUIRE(fake.index_of("adjust_size") < fake.index_of("set_size"));
    REQUIRE(w == 800);
    REQUIRE(h == 600);
    REQUIRE(fake.width == 800);
    REQUIRE(fake.height == 600);

    slot->destroy_hosted_editor(std::move(ed));
}

TEST_CASE("CLAP plugin-initiated resize reaches the installed handler", "[clap][editor]") {
    FakeClapPlugin fake;
    auto slot = make_slot(fake);
    REQUIRE(slot != nullptr);

    ParentView parent;
    auto ed = slot->create_hosted_editor(parent.handle());
    REQUIRE(ed != nullptr);

    uint32_t got_w = 0;
    uint32_t got_h = 0;
    slot->set_editor_resize_request_handler([&](uint32_t w, uint32_t h) {
        got_w = w;
        got_h = h;
        return true;
    });

    auto* host_gui =
        static_cast<const clap_host_gui_t*>(fake.host->get_extension(fake.host, CLAP_EXT_GUI));
    REQUIRE(host_gui != nullptr);
    REQUIRE(host_gui->request_resize(fake.host, 1024, 768));
    REQUIRE(got_w == 1024);
    REQUIRE(got_h == 768);

    slot->destroy_hosted_editor(std::move(ed));
}

TEST_CASE("CLAP host relays the handler's refusal to the plugin", "[clap][editor]") {
    FakeClapPlugin fake;
    auto slot = make_slot(fake);
    REQUIRE(slot != nullptr);

    ParentView parent;
    auto ed = slot->create_hosted_editor(parent.handle());
    REQUIRE(ed != nullptr);

    // A host that declines must report false so the plugin can adapt, rather
    // than claiming success it did not deliver.
    slot->set_editor_resize_request_handler([](uint32_t, uint32_t) { return false; });

    auto* host_gui =
        static_cast<const clap_host_gui_t*>(fake.host->get_extension(fake.host, CLAP_EXT_GUI));
    REQUIRE_FALSE(host_gui->request_resize(fake.host, 1024, 768));

    slot->destroy_hosted_editor(std::move(ed));
}

TEST_CASE("CLAP slot destroyed with an open editor still tears the gui down", "[clap][editor]") {
    FakeClapPlugin fake;
    ParentView parent;
    {
        auto slot = make_slot(fake);
        REQUIRE(slot != nullptr);
        auto ed = slot->create_hosted_editor(parent.handle());
        REQUIRE(ed != nullptr);
        REQUIRE(parent.subview_count() == 1);
        // Deliberately leak the HostedEditor: this is the caller violating the
        // lifetime contract. The slot must still not leave the plugin's view
        // parented into a container it is about to free.
        (void) ed.release();
    }
    REQUIRE(fake.logged("destroy"));
    REQUIRE(parent.subview_count() == 0);
}
