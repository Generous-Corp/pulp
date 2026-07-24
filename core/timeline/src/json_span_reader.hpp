#pragma once

#include <pulp/timeline/serialize.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace pulp::timeline::detail {

struct JsonSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct JsonSpanMember {
    explicit constexpr JsonSpanMember(std::string_view member_name) noexcept
        : name(member_name) {}

    std::string_view name;
    JsonSpan span;
    bool found = false;
};

class JsonSpanReader {
  public:
    JsonSpanReader(std::string_view source, const DecodeLimits& limits) noexcept;

    void skip_space(std::size_t& position) const noexcept;
    bool scan_string(std::size_t& position, std::string* captured = nullptr,
                     std::size_t capture_limit = 128);
    bool skip_value(std::size_t& position, std::size_t depth, JsonSpan& span);
    bool member(JsonSpan object, std::string_view wanted, JsonSpan& result, bool& found);
    bool members(JsonSpan object, std::span<JsonSpanMember> wanted,
                 std::size_t* member_count = nullptr);
    bool string_value(JsonSpan span, std::string& value);
    bool decoded_string(JsonSpan span, std::string& value, std::string path);
    bool canonical_u64(JsonSpan span, std::uint64_t& value, std::string path);
    bool u32_value(JsonSpan span, std::uint32_t& value) const noexcept;

    bool has_error() const noexcept {
        return has_error_;
    }
    const PersistenceError& error() const noexcept {
        return error_;
    }

  private:
    bool decode_string(std::size_t& position, std::string& output);
    bool append_codepoint(std::uint32_t codepoint, std::string& output,
                          std::size_t& decoded_bytes, std::size_t start);
    bool quad(std::size_t& position, std::uint32_t& output);
    static int hex_digit(char value) noexcept;
    void set_error(PersistenceErrorCode code, std::size_t offset, std::uint64_t actual = 0,
                   std::uint64_t limit = 0, std::string path = {});

    std::string_view source_;
    const DecodeLimits& limits_;
    PersistenceError error_;
    bool has_error_ = false;
};

} // namespace pulp::timeline::detail
