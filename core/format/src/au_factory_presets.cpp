#include <pulp/format/au_factory_presets.hpp>

#include <pulp/runtime/log.hpp>

#include <dlfcn.h>

#include <string>
#include <utility>

namespace fs = std::filesystem;

namespace pulp::format::au {

namespace {

constexpr const char* kPresetsFolderName = "Presets";

bool is_flat_bundle_dir(const fs::path& dir) {
    const auto ext = dir.extension().string();
    // The flat shapes a Pulp binary sits directly inside. `.framework` is
    // absent deliberately — a framework keeps resources under `Resources/`,
    // and no Pulp plug-in is loaded as one.
    return ext == ".appex" || ext == ".app" || ext == ".bundle";
}

} // namespace

fs::path factory_presets_dir_for_binary(const fs::path& binary) {
    if (binary.empty())
        return {};

    const fs::path dir = binary.parent_path();
    if (dir.empty())
        return {};

    // macOS bundle: <Foo.component>/Contents/MacOS/<exe>
    if (dir.filename() == "MacOS") {
        const fs::path contents = dir.parent_path();
        if (contents.filename() == "Contents")
            return contents / "Resources" / kPresetsFolderName;
        return {};
    }

    // Flat bundle (iOS app extension, and the macOS `.bundle` layout).
    if (is_flat_bundle_dir(dir))
        return dir / kPresetsFolderName;

    return {};
}

fs::path loaded_bundle_factory_presets_dir() {
    Dl_info info{};
    // Any symbol defined in this translation unit resolves to the image the
    // adapter is linked into — the plug-in binary, not the host application.
    if (dladdr(reinterpret_cast<const void*>(&factory_presets_dir_for_binary), &info) == 0)
        return {};
    if (info.dli_fname == nullptr)
        return {};
    return factory_presets_dir_for_binary(fs::path(info.dli_fname));
}

// ── FactoryPresetTable ────────────────────────────────────────────────────

FactoryPresetTable::~FactoryPresetTable() {
    clear_entries();
}

void FactoryPresetTable::clear_entries() noexcept {
    for (auto& entry : entries_) {
        if (entry.presetName != nullptr)
            CFRelease(entry.presetName);
        entry.presetName = nullptr;
    }
    entries_.clear();
    infos_.clear();
}

void FactoryPresetTable::bind(state::StateStore& store, const std::string& manufacturer,
                              const std::string& plugin_name) {
    presets_ = std::make_unique<state::PresetManager>(store, manufacturer, plugin_name);
    presets_->set_factory_presets_dir(loaded_bundle_factory_presets_dir());
    rebuild();
}

void FactoryPresetTable::set_directory(const fs::path& dir) {
    if (!presets_)
        return;
    presets_->set_factory_presets_dir(dir);
    rebuild();
}

void FactoryPresetTable::set_programs(std::vector<std::string> names, ProgramSelect on_select) {
    programs_ = std::move(names);
    on_select_ = std::move(on_select);
    rebuild();
}

void FactoryPresetTable::rebuild() {
    clear_entries();

    auto append = [this](const std::string& name) {
        CFStringRef cf =
            CFStringCreateWithCString(kCFAllocatorDefault, name.c_str(), kCFStringEncodingUTF8);
        if (cf == nullptr)
            return false;
        AUPreset preset{};
        preset.presetNumber = static_cast<SInt32>(entries_.size());
        preset.presetName = cf;
        entries_.push_back(preset);
        return true;
    };

    // Declared programs come first so their numbers do not move when preset
    // files are added or removed. A name that cannot be encoded keeps its slot
    // under a fallback name, so number n always selects the n-th declaration.
    for (std::size_t i = 0; i < programs_.size(); ++i) {
        if (!append(programs_[i])) {
            programs_[i] = "Program " + std::to_string(i + 1);
            append(programs_[i]);
        }
    }

    if (!presets_)
        return;

    const auto found = presets_->factory_presets();
    infos_.reserve(found.size());
    for (const auto& info : found) {
        if (!append(info.name))
            continue;
        infos_.push_back(info);
    }

    if (!infos_.empty()) {
        runtime::log_info("AU: {} factory preset(s) from '{}'", infos_.size(),
                          presets_->factory_presets_dir().string());
    }
}

std::string FactoryPresetTable::name_at(std::size_t index) const {
    if (index < programs_.size())
        return programs_[index];
    index -= programs_.size();
    if (index >= infos_.size())
        return {};
    return infos_[index].name;
}

const AUPreset* FactoryPresetTable::preset_at(std::size_t index) const noexcept {
    if (index >= entries_.size())
        return nullptr;
    return &entries_[index];
}

bool FactoryPresetTable::load(std::size_t index) {
    if (index < programs_.size()) {
        // Run a copy: the callback may redeclare programs, which would destroy
        // the function object it is executing from.
        if (const ProgramSelect select = on_select_)
            select(index);
        return true;
    }
    index -= programs_.size();
    if (!presets_ || index >= infos_.size())
        return false;
    if (!presets_->load(infos_[index]))
        return false;
    presets_->set_current_preset_name(infos_[index].name);
    return true;
}

OSStatus FactoryPresetTable::copy_presets(CFArrayRef* out_data) const {
    // A plug-in with no bundled presets must fail the property outright.
    // Answering with an empty array instead makes a host render an empty menu.
    if (entries_.empty())
        return kAudioUnitErr_InvalidProperty;
    // The host probes with a null destination to learn whether the property
    // exists at all before asking for its value.
    if (out_data == nullptr)
        return noErr;

    // Null callbacks: the values are `AUPreset*` into this table's storage, not
    // CF objects, which is the layout AU v2 hosts read back.
    CFMutableArrayRef array =
        CFArrayCreateMutable(kCFAllocatorDefault, static_cast<CFIndex>(entries_.size()), nullptr);
    if (array == nullptr)
        return kAudioUnitErr_InvalidProperty;
    for (const auto& entry : entries_)
        CFArrayAppendValue(array, &entry);

    *out_data = array;
    return noErr;
}

} // namespace pulp::format::au
