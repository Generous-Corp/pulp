#pragma once

#include <filesystem>
#include <functional>

namespace pulp::host {

struct GraphSerializerTestAccess {
    static void set_after_first_read(std::function<void(const std::filesystem::path&)> hook);
    static void fail_next_write() noexcept;
    static void fail_next_replace() noexcept;
};

} // namespace pulp::host
