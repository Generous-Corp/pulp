#pragma once

// Forge Modular as a fourth Forge shell.
//
// The three existing shells — FX, Instrument, MIDI — are DSP: they make sound and
// the chrome shows their controls. This one makes *files*: a Eurorack panel plus
// its C++, or a whole patch. So most of ForgeShell's pure virtuals are about
// something this product does not have, and they are answered honestly rather
// than faked. Every deliberately-empty override says why it is empty, because an
// unexplained empty override reads as an oversight.
//
// It is the first real ForgeShell subclass outside Forge itself, which is what
// makes the seam's live-path tests permanent instead of manual demonstrations.

#include "forge/brand.hpp"
#include "forge/shell.hpp"

#include "forge/build_monitor.hpp"
#include "forge/mention_overlay.hpp"
#include "forge/patch_explanation.hpp"
#include "forge/rack_preview.hpp"

#include <pulp/view/buttons.hpp>
#include <pulp/view/widgets.hpp>

#include <memory>
#include <string>
#include <vector>

namespace forge_modular {

/// What Forge Modular is currently set to build.
enum class Artifact { module, patch };

/// Where generation actually runs.
///
/// The shell never compiles anything itself: it hands a prompt to a helper and
/// renders what comes back. That keeps a compiler out of a DAW's plugin sandbox
/// and stops the generation engine existing twice.
class EngineClient {
public:
    virtual ~EngineClient() = default;
    /// False means not installed, which is a real failure worth reporting —
    /// unlike merely not-yet-running.
    virtual bool available() const = 0;
    virtual bool ensure_running() = 0;
    virtual void submit(const std::string& prompt, bool patch_mode) = 0;
    /// Why the last submit did nothing; empty when it started. A Build that
    /// silently does nothing is the worst available outcome, and it shipped once.
    virtual std::string last_error() const { return {}; }

    /// Answer a question about a patch from the patch itself, or empty when
    /// this engine cannot. Kept on the engine because it is the side that
    /// knows where patches live.
    virtual std::string explain(const std::string& patch_path) const {
        (void)patch_path;
        return {};
    }
};

/// Build a Forge Modular processor with its generator already connected.
///
/// Shared by every format. The standalone once wired the engine itself, which
/// meant a plugin build would have had none -- Build would reach a null engine
/// and do nothing, exactly the defect that shipped in the app.
std::unique_ptr<pulp::format::Processor> create_forge_modular();

class ForgeModularShell final : public forge::ForgeShell {
public:
    ForgeModularShell();

    void set_engine(EngineClient* engine) { engine_ = engine; }

    /// Module or patch. The chrome reads this through the copy and the row, so
    /// there is never a second copy of the mode to disagree with.
    Artifact artifact() const { return artifact_; }
    void set_artifact(Artifact a);

    // ── what the chrome asks ─────────────────────────────────────────────────

    forge::ChromeCopy chrome_copy() const override;
    forge::ComposerRow composer_row() override;
    std::unique_ptr<pulp::view::View> home_accessory() override;
    std::unique_ptr<pulp::view::View> overlay_accessory() override;

    /// The mention list, so the composer's keystrokes can reach it and a test
    /// can drive it without a window.
    MentionOverlay& mentions() { return mentions_; }

    /// Start following a generator log and pushing what it says into the chat.
    ///
    /// Forge's Build screen already has the transcript; this only supplies the
    /// lines. Nothing here rebuilds a chat that already exists.
    /// Start a build from the composer's current prompt.
    ///
    /// Returns the reason it did not start, or empty when it did. A Build that
    /// silently does nothing is the worst available outcome, and it shipped
    /// once -- so the refusal is a value the caller must handle, not a log line.
    std::string start_build();

    /// Ask about the artifact without changing it. Distinct from Build because
    /// an Ask that could rewrite the artifact would destroy work on a misread
    /// intent.
    std::string ask();

    /// The patch the workspace currently holds, if any. Ask answers about it.
    void set_open_patch(std::string path) { open_patch_ = std::move(path); }

    /// Load a generated patch onto the Build stage: rack, explanation, tabs.
    ///
    /// Returns why it could not, or empty. An unreadable file must not leave
    /// an empty rack on screen -- that reads as a build that produced nothing.
    std::string open_patch_file(const std::string& path);
    const std::string& open_patch() const { return open_patch_; }

    /// Open the mention list, as typing `@` does.
    void begin_mention();

    /// Which of Forge's model roles an artifact actually consumes.
    ///
    /// Inherited, not duplicated: the selections live in Forge's settings and
    /// this only says which of them a Rack artifact uses. A module compiles
    /// DSP and draws a panel, so it uses both. A patch compiles nothing -- it
    /// wires modules that already exist and writes the explanation -- so the
    /// DSP role is irrelevant to it, and offering that choice would imply the
    /// build does something it does not.
    struct ModelRoles {
        bool dsp = false;
        bool ui = false;
    };
    static ModelRoles roles_for(Artifact artifact);
    ModelRoles model_roles() const { return roles_for(artifact_); }

    void watch_build_log(const std::string& path);

    /// Drain whatever the generator has written since the last call, appending
    /// it to the chat. Called from a UI tick.
    ///
    /// Returns how many lines were added, so a test can assert the pump without
    /// reading the view tree.
    int pump_build_log();

    /// How much a patch explains itself as it is built.
    ///
    /// A patch is a lesson as much as an artifact; a module is not, which is
    /// why the control only appears for patches.
    enum class Depth { terse, standard, learning };

    Depth depth() const { return depth_; }
    void set_depth(Depth d);

    /// Whether a given note survives the current depth. `why` text is the
    /// reasoning behind a connection; Terse drops it, Learning adds the
    /// long-form asides on top of it.
    bool shows_reasoning() const { return depth_ != Depth::terse; }
    bool shows_asides() const { return depth_ == Depth::learning; }

    std::unique_ptr<pulp::view::View> build_accessory() override;
    std::unique_ptr<pulp::view::View> stage_accessory() override;

    /// Put a wired patch on the Build stage, replacing the skeleton.
    void show_rack(std::vector<RackModule> modules,
                   std::vector<Connection> connections);

    RackPreview* rack_preview() { return rack_preview_; }
    PatchExplanation* explanation() { return explanation_; }

    BuildOutcome build_outcome() const { return monitor_.outcome(); }
    const BuildMonitor& monitor() const { return monitor_; }

    /// What a host sees.
    ///
    /// An audio effect passing signal through untouched. Rack Pro wants the
    /// instrument slot, and taking one too would spend a whole track on a chat
    /// window; an insert puts both on the same track. The full reasoning, and
    /// what would change it, is in DECISIONS.md.
    pulp::format::PluginDescriptor descriptor() const override;

    /// None. What this produces is a file on disk, not a sound, so there is
    /// nothing a host should automate -- and a parameter that did nothing would
    /// only invite somebody to draw an envelope on it.
    void define_parameters(pulp::state::StateStore&) override {}

    void prepare(const pulp::format::PrepareContext& ctx) override;

    // ── audio ────────────────────────────────────────────────────────────────

    void process_audio(pulp::audio::BufferView<float>& audio_output,
                       const pulp::audio::BufferView<const float>& audio_input,
                       pulp::midi::MidiBuffer& midi_in,
                       pulp::midi::MidiBuffer& midi_out,
                       const pulp::format::ProcessContext& context) override;

    double current_sample_rate() const override { return sample_rate_; }
    int current_block_size() const override { return block_size_; }

    // ── the build ────────────────────────────────────────────────────────────

    bool has_build() const override { return has_build_; }

    /// None. A Rack artifact's controls live on its own panel, inside Rack, and
    /// a macro here would automate nothing a host could reach.
    std::vector<forge::MacroDesc> macro_descriptors() const override { return {}; }

    bool install_generated_bundle(
        const forge::gen::Bundle& bundle, double sample_rate, int block_size,
        const std::function<void(forge::gen::GenStage, const std::string&)>& progress,
        BundleInstallResult& info, std::string& err,
        std::span<const forge::gen::EffectAssetBinding> effect_assets = {},
        const std::function<bool()>& cancel_requested = {},
        const std::function<bool()>& try_begin_commit = {}) override;

    void reset_to_default_build() override { has_build_ = false; }

    /// Nothing to install. The other products open on a working default so the
    /// user hears something immediately; a Eurorack module has nowhere to sound
    /// until Rack loads it, so opening empty is the honest state.
    void ensure_default_build() override {}

    // ── macros: none, so these are empty on purpose ──────────────────────────

    void restore_macro(int, float) override {}
    void apply_macro_from_store(int, float) override {}
    std::vector<std::pair<int, float>> current_macro_positions() const override {
        return {};
    }

private:
    void style_tabs();
    void offer_random();

    MentionOverlay mentions_;
    BuildMonitor monitor_;
    Depth depth_ = Depth::standard;
    RackPreview* rack_preview_ = nullptr;
    std::string open_patch_;
    PatchExplanation* explanation_ = nullptr;
    std::vector<pulp::view::TextButton*> depth_tabs_;
    std::vector<pulp::view::Label*> depth_labels_;
    pulp::view::View* depth_group_ = nullptr;
    std::vector<pulp::view::Label*> tab_labels_;
    void refresh_depth_tabs();
    std::string last_random_;
    std::size_t next_random_ = 0;
    pulp::view::TextButton* tab_module_ = nullptr;
    pulp::view::TextButton* tab_patch_ = nullptr;
    EngineClient* engine_ = nullptr;
    Artifact artifact_ = Artifact::module;
    bool has_build_ = false;
    double sample_rate_ = 48000.0;
    int block_size_ = 512;
};

}  // namespace forge_modular
