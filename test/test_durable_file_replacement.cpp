#include "../core/runtime/src/durable_file_replacement_test_access.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/temporary_file.hpp>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

using pulp::runtime::TemporaryFile;
using pulp::runtime::detail::DurableFileCommitOutcome;
using pulp::runtime::detail::DurableFileReplacement;
using pulp::runtime::detail::DurableFileReplacementTestAccess;

std::span<const std::uint8_t> bytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("durable replacement reports a completed durable commit", "[runtime][durability]") {
    TemporaryFile destination;
    auto replacement = DurableFileReplacement::create(destination.path());
    REQUIRE(replacement);
    REQUIRE(replacement->write_all(bytes("durable")));

    REQUIRE(replacement->commit() == DurableFileCommitOutcome::ReplacedDurably);
    REQUIRE(read_file(destination.path()) == "durable");
}

TEST_CASE("durable replacement distinguishes a post-rename directory sync failure",
          "[runtime][durability]") {
    TemporaryFile destination;
    {
        std::ofstream original(destination.path(), std::ios::binary);
        original << "original";
    }
    auto replacement = DurableFileReplacement::create(destination.path());
    REQUIRE(replacement);
    REQUIRE(replacement->write_all(bytes("replacement")));
    const auto temporary = replacement->temporary_path();
    DurableFileReplacementTestAccess::fail_parent_directory_sync(*replacement);

    REQUIRE(replacement->commit() == DurableFileCommitOutcome::ReplacedButDirectorySyncFailed);
    REQUIRE(read_file(destination.path()) == "replacement");
    REQUIRE_FALSE(std::filesystem::exists(temporary));
}

TEST_CASE("durable replacement refuses to overwrite a changed destination identity",
          "[runtime][durability]") {
    TemporaryFile destination;
    {
        std::ofstream original(destination.path(), std::ios::binary);
        original << "approved";
    }
    auto replacement = DurableFileReplacement::create(destination.path());
    REQUIRE(replacement);
    REQUIRE(replacement->write_all(bytes("replacement")));

    auto approved_path = destination.path();
    approved_path += ".approved";
    std::filesystem::rename(destination.path(), approved_path);
    {
        std::ofstream attacker(destination.path(), std::ios::binary);
        attacker << "changed identity";
    }

    REQUIRE(replacement->commit() == DurableFileCommitOutcome::NotReplaced);
    REQUIRE(read_file(destination.path()) == "changed identity");
    std::error_code ignored;
    std::filesystem::remove(approved_path, ignored);
}

TEST_CASE("durable replacement rejects a multiply linked destination", "[runtime][durability]") {
    TemporaryFile destination;
    auto alias = destination.path();
    alias += ".alias";
    std::error_code link_error;
    std::filesystem::create_hard_link(destination.path(), alias, link_error);
    REQUIRE_FALSE(link_error);

    REQUIRE_FALSE(DurableFileReplacement::create(destination.path()));
    std::filesystem::remove(alias);
}

TEST_CASE("durable replacement rechecks destination link count before commit",
          "[runtime][durability]") {
    TemporaryFile destination;
    auto replacement = DurableFileReplacement::create(destination.path());
    REQUIRE(replacement);
    REQUIRE(replacement->write_all(bytes("replacement")));
    auto alias = destination.path();
    alias += ".alias";
    std::error_code link_error;
    std::filesystem::create_hard_link(destination.path(), alias, link_error);
    REQUIRE_FALSE(link_error);

    REQUIRE(replacement->commit() == DurableFileCommitOutcome::NotReplaced);
    REQUIRE(read_file(destination.path()).empty());
    std::filesystem::remove(alias);
}

TEST_CASE("durable replacement rechecks temporary link count before commit",
          "[runtime][durability]") {
    TemporaryFile destination;
    auto replacement = DurableFileReplacement::create(destination.path());
    REQUIRE(replacement);
    REQUIRE(replacement->write_all(bytes("replacement")));
    auto alias = replacement->temporary_path();
    alias += ".alias";
    std::error_code link_error;
    std::filesystem::create_hard_link(replacement->temporary_path(), alias, link_error);
    REQUIRE_FALSE(link_error);

    REQUIRE(replacement->commit() == DurableFileCommitOutcome::NotReplaced);
    REQUIRE(read_file(destination.path()).empty());
    std::filesystem::remove(alias);
}

TEST_CASE("durable replacement translates path creation failures", "[runtime][durability]") {
    TemporaryFile anchor;
    const auto invalid_destination = anchor.path().parent_path() / std::string(5000, 'x');
    std::optional<DurableFileReplacement> replacement;
    REQUIRE_NOTHROW(replacement = DurableFileReplacement::create(invalid_destination));
    REQUIRE_FALSE(replacement);
}

#if defined(_WIN32)
TEST_CASE("durable replacement preserves Windows file attributes", "[runtime][durability]") {
    TemporaryFile destination;
    constexpr DWORD preserved = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_READONLY;
    REQUIRE(::SetFileAttributesW(destination.path().c_str(), preserved) != 0);
    auto replacement = DurableFileReplacement::create(destination.path());
    REQUIRE(replacement);
    REQUIRE(replacement->write_all(bytes("replacement")));

    REQUIRE(replacement->commit() == DurableFileCommitOutcome::ReplacedDurably);
    const auto attributes = ::GetFileAttributesW(destination.path().c_str());
    REQUIRE(attributes != INVALID_FILE_ATTRIBUTES);
    REQUIRE((attributes & preserved) == preserved);
    REQUIRE(::SetFileAttributesW(destination.path().c_str(), FILE_ATTRIBUTE_NORMAL) != 0);
}
#endif
