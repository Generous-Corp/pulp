// SPDX-License-Identifier: MIT
#include "isolated_clap_channel.hpp"
#include "isolated_clap_windows_channel.hpp"
#include <pulp/host/plugin_slot.hpp>
#include <pulp/midi/ump_buffer.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/memory_mapped_file.hpp>

#if defined(PULP_ISOLATED_CLAP_SUPPORTED) &&                                                       \
    (defined(PULP_ISOLATED_CLAP_POSIX_TRANSPORT) || defined(PULP_ISOLATED_CLAP_WINDOWS_TRANSPORT))
#include <algorithm>
#include <clap/clap.h>
#include <clap/ext/audio-ports.h>
#include <clap/ext/latency.h>
#include <clap/ext/state.h>
#include <clap/ext/tail.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <thread>

namespace {
using namespace pulp;
using namespace host::isolation;
using Clock = std::chrono::steady_clock;
constexpr auto kControlDeadline = std::chrono::seconds(5);

// The worker end of the private control channel, which is its standard input,
// and the shared mapping it adopts during hello. Only hello differs between
// transports; the load, lifecycle and block loop below are shared.
#if defined(PULP_ISOLATED_CLAP_POSIX_TRANSPORT)
class Transport {
  public:
    enum class Poll { idle, readable, closed, failed };
    // argv: worker, mapping path, plugin, descriptor, profile.
    static constexpr int kArguments = 5;

    bool send(const ControlRecord& record, Deadline deadline) {
        return send_record(STDIN_FILENO, record, deadline);
    }
    bool receive(ControlRecord& record, Deadline deadline) {
        return receive_record(STDIN_FILENO, record, deadline);
    }
    bool move(void* data, std::size_t size, bool sending, Deadline deadline) {
        return transfer(STDIN_FILENO, data, size, sending, deadline);
    }
    Poll poll() {
        pollfd channel{STDIN_FILENO, POLLIN, 0};
        const auto polled = ::poll(&channel, 1, 0);
        if (polled < 0 && errno != EINTR)
            return Poll::failed;
        if (channel.revents & (POLLHUP | POLLERR | POLLNVAL))
            return Poll::closed;
        return (channel.revents & POLLIN) ? Poll::readable : Poll::idle;
    }
    void idle_wait() {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    static std::filesystem::path own_executable(char** argv) {
        std::error_code ec;
        return std::filesystem::canonical(argv[0], ec);
    }

    /// Receives hello and maps the file it names. Returns 0 or the exit code.
    int receive_hello(char** argv, ControlRecord& hello, Deadline deadline) {
        // A socket's SIGPIPE policy is per descriptor; it cannot change host policy.
        int no_sigpipe = 1;
        if (::setsockopt(STDIN_FILENO, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)))
            return 65;
        if (!receive(hello, deadline) || !hello_header_ok(hello))
            return 66;
        if (!mapping_.open_no_follow(argv[1], runtime::MapMode::ReadWrite, kMappingBytes) ||
            mapping_.size() != kMappingBytes)
            return 67;
        auto identity = mapping_.opened_file_identity();
        if (!identity.valid || identity.volume != hello.volume || identity.file != hello.file ||
            identity.generation != hello.file_generation ||
            !mapping_.path_refers_to_open_file(argv[1]))
            return 68;
        shared_ = reinterpret_cast<Mapping*>(mapping_.mutable_data());
        if (shared_->magic != kMagic || shared_->abi != kAbi ||
            shared_->generation != hello.generation)
            return 69;
        return 0;
    }
    std::uint64_t process_id() const {
        return static_cast<std::uint64_t>(::getpid());
    }
    Mapping* shared() const {
        return shared_;
    }

  private:
    runtime::MemoryMappedFile mapping_;
    Mapping* shared_ = nullptr;
};
#else
using windows::Deadline;
class Transport {
  public:
    enum class Poll { idle, readable, closed, failed };
    // argv: worker, plugin, descriptor, profile. The section arrives as a handle.
    static constexpr int kArguments = 4;

    Transport() {
        // A high-resolution timer keeps the prepared loop's idle wait near the
        // POSIX 50 us poll; Sleep would round up to the scheduler tick.
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
    }
    ~Transport() {
        if (timer_ != nullptr)
            CloseHandle(timer_);
    }
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    bool send(const ControlRecord& record, Deadline deadline) {
        return windows::send_record(channel_, record, deadline);
    }
    bool receive(ControlRecord& record, Deadline deadline) {
        return windows::receive_record(channel_, record, deadline);
    }
    bool move(void* data, std::size_t size, bool sending, Deadline deadline) {
        return windows::transfer(channel_, data, size, sending, deadline);
    }
    Poll poll() {
        DWORD available = 0;
        if (!PeekNamedPipe(reinterpret_cast<HANDLE>(channel_), nullptr, 0, nullptr, &available,
                           nullptr))
            return GetLastError() == ERROR_BROKEN_PIPE ? Poll::closed : Poll::failed;
        return available != 0 ? Poll::readable : Poll::idle;
    }
    void idle_wait() {
        LARGE_INTEGER due{};
        due.QuadPart = -500; // 50 us, relative, in 100 ns units.
        if (timer_ != nullptr && SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0))
            WaitForSingleObject(timer_, INFINITE);
        else
            SwitchToThread();
    }
    static std::filesystem::path own_executable(char**) {
        std::wstring path(32768, L'\0');
        const DWORD size =
            GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (size == 0 || size >= path.size())
            return {};
        path.resize(size);
        std::error_code ec;
        return std::filesystem::canonical(path, ec);
    }

    /// Receives hello and adopts the section it names. Returns 0 or the exit code.
    int receive_hello(char**, ControlRecord& hello, Deadline deadline) {
        if (channel_ == 0 || channel_ == reinterpret_cast<std::intptr_t>(INVALID_HANDLE_VALUE))
            return 65;
        if (!windows::worker_receive_hello(channel_, section_, hello, deadline))
            return 66;
        return 0;
    }
    std::uint64_t process_id() const {
        return GetCurrentProcessId();
    }
    Mapping* shared() const {
        return section_.mapping();
    }

  private:
    std::intptr_t channel_ = reinterpret_cast<std::intptr_t>(GetStdHandle(STD_INPUT_HANDLE));
    windows::SharedSection section_;
    HANDLE timer_ = nullptr;
};
#endif

struct Native : host::NativeHandleVisitor {
    const clap_plugin_t* plugin = nullptr;
    void visit_clap(const host::PluginSlot&, const host::ClapNativeHandle& handle) override {
        plugin = static_cast<const clap_plugin_t*>(handle.plugin);
    }
};

bool admitted(const clap_plugin_t* plugin) {
    if (!plugin || !plugin->desc || !plugin->desc->features)
        return false;
    bool effect = false;
    for (const char* const* feature = plugin->desc->features; *feature; ++feature) {
        if (std::strcmp(*feature, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0)
            return false;
        effect |= std::strcmp(*feature, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT) == 0;
    }
    auto* ports = static_cast<const clap_plugin_audio_ports_t*>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    auto* latency =
        static_cast<const clap_plugin_latency_t*>(plugin->get_extension(plugin, CLAP_EXT_LATENCY));
    auto* tail =
        static_cast<const clap_plugin_tail_t*>(plugin->get_extension(plugin, CLAP_EXT_TAIL));
    if (!effect || !ports || !latency || !tail || latency->get(plugin) != 0 ||
        tail->get(plugin) != 0)
        return false;
    for (bool input : {true, false}) {
        clap_audio_port_info_t port{};
        if (ports->count(plugin, input) != 1 || !ports->get(plugin, 0, input, &port) ||
            port.channel_count != kChannels || !(port.flags & CLAP_AUDIO_PORT_IS_MAIN))
            return false;
    }
    return true;
}

struct CappedState {
    std::vector<std::uint8_t> bytes;
    bool failed = false;
    static std::int64_t CLAP_ABI write(const clap_ostream_t* stream, const void* data,
                                       std::uint64_t count) {
        auto& state = *static_cast<CappedState*>(stream->ctx);
        if (count == 0)
            return 0;
        if (!data || state.failed || count > kCheckpointBytes - state.bytes.size()) {
            state.failed = true;
            return -1;
        }
        const auto* start = static_cast<const std::uint8_t*>(data);
        state.bytes.insert(state.bytes.end(), start, start + count);
        return static_cast<std::int64_t>(count);
    }
};

bool save(const clap_plugin_t* plugin, std::vector<std::uint8_t>& bytes) {
    auto* extension =
        static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    if (!extension)
        return false;
    CappedState state;
    state.bytes.reserve(kCheckpointBytes);
    const clap_ostream_t stream{&state, CappedState::write};
    if (!extension->save(plugin, &stream) || state.failed)
        return false;
    bytes = std::move(state.bytes);
    return true;
}

int run(int argc, char** argv) {
    if (argc != Transport::kArguments || std::strcmp(argv[argc - 1], kProfileName) != 0)
        return 64;
    const char* const plugin_path = argv[argc - 3];
    const char* const descriptor = argv[argc - 2];
    Transport channel;
    ControlRecord hello;
    auto deadline = Clock::now() + kControlDeadline;
    if (const int refused = channel.receive_hello(argv, hello, deadline))
        return refused;
    auto* shared = channel.shared();
    const auto worker_hash =
        runtime::sha256_file_hex(Transport::own_executable(argv), std::uint64_t{1} << 32);
    const auto plugin_hash =
        runtime::sha256_file_hex(clap_hash_path(plugin_path), std::uint64_t{1} << 32);
    if (!worker_hash || !plugin_hash ||
        !hello_identity_matches(hello, *worker_hash, *plugin_hash, PULP_CLAP_WORKER_BUILD))
        return 70;
    // Mapping handshake precedes vendor loading, so the parent can release the
    // mapping name (POSIX) even when the vendor's factory never returns.
    hello.process_id = channel.process_id();
    hello.result = 1;
    if (!channel.send(hello, deadline))
        return 71;
    host::PluginSelection selection{plugin_path, {"clap", descriptor}};
    auto loaded = host::PluginSlot::load_exact(selection);
    if (!std::holds_alternative<std::unique_ptr<host::PluginSlot>>(loaded.value))
        return 72;
    auto slot = std::move(std::get<std::unique_ptr<host::PluginSlot>>(loaded.value));
    Native native;
    slot->accept(native);
    auto after_hash = runtime::sha256_file_hex(clap_hash_path(plugin_path), std::uint64_t{1} << 32);
    if (!after_hash || after_hash != plugin_hash || !admitted(native.plugin))
        return 73;
    host::PluginHostSettings settings;
    settings.deliver_transport = false;
    if (!slot->set_host_settings(settings))
        return 74;
    slot->set_preferred_channel_layout(2, 2);
    hello.result = 2;
    if (!channel.send(hello, Clock::now() + kControlDeadline))
        return 75;

    audio::Buffer<float> input(kChannels, kFrames), output(kChannels, kFrames);
    midi::MidiBuffer midi_in, midi_out;
    host::ParameterEventQueue params;
    bool prepared = false;
    std::uint64_t sequence = 0;
    for (;;) {
        const auto polled = channel.poll();
        if (polled == Transport::Poll::failed)
            return 76;
        if (polled == Transport::Poll::closed)
            return 77;
        if (polled == Transport::Poll::readable) {
            ControlRecord request;
            deadline = Clock::now() + kControlDeadline;
            if (!channel.receive(request, deadline) || request.generation != hello.generation ||
                request.payload_bytes > kCheckpointBytes)
                return 86;
            auto reply = request;
            reply.result = 0;
            reply.payload_bytes = 0;
            std::vector<std::uint8_t> state;
            switch (request.command) {
            case Command::prepare:
                if (!prepared && slot->prepare(kSampleRate, kFrames) && admitted(native.plugin)) {
                    prepared = true;
                    reply.result = 1;
                }
                break;
            case Command::stop_processing:
                if (prepared)
                    slot->release();
                prepared = false;
                reply.result = 1;
                break;
            case Command::checkpoint:
                if (!prepared && save(native.plugin, state)) {
                    reply.result = 1;
                    reply.payload_bytes = state.size();
                }
                break;
            case Command::restore:
                state.resize(static_cast<std::size_t>(request.payload_bytes));
                if (!channel.move(state.data(), state.size(), false, deadline))
                    return 79;
                if (!prepared && slot->restore_state(state))
                    reply.result = 1;
                break;
            case Command::destroy:
                if (prepared)
                    slot->release();
                slot.reset();
                reply.result = 1;
                if (!channel.send(reply, deadline))
                    return 80;
                return 0;
            default:
                return 81;
            }
            if (!channel.send(reply, deadline))
                return 82;
            if (request.command == Command::checkpoint && reply.result &&
                !channel.move(state.data(), state.size(), true, deadline))
                return 83;
        }
        if (!prepared) {
            channel.idle_wait();
            continue;
        }
        auto& request = shared->requests[sequence % kSlots];
        // Acquire exclusive payload ownership from the callback producer.
        if (request.ready.load(std::memory_order_acquire) == 0) {
            channel.idle_wait();
            continue;
        }
        const Block block = request.block;
        request.ready.store(0, std::memory_order_release);
        if (!valid_block(block, hello.generation, sequence))
            return 84;
        auto& completion = shared->completions[sequence % kSlots];
        if (completion.ready.load(std::memory_order_acquire) != 0)
            return 85;
        for (std::size_t c = 0; c < kChannels; ++c) {
            std::copy_n(block.audio.data() + c * kFrames, kFrames, input.channel(c).data());
            std::fill_n(output.channel(c).data(), kFrames, 0.0f);
        }
        auto in = static_cast<const audio::Buffer<float>&>(input).view();
        auto out = output.view();
        midi_out.clear();
        midi_out.clear_sysex();
        if (midi_out.ump())
            midi_out.ump()->clear();
        slot->begin_process();
        slot->process(out, in, midi_in, midi_out, params, static_cast<int>(kFrames));
        Block result;
        result.generation = hello.generation;
        result.sequence = sequence;
        // Finite partial writes do not make CLAP_PROCESS_ERROR usable audio.
        if (slot->process_status() != host::PluginSlot::ProcessStatus::ok)
            result.result = BlockResult::plugin_error;
        for (std::size_t c = 0; c < kChannels; ++c) {
            for (std::size_t n = 0; n < kFrames; ++n) {
                const auto sample = output.channel(c).data()[n];
                if (!std::isfinite(sample))
                    result.result = BlockResult::invalid_audio;
                result.audio[c * kFrames + n] = sample;
            }
        }
        if (slot->restart_request_count() != 0 || !midi_out.empty() || !midi_out.sysex().empty() ||
            (midi_out.ump() && !midi_out.ump()->empty()))
            result.result = BlockResult::plugin_error;
        completion.block = result;
        completion.ready.store(1, std::memory_order_release);
        ++sequence;
    }
}
} // namespace
int main(int argc, char** argv) {
    return run(argc, argv);
}
#else
int main() {
    return 78;
} // Explicitly unsupported; no vendor load.
#endif
