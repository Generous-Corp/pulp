#include "create_build_commands.hpp"

#include "create_targets.hpp"
#include "shell_quote.hpp"

namespace fs = std::filesystem;

namespace pulp::cli {

std::string create_standalone_configure_command(const fs::path& source_dir,
                                                const fs::path& build_dir,
                                                bool debug_build,
                                                const fs::path& sdk_dir) {
    return "cmake -S " + shell_quote(source_dir)
        + " -B " + shell_quote(build_dir)
        + " -DCMAKE_BUILD_TYPE=" + shell_quote(create_build_config(debug_build))
        + " -DCMAKE_PREFIX_PATH=" + shell_quote(sdk_dir);
}

std::string create_standalone_ctest_command(const fs::path& build_dir,
                                            bool debug_build) {
    return "ctest --test-dir " + shell_quote(build_dir)
        + " -C " + shell_quote(create_build_config(debug_build))
        + " --output-on-failure";
}

}  // namespace pulp::cli
