// A Linux reload host must export what its thin reload logic needs.
//
//   pulp-test-reload-cycle-host <host.clap> <logic-warm.so> <logic-harsh.so> <work-dir>
//
// A thin logic module (pulp_add_reload_logic RESOLVE_FROM_HOST) leaves its
// pulp:: references undefined and resolves them from the plug-in that loads it.
// A Linux plug-in exports only its entry points unless it is a reload host
// (pulp_reload_host / pulp_reload_host_ui), whose version script republishes
// the SDK. This host checks that from both sides:
//
// 1. Symbols: every undefined, non-weak dynamic symbol of each logic module must
//    be exported by the plug-in or defined by one of the logic's own DT_NEEDED
//    libraries. Any other symbol is named, so a missing version-script pattern
//    is visible directly.
// 2. A real reload cycle: the plug-in is loaded the way a Linux CLAP host loads
//    it (RTLD_LOCAL), with PULP_RELOAD_LOGIC_PATH pointing at the warm logic. It
//    renders a constant input, which the warm logic shapes with a sine tremolo.
//    The harsh logic then replaces the watched file, and the host keeps
//    rendering until the output takes the square chop's two levels. A load
//    failure leaves the shell in passthrough, which neither shape matches.

#include <clap/clap.h>

#include <dlfcn.h>
#include <elf.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

struct DynamicSymbols {
    std::set<std::string> defined;   // GLOBAL/WEAK/UNIQUE, default or protected visibility
    std::set<std::string> undefined; // GLOBAL (not WEAK) references
    std::vector<std::string> needed; // DT_NEEDED
};

bool read_dynamic(const char* path, DynamicSymbols& out) {
    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), {});
    if (data.size() < sizeof(Elf64_Ehdr) || std::memcmp(data.data(), ELFMAG, SELFMAG) != 0 ||
        data[EI_CLASS] != ELFCLASS64)
        return false;
    Elf64_Ehdr eh;
    std::memcpy(&eh, data.data(), sizeof eh);
    std::vector<Elf64_Shdr> sections(eh.e_shnum);
    std::memcpy(sections.data(), data.data() + eh.e_shoff, eh.e_shnum * sizeof(Elf64_Shdr));
    for (const auto& sec : sections) {
        const char* strtab =
            reinterpret_cast<const char*>(data.data() + sections[sec.sh_link].sh_offset);
        if (sec.sh_type == SHT_DYNSYM) {
            for (std::size_t i = 1; i < sec.sh_size / sizeof(Elf64_Sym); ++i) {
                Elf64_Sym sym;
                std::memcpy(&sym, data.data() + sec.sh_offset + i * sizeof(Elf64_Sym), sizeof sym);
                const int bind = ELF64_ST_BIND(sym.st_info);
                const int vis = ELF64_ST_VISIBILITY(sym.st_other);
                const std::string name = strtab + sym.st_name;
                if (name.empty())
                    continue;
                if (sym.st_shndx == SHN_UNDEF) {
                    if (bind == STB_GLOBAL)
                        out.undefined.insert(name);
                } else if ((bind == STB_GLOBAL || bind == STB_WEAK || bind == STB_GNU_UNIQUE) &&
                           (vis == STV_DEFAULT || vis == STV_PROTECTED)) {
                    out.defined.insert(name);
                }
            }
        } else if (sec.sh_type == SHT_DYNAMIC) {
            for (std::size_t i = 0; i < sec.sh_size / sizeof(Elf64_Dyn); ++i) {
                Elf64_Dyn dyn;
                std::memcpy(&dyn, data.data() + sec.sh_offset + i * sizeof(Elf64_Dyn), sizeof dyn);
                if (dyn.d_tag == DT_NULL)
                    break;
                if (dyn.d_tag == DT_NEEDED)
                    out.needed.emplace_back(strtab + dyn.d_un.d_val);
            }
        }
    }
    return true;
}

// Every undefined symbol of the logic resolves against the plug-in's exports
// or one of the logic's own dependencies.
int check_symbols(const char* plugin, const char* logic) {
    DynamicSymbols host, mod;
    if (!read_dynamic(plugin, host) || !read_dynamic(logic, mod)) {
        std::printf("FAIL could not read %s or %s as ELF64\n", plugin, logic);
        return 1;
    }
    std::vector<void*> deps;
    for (const auto& soname : mod.needed)
        if (void* h = dlopen(soname.c_str(), RTLD_NOW | RTLD_LOCAL))
            deps.push_back(h);
    std::vector<std::string> missing;
    std::size_t from_host = 0;
    for (const auto& name : mod.undefined) {
        if (host.defined.count(name)) {
            ++from_host;
            continue;
        }
        bool found = false;
        for (void* h : deps)
            if (dlsym(h, name.c_str())) {
                found = true;
                break;
            }
        if (!found)
            missing.push_back(name);
    }
    if (from_host == 0) {
        std::printf("FAIL %s resolves nothing from the plug-in; it is not a thin logic, so this "
                    "check proves "
                    "nothing\n",
                    logic);
        return 1;
    }
    if (!missing.empty()) {
        std::printf("FAIL %s: %zu undefined symbols are exported by neither the plug-in nor the "
                    "logic's own "
                    "libraries:\n",
                    logic, missing.size());
        for (std::size_t i = 0; i < missing.size() && i < 40; ++i)
            std::printf("  %s\n", missing[i].c_str());
        return 1;
    }
    std::printf(
        "ok   %s: %zu undefined symbols, %zu from the plug-in, the rest from its own libraries\n",
        logic, mod.undefined.size(), from_host);
    return 0;
}

const void* host_get_extension(const clap_host_t*, const char*) {
    return nullptr;
}
void host_request(const clap_host_t*) {}

enum class Shape { warm, harsh, other };

// Constant input 1.0 → output is the tremolo gain. Warm: a sine between 1-depth
// and 1. Harsh: exactly 1 or 1-depth. Passthrough: exactly 1.
Shape classify(const std::vector<float>& y) {
    const float lo = *std::min_element(y.begin(), y.end());
    std::size_t on_levels = 0;
    for (float v : y)
        if (std::fabs(v - 1.0f) < 1e-5f || std::fabs(v - lo) < 1e-5f)
            ++on_levels;
    const double level_share = double(on_levels) / double(y.size());
    if (lo > 0.95f)
        return Shape::other; // no modulation: passthrough
    if (level_share > 0.98)
        return Shape::harsh;
    if (level_share < 0.5)
        return Shape::warm;
    return Shape::other; // mid-crossfade
}

const char* name(Shape s) {
    return s == Shape::warm ? "warm" : s == Shape::harsh ? "harsh" : "other";
}

int reload_cycle(const char* plugin_path, const char* warm, const char* harsh,
                 const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string watched = dir + "/logic.so";
    fs::copy_file(warm, watched, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::printf("FAIL copy %s: %s\n", warm, ec.message().c_str());
        return 1;
    }
    setenv("PULP_RELOAD_LOGIC_PATH", watched.c_str(), 1);

    void* handle = dlopen(plugin_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::printf("FAIL dlopen %s: %s\n", plugin_path, dlerror());
        return 1;
    }
    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(handle, "clap_entry"));
    if (!entry || !entry->init(plugin_path)) {
        std::printf("FAIL clap_entry\n");
        return 1;
    }
    auto* factory =
        static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory || factory->get_plugin_count(factory) == 0) {
        std::printf("FAIL plugin factory\n");
        return 1;
    }
    clap_host_t host{};
    host.clap_version = CLAP_VERSION;
    host.name = "Pulp reload cycle probe";
    host.vendor = "Pulp";
    host.url = "https://pulp.audio";
    host.version = "1.0.0";
    host.get_extension = host_get_extension;
    host.request_restart = host_request;
    host.request_process = host_request;
    host.request_callback = host_request;
    const clap_plugin_t* plugin =
        factory->create_plugin(factory, &host, factory->get_plugin_descriptor(factory, 0)->id);
    constexpr std::uint32_t kFrames = 256;
    constexpr double kRate = 48000.0;
    if (!plugin || !plugin->init(plugin) || !plugin->activate(plugin, kRate, 1, kFrames) ||
        !plugin->start_processing(plugin)) {
        std::printf("FAIL create / init / activate\n");
        return 1;
    }

    std::array<float, kFrames> in_l, in_r, out_l{}, out_r{};
    in_l.fill(1.0f);
    in_r.fill(1.0f);
    float* inputs[] = {in_l.data(), in_r.data()};
    float* outputs[] = {out_l.data(), out_r.data()};
    clap_audio_buffer_t input{};
    input.data32 = inputs;
    input.channel_count = 2;
    clap_audio_buffer_t output{};
    output.data32 = outputs;
    output.channel_count = 2;
    clap_process_t process{};
    process.frames_count = kFrames;
    process.audio_inputs = &input;
    process.audio_inputs_count = 1;
    process.audio_outputs = &output;
    process.audio_outputs_count = 1;

    // One window is a full LFO period at the default 6 Hz rate.
    const std::size_t window_blocks = std::size_t(kRate / 6.0 / kFrames) + 1;
    auto render_window = [&] {
        std::vector<float> y;
        for (std::size_t b = 0; b < window_blocks; ++b) {
            plugin->process(plugin, &process);
            y.insert(y.end(), out_l.begin(), out_l.end());
        }
        return y;
    };

    int failures = 0;
    const Shape first = classify(render_window());
    std::printf("%s initial logic renders as %s\n", first == Shape::warm ? "ok  " : "FAIL",
                name(first));
    failures += first == Shape::warm ? 0 : 1;

    // Replace the watched file with a fresh inode, as a build output step does.
    const std::string staged = dir + "/logic.so.next";
    fs::copy_file(harsh, staged, fs::copy_options::overwrite_existing, ec);
    fs::rename(staged, watched, ec);
    if (ec) {
        std::printf("FAIL replace the watched logic: %s\n", ec.message().c_str());
        return 1;
    }
    Shape now = Shape::other;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (now != Shape::harsh && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(50)); // the watcher polls every 150 ms
        now = classify(render_window());
    }
    std::printf("%s after the hot swap the plug-in renders as %s\n",
                now == Shape::harsh ? "ok  " : "FAIL", name(now));
    failures += now == Shape::harsh ? 0 : 1;

    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    entry->deinit();
    return failures;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: %s <host.clap> <logic-warm.so> <logic-harsh.so> <work-dir>\n",
                     argv[0]);
        return 2;
    }
    int failures = check_symbols(argv[1], argv[2]) + check_symbols(argv[1], argv[3]);
    failures += reload_cycle(argv[1], argv[2], argv[3], argv[4]);
    return failures ? 1 : 0;
}
