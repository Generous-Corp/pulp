// test_icu_data_breaks.cpp: break iteration through the ICU data Pulp links.
//
// Pulp links Skia's ICU with a trimmed data package (see
// tools/cmake/scripts/icu_data_subset.py). Break iteration is the part of
// ICU that reads the most data at run time: rule files per break type and
// locale, plus the dictionaries that segment Thai, Lao, Khmer, Burmese and
// CJK text. Each expectation below is what the untrimmed ICU data produces,
// so a subset that drops a rule file or dictionary still in use changes a
// boundary list here and fails.

#include <catch2/catch_test_macros.hpp>

#ifdef PULP_HAS_SKIA
#include "modules/skunicode/include/SkUnicode_icu.h"
#endif

#include <string>
#include <vector>

namespace {

#ifdef PULP_HAS_SKIA
std::vector<int> boundaries(const char* locale, SkUnicode::BreakType type,
                            const std::string& text) {
    static sk_sp<SkUnicode> unicode = SkUnicodes::ICU::Make();
    REQUIRE(unicode);
    auto it = unicode->makeBreakIterator(locale, type);
    REQUIRE(it);
    REQUIRE(it->setText(text.data(), static_cast<int>(text.size())));
    std::vector<int> out;
    for (auto pos = it->first(); !it->isDone(); pos = it->next())
        out.push_back(pos);
    return out;
}
#endif

} // namespace

TEST_CASE("ICU data keeps Latin word, line and sentence rules", "[icu][text][footprint]") {
#ifndef PULP_HAS_SKIA
    SKIP("Skia not linked");
#else
    const std::string text = "Hello, world! It's 3.5 dB. Next line";
    CHECK(boundaries("en", SkUnicode::BreakType::kWords, text) ==
          std::vector<int>{0, 5, 6, 7, 12, 13, 14, 18, 19, 22, 23, 25, 26, 27, 31, 32, 36});
    CHECK(boundaries("en", SkUnicode::BreakType::kLines, text) ==
          std::vector<int>{0, 7, 14, 19, 23, 27, 32, 36});
    CHECK(boundaries("en", SkUnicode::BreakType::kSentences, text) ==
          std::vector<int>{0, 14, 27, 36});
#endif
}

TEST_CASE("ICU data keeps the South-East Asian dictionaries", "[icu][text][footprint]") {
#ifndef PULP_HAS_SKIA
    SKIP("Skia not linked");
#else
    // Thai and Khmer have no spaces between words; only the dictionaries
    // find the boundaries.
    const std::string thai =
        "\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5"
        "\xE0\xB8\x84\xE0\xB8\xA3\xE0\xB8\xB1\xE0\xB8\x9A"
        "\xE0\xB8\xA2\xE0\xB8\xB4\xE0\xB8\x99\xE0\xB8\x94\xE0\xB8\xB5"; // สวัสดีครับยินดี
    CHECK(boundaries("th", SkUnicode::BreakType::kWords, thai) == std::vector<int>{0, 18, 30, 45});
    CHECK(boundaries("th", SkUnicode::BreakType::kLines, thai) == std::vector<int>{0, 18, 30, 45});

    const std::string khmer =
        "\xE1\x9E\x81\xE1\x9F\x92\xE1\x9E\x89\xE1\x9E\xBB\xE1\x9F\x86"
        "\xE1\x9E\x9F\xE1\x9F\x92\xE1\x9E\x9A\xE1\x9E\x9B\xE1\x9E\xB6\xE1\x9E\x89\xE1\x9F\x8B"
        "\xE1\x9E\xA2\xE1\x9F\x92\xE1\x9E\x93\xE1\x9E\x80"; // ខ្ញុំស្រលាញ់អ្នក
    CHECK(boundaries("km", SkUnicode::BreakType::kWords, khmer) == std::vector<int>{0, 15, 36, 48});
#endif
}

TEST_CASE("ICU data keeps the CJK dictionary and line rules", "[icu][text][footprint]") {
#ifndef PULP_HAS_SKIA
    SKIP("Skia not linked");
#else
    const std::string ja = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE"
                           "\xE3\x83\x86\xE3\x82\xAD\xE3\x82\xB9\xE3\x83\x88"; // 日本語のテキスト
    CHECK(boundaries("ja", SkUnicode::BreakType::kWords, ja) == std::vector<int>{0, 9, 12, 24});
    CHECK(boundaries("ja", SkUnicode::BreakType::kLines, ja) ==
          std::vector<int>{0, 3, 6, 9, 12, 15, 18, 21, 24});

    const std::string zh = "\xE6\x88\x91\xE4\xBB\xAC\xE4\xBB\x8A\xE5\xA4\xA9"
                           "\xE5\x8E\xBB\xE5\x85\xAC\xE5\x9B\xAD"; // 我们今天去公园
    CHECK(boundaries("zh", SkUnicode::BreakType::kWords, zh) == std::vector<int>{0, 6, 12, 15, 21});
#endif
}

TEST_CASE("ICU data keeps grapheme rules for emoji and combining marks", "[icu][text][footprint]") {
#ifndef PULP_HAS_SKIA
    SKIP("Skia not linked");
#else
    // Family ZWJ sequence (one cluster), "e" + combining acute, flag pair.
    const std::string text =
        "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7"
        "e\xCC\x81"
        "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5";
    CHECK(boundaries("en", SkUnicode::BreakType::kGraphemes, text) ==
          std::vector<int>{0, 18, 21, 29});
#endif
}
