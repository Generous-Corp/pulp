#include <catch2/catch_test_macros.hpp>
#include <pulp/view/widget_bridge.hpp>

#include "support/unique_temp_dir.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

using namespace pulp::view;
using namespace pulp::state;

namespace {

// Presets land under the per-user settings root; point it at a scratch dir so
// the test never touches the host's real app data, and locate the written file
// by name beneath it.
class ScopedSettingsRoot {
  public:
    ScopedSettingsRoot() : root_(pulp::test::make_unique_temp_dir("pulp-style-pack")) {
        if (const char* old = std::getenv(var()))
            old_ = std::string(old);
        set(root_.string().c_str());
    }
    ~ScopedSettingsRoot() {
#ifdef _WIN32
        _putenv_s(var(), old_ ? old_->c_str() : "");
#else
        if (old_)
            setenv(var(), old_->c_str(), 1);
        else
            unsetenv(var());
#endif
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }
    std::filesystem::path find(const std::string& file_name) const {
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(root_, ec), end; !ec && it != end;
             it.increment(ec))
            if (it->path().filename() == file_name)
                return it->path();
        return {};
    }

  private:
    static const char* var() {
#if defined(_WIN32)
        return "APPDATA";
#elif defined(__APPLE__)
        return "HOME";
#else
        return "XDG_CONFIG_HOME";
#endif
    }
    void set(const char* value) {
#ifdef _WIN32
        _putenv_s(var(), value);
#else
        setenv(var(), value, 1);
#endif
    }
    std::filesystem::path root_;
    std::optional<std::string> old_;
};

} // namespace

TEST_CASE("Style presets round-trip through saveStylePreset/loadStylePreset", "[view][style-pack]") {
    ScopedSettingsRoot settings_root;
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    auto preset_name = std::string("test_style_pack_roundtrip");

    bridge.load_script(
        "saveStylePreset('" + preset_name + "', {"
        "shader:'half4 main(float2 coord){return half4(1);}',"
        "schema:{type:'knob'},"
        "tokens:{colors:{'accent.primary':'#123456'}}"
        "});");

    const auto preset_path = settings_root.find(preset_name + ".json");
    REQUIRE_FALSE(preset_path.empty());
    REQUIRE(std::filesystem::exists(preset_path));

    auto loaded = engine.evaluate("JSON.stringify(loadStylePreset('" + preset_name + "'))").toString();
    REQUIRE(loaded.find("\"shader\"") != std::string::npos);
    REQUIRE(loaded.find("#123456") != std::string::npos);
}

TEST_CASE("Style presets sanitize names with spaces", "[view][style-pack]") {
    ScopedSettingsRoot settings_root;
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    std::string display_name = "Warm Analog";

    bridge.load_script(
        "saveStylePreset('" + display_name + "', {tokens:{colors:{'accent.primary':'#abcdef'}}});");

    const auto preset_path = settings_root.find("Warm_Analog.json");
    REQUIRE_FALSE(preset_path.empty());
    REQUIRE(std::filesystem::exists(preset_path));
    auto loaded = engine.evaluate("JSON.stringify(loadStylePreset('Warm Analog'))").toString();
    REQUIRE(loaded.find("#abcdef") != std::string::npos);
}
