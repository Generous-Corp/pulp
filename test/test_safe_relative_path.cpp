#include <pulp/runtime/safe_relative_path.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using pulp::runtime::is_safe_relative_path;
using pulp::runtime::is_within_directory;
namespace fs = std::filesystem;

namespace {
bool safe(const char* text) {
    return is_safe_relative_path(fs::path(text));
}
} // namespace

TEST_CASE("safe relative paths accept plain nested names", "[runtime][path-safety]") {
    CHECK(safe("ui.js"));
    CHECK(safe("sub/theme.json"));
    CHECK(safe("a/b/c.txt"));
    CHECK(safe("name with spaces.wav"));
    CHECK(safe("m\xC3\xADxed.wav"));
}

TEST_CASE("safe relative paths refuse every rooted form on every platform",
          "[runtime][path-safety]") {
    CHECK_FALSE(safe(""));
    CHECK_FALSE(safe("/rooted"));
    CHECK_FALSE(safe("/etc/passwd"));
    CHECK_FALSE(safe("\\rooted"));
    CHECK_FALSE(safe("C:foo"));
    CHECK_FALSE(safe("C:\\x"));
    CHECK_FALSE(safe("C:/x"));
    CHECK_FALSE(safe("\\\\server\\share"));
    CHECK_FALSE(safe("//server/share"));
}

TEST_CASE("safe relative paths refuse traversal and separator smuggling",
          "[runtime][path-safety]") {
    CHECK_FALSE(safe(".."));
    CHECK_FALSE(safe("../escape.txt"));
    CHECK_FALSE(safe("a/../../b"));
    CHECK_FALSE(safe("a/../b"));
    CHECK_FALSE(safe("a\\..\\b"));
    CHECK_FALSE(safe("a/..\\..\\b"));
    CHECK_FALSE(safe("."));
    CHECK_FALSE(safe("./a.txt"));
    CHECK_FALSE(safe("a/./b.txt"));
    CHECK_FALSE(safe("a/"));
    CHECK_FALSE(safe("a:b"));
    CHECK_FALSE(safe("a/b:c"));
    CHECK_FALSE(is_safe_relative_path(fs::path(std::string("a\0b", 3))));
}

TEST_CASE("is_within_directory compares normalized paths", "[runtime][path-safety]") {
    const fs::path root = fs::path("dest") / "root";
    CHECK(is_within_directory(root, root / "a" / "b.txt"));
    CHECK(is_within_directory(root, root));
    CHECK(is_within_directory(fs::path("dest/root/"), fs::path("dest/root/a")));
    CHECK_FALSE(is_within_directory(root, root / ".." / "sibling"));
    CHECK_FALSE(is_within_directory(root, fs::path("dest") / "rootish" / "a"));
    CHECK_FALSE(is_within_directory(root, fs::path("dest")));
}

TEST_CASE("is_within_directory refuses an empty directory", "[runtime][path-safety]") {
    CHECK_FALSE(is_within_directory(fs::path(""), fs::path("/etc")));
    CHECK_FALSE(is_within_directory(fs::path(""), fs::path("a")));
    CHECK_FALSE(is_within_directory(fs::path(""), fs::path("")));
}

TEST_CASE("is_within_directory treats a dot directory as the current directory",
          "[runtime][path-safety]") {
    CHECK(is_within_directory(fs::path("."), fs::path("./x")));
    CHECK(is_within_directory(fs::path("."), fs::path("x/y")));
    CHECK(is_within_directory(fs::path("."), fs::path(".")));
    CHECK(is_within_directory(fs::path("a/.."), fs::path("x")));
    CHECK_FALSE(is_within_directory(fs::path("."), fs::path("../x")));
    CHECK_FALSE(is_within_directory(fs::path("."), fs::path("x/../../y")));
    CHECK_FALSE(is_within_directory(fs::path("."), fs::path("/x")));
}
