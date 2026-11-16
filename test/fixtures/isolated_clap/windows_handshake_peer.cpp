// SPDX-License-Identifier: MIT
// Worker half of the Windows isolated CLAP handshake, without vendor loading.
// The mode argument selects one honest or deliberately defective behavior.
#include "isolated_clap_windows_channel.hpp"

#include <cstring>
#include <thread>

int main(int argc, char** argv) {
    using namespace pulp::host::isolation;
    if (argc != 2)
        return 64;
    const std::string_view mode = argv[1];
    const auto channel = reinterpret_cast<std::intptr_t>(GetStdHandle(STD_INPUT_HANDLE));
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    windows::SharedSection section;
    ControlRecord hello;
    if (!windows::worker_receive_hello(channel, section, hello, deadline))
        return 66;
    if (mode == "silent") {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    if (mode == "wrong-section")
        hello.file += 4;
    if (mode == "wrong-generation")
        hello.generation ^= 1;
    if (mode == "wrong-build")
        hello.build[0] ^= 1;
    if (mode == "wrong-pid") {
        hello.result = 1;
        hello.process_id = GetCurrentProcessId() + 4;
        if (!windows::send_record(channel, hello, deadline))
            return 71;
    } else if (!windows::worker_reply_hello(channel, hello, deadline)) {
        return 71;
    }
    if (mode != "skip-write") {
        auto& completion = section.mapping()->completions[0];
        completion.block.generation = hello.generation;
        completion.block.sequence = 0;
        completion.block.audio.fill(0.5f);
        completion.ready.store(1, std::memory_order_release);
    }
    // Lifecycle tail: answer one destroy, then exit.
    ControlRecord request;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    if (!windows::receive_record(channel, request, deadline) || request.command != Command::destroy)
        return 72;
    request.result = 1;
    return windows::send_record(channel, request, deadline) ? 0 : 73;
}
