#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

namespace pulp_test {

// The checkout root named by a compile-time definition, checked before any
// fixture path is built from it. A wrong root fails the case here, naming the
// root and the working directory, instead of surfacing later as an empty read.
//
// Fixture paths come from a definition rather than __FILE__: with ccache's
// base_dir set, __FILE__ is rewritten relative to the compiler's working
// directory (the build tree under Ninja), so it stops resolving once the test
// runs from any other directory. tools/scripts/check_test_file_paths.py
// rejects __FILE__ used as a path anywhere under test/.
inline std::filesystem::path checked_checkout_root(const char* value, const char* definition) {
    const std::filesystem::path root{value ? value : ""};
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(root / "test", ec)) {
        throw std::runtime_error(std::string(definition) + " is '" + root.string() +
                                 "', which holds no test/ directory (working directory: " +
                                 std::filesystem::current_path(ec).string() + ")");
    }
    return root;
}

#ifdef PULP_SOURCE_DIR
// The checkout root pulp_test_data() defines for a declaring source.
inline std::filesystem::path fixture_root() {
    return checked_checkout_root(PULP_SOURCE_DIR, "PULP_SOURCE_DIR");
}
#endif

}  // namespace pulp_test
