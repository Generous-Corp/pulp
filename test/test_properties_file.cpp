#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/state/properties_file.hpp>
#include <pulp/runtime/temporary_file.hpp>
#include <filesystem>
#include <fstream>

using namespace pulp::state;
using namespace pulp::runtime;
using Catch::Matchers::WithinAbs;

// ── PropertiesFile basic operations ─────────────────────────────────────

TEST_CASE("PropertiesFile set and get string", "[state][properties]") {
    PropertiesFile props;

    props.set_string("name", "TestPlugin");
    auto val = props.get_string("name");
    REQUIRE(val.has_value());
    REQUIRE(*val == "TestPlugin");
}

TEST_CASE("PropertiesFile get missing key returns nullopt", "[state][properties]") {
    PropertiesFile props;
    REQUIRE_FALSE(props.get_string("nonexistent").has_value());
    REQUIRE_FALSE(props.get_int("nonexistent").has_value());
    REQUIRE_FALSE(props.get_double("nonexistent").has_value());
    REQUIRE_FALSE(props.get_bool("nonexistent").has_value());
}

TEST_CASE("PropertiesFile numeric getters reject invalid stored strings", "[state][properties]") {
    PropertiesFile props;
    props.set_string("count", "not-an-int");
    props.set_string("gain", "not-a-double");

    REQUIRE_FALSE(props.get_int("count").has_value());
    REQUIRE_FALSE(props.get_double("gain").has_value());
}

TEST_CASE("PropertiesFile set and get int", "[state][properties]") {
    PropertiesFile props;
    props.set_int("buffer_size", 512);
    auto val = props.get_int("buffer_size");
    REQUIRE(val.has_value());
    REQUIRE(*val == 512);
}

TEST_CASE("PropertiesFile set and get double", "[state][properties]") {
    PropertiesFile props;
    props.set_double("sample_rate", 44100.0);
    auto val = props.get_double("sample_rate");
    REQUIRE(val.has_value());
    REQUIRE_THAT(*val, WithinAbs(44100.0, 0.1));
}

TEST_CASE("PropertiesFile set and get bool", "[state][properties]") {
    PropertiesFile props;
    props.set_bool("auto_save", true);
    auto val = props.get_bool("auto_save");
    REQUIRE(val.has_value());
    REQUIRE(*val == true);

    props.set_bool("auto_save", false);
    val = props.get_bool("auto_save");
    REQUIRE(val.has_value());
    REQUIRE(*val == false);
}

TEST_CASE("PropertiesFile remove", "[state][properties]") {
    PropertiesFile props;
    props.set_string("key", "value");
    REQUIRE(props.contains("key"));
    props.remove("key");
    REQUIRE_FALSE(props.contains("key"));
}

TEST_CASE("PropertiesFile clear", "[state][properties]") {
    PropertiesFile props;
    props.set_string("a", "1");
    props.set_string("b", "2");
    REQUIRE(props.size() == 2);
    props.clear();
    REQUIRE(props.size() == 0);
}

TEST_CASE("PropertiesFile keys", "[state][properties]") {
    PropertiesFile props;
    props.set_string("zebra", "z");
    props.set_string("apple", "a");

    auto k = props.keys();
    REQUIRE(k.size() == 2);
    // std::map orders alphabetically
    REQUIRE(k[0] == "apple");
    REQUIRE(k[1] == "zebra");
}

// ── JSON file persistence ───────────────────────────────────────────────

TEST_CASE("PropertiesFile save and load round-trip", "[state][properties]") {
    TemporaryFile tmp(".json");

    // Save
    {
        PropertiesFile props;
        props.set_string("plugin_name", "PulpGain");
        props.set_int("version", 100);
        props.set_double("default_gain", -6.0);
        props.set_bool("bypass", false);
        REQUIRE(props.save(tmp.path_string()));
    }

    // Verify the file contains valid JSON
    {
        std::ifstream f(tmp.path());
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        REQUIRE(content.find("PulpGain") != std::string::npos);
        REQUIRE(content.find("plugin_name") != std::string::npos);
    }

    // Load
    {
        PropertiesFile props;
        REQUIRE(props.load(tmp.path_string()));

        auto name = props.get_string("plugin_name");
        REQUIRE(name.has_value());
        REQUIRE(*name == "PulpGain");

        auto version = props.get_int("version");
        REQUIRE(version.has_value());
        REQUIRE(*version == 100);

        auto gain = props.get_double("default_gain");
        REQUIRE(gain.has_value());
        REQUIRE_THAT(*gain, WithinAbs(-6.0, 0.1));

        auto bypass = props.get_bool("bypass");
        REQUIRE(bypass.has_value());
        REQUIRE(*bypass == false);
    }
}

TEST_CASE("PropertiesFile load nonexistent file returns false", "[state][properties]") {
    PropertiesFile props;
    REQUIRE_FALSE(props.load("/tmp/pulp_nonexistent_props_12345.json"));
}

TEST_CASE("PropertiesFile load invalid JSON returns false", "[state][properties]") {
    TemporaryFile tmp(".json");
    {
        std::ofstream f(tmp.path());
        f << "this is not json {{{";
    }

    PropertiesFile props;
    REQUIRE_FALSE(props.load(tmp.path_string()));
}

TEST_CASE("PropertiesFile load empty file clears existing values", "[state][properties]") {
    TemporaryFile tmp(".json");
    {
        std::ofstream f(tmp.path());
    }

    PropertiesFile props;
    props.set_string("stale", "value");

    REQUIRE(props.load(tmp.path_string()));
    REQUIRE_FALSE(props.contains("stale"));
    REQUIRE(props.size() == 0);
}

TEST_CASE("PropertiesFile save without path returns false", "[state][properties]") {
    PropertiesFile props;
    props.set_string("key", "value");

    REQUIRE_FALSE(props.save());
}

TEST_CASE("PropertiesFile save creates parent directories", "[state][properties]") {
    auto tmp_dir = std::filesystem::temp_directory_path() / "pulp_test_nested" / "deep";
    auto path = (tmp_dir / "settings.json").string();

    PropertiesFile props;
    props.set_string("test", "value");
    REQUIRE(props.save(path));
    REQUIRE(std::filesystem::exists(path));

    // Clean up
    std::filesystem::remove_all(std::filesystem::temp_directory_path() / "pulp_test_nested");
}

TEST_CASE("PropertiesFile save and reload preserves path", "[state][properties]") {
    TemporaryFile tmp(".json");

    PropertiesFile props;
    props.set_string("key", "val");
    props.set_path(tmp.path_string());
    REQUIRE(props.save());  // Uses stored path
    REQUIRE(props.path() == tmp.path_string());

    // Modify and save again
    props.set_string("key2", "val2");
    REQUIRE(props.save());

    PropertiesFile props2;
    REQUIRE(props2.load(tmp.path_string()));
    REQUIRE(props2.get_string("key2").value_or("") == "val2");
}

// ── ApplicationProperties ───────────────────────────────────────────────

TEST_CASE("ApplicationProperties paths are platform-appropriate", "[state][properties]") {
    auto user_dir = ApplicationProperties::user_settings_dir("PulpTest");
    auto common_dir = ApplicationProperties::common_settings_dir("PulpTest");

    REQUIRE_FALSE(user_dir.empty());
    REQUIRE_FALSE(common_dir.empty());

#ifdef __APPLE__
    REQUIRE(user_dir.find("Library/Preferences") != std::string::npos);
#elif defined(__linux__)
    REQUIRE(user_dir.find(".config") != std::string::npos);
#endif
}
