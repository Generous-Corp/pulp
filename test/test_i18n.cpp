#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/i18n.hpp>
#include <pulp/runtime/temporary_file.hpp>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>

using namespace pulp::runtime;

namespace {

class ScopedLang {
public:
    explicit ScopedLang(const char* value) {
        if (const char* existing = std::getenv("LANG"))
            previous_ = std::string(existing);
        if (value)
            set(value);
        else
            unset();
    }

    ~ScopedLang() {
        if (previous_)
            set(previous_->c_str());
        else
            unset();
    }

    ScopedLang(const ScopedLang&) = delete;
    ScopedLang& operator=(const ScopedLang&) = delete;

private:
    static void set(const char* value) {
#if defined(_WIN32)
        _putenv_s("LANG", value);
#else
        setenv("LANG", value, 1);
#endif
    }

    static void unset() {
#if defined(_WIN32)
        _putenv_s("LANG", "");
#else
        unsetenv("LANG");
#endif
    }

    std::optional<std::string> previous_;
};

} // namespace

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

TEST_CASE("i18n accepts empty in-memory keys", "[runtime][i18n][coverage][phase3-batch742]") {
    LocalisedStrings strings;
    strings.add("", "metadata");
    strings.add("normal", "value");

    REQUIRE(strings.count() == 2);
    REQUIRE(strings.has(""));
    REQUIRE(strings.translate("") == "metadata");
    REQUIRE(strings.translate("normal") == "value");
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

TEST_CASE("i18n argument substitution handles adjacent placeholders",
          "[runtime][i18n][coverage][phase3-large]") {
    LocalisedStrings strings;
    strings.add("compact", "{0}{1}{0}");
    REQUIRE(strings.translate("compact", {"A", "B"}) == "ABA");
}

TEST_CASE("i18n argument substitution treats double braces as literal text",
          "[runtime][i18n][coverage][phase3-large]") {
    LocalisedStrings strings;
    strings.add("template", "{{0}} {0}");
    REQUIRE(strings.translate("template", {"value"}) == "{value} value");
}

TEST_CASE("i18n argument substitution applies later indexes after earlier replacements",
          "[runtime][i18n][coverage][phase3-large]") {
    LocalisedStrings strings;
    strings.add("nested", "{0} {1}");
    REQUIRE(strings.translate("nested", {"{1}", "done"}) == "done done");
}

TEST_CASE("i18n clear removes all translations", "[runtime][i18n]") {
    LocalisedStrings strings;
    strings.add("a", "1");
    strings.add("b", "2");
    REQUIRE(strings.count() == 2);
    strings.clear();
    REQUIRE(strings.count() == 0);
}

TEST_CASE("i18n clear leaves selected locale intact",
          "[runtime][i18n][coverage][phase3]") {
    LocalisedStrings strings;
    strings.set_locale("ja");
    strings.add("hello", "konnichiwa");

    strings.clear();

    REQUIRE(strings.count() == 0);
    REQUIRE(strings.locale() == "ja");
    REQUIRE(strings.translate("hello") == "hello");
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

TEST_CASE("i18n .strings parser accepts keys and values with spaces",
          "[runtime][i18n][coverage][phase3-large]") {
    TemporaryFile tmp(".strings");
    {
        std::ofstream f(tmp.path());
        f << "\"menu title\" = \"Save Project\";\n";
        f << "\" padded \" = \" value with spaces \";\n";
    }

    LocalisedStrings strings;
    REQUIRE(strings.load_strings_file(tmp.path_string()));
    REQUIRE(strings.translate("menu title") == "Save Project");
    REQUIRE(strings.translate(" padded ") == " value with spaces ");
}

TEST_CASE("i18n .strings parser keeps entries without semicolons",
          "[runtime][i18n][coverage][phase3-large]") {
    TemporaryFile tmp(".strings");
    {
        std::ofstream f(tmp.path());
        f << "\"loose\" = \"accepted\"\n";
    }

    LocalisedStrings strings;
    REQUIRE(strings.load_strings_file(tmp.path_string()));
    REQUIRE(strings.translate("loose") == "accepted");
}

TEST_CASE("i18n .strings parser permits empty keys",
          "[runtime][i18n][coverage][phase3-large]") {
    TemporaryFile tmp(".strings");
    {
        std::ofstream f(tmp.path());
        f << "\"\" = \"metadata\";\n";
        f << "\"normal\" = \"value\";\n";
    }

    LocalisedStrings strings;
    REQUIRE(strings.load_strings_file(tmp.path_string()));
    REQUIRE(strings.count() == 2);
    REQUIRE(strings.translate("") == "metadata");
}

TEST_CASE("i18n .strings load failure leaves existing translations intact",
          "[runtime][i18n][coverage][phase3-batch742]") {
    LocalisedStrings strings;
    strings.add("existing", "kept");

    REQUIRE_FALSE(strings.load_strings_file("/tmp/pulp_missing_strings_742.strings"));
    REQUIRE(strings.count() == 1);
    REQUIRE(strings.translate("existing") == "kept");
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

TEST_CASE("i18n .po parser replaces duplicate msgids",
          "[runtime][i18n][coverage][phase3-large]") {
    TemporaryFile tmp(".po");
    {
        std::ofstream f(tmp.path());
        f << "msgid \"mode\"\n";
        f << "msgstr \"old\"\n";
        f << "msgid \"mode\"\n";
        f << "msgstr \"new\"\n";
    }

    LocalisedStrings strings;
    REQUIRE(strings.load_po_file(tmp.path_string()));
    REQUIRE(strings.count() == 1);
    REQUIRE(strings.translate("mode") == "new");
}

TEST_CASE("i18n .po parser ignores msgids without translations",
          "[runtime][i18n][coverage][phase3-large]") {
    TemporaryFile tmp(".po");
    {
        std::ofstream f(tmp.path());
        f << "msgid \"missing\"\n";
        f << "msgid \"present\"\n";
        f << "msgstr \"yes\"\n";
    }

    LocalisedStrings strings;
    REQUIRE(strings.load_po_file(tmp.path_string()));
    REQUIRE_FALSE(strings.has("missing"));
    REQUIRE(strings.translate("present") == "yes");
}

TEST_CASE("i18n .po parser accepts empty msgid continuations",
          "[runtime][i18n][coverage][phase3-large]") {
    TemporaryFile tmp(".po");
    {
        std::ofstream f(tmp.path());
        f << "msgid \"prefix\"\n";
        f << "\"\"\n";
        f << "\"_suffix\"\n";
        f << "msgstr \"value\"\n";
    }

    LocalisedStrings strings;
    REQUIRE(strings.load_po_file(tmp.path_string()));
    REQUIRE(strings.translate("prefix_suffix") == "value");
}

TEST_CASE("i18n .po load failure leaves translations intact",
          "[runtime][i18n][coverage][phase3-batch742]") {
    LocalisedStrings strings;
    strings.add("existing", "kept");

    REQUIRE_FALSE(strings.load_po_file("/tmp/pulp_missing_po_742.po"));
    REQUIRE(strings.count() == 1);
    REQUIRE(strings.translate("existing") == "kept");
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

TEST_CASE("i18n JSON load failure preserves existing translations",
          "[runtime][i18n][coverage][phase3-batch742]") {
    TemporaryFile tmp(".json");
    {
        std::ofstream f(tmp.path());
        f << "[]";
    }

    LocalisedStrings strings;
    strings.add("existing", "kept");

    REQUIRE_FALSE(strings.load_json_file(tmp.path_string()));
    REQUIRE(strings.count() == 1);
    REQUIRE(strings.translate("existing") == "kept");
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

TEST_CASE("i18n JSON parser accepts compact objects without whitespace",
          "[runtime][i18n][coverage][phase3-large]") {
    TemporaryFile tmp(".json");
    {
        std::ofstream f(tmp.path());
        f << "{\"a\":\"1\",\"b\":\"2\"}";
    }

    LocalisedStrings strings;
    REQUIRE(strings.load_json_file(tmp.path_string()));
    REQUIRE(strings.translate("a") == "1");
    REQUIRE(strings.translate("b") == "2");
}

TEST_CASE("i18n JSON parser treats unknown escapes as literal escaped char",
          "[runtime][i18n][coverage][phase3-large]") {
    TemporaryFile tmp(".json");
    {
        std::ofstream f(tmp.path());
        f << "{\"slash\":\"\\/\",\"bell\":\"\\a\"}";
    }

    LocalisedStrings strings;
    REQUIRE(strings.load_json_file(tmp.path_string()));
    REQUIRE(strings.translate("slash") == "/");
    REQUIRE(strings.translate("bell") == "a");
}

TEST_CASE("i18n JSON parser tolerates missing closing brace after entries",
          "[runtime][i18n][coverage][phase3-large]") {
    TemporaryFile tmp(".json");
    {
        std::ofstream f(tmp.path());
        f << "{\"partial\":\"kept\"";
    }

    LocalisedStrings strings;
    REQUIRE(strings.load_json_file(tmp.path_string()));
    REQUIRE(strings.translate("partial") == "kept");
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

TEST_CASE("i18n system_locale normalizes LANG where supported",
          "[runtime][i18n][coverage]") {
    ScopedLang lang("fr_CA.UTF-8");

#if defined(_WIN32)
    REQUIRE(LocalisedStrings::system_locale() == "fr_CA.UTF-8");
#else
    REQUIRE(LocalisedStrings::system_locale() == "fr");
#endif
}

TEST_CASE("i18n system_locale falls back when LANG is absent",
          "[runtime][i18n][coverage]") {
    ScopedLang lang(nullptr);
    REQUIRE(LocalisedStrings::system_locale() == "en");
}
