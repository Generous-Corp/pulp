#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/i18n.hpp>
#include <pulp/runtime/temporary_file.hpp>
#include <fstream>

using namespace pulp::runtime;

// ── In-memory operations ────────────────────────────────────────────────

TEST_CASE("i18n add and translate", "[runtime][i18n]") {
    LocalisedStrings strings;
    strings.add("greeting", "Hello");
    REQUIRE(strings.translate("greeting") == "Hello");
    REQUIRE(strings.t("greeting") == "Hello");
    REQUIRE(strings.has("greeting"));
    REQUIRE(strings.count() == 1);
}

TEST_CASE("i18n duplicate keys overwrite previous values", "[runtime][i18n][coverage][issue-656]") {
    LocalisedStrings strings;
    strings.add("mode", "old");
    strings.add("mode", "new");
    REQUIRE(strings.count() == 1);
    REQUIRE(strings.translate("mode") == "new");
}

TEST_CASE("i18n translate missing key returns key", "[runtime][i18n]") {
    LocalisedStrings strings;
    REQUIRE(strings.translate("missing_key") == "missing_key");
}

TEST_CASE("i18n argument substitution", "[runtime][i18n]") {
    LocalisedStrings strings;
    strings.add("welcome", "Hello {0}, you have {1} messages");
    auto result = strings.translate("welcome", {"Alice", "5"});
    REQUIRE(result == "Hello Alice, you have 5 messages");
}

TEST_CASE("i18n argument substitution with multiple occurrences", "[runtime][i18n]") {
    LocalisedStrings strings;
    strings.add("repeat", "{0} and {0}");
    REQUIRE(strings.translate("repeat", {"yes"}) == "yes and yes");
}

TEST_CASE("i18n argument substitution leaves unmatched placeholders", "[runtime][i18n]") {
    LocalisedStrings strings;
    strings.add("mixed", "{0}/{2}/{1}/{0}");
    strings.add("empty", "before{0}after");

    REQUIRE(strings.translate("mixed", {"left", "right"}) == "left/{2}/right/left");
    REQUIRE(strings.translate("empty", {""}) == "beforeafter");
}

TEST_CASE("i18n argument substitution also applies to missing-key fallback",
          "[runtime][i18n][issue-641]") {
    LocalisedStrings strings;
    REQUIRE(strings.translate("missing {0}/{1}/{2}", {"a", "b"}) == "missing a/b/{2}");
}

TEST_CASE("i18n clear removes all translations", "[runtime][i18n]") {
    LocalisedStrings strings;
    strings.add("a", "1");
    strings.add("b", "2");
    REQUIRE(strings.count() == 2);
    strings.clear();
    REQUIRE(strings.count() == 0);
}

TEST_CASE("i18n locale get and set", "[runtime][i18n]") {
    LocalisedStrings strings;
    REQUIRE(strings.locale() == "en");
    strings.set_locale("de");
    REQUIRE(strings.locale() == "de");
}

// ── .strings file format ─────────────────────────────────────────────────

TEST_CASE("i18n load .strings file", "[runtime][i18n]") {
    TemporaryFile tmp(".strings");
    {
        std::ofstream f(tmp.path());
        f << "/* Comment */\n";
        f << "\"cancel\" = \"Cancel\";\n";
        f << "\"ok\" = \"OK\";\n";
        f << "\"save_as\" = \"Save As...\";\n";
    }

    LocalisedStrings strings;
    REQUIRE(strings.load_strings_file(tmp.path_string()));
    REQUIRE(strings.count() == 3);
    REQUIRE(strings.translate("cancel") == "Cancel");
    REQUIRE(strings.translate("ok") == "OK");
    REQUIRE(strings.translate("save_as") == "Save As...");
}

TEST_CASE("i18n .strings parser ignores malformed lines", "[runtime][i18n]") {
    TemporaryFile tmp(".strings");
    {
        std::ofstream f(tmp.path());
        f << "plain text without quotes\n";
        f << "\"missing_end = \"ignored\";\n";
        f << "\"missing_value\";\n";
        f << "\"missing_value_end\" = \"unterminated\n";
        f << "\"valid\" = \"kept\";\n";
    }

    LocalisedStrings strings;
    REQUIRE(strings.load_strings_file(tmp.path_string()));
    REQUIRE(strings.count() == 1);
    REQUIRE(strings.translate("valid") == "kept");
    REQUIRE_FALSE(strings.has("missing_value"));
}

TEST_CASE("i18n .strings parser lets later entries replace earlier ones",
          "[runtime][i18n][coverage][issue-656]") {
    TemporaryFile tmp(".strings");
    {
        std::ofstream f(tmp.path());
        f << "\"label\" = \"First\";\n";
        f << "\"label\" = \"Second\";\n";
        f << "\"other\" = \"Value\";\n";
    }

    LocalisedStrings strings;
    REQUIRE(strings.load_strings_file(tmp.path_string()));
    REQUIRE(strings.count() == 2);
    REQUIRE(strings.translate("label") == "Second");
    REQUIRE(strings.translate("other") == "Value");
}

// ── .po file format ─────────────────────────────────────────────────────

TEST_CASE("i18n load .po file", "[runtime][i18n]") {
    TemporaryFile tmp(".po");
    {
        std::ofstream f(tmp.path());
        f << "# Translation file\n";
        f << "msgid \"hello\"\n";
        f << "msgstr \"Hallo\"\n";
        f << "\n";
        f << "msgid \"goodbye\"\n";
        f << "msgstr \"Auf Wiedersehen\"\n";
    }

    LocalisedStrings strings;
    REQUIRE(strings.load_po_file(tmp.path_string()));
    REQUIRE(strings.count() == 2);
    REQUIRE(strings.translate("hello") == "Hallo");
    REQUIRE(strings.translate("goodbye") == "Auf Wiedersehen");
}

TEST_CASE("i18n .po parser handles continuations and empty entries", "[runtime][i18n]") {
    TemporaryFile tmp(".po");
    {
        std::ofstream f(tmp.path());
        f << "\"orphan continuation\"\n";
        f << "msgid \"long\"\n";
        f << "\"_key\"\n";
        f << "msgstr \"lange\"\n";
        f << "\"_wert\"\n";
        f << "\n";
        f << "msgid \"empty_translation\"\n";
        f << "msgstr \"\"\n";
        f << "\n";
        f << "msgid \"\"\n";
        f << "msgstr \"metadata ignored\"\n";
    }

    LocalisedStrings strings;
    REQUIRE(strings.load_po_file(tmp.path_string()));
    REQUIRE(strings.count() == 1);
    REQUIRE(strings.translate("long_key") == "lange_wert");
    REQUIRE_FALSE(strings.has("empty_translation"));
}

TEST_CASE("i18n .po parser commits previous entry when new msgid starts",
          "[runtime][i18n][issue-641]") {
    TemporaryFile tmp(".po");
    {
        std::ofstream f(tmp.path());
        f << "msgid \"first\"\n";
        f << "msgstr \"one\"\n";
        f << "msgid \"second\"\n";
        f << "msgstr \"two\"\n";
    }

    LocalisedStrings strings;
    REQUIRE(strings.load_po_file(tmp.path_string()));
    REQUIRE(strings.count() == 2);
    REQUIRE(strings.translate("first") == "one");
    REQUIRE(strings.translate("second") == "two");
}

// ── JSON file format ────────────────────────────────────────────────────

TEST_CASE("i18n load JSON file", "[runtime][i18n]") {
    TemporaryFile tmp(".json");
    {
        std::ofstream f(tmp.path());
        f << "{\n";
        f << "  \"title\": \"Pulp Audio\",\n";
        f << "  \"version_label\": \"Version {0}\",\n";
        f << "  \"empty\": \"\"\n";
        f << "}\n";
    }

    LocalisedStrings strings;
    REQUIRE(strings.load_json_file(tmp.path_string()));
    REQUIRE(strings.count() == 3);
    REQUIRE(strings.translate("title") == "Pulp Audio");
    REQUIRE(strings.translate("version_label", {"1.0"}) == "Version 1.0");
    REQUIRE(strings.translate("empty") == "");
}

TEST_CASE("i18n JSON parser handles escapes and keyless entries", "[runtime][i18n]") {
    TemporaryFile tmp(".json");
    {
        std::ofstream f(tmp.path());
        f << "{\n";
        f << "  : \"ignored\",\n";
        f << "  \"newline\": \"line\\nbreak\",\n";
        f << "  \"tab\": \"left\\tright\",\n";
        f << "  \"quote\": \"say \\\"hello\\\"\",\n";
        f << "  \"slash\": \"path\\\\file\"\n";
        f << "}\n";
    }

    LocalisedStrings strings;
    REQUIRE(strings.load_json_file(tmp.path_string()));
    REQUIRE(strings.count() == 4);
    REQUIRE(strings.translate("newline") == "line\nbreak");
    REQUIRE(strings.translate("tab") == "left\tright");
    REQUIRE(strings.translate("quote") == "say \"hello\"");
    REQUIRE(strings.translate("slash") == "path\\file");
}

TEST_CASE("i18n JSON parser accepts commas and duplicate keys", "[runtime][i18n][coverage][issue-656]") {
    TemporaryFile tmp(".json");
    {
        std::ofstream f(tmp.path());
        f << "{\n";
        f << "  \"mode\": \"old\",\n";
        f << "  \"mode\": \"new\",\n";
        f << "  \"tail\": \"kept\",\n";
        f << "}\n";
    }

    LocalisedStrings strings;
    REQUIRE(strings.load_json_file(tmp.path_string()));
    REQUIRE(strings.count() == 2);
    REQUIRE(strings.translate("mode") == "new");
    REQUIRE(strings.translate("tail") == "kept");
}

TEST_CASE("i18n JSON parser rejects missing object opener", "[runtime][i18n]") {
    TemporaryFile tmp(".json");

    {
        std::ofstream f(tmp.path());
        f << "";
    }

    LocalisedStrings strings;
    REQUIRE_FALSE(strings.load_json_file(tmp.path_string()));
    REQUIRE(strings.count() == 0);

    {
        std::ofstream f(tmp.path());
        f << "[]";
    }

    REQUIRE_FALSE(strings.load_json_file(tmp.path_string()));
    REQUIRE(strings.count() == 0);
}

TEST_CASE("i18n JSON parser allows duplicate keys and trailing comma",
          "[runtime][i18n][issue-641]") {
    TemporaryFile tmp(".json");
    {
        std::ofstream f(tmp.path());
        f << "{\n";
        f << "  \"name\": \"Old\",\n";
        f << "  \"name\": \"New\",\n";
        f << "  \"last\": \"value\",\n";
        f << "}\n";
    }

    LocalisedStrings strings;
    REQUIRE(strings.load_json_file(tmp.path_string()));
    REQUIRE(strings.count() == 2);
    REQUIRE(strings.translate("name") == "New");
    REQUIRE(strings.translate("last") == "value");
}

// ── File load failures ──────────────────────────────────────────────────

TEST_CASE("i18n load nonexistent file returns false", "[runtime][i18n]") {
    LocalisedStrings strings;
    REQUIRE_FALSE(strings.load_strings_file("/tmp/nonexistent_i18n_12345.strings"));
    REQUIRE_FALSE(strings.load_po_file("/tmp/nonexistent_i18n_12345.po"));
    REQUIRE_FALSE(strings.load_json_file("/tmp/nonexistent_i18n_12345.json"));
}

// ── Global instance ─────────────────────────────────────────────────────

TEST_CASE("i18n global instance", "[runtime][i18n]") {
    auto& inst = LocalisedStrings::instance();
    inst.add("test_global", "works");
    REQUIRE(tr("test_global") == "works");
    inst.add("test_global_args", "{0} works");
    REQUIRE(tr("test_global_args", {"also"}) == "also works");
    inst.clear();
}

TEST_CASE("i18n global tr supports argument substitution", "[runtime][i18n][issue-641]") {
    auto& inst = LocalisedStrings::instance();
    inst.clear();
    inst.add("global_args", "{0}:{1}");

    REQUIRE(tr("global_args", {"left", "right"}) == "left:right");

    inst.clear();
}

// ── System locale detection ─────────────────────────────────────────────

TEST_CASE("i18n system_locale returns non-empty", "[runtime][i18n]") {
    auto locale = LocalisedStrings::system_locale();
    REQUIRE_FALSE(locale.empty());
}
