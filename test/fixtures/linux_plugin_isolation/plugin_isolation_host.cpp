// Two Pulp plug-ins in one Linux host process must stay two plug-ins.
//
//   pulp-test-plugin-isolation-host <clap|vst3|lv2> <A> <B> <allowed-export>...
//
// For each image alone, then for both in one process under RTLD_LOCAL and
// RTLD_GLOBAL and in both load orders (each run in its own child process,
// because an image carrying STB_GNU_UNIQUE symbols can never be unloaded), the
// host loads the images the way that format's hosts do and lists what each
// image's own factory reports: CLAP plug-in ids, VST3 class names, LV2 URIs.
// A listing that changes when the other plug-in is present means the two
// images share state. The two images' own listings must also differ, or the
// comparison could not see sharing at all.
//
// It then reads each image's dynamic symbol table and fails on any defined
// export outside the allowed list: an exported internal symbol is what one
// plug-in binds to in the other.

#include <dlfcn.h>
#include <elf.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace {

using Listing = std::vector<std::string>;

// ── CLAP: the C ABI, declared here so the host carries no SDK ───────────────
struct ClapVersion {
    std::uint32_t major, minor, revision;
};
struct ClapDescriptorHead {
    ClapVersion version;
    const char* id;
    const char* name;
};
struct ClapFactoryHead {
    std::uint32_t (*get_plugin_count)(const void* factory);
    const ClapDescriptorHead* (*get_plugin_descriptor)(const void* factory, std::uint32_t index);
};
struct ClapEntry {
    ClapVersion version;
    bool (*init)(const char* path);
    void (*deinit)();
    const void* (*get_factory)(const char* id);
};

// ── VST3: IPluginFactory's vtable order and PClassInfo layout ───────────────
struct Vst3ClassInfo {
    char cid[16];
    std::int32_t cardinality;
    char category[32];
    char name[64];
};
struct Vst3Factory {
    struct VTable {
        void* query_interface;
        void* add_ref;
        void* release;
        void* get_factory_info;
        std::int32_t (*count_classes)(Vst3Factory*);
        std::int32_t (*get_class_info)(Vst3Factory*, std::int32_t, Vst3ClassInfo*);
    }* vtable;
};

// ── LV2 ─────────────────────────────────────────────────────────────────────
struct Lv2DescriptorHead {
    const char* uri;
};

bool load_and_list(const std::string& format, const char* path, int mode, void** handle,
                   Listing& out) {
    *handle = dlopen(path, RTLD_NOW | mode);
    if (!*handle) {
        std::fprintf(stderr, "dlopen %s: %s\n", path, dlerror());
        return false;
    }
    void* h = *handle;
    if (format == "clap") {
        auto* entry = static_cast<const ClapEntry*>(dlsym(h, "clap_entry"));
        if (!entry || !entry->init(path))
            return false;
        auto* factory =
            static_cast<const ClapFactoryHead*>(entry->get_factory("clap.plugin-factory"));
        if (!factory)
            return false;
        for (std::uint32_t i = 0, n = factory->get_plugin_count(factory); i < n; ++i)
            out.emplace_back(factory->get_plugin_descriptor(factory, i)->id);
    } else if (format == "vst3") {
        auto module_entry = reinterpret_cast<bool (*)(void*)>(dlsym(h, "ModuleEntry"));
        auto get_factory = reinterpret_cast<Vst3Factory* (*)()>(dlsym(h, "GetPluginFactory"));
        if (!module_entry || !get_factory || !module_entry(h))
            return false;
        Vst3Factory* factory = get_factory();
        if (!factory)
            return false;
        for (std::int32_t i = 0, n = factory->vtable->count_classes(factory); i < n; ++i) {
            Vst3ClassInfo info{};
            factory->vtable->get_class_info(factory, i, &info);
            out.emplace_back(std::string(info.category) + ":" + info.name);
        }
    } else if (format == "lv2") {
        auto descriptor = reinterpret_cast<const Lv2DescriptorHead* (*)(std::uint32_t)>(
            dlsym(h, "lv2_descriptor"));
        if (!descriptor)
            return false;
        for (std::uint32_t i = 0; const Lv2DescriptorHead* d = descriptor(i); ++i)
            out.emplace_back(d->uri ? d->uri : "(null)");
    } else {
        return false;
    }
    return true;
}

std::string joined(const Listing& listing) {
    std::string s;
    for (const auto& item : listing)
        s += (s.empty() ? "" : " ") + item;
    return s.empty() ? "(none)" : s;
}

// Runs one load scenario in a child process; the child writes one listing
// line per image to the pipe.
bool scenario(const std::string& format, const std::vector<const char*>& paths, int mode,
              std::vector<std::string>& lines) {
    int fds[2];
    if (pipe(fds) != 0)
        return false;
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        std::vector<Listing> listings(paths.size());
        std::vector<void*> handles(paths.size());
        for (std::size_t i = 0; i < paths.size(); ++i)
            if (!load_and_list(format, paths[i], mode, &handles[i], listings[i]))
                _exit(3);
        std::string text;
        for (const auto& l : listings)
            text += joined(l) + "\n";
        (void)!write(fds[1], text.data(), text.size());
        _exit(0);
    }
    close(fds[1]);
    std::string text;
    char buf[4096];
    for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;)
        text.append(buf, static_cast<std::size_t>(n));
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return false;
    lines.clear();
    for (std::size_t at = 0, nl; (nl = text.find('\n', at)) != std::string::npos; at = nl + 1)
        lines.push_back(text.substr(at, nl - at));
    return lines.size() == paths.size();
}

// Defined GLOBAL/WEAK default- or protected-visibility names in .dynsym.
bool dynamic_exports(const char* path, std::set<std::string>& out) {
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
        if (sec.sh_type != SHT_DYNSYM)
            continue;
        const char* strtab =
            reinterpret_cast<const char*>(data.data() + sections[sec.sh_link].sh_offset);
        for (std::size_t i = 0; i < sec.sh_size / sizeof(Elf64_Sym); ++i) {
            Elf64_Sym sym;
            std::memcpy(&sym, data.data() + sec.sh_offset + i * sizeof(Elf64_Sym), sizeof sym);
            const int bind = ELF64_ST_BIND(sym.st_info);
            const int vis = ELF64_ST_VISIBILITY(sym.st_other);
            if (sym.st_shndx != SHN_UNDEF &&
                (bind == STB_GLOBAL || bind == STB_WEAK || bind == STB_GNU_UNIQUE) &&
                (vis == STV_DEFAULT || vis == STV_PROTECTED))
                out.insert(strtab + sym.st_name);
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <clap|vst3|lv2> <A> <B> <allowed-export>...\n", argv[0]);
        return 2;
    }
    const std::string format = argv[1];
    const char* a = argv[2];
    const char* b = argv[3];
    const std::set<std::string> allowed(argv + 4, argv + argc);
    int failures = 0;

    std::vector<std::string> alone_a, alone_b, lines;
    if (!scenario(format, {a}, RTLD_LOCAL, alone_a) ||
        !scenario(format, {b}, RTLD_LOCAL, alone_b)) {
        std::fprintf(stderr, "FAIL could not load and list each image on its own\n");
        return 1;
    }
    std::printf("alone: %s -> %s\nalone: %s -> %s\n", a, alone_a[0].c_str(), b, alone_b[0].c_str());
    if (alone_a[0] == alone_b[0] || alone_a[0] == "(none)") {
        std::fprintf(
            stderr,
            "FAIL the two images list the same thing on their own; the test cannot see sharing\n");
        return 1;
    }

    for (int mode : {RTLD_LOCAL, RTLD_GLOBAL}) {
        const char* mode_name = mode == RTLD_LOCAL ? "RTLD_LOCAL" : "RTLD_GLOBAL";
        for (bool a_first : {true, false}) {
            const char* first = a_first ? a : b;
            const char* second = a_first ? b : a;
            if (!scenario(format, {first, second}, mode, lines)) {
                std::printf("FAIL %s, %s first: loading both failed\n", mode_name, first);
                ++failures;
                continue;
            }
            const std::string& want_first = a_first ? alone_a[0] : alone_b[0];
            const std::string& want_second = a_first ? alone_b[0] : alone_a[0];
            const bool ok = lines[0] == want_first && lines[1] == want_second;
            std::printf("%s %s, %s first: %s | %s\n", ok ? "ok  " : "FAIL", mode_name, first,
                        lines[0].c_str(), lines[1].c_str());
            failures += ok ? 0 : 1;
        }
    }

    for (const char* image : {a, b}) {
        std::set<std::string> exports;
        if (!dynamic_exports(image, exports)) {
            std::printf("FAIL %s: unreadable ELF64 image\n", image);
            ++failures;
            continue;
        }
        std::vector<std::string> extra;
        for (const auto& name : exports)
            if (!allowed.count(name))
                extra.push_back(name);
        for (const auto& name : allowed)
            if (!exports.count(name)) {
                std::printf("FAIL %s: entry point %s is not exported\n", image, name.c_str());
                ++failures;
            }
        if (extra.empty()) {
            std::printf("ok   %s exports only its entry points (%zu)\n", image, exports.size());
        } else {
            std::printf("FAIL %s exports %zu symbols besides its entry points, e.g. %s\n", image,
                        extra.size(), extra.front().c_str());
            ++failures;
        }
    }
    return failures ? 1 : 0;
}
