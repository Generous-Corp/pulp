// SPDX-License-Identifier: MIT
// Extends planning-owned G9 lifecycle fixture. Hostile test code; never installed.
#include <array>
#include <chrono>
#include <clap/clap.h>
#include <clap/ext/audio-ports.h>
#include <clap/ext/latency.h>
#include <clap/ext/state.h>
#include <clap/ext/tail.h>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <thread>
#include <unistd.h>

namespace {
enum class Mode {
    healthy,
    state,
    slow,
    overrun,
    never_return,
    partial_error,
    load_abort,
    activate_abort,
    state_save_abort,
    state_load_abort,
    destroy_abort,
    load_hang,
    prepare_hang,
    process_abort,
    save_hang,
    restore_hang,
    destroy_hang,
    save_oversize,
    save_ignore_error,
    descendant,
    exited_leader,
    nan,
    latency,
    tail,
    mono,
    corrupt_state,
    save_once,
    sysex_output
};
const char* const features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
#define DESC(id, name)                                                                             \
    {CLAP_VERSION_INIT, id, name, "Pulp test", "", "", "", "1", "G9 lifecycle evidence fixture",   \
     features}
const clap_plugin_descriptor_t descriptors[] = {
    DESC("dev.pulp.g9.lifecycle.healthy", "G9 lifecycle healthy"),
    DESC("dev.pulp.g9.lifecycle.state", "G9 lifecycle state"),
    DESC("dev.pulp.g9.lifecycle.slow", "G9 lifecycle slow within budget"),
    DESC("dev.pulp.g9.lifecycle.overrun", "G9 lifecycle deadline overrun"),
    DESC("dev.pulp.g9.lifecycle.never-return", "G9 lifecycle never return"),
    DESC("dev.pulp.g9.lifecycle.partial-error", "G9 lifecycle partial error"),
    DESC("dev.pulp.g9.lifecycle.load-abort", "G9 lifecycle load abort"),
    DESC("dev.pulp.g9.lifecycle.activate-abort", "G9 lifecycle activate abort"),
    DESC("dev.pulp.g9.lifecycle.state-save-abort", "G9 lifecycle state save abort"),
    DESC("dev.pulp.g9.lifecycle.state-load-abort", "G9 lifecycle state load abort"),
    DESC("dev.pulp.g9.lifecycle.destroy-abort", "G9 lifecycle destroy abort"),
    DESC("dev.pulp.g9.lifecycle.load-hang", "G9 load-hang"),
    DESC("dev.pulp.g9.lifecycle.prepare-hang", "G9 prepare-hang"),
    DESC("dev.pulp.g9.lifecycle.process-abort", "G9 process-abort"),
    DESC("dev.pulp.g9.lifecycle.save-hang", "G9 save-hang"),
    DESC("dev.pulp.g9.lifecycle.restore-hang", "G9 restore-hang"),
    DESC("dev.pulp.g9.lifecycle.destroy-hang", "G9 destroy-hang"),
    DESC("dev.pulp.g9.lifecycle.save-oversize", "G9 save-oversize"),
    DESC("dev.pulp.g9.lifecycle.save-ignore-error", "G9 save-ignore-error"),
    DESC("dev.pulp.g9.lifecycle.descendant", "G9 descendant"),
    DESC("dev.pulp.g9.lifecycle.exited-leader", "G9 exited-leader"),
    DESC("dev.pulp.g9.lifecycle.nan", "G9 nan"),
    DESC("dev.pulp.g9.lifecycle.latency", "G9 latency"),
    DESC("dev.pulp.g9.lifecycle.tail", "G9 tail"),
    DESC("dev.pulp.g9.lifecycle.mono", "G9 mono"),
    DESC("dev.pulp.g9.lifecycle.corrupt-state", "G9 corrupt-state"),
    DESC("dev.pulp.g9.lifecycle.save-once", "G9 save-once"),
    DESC("dev.pulp.g9.lifecycle.sysex-output", "G9 sysex-output"),
};
struct Instance {
    clap_plugin_t plugin;
    Mode mode;
    float gain = 0.5f;
    bool spawned = false;
    mutable unsigned saves = 0;
};
void witness(const char* stage, uint32_t frames = 0) {
    const char* path = std::getenv("PULP_G9_WITNESS");
    if (!path)
        return;
    const int fd = ::open(path, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd < 0)
        return;
    char line[160];
    const int size =
        std::snprintf(line, sizeof(line), "%s %ld %ld %u\n", stage, static_cast<long>(getpid()),
                      static_cast<long>(getppid()), frames);
    if (size > 0)
        (void)::write(fd, line, static_cast<std::size_t>(size));
    ::close(fd);
}
[[noreturn]] void hang() {
    for (;;)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
void descendants() {
    int ready[2];
    if (::pipe(ready))
        std::abort();
    if (::fork() == 0) {
        ::close(ready[0]);
        std::signal(SIGTERM, SIG_IGN);
        witness("descendant");
        if (::fork() == 0) {
            witness("grandchild");
            (void)::write(ready[1], "x", 1);
            hang();
        }
        hang();
    }
    ::close(ready[1]);
    char byte;
    if (::read(ready[0], &byte, 1) != 1)
        std::abort();
    ::close(ready[0]);
}
[[noreturn]] void fault(const char* stage, uint32_t frames = 0) {
    witness(stage, frames);
    std::abort();
}
bool init(const clap_plugin_t* p) {
    witness("load");
    if (std::getenv("PULP_G9_CONSTRUCTION_EXIT"))
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            ::_exit(0);
        }).detach();
    if (static_cast<const Instance*>(p->plugin_data)->mode == Mode::load_hang)
        hang();
    if (static_cast<const Instance*>(p->plugin_data)->mode == Mode::load_abort)
        fault("load");
    return true;
}
void destroy(const clap_plugin_t* p) {
    auto* i = static_cast<Instance*>(p->plugin_data);
    witness("destroy");
    if (i->mode == Mode::destroy_hang)
        hang();
    if (i->mode == Mode::destroy_abort)
        fault("destroy");
    delete i;
}
bool activate(const clap_plugin_t* p, double, uint32_t, uint32_t) {
    witness("prepare");
    if (static_cast<const Instance*>(p->plugin_data)->mode == Mode::prepare_hang)
        hang();
    if (static_cast<const Instance*>(p->plugin_data)->mode == Mode::activate_abort)
        fault("activate");
    return true;
}
void deactivate(const clap_plugin_t*) {}
bool start(const clap_plugin_t*) {
    return true;
}
void stop(const clap_plugin_t*) {}
void reset(const clap_plugin_t*) {}
clap_process_status process(const clap_plugin_t* p, const clap_process_t* ctx) {
    auto* i = static_cast<Instance*>(p->plugin_data);
    const auto started = std::chrono::steady_clock::now();
    witness("process", ctx->frames_count);
    if (i->mode == Mode::process_abort)
        std::abort();
    if ((i->mode == Mode::descendant || i->mode == Mode::exited_leader) && !i->spawned) {
        i->spawned = true;
        descendants();
        if (i->mode == Mode::exited_leader)
            ::_exit(0);
        hang();
    }
    if (i->mode == Mode::never_return) {
        witness("process", ctx->frames_count);
        for (;;)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (i->mode == Mode::overrun)
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    if (i->mode == Mode::slow)
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    if (i->mode == Mode::partial_error) {
        if (ctx->audio_outputs_count && ctx->audio_outputs[0].data32 &&
            ctx->audio_outputs[0].channel_count >= 2) {
            auto& out = ctx->audio_outputs[0];
            for (uint32_t n = 0; n < ctx->frames_count / 2; ++n) {
                out.data32[0][n] = 0.125f;
                out.data32[1][n] = -0.25f;
            }
        }
        witness("process-error", ctx->frames_count);
        return CLAP_PROCESS_ERROR;
    }
    if (ctx->audio_inputs_count != 1 || ctx->audio_outputs_count != 1)
        return CLAP_PROCESS_ERROR;
    const auto& in = ctx->audio_inputs[0];
    auto& out = ctx->audio_outputs[0];
    if (in.channel_count != 2 || out.channel_count != 2 || !in.data32 || !out.data32)
        return CLAP_PROCESS_ERROR;
    for (uint32_t c = 0; c < 2; ++c)
        for (uint32_t n = 0; n < ctx->frames_count; ++n)
            out.data32[c][n] = i->gain * in.data32[c][n];
    if (false) {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
        std::fprintf(stderr,
                     "G9_CALLBACK_STAGE=process-timed pid=%ld ppid=%ld frames=%u elapsed_us=%lld\n",
                     static_cast<long>(getpid()), static_cast<long>(getppid()), ctx->frames_count,
                     static_cast<long long>(us));
        std::fflush(stderr);
    }
    if (i->mode == Mode::sysex_output) {
        const std::uint8_t bytes[]{0xf0, 0x7d, 0x01, 0xf7};
        const clap_event_midi_sysex_t event{{sizeof(clap_event_midi_sysex_t), 0,
                                             CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI_SYSEX, 0},
                                            0,
                                            bytes,
                                            sizeof(bytes)};
        ctx->out_events->try_push(ctx->out_events, &event.header);
    }
    if (i->mode == Mode::nan)
        out.data32[0][0] = std::numeric_limits<float>::quiet_NaN();
    return CLAP_PROCESS_CONTINUE;
}
bool state_save(const clap_plugin_t* p, const clap_ostream_t* stream) {
    auto* i = static_cast<const Instance*>(p->plugin_data);
    if (i->mode == Mode::state_save_abort)
        fault("state-save");
    if (i->mode == Mode::save_hang)
        hang();
    if (i->mode == Mode::save_oversize || i->mode == Mode::save_ignore_error ||
        (i->mode == Mode::save_once && i->saves++ > 0)) {
        const std::array<unsigned char, 4096> bytes{};
        for (int n = 0; n < 257; ++n) {
            auto written = stream->write(stream, bytes.data(), bytes.size());
            if (written != bytes.size() && i->mode == Mode::save_oversize)
                return false;
        }
        return true;
    }
    const float saved = i->mode == Mode::corrupt_state ? std::numeric_limits<float>::quiet_NaN()
                                                       : (i->mode == Mode::state ? 0.75f : i->gain);
    return stream->write(stream, &saved, sizeof(saved)) == sizeof(saved);
}
bool state_load(const clap_plugin_t* p, const clap_istream_t* stream) {
    auto* i = static_cast<Instance*>(p->plugin_data);
    if (i->mode == Mode::state_load_abort)
        fault("state-load");
    if (i->mode == Mode::restore_hang)
        hang();
    float restored = 0.0f;
    if (stream->read(stream, &restored, sizeof(restored)) != sizeof(restored) ||
        !std::isfinite(restored))
        return false;
    i->gain = restored;
    return true;
}
uint32_t port_count(const clap_plugin_t*, bool) {
    return 1;
}
bool port_get(const clap_plugin_t* p, uint32_t index, bool input, clap_audio_port_info_t* info) {
    if (index)
        return false;
    std::memset(info, 0, sizeof(*info));
    info->id = input ? 0 : 1;
    std::strncpy(info->name, input ? "Input" : "Output", sizeof(info->name) - 1);
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = static_cast<const Instance*>(p->plugin_data)->mode == Mode::mono ? 1 : 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t ports = {port_count, port_get};
const clap_plugin_state_t state = {state_save, state_load};
uint32_t latency_get(const clap_plugin_t* p) {
    return static_cast<const Instance*>(p->plugin_data)->mode == Mode::latency ? 7 : 0;
}
uint32_t tail_get(const clap_plugin_t* p) {
    return static_cast<const Instance*>(p->plugin_data)->mode == Mode::tail ? 7 : 0;
}
const clap_plugin_latency_t latency{latency_get};
const clap_plugin_tail_t tail{tail_get};
const void* extension(const clap_plugin_t*, const char* id) {
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)
        return &latency;
    if (std::strcmp(id, CLAP_EXT_TAIL) == 0)
        return &tail;
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
        return &ports;
    if (std::strcmp(id, CLAP_EXT_STATE) == 0)
        return &state;
    return nullptr;
}
void on_main(const clap_plugin_t*) {}
uint32_t count(const clap_plugin_factory_t*) {
    return static_cast<uint32_t>(sizeof(descriptors) / sizeof(descriptors[0]));
}
const clap_plugin_descriptor_t* descriptor(const clap_plugin_factory_t*, uint32_t n) {
    return n < count(nullptr) ? &descriptors[n] : nullptr;
}
const clap_plugin_t* create(const clap_plugin_factory_t*, const clap_host_t*, const char* id) {
    int n = -1;
    for (uint32_t k = 0; k < count(nullptr); ++k)
        if (std::strcmp(id, descriptors[k].id) == 0)
            n = static_cast<int>(k);
    if (n < 0)
        return nullptr;
    auto* i = new Instance{};
    i->mode = static_cast<Mode>(n);
    i->plugin = {&descriptors[n], i,    init,  destroy, activate,  deactivate,
                 start,           stop, reset, process, extension, on_main};
    return &i->plugin;
}
const clap_plugin_factory_t factory = {count, descriptor, create};
bool entry_init(const char* path) {
    if (std::getenv("PULP_G9_REQUIRE_BUNDLE"))
        return std::filesystem::is_regular_file(std::filesystem::path(path) / "Contents" /
                                                "Resources" / "required.txt");
    return true;
}
void entry_deinit() {}
const void* get_factory(const char* id) {
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr;
}
} // namespace
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init,
                                                               entry_deinit, get_factory};
