// Child process for test_unique_temp_dir.cpp: creates <count> directories
// under <parent> with make_unique_temp_dir and prints its pid, then one created
// path per line.

#include "support/unique_temp_dir.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <parent> <count> <prefix>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path parent = argv[1];
    const int count = std::atoi(argv[2]);
    try {
        std::printf("pid %llu\n",
                    static_cast<unsigned long long>(pulp::test::current_process_id()));
        for (int i = 0; i < count; ++i) {
            const auto dir = pulp::test::make_unique_temp_dir(argv[3], parent);
            std::printf("%s\n", dir.string().c_str());
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
