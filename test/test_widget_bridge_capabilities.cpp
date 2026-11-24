// Capability-scoped WidgetBridge. Proves the effectful register_*_api groups are
// gated by the granted CapabilitySet: an ungranted group's JS function is ABSENT
// (typeof === "undefined"), a granted one is present, and the gates are INDEPENDENT
// (granting Filesystem does not grant Exec). Also proves the default constructor
// (all()) preserves the full surface — the full-featured posture for trusted UIs.
//
// Names checked are eager (registered in register_api at construction):
//   exec           → ReloadCapability::Exec       (register_platform_services_exec_api)
//   setImageSource → ReloadCapability::Filesystem (register_widget_assets_api)
// runtime_import (__pulpRuntimeImport__) installs lazily, so it is covered by the
// early-return guard in install_runtime_import_handlers, not a typeof check here.
//
// `exec` has a second, compile-time lock: PULP_BRIDGE_EXEC_ENABLED (from the
// PULP_DEV_BRIDGE_EXEC CMake option, mirrored onto this suite by its manifest
// entry). When it is 0 the registrar is not compiled at all, so `exec` must be
// absent even for a UI granted every capability.
#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/view/font_container.hpp>
#include <pulp/view/asset_manager.hpp>
#include <pulp/view/canvas_widget.hpp>
#ifndef PULP_BRIDGE_EXEC_ENABLED
#error                                                                                             \
    "PULP_BRIDGE_EXEC_ENABLED must mirror PULP_DEV_BRIDGE_EXEC; see test/cmake/view_widget_bridge_tests.cmake"
#endif

#include <pulp/view/reload_capabilities.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/state/store.hpp>
#include <pulp/runtime/crypto.hpp>

#include "support/unique_temp_dir.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace pulp::view;
using pulp::state::StateStore;

namespace {
// typeof a global in a freshly-constructed bridge scoped to `caps`.
std::string typeof_global(CapabilitySet caps, const char* name) {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 200, 200});
    StateStore store;
    WidgetBridge bridge(engine, root, store, nullptr, caps);
    return engine.evaluate(std::string("typeof ") + name).toString();
}
}  // namespace

TEST_CASE("empty capabilities → effectful JS functions are absent",
          "[view][bridge][capabilities]") {
    CapabilitySet none;  // consumer default = most restrictive
    REQUIRE(typeof_global(none, "exec") == "undefined");
    REQUIRE(typeof_global(none, "setImageSource") == "undefined");
}

namespace {

std::vector<unsigned char> read_jost_regular() {
    std::ifstream input(std::string(PULP_SOURCE_DIR) + "/external/fonts/Jost-Regular.ttf",
                        std::ios::binary);
    REQUIRE(input.good());
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
    REQUIRE_FALSE(bytes.empty());
    return bytes;
}

// Font registration is process-global and this suite shares one process with
// other cases, so every probe uses a family name nothing else registers.
std::string sealed_probe_family(const std::string& label) {
    return "SealedProbe-" + label + "-" +
           pulp::test::make_unique_temp_dir("pulp-sealed-family").filename().string();
}

bool register_embedded(ScriptEngine& engine, const std::string& family, const std::string& name) {
    return engine.evaluate("registerEmbeddedFont('" + family + "', '" + name + "', 400, 700)")
        .getWithDefault<bool>(true);
}

} // namespace

TEST_CASE("empty capabilities register only sealed embedded fonts",
          "[view][bridge][capabilities][fonts][security]") {
    const auto bytes = read_jost_regular();
    // Bytes that carry a font signature but do not parse, and a container
    // over the shipped-font limit: neither may reach the registry.
    std::vector<unsigned char> malformed{'O', 'T', 'T', 'O', 'n', 'o', 't',
                                         ' ', 'a', ' ', 'f', 'o', 'n', 't'};
    std::vector<unsigned char> oversized(pulp::view::kMaxShippedFontBytes + 1, 0);
    std::copy(bytes.begin(), bytes.end(), oversized.begin());
    static const char text[] = "plain text, not a font";

    const auto script_dir = pulp::test::make_unique_temp_dir("pulp-sealed-font-script");
    std::filesystem::create_directories(script_dir / "assets");
    // A real font beside the script that the panel did not seal.
    std::filesystem::copy_file(std::string(PULP_SOURCE_DIR) + "/external/fonts/Jost-Regular.ttf",
                               script_dir / "assets" / "unsealed.ttf");
    std::filesystem::copy_file(std::string(PULP_SOURCE_DIR) + "/external/fonts/Jost-Regular.ttf",
                               script_dir / "userfont-1");

    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store, nullptr, CapabilitySet{});
    bridge.set_script_base_dir(script_dir);
    bridge.set_embedded_assets({
        WidgetBridge::EmbeddedAsset{"assets/sealed.ttf", bytes.data(), bytes.size()},
        WidgetBridge::EmbeddedAsset{"assets/malformed.ttf", malformed.data(), malformed.size()},
        WidgetBridge::EmbeddedAsset{"assets/oversized.ttf", oversized.data(), oversized.size()},
        WidgetBridge::EmbeddedAsset{"assets/text.ttf", reinterpret_cast<const unsigned char*>(text),
                                    sizeof(text) - 1},
    });
    REQUIRE(engine.evaluate("typeof registerEmbeddedFont").toString() == "function");
    REQUIRE(engine.evaluate("typeof registerFont").toString() == "undefined");

    const auto sealed = sealed_probe_family("sealed");
    REQUIRE(register_embedded(engine, sealed, "assets/sealed.ttf"));
    REQUIRE(pulp::canvas::is_font_registered(sealed));
    const auto dotted = sealed_probe_family("dotted");
    REQUIRE(register_embedded(engine, dotted, "./assets/sealed.ttf"));
    REQUIRE(pulp::canvas::is_font_registered(dotted));

    // Nothing outside the sealed set resolves: not a real font on disk beside
    // the script, not a climb out of it, an absolute path or a file:// URL.
    const auto refused = [&](const std::string& label, const std::string& name) {
        const auto family = sealed_probe_family(label);
        INFO(name);
        CHECK_FALSE(register_embedded(engine, family, name));
        CHECK_FALSE(pulp::canvas::is_font_registered(family));
    };
    const auto disk_font = (script_dir / "assets" / "unsealed.ttf").generic_string();
    refused("unsealed", "assets/unsealed.ttf");
    refused("bare", "userfont-1");
    refused("climb", "../userfont-1");
    refused("climb-sealed", "assets/../../assets/sealed.ttf");
    refused("hosts", "/etc/hosts");
    refused("absolute", disk_font);
    refused("file-url", "file://" + disk_font);
    refused("file-relative", "file://assets/sealed.ttf");
    // Sealed bytes that are not a usable font never reach the font parser's
    // success path.
    refused("malformed", "assets/malformed.ttf");
    refused("oversized", "assets/oversized.ttf");
    refused("text", "assets/text.ttf");

    std::error_code ec;
    std::filesystem::remove_all(script_dir, ec);
}

TEST_CASE("sealed WOFF2 registers only through the WOFF2 decoder",
          "[view][bridge][capabilities][fonts][security]") {
    REQUIRE(pulp::canvas::woff2_decoder_available());
    std::ifstream input(std::string(PULP_SOURCE_DIR) +
                            "/packages/pulp-web-player/src/theme/inter.woff2",
                        std::ios::binary);
    REQUIRE(input.good());
    const std::vector<unsigned char> woff2((std::istreambuf_iterator<char>(input)), {});
    REQUIRE(woff2.size() > 48);
    const auto declaring = [&](std::uint32_t sfnt_size) {
        auto bytes = woff2;
        bytes[16] = static_cast<unsigned char>(sfnt_size >> 24);
        bytes[17] = static_cast<unsigned char>(sfnt_size >> 16);
        bytes[18] = static_cast<unsigned char>(sfnt_size >> 8);
        bytes[19] = static_cast<unsigned char>(sfnt_size);
        return bytes;
    };
    // A decompression bomb declares a face far over the decoder's bound; a
    // malformed container declares none or is cut short.
    const auto bomb = declaring(512u * 1024u * 1024u);
    const auto undeclared = declaring(0);
    const std::vector<unsigned char> truncated(woff2.begin(), woff2.begin() + woff2.size() / 2);

    const auto script_dir = pulp::test::make_unique_temp_dir("pulp-sealed-woff2");
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store, nullptr, CapabilitySet{});
    bridge.set_script_base_dir(script_dir);
    bridge.set_embedded_assets({
        WidgetBridge::EmbeddedAsset{"assets/inter.woff2", woff2.data(), woff2.size()},
        WidgetBridge::EmbeddedAsset{"assets/bomb.woff2", bomb.data(), bomb.size()},
        WidgetBridge::EmbeddedAsset{"assets/undeclared.woff2", undeclared.data(),
                                    undeclared.size()},
        WidgetBridge::EmbeddedAsset{"assets/truncated.woff2", truncated.data(), truncated.size()},
    });

    const auto decoded = sealed_probe_family("woff2");
    REQUIRE(register_embedded(engine, decoded, "assets/inter.woff2"));
    REQUIRE(pulp::canvas::is_font_registered(decoded));
    for (const char* name :
         {"assets/bomb.woff2", "assets/undeclared.woff2", "assets/truncated.woff2"}) {
        const auto family = sealed_probe_family("woff2-refused");
        INFO(name);
        CHECK_FALSE(register_embedded(engine, family, name));
        CHECK_FALSE(pulp::canvas::is_font_registered(family));
    }
    std::error_code ec;
    std::filesystem::remove_all(script_dir, ec);
}

TEST_CASE("a panel cannot rebind a family another owner registered",
          "[view][bridge][capabilities][fonts][security]") {
    const auto bytes = read_jost_regular();
    const auto make_bridge = [&](ScriptEngine& engine, View& root, StateStore& store,
                                 const std::string& identity) {
        auto bridge = std::make_unique<WidgetBridge>(engine, root, store, nullptr, CapabilitySet{});
        bridge->set_storage_identity(identity);
        bridge->set_embedded_assets(
            {WidgetBridge::EmbeddedAsset{"assets/face.ttf", bytes.data(), bytes.size()}});
        return bridge;
    };
    const auto first_owner = sealed_probe_family("owner-a");
    const auto second_owner = sealed_probe_family("owner-b");
    ScriptEngine engine_a, engine_b, engine_a2;
    View root_a, root_b, root_a2;
    StateStore store_a, store_b, store_a2;
    auto bridge_a = make_bridge(engine_a, root_a, store_a, first_owner);
    auto bridge_b = make_bridge(engine_b, root_b, store_b, second_owner);
    auto bridge_a2 = make_bridge(engine_a2, root_a2, store_a2, first_owner);

    const auto family = sealed_probe_family("shared");
    REQUIRE(register_embedded(engine_a, family, "assets/face.ttf"));
    // Another panel may not take the family over.
    CHECK_FALSE(register_embedded(engine_b, family, "assets/face.ttf"));
    // The owning panel adds faces, and another instance of it shares them.
    CHECK(register_embedded(engine_a, family, "assets/face.ttf"));
    CHECK(register_embedded(engine_a2, family, "assets/face.ttf"));

    // A family host code registered is not a panel's to rebind either.
    const auto host_family = sealed_probe_family("host");
    REQUIRE(pulp::canvas::register_font(bytes.data(), bytes.size(), host_family));
    CHECK_FALSE(register_embedded(engine_a, host_family, "assets/face.ttf"));
}

TEST_CASE("all() capabilities → effectful JS functions are present (dev-unenforced)",
          "[view][bridge][capabilities]") {
    auto all = CapabilitySet::all();
#if PULP_BRIDGE_EXEC_ENABLED
    REQUIRE(typeof_global(all, "exec") == "function");
#else
    REQUIRE(typeof_global(all, "exec") == "undefined");
#endif
    REQUIRE(typeof_global(all, "setImageSource") == "function");
}

TEST_CASE("capability gates are independent — Filesystem does not grant Exec",
          "[view][bridge][capabilities]") {
    CapabilitySet fs;
    fs.grant(ReloadCapability::Filesystem);
    REQUIRE(typeof_global(fs, "setImageSource") == "function");  // granted
    REQUIRE(typeof_global(fs, "exec") == "undefined");           // NOT granted

    CapabilitySet ex;
    ex.grant(ReloadCapability::Exec);
#if PULP_BRIDGE_EXEC_ENABLED
    REQUIRE(typeof_global(ex, "exec") == "function");            // granted
#else
    REQUIRE(typeof_global(ex, "exec") == "undefined"); // granted, but not compiled in
#endif
    REQUIRE(typeof_global(ex, "setImageSource") == "undefined"); // NOT granted
}

// The compile-time lock is independent of the capability lock. With the bridge
// compiled in, Exec alone exposes both shell functions and nothing less does;
// with it compiled out, no grant — not even all() — can bring them back.
TEST_CASE("the shell exec bridge honours its compile-time gate whatever is granted",
          "[view][bridge][capabilities]") {
    const CapabilitySet none;
    CapabilitySet ex;
    ex.grant(ReloadCapability::Exec);
    const auto all = CapabilitySet::all();
    for (const char* name : {"exec", "execAsync"}) {
        INFO("shell function: " << name);
        REQUIRE(typeof_global(none, name) == "undefined");
#if PULP_BRIDGE_EXEC_ENABLED
        REQUIRE(typeof_global(ex, name) == "function");
        REQUIRE(typeof_global(all, name) == "function");
#else
        REQUIRE(typeof_global(ex, name) == "undefined");
        REQUIRE(typeof_global(all, name) == "undefined");
#endif
    }
}

// register_api() drives a table whose entries carry their required capability as
// DATA, so the gate for every effectful group is one field rather than a
// hand-written `if` per call site. Sweep the whole gated set: one representative
// JS symbol per gated group, proven absent under the empty set and present under
// exactly its own capability. A group that loses (or gains) a gate in the table
// fails here rather than shipping an ungated native surface.
//
//   setImageSource   → Filesystem (register_widget_assets_api)
//   showOpenDialog   → Filesystem (register_platform_services_dialog_api)
//   __loadAssetSync__→ Filesystem (register_asset_loading_api)
//   loadFont         → Filesystem (register_font_assets_api)
//   setWidgetSchema  → Storage    (register_widget_schema_api)
//   storageGetItem   → Storage    (register_storage_key_value_api)
//   setAICli         → Ai         (register_platform_services_ai_api)
//   readClipboard    → Clipboard  (register_platform_services_clipboard_api)
//   exec             → Exec       (register_platform_services_exec_api)
TEST_CASE("every gated API group is absent without its capability and present with it",
          "[view][bridge][capabilities]") {
    struct GatedSymbol {
        ReloadCapability gate;
        const char* js_name;
    };
    const GatedSymbol gated[] = {
        {ReloadCapability::Filesystem, "setImageSource"},
        {ReloadCapability::Filesystem, "showOpenDialog"},
        {ReloadCapability::Filesystem, "__loadAssetSync__"},
        {ReloadCapability::Filesystem, "loadFont"},
        {ReloadCapability::Storage, "setWidgetSchema"},
        {ReloadCapability::Storage, "storageGetItem"},
        {ReloadCapability::Ai, "setAICli"},
        {ReloadCapability::Clipboard, "readClipboard"},
#if PULP_BRIDGE_EXEC_ENABLED
        {ReloadCapability::Exec, "exec"},
#endif
    };

    const CapabilitySet none;
    for (const auto& entry : gated) {
        INFO("gated symbol: " << entry.js_name);
        // Absent under the empty (most restrictive) set…
        REQUIRE(typeof_global(none, entry.js_name) == "undefined");

        // …present once its OWN capability is granted…
        CapabilitySet own;
        own.grant(entry.gate);
        REQUIRE(typeof_global(own, entry.js_name) == "function");

        // …and still absent under an unrelated capability.
        const auto unrelated = entry.gate == ReloadCapability::Network
                                   ? ReloadCapability::Exec
                                   : ReloadCapability::Network;
        CapabilitySet other;
        other.grant(unrelated);
        REQUIRE(typeof_global(other, entry.js_name) == "undefined");
    }
}

// Pure-UI groups carry no effect and are registered whatever the granted set is.
// This is the other half of the table's contract: an entry with no gate must not
// acquire one by accident.
TEST_CASE("ungated API groups are present under the empty capability set",
          "[view][bridge][capabilities]") {
    const CapabilitySet none;
    REQUIRE(typeof_global(none, "createKnob") == "function");   // widget factory
    REQUIRE(typeof_global(none, "setBackground") == "function"); // visual style
    REQUIRE(typeof_global(none, "setTheme") == "function");      // theme
    REQUIRE(typeof_global(none, "removeWidget") == "function");  // metadata/removal
}

// ── Persisted storage: per-user, per-bridge, traversal- and symlink-proof ─────
//
// storageSetItem/storageGetItem/storageRemoveItem and saveStylePreset persist
// under the per-user settings root (HOME / APPDATA / XDG_CONFIG_HOME), never
// the world-shared temp dir, and a script-supplied key can never name a path
// outside that store. The fixture points the settings root at a fresh temp
// directory so the host's real app-data stays untouched.
namespace {

namespace fs = std::filesystem;

class ScopedEnvVar {
  public:
    ScopedEnvVar(const char* name, const std::string& value) : name_(name) {
        if (const char* old = std::getenv(name_))
            old_ = std::string(old);
        set(value.c_str());
    }
    ~ScopedEnvVar() {
#ifdef _WIN32
        _putenv_s(name_, old_ ? old_->c_str() : "");
#else
        if (old_)
            setenv(name_, old_->c_str(), 1);
        else
            unsetenv(name_);
#endif
    }

  private:
    void set(const char* value) {
#ifdef _WIN32
        _putenv_s(name_, value);
#else
        setenv(name_, value, 1);
#endif
    }
    const char* name_;
    std::optional<std::string> old_;
};

#if defined(_WIN32)
constexpr const char* kSettingsRootVar = "APPDATA";
#elif defined(__APPLE__)
constexpr const char* kSettingsRootVar = "HOME";
#else
constexpr const char* kSettingsRootVar = "XDG_CONFIG_HOME";
#endif

struct StorageRig {
    fs::path root = pulp::test::make_unique_temp_dir("pulp-bridge-storage");
    ScopedEnvVar env{kSettingsRootVar, root.string()};
    ScriptEngine engine;
    View view;
    StateStore store;
    CapabilitySet caps = [] {
        CapabilitySet c;
        c.grant(ReloadCapability::Storage);
        return c;
    }();
    WidgetBridge bridge{engine, view, store, nullptr, caps};

    StorageRig() {
        view.set_bounds({0, 0, 200, 200});
    }
    ~StorageRig() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    std::string eval(const std::string& js) {
        return engine.evaluate(js).toString();
    }

    // Every regular file currently under the redirected settings root.
    std::vector<fs::path> files_under_root() const {
        std::vector<fs::path> out;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec))
                out.push_back(it->path());
        return out;
    }
};

bool path_has_component(const fs::path& p, const std::string& component) {
    for (const auto& part : p)
        if (part.string() == component)
            return true;
    return false;
}

} // namespace

TEST_CASE("storageSetItem with a traversal key creates nothing outside the store",
          "[view][bridge][storage][security]") {
    StorageRig rig;
    // The pre-fix location a "../" key escaped to; clear any stale leftover so
    // its absence afterwards is a verdict on this run.
    const auto escaped = fs::temp_directory_path() / "escape.dat";
    std::error_code ec;
    fs::remove(escaped, ec);

    rig.eval("storageSetItem('../escape', 'x')");

    REQUIRE_FALSE(fs::exists(escaped));
    REQUIRE_FALSE(fs::exists(fs::temp_directory_path() / "pulp-storage" / "../escape.dat"));
    const auto files = rig.files_under_root();
    REQUIRE_FALSE(files.empty());
    for (const auto& f : files) {
        INFO("stored file: " << f.string());
        // Entry sits at widget-storage/<bridge namespace>/<hashed key>.
        REQUIRE(f.parent_path().parent_path().filename() == "widget-storage");
        REQUIRE(f.filename().string().find("escape") == std::string::npos);
        REQUIRE(f.filename().string().find("..") == std::string::npos);
    }
    // The key is stored by hash, so it still round-trips - just inside the store.
    REQUIRE(rig.eval("storageGetItem('../escape')") == "x");
}

TEST_CASE("storage round-trips a plain key under the per-user store, not the temp dir",
          "[view][bridge][storage]") {
    StorageRig rig;
    const auto legacy = fs::temp_directory_path() / "pulp-storage" / "theme.dat";
    std::error_code ec;
    fs::remove(legacy, ec);

    REQUIRE(rig.eval("storageGetItem('theme')") == "");
    rig.eval("storageSetItem('theme', 'dark')");
    REQUIRE(rig.eval("storageGetItem('theme')") == "dark");
    REQUIRE_FALSE(fs::exists(legacy));

    const auto files = rig.files_under_root();
    REQUIRE(files.size() == 1);
    REQUIRE(path_has_component(files.front(), "widget-storage"));
    REQUIRE(files.front().filename() == pulp::runtime::sha256_hex("theme") + ".dat");

    rig.eval("storageRemoveItem('theme')");
    REQUIRE(rig.eval("storageGetItem('theme')") == "");
    REQUIRE(rig.files_under_root().empty());
}

TEST_CASE("repeated writes to one key reuse the resolved store and keep one entry",
          "[view][bridge][storage]") {
    StorageRig rig;
    rig.eval("storageSetItem('knob', '0.25')");
    rig.eval("storageSetItem('knob', '0.5')");
    rig.eval("storageSetItem('knob', '0.75')");
    REQUIRE(rig.eval("storageGetItem('knob')") == "0.75");
    const auto files = rig.files_under_root();
    REQUIRE(files.size() == 1); // no leftover temporaries, one entry
    REQUIRE(files.front().filename() == pulp::runtime::sha256_hex("knob") + ".dat");

    // Moving the script base dir invalidates the cached directory: the same
    // key now resolves inside a different namespace.
    const auto moved = rig.root / "moved-plugin";
    fs::create_directories(moved);
    rig.bridge.set_script_base_dir(moved);
    REQUIRE(rig.eval("storageGetItem('knob')") == "");
    rig.eval("storageSetItem('knob', '1')");
    REQUIRE(rig.eval("storageGetItem('knob')") == "1");
    REQUIRE(rig.files_under_root().size() == 2);
}

TEST_CASE("the shared Pulp root keeps its permissions; only the store subtree is owner-only",
          "[view][bridge][storage][security]") {
    StorageRig rig;
#if defined(_WIN32)
    const auto pulp_root = rig.root / "Pulp";
#elif defined(__APPLE__)
    const auto pulp_root = rig.root / "Library" / "Application Support" / "Pulp";
#else
    const auto pulp_root = rig.root / "Pulp";
#endif
    fs::create_directories(pulp_root);
    std::error_code ec;
    fs::permissions(pulp_root, fs::perms::owner_all | fs::perms::group_all,
                    fs::perm_options::replace, ec);
    const auto before = fs::status(pulp_root).permissions();

    rig.eval("storageSetItem('k', 'v')");

    REQUIRE(fs::status(pulp_root).permissions() == before);
#if !defined(_WIN32)
    const auto store = pulp_root / "widget-storage";
    REQUIRE(fs::is_directory(store));
    REQUIRE((fs::status(store).permissions() & (fs::perms::group_all | fs::perms::others_all)) ==
            fs::perms::none);
#endif
}

TEST_CASE("storage entries are namespaced per bridge script base dir", "[view][bridge][storage]") {
    StorageRig a;
    const auto base_a = a.root / "plugin-a";
    const auto base_b = a.root / "plugin-b";
    fs::create_directories(base_a);
    fs::create_directories(base_b);
    a.bridge.set_script_base_dir(base_a);
    a.eval("storageSetItem('shared-key', 'from-a')");

    ScriptEngine engine_b;
    View view_b;
    StateStore store_b;
    WidgetBridge bridge_b(engine_b, view_b, store_b, nullptr, a.caps);
    bridge_b.set_script_base_dir(base_b);
    REQUIRE(engine_b.evaluate("storageGetItem('shared-key')").toString() == "");
    engine_b.evaluate("storageSetItem('shared-key', 'from-b')");
    REQUIRE(engine_b.evaluate("storageGetItem('shared-key')").toString() == "from-b");
    REQUIRE(a.eval("storageGetItem('shared-key')") == "from-a");
}

#if !defined(_WIN32)
TEST_CASE("storage refuses to read through or write through a symlinked entry",
          "[view][bridge][storage][security]") {
    StorageRig rig;
    rig.eval("storageSetItem('probe', '1')");
    const auto files = rig.files_under_root();
    REQUIRE(files.size() == 1);
    const auto store_dir = files.front().parent_path();

    const auto target = rig.root / "victim.txt";
    {
        std::ofstream(target) << "secret";
    }
    const auto link = store_dir / (pulp::runtime::sha256_hex("linked") + ".dat");
    fs::create_symlink(target, link);

    REQUIRE(rig.eval("storageGetItem('linked')") == "");
    rig.eval("storageSetItem('linked', 'overwritten')");
    std::ifstream in(target);
    const std::string after((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(after == "secret");
}
#endif

TEST_CASE("style presets persist under the per-user store, not the temp dir",
          "[view][bridge][storage]") {
    StorageRig rig;
    const auto legacy = fs::temp_directory_path() / "pulp-style-presets" / "caps_preset.json";
    std::error_code ec;
    fs::remove(legacy, ec);

    REQUIRE(rig.eval("String(saveStylePreset('caps preset', {tokens:{a:1}}))") == "true");
    REQUIRE_FALSE(fs::exists(legacy));
    const auto files = rig.files_under_root();
    REQUIRE(files.size() == 1);
    REQUIRE(path_has_component(files.front(), "style-presets"));
    REQUIRE(files.front().filename() == "caps_preset.json");
    REQUIRE(rig.eval("JSON.stringify(loadStylePreset('caps preset'))").find("\"a\"") !=
            std::string::npos);
}

// ── File paths named by a script stay inside reviewed roots ───────────────────
//
// setImageSource / registerFont / loadFont / __loadAssetSync__ accept paths the
// UI chose. By default an absolute path or file:// URL is refused unless it
// resolves inside the script directory or a reviewed asset root, and the asset
// loader never falls back to a raw filesystem read; embedded `pulp://` assets
// are unaffected. The legacy all() grant holds Filesystem, so the containment
// rule, not the capability, is what protects a hosted editor today.
namespace {

struct AssetPathRig {
    fs::path root = pulp::test::make_unique_temp_dir("pulp-bridge-asset-paths");
    fs::path script_dir = root / "plugin";
    fs::path outside = root / "outside";
    ScriptEngine engine;
    View view;
    StateStore store;
    WidgetBridge bridge{engine, view, store, nullptr, CapabilitySet::all()};

    AssetPathRig() {
        view.set_bounds({0, 0, 200, 200});
        fs::create_directories(script_dir / "assets");
        fs::create_directories(outside);
        std::ofstream(script_dir / "assets" / "hero.png", std::ios::binary) << "inside";
        std::ofstream(outside / "secret.png", std::ios::binary) << "outside";
        std::ofstream(outside / "face.ttf", std::ios::binary) << "font";
    }
    ~AssetPathRig() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    int load_status(const std::string& url) {
        return engine.evaluate("__loadAssetSync__('" + url + "').status").getWithDefault<int>(-1);
    }
    std::string image_path(const std::string& source) {
        engine.evaluate("createImage('img', ''); setImageSource('img', '" + source + "');");
        auto* image = dynamic_cast<ImageView*>(bridge.widget("img"));
        REQUIRE(image != nullptr);
        return image->image_path().empty() ? image->image_source() : image->image_path();
    }
};

} // namespace

TEST_CASE("asset loader refuses file:// and absolute paths when no root is configured",
          "[view][bridge][assets][security]") {
    AssetPathRig rig;
    REQUIRE(rig.load_status("file:///etc/hosts") == 403);
    REQUIRE(rig.load_status("/etc/hosts") == 403);
    REQUIRE(rig.load_status("file://" + (rig.outside / "secret.png").generic_string()) == 403);

    // Control: the same loader, with the app-side opt-in, reads the same file.
    rig.bridge.set_allow_unreviewed_file_paths(true);
    REQUIRE(rig.load_status("file://" + (rig.outside / "secret.png").generic_string()) == 200);
}

TEST_CASE("embedded pulp:// assets load without any root or opt-in", "[view][bridge][assets]") {
    AssetPathRig rig;
    const std::string name = "bridge-caps-" + rig.root.filename().string() + ".txt";
    static const char payload[] = "embedded-ok";
    AssetManager::instance().register_embedded(name, reinterpret_cast<const uint8_t*>(payload),
                                               sizeof(payload) - 1);
    REQUIRE(rig.load_status("pulp://" + name) == 200);
    REQUIRE(rig.engine.evaluate("__loadAssetSync__('pulp://" + name + "').text").toString() ==
            "embedded-ok");
    // A pulp:// name that is neither embedded nor under a root does not fall
    // through to the filesystem.
    REQUIRE(rig.load_status("pulp://" + (rig.outside / "secret.png").generic_string()) == 404);
}

TEST_CASE("setImageSource keeps absolute paths inside the script directory",
          "[view][bridge][assets][security]") {
    AssetPathRig rig;
    rig.bridge.set_script_base_dir(rig.script_dir);
    const auto inside = (rig.script_dir / "assets" / "hero.png").generic_string();
    const auto escaped = (rig.outside / "secret.png").generic_string();

    REQUIRE(rig.image_path(inside) == "file://" + inside);
    REQUIRE(rig.image_path("assets/hero.png") == "file://" + inside);
    REQUIRE(rig.image_path("file://" + inside) == "file://" + inside);

    // Fresh widget per refusal so a stale path cannot mask a no-op.
    rig.engine.evaluate("removeWidget('img')");
    REQUIRE(rig.image_path(escaped).empty());
    rig.engine.evaluate("removeWidget('img')");
    REQUIRE(rig.image_path("file://" + escaped).empty());
    rig.engine.evaluate("removeWidget('img')");
    REQUIRE(rig.image_path("../outside/secret.png").empty());

    rig.bridge.set_allow_unreviewed_file_paths(true);
    rig.engine.evaluate("removeWidget('img')");
    REQUIRE(rig.image_path(escaped) == "file://" + escaped);
}

TEST_CASE("registerFont and loadFont refuse an existing font outside every root",
          "[view][bridge][assets][security]") {
    AssetPathRig rig;
    rig.bridge.set_script_base_dir(rig.script_dir);
    const auto font = (rig.outside / "face.ttf").generic_string();
    REQUIRE(fs::exists(font));
    REQUIRE(
        rig.engine.evaluate("registerFont('Face', '" + font + "')").getWithDefault<bool>(true) ==
        false);
    REQUIRE(rig.engine.evaluate("loadFont('" + font + "')").getWithDefault<bool>(true) == false);

    // Control: the same font under the script directory registers.
    fs::copy_file(rig.outside / "face.ttf", rig.script_dir / "assets" / "face.ttf");
    const auto contained = (rig.script_dir / "assets" / "face.ttf").generic_string();
    REQUIRE(rig.engine.evaluate("loadFont('" + contained + "')").getWithDefault<bool>(false));
    REQUIRE(
        rig.engine.evaluate("registerFont('Face', 'assets/face.ttf')").getWithDefault<bool>(false));
}

TEST_CASE("a reviewed asset root admits absolute paths beneath it", "[view][bridge][assets]") {
    AssetPathRig rig;
    REQUIRE_FALSE(rig.bridge.permits_file_path(rig.outside / "secret.png"));
    rig.bridge.set_asset_roots({rig.outside});
    REQUIRE(rig.bridge.permits_file_path(rig.outside / "secret.png"));
    REQUIRE_FALSE(rig.bridge.permits_file_path(rig.script_dir / "assets" / "hero.png"));
    // file:// follows the same rule as a bare absolute path and setImageSource.
    REQUIRE(rig.load_status("file://" + (rig.outside / "secret.png").generic_string()) == 200);
    REQUIRE(rig.load_status((rig.outside / "secret.png").generic_string()) == 200);
    REQUIRE(rig.load_status("secret.png") == 200);
    REQUIRE(rig.load_status("file://" +
                            (rig.script_dir / "assets" / "hero.png").generic_string()) == 403);
}

namespace {

// Restores the process working directory on scope exit.
struct WorkingDirectoryGuard {
    fs::path saved = fs::current_path();
    ~WorkingDirectoryGuard() {
        std::error_code ec;
        fs::current_path(saved, ec);
    }
};

} // namespace

TEST_CASE("relative names that miss every root are not opened from the working directory",
          "[view][bridge][assets][security]") {
    AssetPathRig rig;
    rig.bridge.set_script_base_dir(rig.script_dir);
    // The file exists relative to the working directory, not under any root.
    WorkingDirectoryGuard cwd;
    fs::current_path(rig.outside);
    REQUIRE(fs::exists("face.ttf"));

    REQUIRE_FALSE(
        rig.engine.evaluate("registerFont('Face', 'face.ttf')").getWithDefault<bool>(true));
    REQUIRE_FALSE(rig.engine.evaluate("loadFont('face.ttf')").getWithDefault<bool>(true));
    REQUIRE_FALSE(
        rig.engine.evaluate("registerFont('Face', 'file://face.ttf')").getWithDefault<bool>(true));
    REQUIRE(rig.image_path("secret.png").empty());
    REQUIRE(rig.bridge.resolve_script_relative("secret.png").empty());
    REQUIRE(rig.bridge.resolve_script_relative("x://secret.png").empty());

    // Control: the host opt-in restores the working-directory lookup.
    rig.bridge.set_allow_unreviewed_file_paths(true);
    REQUIRE(rig.engine.evaluate("loadFont('face.ttf')").getWithDefault<bool>(false));
}

TEST_CASE("canvasDrawImage and setMaskImage hold url() and paths to the reviewed roots",
          "[view][bridge][assets][security]") {
    AssetPathRig rig;
    rig.bridge.set_script_base_dir(rig.script_dir);
    const auto inside =
        (rig.script_dir / "assets" / "hero.png").lexically_normal().generic_string();
    const auto escaped = (rig.outside / "secret.png").generic_string();
    rig.engine.evaluate("createCanvas('cv', ''); createPanel('p', '');");
    auto* canvas = dynamic_cast<CanvasWidget*>(rig.bridge.widget("cv"));
    auto* panel = rig.bridge.widget("p");
    REQUIRE(canvas != nullptr);
    REQUIRE(panel != nullptr);

    const auto draw = [&](const std::string& src) {
        canvas->clear_commands();
        rig.engine.evaluate("canvasDrawImage('cv', '" + src + "', 0, 0, 8, 8)");
        REQUIRE(canvas->commands().size() == 1);
        return canvas->commands().front().text;
    };
    REQUIRE(draw("assets/hero.png") == inside);
    REQUIRE(draw(inside) == inside);
    REQUIRE(draw(escaped).empty());
    REQUIRE(draw("file://" + escaped).empty());
    REQUIRE(draw("../outside/secret.png").empty());
    REQUIRE(draw("data:image/png;base64,AAAA") == "data:image/png;base64,AAAA");

    const auto mask = [&](const std::string& value) {
        rig.engine.evaluate("setMaskImage('p', \"" + value + "\")");
        return panel->mask_image();
    };
    REQUIRE(mask("url(assets/hero.png)") == "url(\"" + inside + "\")");
    REQUIRE(mask("url('file://" + escaped + "')") == "none");
    REQUIRE(mask("url(" + escaped + "), linear-gradient(black, transparent)") ==
            "none, linear-gradient(black, transparent)");
    REQUIRE(mask("url(#mask-id)") == "url(#mask-id)");

    // Control: the host opt-in admits the outside file.
    rig.bridge.set_allow_unreviewed_file_paths(true);
    REQUIRE(draw(escaped) == escaped);
    REQUIRE(mask("url(" + escaped + ")") == "url(\"" + escaped + "\")");
}

TEST_CASE("canvas patterns hold image paths to the reviewed roots",
          "[view][bridge][assets][security]") {
    AssetPathRig rig;
    rig.bridge.set_script_base_dir(rig.script_dir);
    const auto inside =
        (rig.script_dir / "assets" / "hero.png").lexically_normal().generic_string();
    const auto escaped = (rig.outside / "secret.png").generic_string();
    rig.engine.evaluate("createCanvas('cv', '');");
    auto* canvas = dynamic_cast<CanvasWidget*>(rig.bridge.widget("cv"));
    REQUIRE(canvas != nullptr);

    const auto pattern = [&](const std::string& fn, const std::string& src) {
        canvas->clear_commands();
        rig.engine.evaluate(fn + "('cv', '" + src + "', 'repeat', 'repeat')");
        REQUIRE(canvas->commands().size() == 1);
        return canvas->commands().front().text;
    };
    for (const std::string fn : {"canvasSetFillPattern", "canvasSetStrokePattern"}) {
        CAPTURE(fn);
        CHECK(pattern(fn, "assets/hero.png") == inside);
        CHECK(pattern(fn, escaped).empty());
        CHECK(pattern(fn, "file://" + escaped).empty());
        CHECK(pattern(fn, "../outside/secret.png").empty());
        CHECK(pattern(fn, "data:image/png;base64,AAAA") == "data:image/png;base64,AAAA");
    }

    // Control: the host opt-in admits the outside file.
    rig.bridge.set_allow_unreviewed_file_paths(true);
    CHECK(pattern("canvasSetFillPattern", escaped) == escaped);
}

TEST_CASE("bridge path APIs apply the per-file read ceiling", "[view][bridge][assets][security]") {
    AssetPathRig rig;
    rig.bridge.set_script_base_dir(rig.script_dir);
    fs::copy_file(rig.outside / "face.ttf", rig.script_dir / "assets" / "face.ttf");
    auto& assets = AssetManager::instance();
    const auto saved = assets.max_file_bytes();
    assets.set_max_file_bytes(2); // every fixture file is larger
    const bool loaded =
        rig.engine.evaluate("loadFont('assets/face.ttf')").getWithDefault<bool>(true);
    const auto resolved = rig.bridge.resolve_script_relative("assets/hero.png");
    assets.set_max_file_bytes(saved);
    REQUIRE_FALSE(loaded);
    REQUIRE(resolved.empty());

    // Control: under the default ceiling the same font resolves.
    REQUIRE(rig.engine.evaluate("loadFont('assets/face.ttf')").getWithDefault<bool>(false));
}
