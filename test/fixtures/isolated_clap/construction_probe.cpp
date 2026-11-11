// SPDX-License-Identifier: MIT
#include <cstdlib>
#include <iostream>
#include <pulp/host/isolated_plugin_slot.hpp>
#include <pulp/runtime/crypto.hpp>
#include <string_view>
#include <thread>

using namespace pulp;
using namespace pulp::host;

// Private subprocess: normal failed-handshake cleanup permits creation. The
// injected post-launch construction failure must retain an incomplete finite
// reservation. Its fixture worker exits deliberately, without descendants.
int main(int argc, char** argv) {
    if (argc != 5)
        return 64;
    const auto plugin_hash = runtime::sha256_file_hex(argv[3], std::uint64_t{1} << 32);
    const auto bad_hash = runtime::sha256_file_hex(argv[1], std::uint64_t{1} << 32);
    const auto good_hash = runtime::sha256_file_hex(argv[2], std::uint64_t{1} << 32);
    if (!plugin_hash || !bad_hash || !good_hash)
        return 65;
    const bool expect_quarantine = std::string_view(argv[4]) == "quarantine";
    IsolatedClapOptions config{{argv[3], {"clap", "dev.pulp.g9.lifecycle.healthy"}},
                               expect_quarantine ? argv[2] : argv[1],
                               *plugin_hash,
                               expect_quarantine ? *good_hash : *bad_hash};
    if (expect_quarantine)
        ::setenv("PULP_G9_CONSTRUCTION_EXIT", "1", 1);
    const auto failed = load_isolated_clap(config);
    ::unsetenv("PULP_G9_CONSTRUCTION_EXIT");
    if (failed.error != IsolationReason::launch_failed)
        return 66;
    config.worker_path = argv[2];
    config.worker_sha256 = *good_hash;
    auto next = load_isolated_clap(config);
    if (expect_quarantine)
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    const bool quarantined = next.error == IsolationReason::quarantine_active;
    const bool complete = next && next.control->stop() == IsolationCleanup::complete;
    std::cout << "expect_quarantine=" << expect_quarantine << " quarantined=" << quarantined
              << " healthy_construction_complete=" << complete << '\n';
    return expect_quarantine ? (quarantined ? 0 : 42) : (complete ? 0 : 42);
}
