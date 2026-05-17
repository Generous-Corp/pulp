#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/state/properties_file.hpp>
#include <pulp/runtime/temporary_file.hpp>
#include <filesystem>
#include <fstream>
#include <random>

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

TEST_CASE("PropertiesFile bool getter treats stored false-like values as false",
          "[state][properties][issue-641]") {
    PropertiesFile props;
    props.set_string("word_false", "false");
    props.set_string("zero", "0");
    props.set_string("garbage", "not-a-bool");

    REQUIRE(props.get_bool("word_false").has_value());
    REQUIRE_FALSE(*props.get_bool("word_false"));
    REQUIRE(props.get_bool("zero").has_value());
    REQUIRE_FALSE(*props.get_bool("zero"));
    REQUIRE(props.get_bool("garbage").has_value());
    REQUIRE_FALSE(*props.get_bool("garbage"));
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

TEST_CASE("PropertiesFile bool getter accepts true and numeric-one strings",
          "[state][properties][codecov]") {
    PropertiesFile props;
    props.set_string("true_text", "true");
    props.set_string("one_text", "1");
    props.set_string("false_text", "false");
    props.set_string("other_text", "yes");

    REQUIRE(props.get_bool("true_text").value_or(false));
    REQUIRE(props.get_bool("one_text").value_or(false));
    REQUIRE_FALSE(props.get_bool("false_text").value_or(true));
    REQUIRE_FALSE(props.get_bool("other_text").value_or(true));
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

TEST_CASE("PropertiesFile load non-object JSON leaves existing values intact",
          "[state][properties][issue-641]") {
    TemporaryFile tmp(".json");
    {
        std::ofstream f(tmp.path());
        f << "[\"not\", \"an\", \"object\"]";
    }

    PropertiesFile props;
    props.set_string("stale", "value");
    REQUIRE_FALSE(props.load(tmp.path_string()));
    REQUIRE(props.get_string("stale").value_or("") == "value");
    REQUIRE(props.path() == tmp.path_string());
}

TEST_CASE("PropertiesFile load rejects non-object JSON without clearing values",
          "[state][properties][codecov]") {
    TemporaryFile tmp(".json");
    {
        std::ofstream f(tmp.path());
        f << R"json(["not", "an", "object"])json";
    }

    PropertiesFile props;
    props.set_string("existing", "kept");

    REQUIRE_FALSE(props.load(tmp.path_string()));
    REQUIRE(props.path() == tmp.path_string());
    REQUIRE(props.get_string("existing").value_or("") == "kept");
}

TEST_CASE("PropertiesFile load coerces scalar JSON members and skips nested values",
          "[state][properties][codecov]") {
    TemporaryFile tmp(".json");
    {
        std::ofstream f(tmp.path());
        f << R"json({
            "name": "Pulp",
            "enabled": true,
            "count": 42,
            "wide": 4294967296,
            "gain": -3.5,
            "nested": {"skip": true},
            "items": [1, 2, 3]
        })json";
    }

    PropertiesFile props;
    REQUIRE(props.load(tmp.path_string()));

    REQUIRE(props.get_string("name").value_or("") == "Pulp");
    REQUIRE(props.get_bool("enabled").value_or(false));
    REQUIRE(props.get_int("count").value_or(0) == 42);
    REQUIRE(props.get_int("wide").value_or(0) == 4294967296LL);
    REQUIRE_THAT(props.get_double("gain").value_or(0.0), WithinAbs(-3.5, 0.001));
    REQUIRE_FALSE(props.contains("nested"));
    REQUIRE_FALSE(props.contains("items"));
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

TEST_CASE("PropertiesFile load skips unsupported JSON member types",
          "[state][properties][json][issue-641]") {
    TemporaryFile tmp(".json");
    {
        std::ofstream f(tmp.path());
        f << R"JSON({
            "name": "Pulp",
            "enabled": true,
            "count": 7,
            "gain": 0.5,
            "ignored_null": null,
            "ignored_array": [1, 2],
            "ignored_object": {"nested": true}
        })JSON";
    }

    PropertiesFile props;
    REQUIRE(props.load(tmp.path_string()));
    REQUIRE(props.size() == 4);
    REQUIRE(props.get_string("name").value_or("") == "Pulp");
    REQUIRE(props.get_bool("enabled").value_or(false));
    REQUIRE(props.get_int("count").value_or(0) == 7);
    REQUIRE_THAT(props.get_double("gain").value_or(0.0), WithinAbs(0.5, 1e-9));
    REQUIRE_FALSE(props.contains("ignored_null"));
    REQUIRE_FALSE(props.contains("ignored_array"));
    REQUIRE_FALSE(props.contains("ignored_object"));
}

TEST_CASE("PropertiesFile save without path returns false", "[state][properties]") {
    PropertiesFile props;
    props.set_string("key", "value");

    REQUIRE_FALSE(props.save());
}

TEST_CASE("PropertiesFile save fails when destination is a directory",
          "[state][properties][issue-641]") {
    auto dir = std::filesystem::temp_directory_path() /
               ("pulp_props_dir_dest_" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);

    PropertiesFile props;
    props.set_string("key", "value");
    REQUIRE_FALSE(props.save(dir.string()));
    REQUIRE(props.path() == dir.string());

    std::filesystem::remove_all(dir);
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

TEST_CASE("PropertiesFile remove missing key and clear empty file are no-ops",
          "[state][properties][issue-641]") {
    PropertiesFile props;
    props.remove("missing");
    REQUIRE(props.size() == 0);

    props.clear();
    REQUIRE(props.keys().empty());

    props.set_string("present", "yes");
    props.remove("missing");
    REQUIRE(props.size() == 1);
    REQUIRE(props.contains("present"));
}

TEST_CASE("PropertiesFile save and reload preserves escaped string values",
          "[state][properties][coverage][phase3-large]") {
    TemporaryFile tmp(".json");

    PropertiesFile props;
    props.set_string("quote", R"(Pulp "quoted" \ path)");
    props.set_string("newline", "line one\nline two");
    REQUIRE(props.save(tmp.path_string()));

    PropertiesFile reloaded;
    REQUIRE(reloaded.load(tmp.path_string()));
    REQUIRE(reloaded.get_string("quote").value_or("") == R"(Pulp "quoted" \ path)");
    REQUIRE(reloaded.get_string("newline").value_or("") == "line one\nline two");
}

TEST_CASE("PropertiesFile set_path can clear the implicit save destination",
          "[state][properties][coverage][phase3-large]") {
    TemporaryFile tmp(".json");
    PropertiesFile props;
    props.set_string("key", "value");
    REQUIRE(props.save(tmp.path_string()));

    props.set_path("");

    REQUIRE(props.path().empty());
    REQUIRE_FALSE(props.save());
}

TEST_CASE("PropertiesFile successful load replaces stale values",
          "[state][properties][coverage][phase3-large]") {
    TemporaryFile tmp(".json");
    {
        std::ofstream f(tmp.path());
        f << R"json({"fresh":"yes","count":3})json";
    }

    PropertiesFile props;
    props.set_string("stale", "old");
    props.set_bool("enabled", true);

    REQUIRE(props.load(tmp.path_string()));
    REQUIRE_FALSE(props.contains("stale"));
    REQUIRE_FALSE(props.contains("enabled"));
    REQUIRE(props.get_string("fresh").value_or("") == "yes");
    REQUIRE(props.get_int("count").value_or(0) == 3);
}

TEST_CASE("PropertiesFile setters copy string-view keys and values",
          "[state][properties][coverage][phase3-large]") {
    std::string key = "dynamic_key";
    std::string value = "dynamic_value";

    PropertiesFile props;
    props.set_string(std::string_view(key), std::string_view(value));
    key.assign("mutated_key");
    value.assign("mutated_value");

    REQUIRE(props.contains("dynamic_key"));
    REQUIRE_FALSE(props.contains("mutated_key"));
    REQUIRE(props.get_string("dynamic_key").value_or("") == "dynamic_value");
}

TEST_CASE("PropertiesFile remove and reinsert keeps sorted key enumeration",
          "[state][properties][coverage][phase3-large]") {
    PropertiesFile props;
    props.set_string("b", "2");
    props.set_string("a", "1");
    props.set_string("c", "3");
    props.remove("b");
    props.set_string("b", "new");

    REQUIRE(props.keys() == std::vector<std::string>{"a", "b", "c"});
    REQUIRE(props.get_string("b").value_or("") == "new");
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
