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
#include <catch2/catch_test_macros.hpp>

#include <pulp/view/asset_manager.hpp>
#include <pulp/view/reload_capabilities.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/state/store.hpp>
#include <pulp/runtime/crypto.hpp>

#include "support/unique_temp_dir.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

TEST_CASE("all() capabilities → effectful JS functions are present (dev-unenforced)",
          "[view][bridge][capabilities]") {
    auto all = CapabilitySet::all();
    REQUIRE(typeof_global(all, "exec") == "function");
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
    REQUIRE(typeof_global(ex, "exec") == "function");            // granted
    REQUIRE(typeof_global(ex, "setImageSource") == "undefined"); // NOT granted
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
        {ReloadCapability::Storage,    "setWidgetSchema"},
        {ReloadCapability::Storage,    "storageGetItem"},
        {ReloadCapability::Ai,         "setAICli"},
        {ReloadCapability::Clipboard,  "readClipboard"},
        {ReloadCapability::Exec,       "exec"},
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
    REQUIRE(rig.load_status("file://" + (rig.outside / "secret.png").generic_string()) == 403);
    REQUIRE(rig.load_status("secret.png") == 200);
}
