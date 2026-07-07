// VirtualList sample-manager proof.
//
//   pulp-virtual-list-sample-manager --out /tmp/sample-manager.png

#include "sample_manager_virtual_list.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

using namespace pulp::examples::virtual_list_sample_manager;

int main(int argc, char** argv) {
    std::string out = "virtual-list-sample-manager.png";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    }

    auto fixture = build_sample_manager();
    auto result = pulp::view::capture_view(*fixture.root, kWidth, kHeight, 2.0f);
    if (!result.ok) {
        std::fprintf(stderr, "capture failed: %s\n", result.reason.c_str());
        return 1;
    }

    std::ofstream file(out, std::ios::binary);
    file.write(reinterpret_cast<const char*>(result.png.data()),
               static_cast<std::streamsize>(result.png.size()));
    std::printf("wrote %s (%ux%u, %zu bytes)\n", out.c_str(), kWidth, kHeight,
                result.png.size());
    return file.good() ? 0 : 1;
}
