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
#include "forge/installation.hpp"
#include "forge/mention_overlay.hpp"
#include "forge/patch_explanation.hpp"
#include "forge/rack_preview.hpp"

#include <pulp/view/buttons.hpp>
#include <pulp/view/widgets.hpp>

#include <chrono>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace forge_modular {

class ModuleSummary;

/// Whether an @-mentioned module can be fetched, and what to say if not.
struct MentionFetch {
    bool fetch = false;   ///< spawn the download
    std::string why;      ///< appended to the module name, shown to the user
};

/// Decide before promising.
///
/// A background fetch that cannot possibly succeed still prints "fetching…",
/// and its failure lands in a log nobody opens. Every reason below is knowable
/// instantly from local files, so none of them needs a network round trip or
/// an asynchronous report -- refusing up front is both cheaper and honest.
MentionFetch plan_mention_fetch(MentionCandidate::Availability state,
                                bool signed_in, const std::string& pref);

/// Whether Rack holds a non-empty library token. Presence only: the token is
/// the user's credential and nothing here reads or copies its value.
bool rack_signed_in();

/// Any Forge Modular preference, read from the product's settings file with
/// the caller's fallback standing in for patch.py's default, so the two sides
/// cannot disagree about what is going to happen. String values only, which
/// is what both user-visible preferences are.
std::string modular_setting(const std::string& key, const std::string& fallback);

/// The `auto_download` preference, defaulting exactly as patch.py does.
std::string auto_download_pref();

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
    virtual void explain_async(const std::string& patch_path,
                               std::function<void(std::string)> done) const {
        if (done) done(explain(patch_path));
    }

    /// The log the run just submitted is writing, or empty when this engine
    /// does not keep one per run. Every run sharing a single log let two
    /// overlapping generations overwrite each other's output, and the shell
    /// reads the outcome, the stage and the artifact path out of that file.
    virtual std::string log_path() const { return {}; }

    /// True when a generator is running right now, whoever launched it —
    /// including one left over from a previous launch of the app, which
    /// survives by design (nohup + setsid) and which the shell's own `busy()`
    /// therefore cannot see.
    virtual bool generator_running() const { return false; }
};

/// Build a Forge Modular processor with its generator already connected.
///
/// Shared by every format. The standalone once wired the engine itself, which
/// meant a plugin build would have had none -- Build would reach a null engine
/// and do nothing, exactly the defect that shipped in the app.
std::unique_ptr<pulp::format::Processor> create_forge_modular();

/// Where a Rack artifact can be opened on this machine.
///
/// Public because it is now SHOWN. The app knew all three of these and told
/// nobody: the state only reached a person as the wording of an error after
/// they had already pressed a button, so "why is nothing happening" and "Rack
/// is not installed" looked identical until then.
struct RackPresence {
    bool standalone_installed = false;
    bool standalone_running = false;
    bool plugin_installed = false;     ///< Rack Pro as AU/VST3/CLAP
    std::string standalone_app;        ///< Exact detected .app, if installed

    /// The one phrase that names this state, for the pill beside the button.
    std::string phrase() const;
};

/// What this machine has, probed now.
RackPresence look_for_rack();

/// Exact detected Rack app, or an application name that can address a running
/// copy when the installation lives outside the standard Applications folder.
std::string rack_app_to_launch(const RackPresence& presence);

/// The shell command that opens `patch` in Rack.
///
/// Rack takes a patch path as a positional argument and loads THAT instead of
/// restoring its autosave -- which is the whole point here. A patch handed
/// over as a document (`open -a Rack file`) left Rack restoring the previous
/// session, so a stray TURBID with no cables kept appearing in front of a
/// patch that had loaded perfectly well and was sitting behind it.
///
/// `--args` only reaches an app being LAUNCHED, so a Rack already running is
/// handed the file the other way and told about it.
std::string rack_open_command(const std::string& app, const std::string& patch,
                              bool already_running);

/// Reveal a patch in Finder without allowing its filename to become shell.
std::string finder_reveal_command(const std::string& patch);

class ForgeModularShell final : public forge::ForgeShell {
public:
    ForgeModularShell();
    ~ForgeModularShell() override;

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

    /// The Terse/Standard/Learning buttons, and whether their container shows.
    /// Exposed so a test can ask whether a user can actually REACH the depth
    /// control -- asking set_depth() what it does cannot see a hidden button.
    const std::vector<pulp::view::TextButton*>& depth_tabs() const {
        return depth_tabs_;
    }

    /// The generation preferences a person can change without opening a JSON
    /// file: where modules come from (module_source) and whether missing ones
    /// may be fetched (auto_download). They live in the gear-menu settings
    /// sheet, on the Permissions tab, through ForgeShell::settings_choices()
    /// -- the sheet renders them in its own row idiom, so this shell hands
    /// over words, values and callbacks, never a view.
    std::vector<forge::ForgeShell::SettingsChoice> settings_choices() override;

    /// This product never meets a provider's approvals.
    ///
    /// A patch is one question with one text answer: the model is asked for
    /// JSON and the JSON is read here. Nothing it returns is executed and
    /// nothing it does asks the user to approve a tool, so the Permissions
    /// tab's card explaining that approval prompts cannot be bypassed would
    /// be describing a prompt nobody using this will ever see.
    bool has_provider_permissions() const override { return false; }

    /// What the controls currently show, for tests.
    const std::string& module_source_shown() const { return module_source_; }
    const std::string& auto_download_shown() const { return auto_download_; }
    int generation_minutes_shown() const { return generation_minutes_; }

    /// What this build is and what it is running: version, packaged date, the
    /// LIVE generator path and its stamp, the index, the Rack SDK and whether
    /// a VCV sign-in was found. Gathered here so the details row and a test
    /// read the same facts.
    AppDetails gather_details();

    /// The line under the library-index row: what is in the index, how old it
    /// is, and how the last refresh went. Shown when idle too, because "it
    /// says nothing" and "it did nothing" were indistinguishable.
    std::string library_status_line();

    /// Rebuild the index now, whatever its state. What the Refresh control does.
    void refresh_library_index();

    /// The last failed run as one copyable block, or empty if none has failed.
    ///
    /// Everything about a failure was unselectable text on a screen: one
    /// narrated line, a transcript in a Label, and a log at a path nothing
    /// named. A person who lost a five-attempt run could not even quote it
    /// back. This is what the Copy control puts on the clipboard, and it is
    /// public so a test can read it without a clipboard or a window.
    std::string failure_report() const;

    /// Whether the composer is currently offering to copy that report.
    bool copy_failure_offered() const { return failed_report_; }

    /// Which settings row the library-index status belongs to. The order in
    /// settings_choices() is the contract between the two, so it is named
    /// once rather than counted twice.
    static constexpr std::size_t kLibraryIndexRow = 3;

    /// Adopt a preference and hand the write to patch.py, the one validated
    /// writer of the settings file. A no-op when the value is already
    /// current, so re-clicking the chosen option does not spawn a process to
    /// write what is already written.
    void choose_module_source(const std::string& value);
    void choose_auto_download(const std::string& value);
    bool depth_group_visible() const {
        return depth_group_ != nullptr && depth_group_->visible();
    }

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

    /// The one path every build route funnels through.
    std::string start_build_with(const std::string& prompt);

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

    /// Open the finished artifact in VCV Rack.
    ///
    /// Returns why it could not, or empty. The generator already prints the
    /// exact command; leaving that in a log and offering no button meant the
    /// last step of every build was "go find the file yourself".
    std::string open_in_rack();

    /// How this shell is allowed to start another application.
    ///
    /// Nothing is launched unless something INSTALLS one of these. A bare
    /// shell -- which is what every test builds -- decides what it would run,
    /// records it, and runs nothing.
    ///
    /// It used to call `open` directly, so the test suite really launched VCV
    /// Rack: twice per run, taking over the screen of whoever was using the
    /// machine, and again for every agent looping the suite. A test may prove
    /// which command it chose; it may not open an application on somebody's
    /// desktop.
    using Launcher = std::function<void(const std::string& command)>;
    void set_launcher(Launcher l) { launcher_ = std::move(l); }

    /// Every command this shell would have run, in order. Exposed so a test
    /// can assert the DECISION -- which command, and when -- without anything
    /// being launched.
    const std::vector<std::string>& launched() const { return launched_; }

    /// True only for the standalone app. Hosted builds must not steal focus
    /// from the DAW session by launching another application.
    bool is_standalone() const;
    void set_standalone(bool yes) { standalone_ = yes; }

    /// The path the generator reported, or empty until a build succeeds.
    std::string artifact_path() const;

    /// What the presence pill currently says. Exposed so a test can read the
    /// words a person is actually shown rather than the flags behind them.
    const std::string& rack_presence_phrase() const { return rack_phrase_; }
    const std::string& unmapped_note() const { return unmapped_note_; }

    /// Re-probe for Rack and update the pill. Called from the poll, throttled;
    /// exposed so a test need not wait for a timer.
    void refresh_rack_presence();

    /// Put a finished patch in "My projects" so it can be found again.
    void save_project_for(const std::string& artifact);

    /// The same save the build path runs, reachable by a test. Exposed so the
    /// test drives the REAL code rather than a reimplementation of it.
    void save_project_for_test(const std::string& artifact) {
        save_project_for(artifact);
    }

    /// Open one of OUR projects: the entry carries a Rack patch beside it, not
    /// a Pulp signal graph, so the base's bake-and-install path cannot load it.
    /// Access is not widened -- the chrome calls this through ForgeShell, where
    /// it is public, and virtual dispatch does not consult the override's.
    bool open_project_entry(const std::string& id, std::string& err) override;
    /// Put the mention keys on the root the window actually dispatches to.
    void ensure_key_hook();

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
    std::unique_ptr<pulp::view::View> chat_accessory() override;

    void on_poll() override;

    /// The parts of on_poll that keep a surface honest: where the composer
    /// is, how a module download ended, and what the library-index row says.
    /// Public so a test can tick them without a window or a host clock.
    void poll_surfaces();

    /// Forget every pointer into the editor's view tree.
    ///
    /// This shell IS the processor: it outlives every editor its host opens.
    /// The pointers below are borrowed from a tree the chrome owns, so once the
    /// editor closes they name freed memory -- and a build finishing a beat
    /// later still walks them, because `on_poll` reaches `show_rack` through
    /// the outcome it has just read from the log. `set_connections` then
    /// move-assigns over the destroyed view's `connections_`, freeing a pointer
    /// that was never allocated, which is an immediate abort inside libmalloc.
    ///
    /// Cleared BEFORE the base class runs, because the chrome owns some of
    /// these views outright rather than merely containing them: the chat
    /// accessory is held in `retired_chat_`, so it dies inside `~ForgeChrome`
    /// -- during this call -- and not with the tree afterwards.
    /// The editor's root, with the mention list's key hook attached.
    ///
    /// `on_global_key` fires only when set on the ROOT view -- setting it on
    /// the overlay's own root (a child) did nothing, which is why Up and Down
    /// still went to the text field's caret instead of the open list.
    std::unique_ptr<pulp::view::View> create_view() override;
    /// Backspace over a finished mention removes the whole token.
    bool handle_prompt_key(const pulp::view::KeyEvent& event);
    void on_view_closed(pulp::view::View& view) override;

    bool busy() const override;
    bool owns_generation() const override { return true; }
    std::string submit_own(const std::string& prompt) override;

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
    /// The view the mention keys are installed on, so a tree that is
    /// re-parented later gets them on its NEW root rather than keeping
    /// them on a root the window no longer dispatches to.
    pulp::view::View* key_hook_root_ = nullptr;
    /// What the last shown rack lacks measurements for, and how to fix it.
    /// Exposed so a test can read it rather than scrape the chrome.
    std::string unmapped_note_;
    BuildMonitor monitor_;
    bool watching_ = false;
    std::string monitor_log_path_;
    BuildOutcome reported_outcome_ = BuildOutcome::running;
    int reported_stage_ = -2;
    std::chrono::steady_clock::time_point run_started_{};
    std::chrono::steady_clock::time_point stage_started_{};
    bool standalone_ = false;
    Depth depth_ = Depth::standard;
    RackPreview* rack_preview_ = nullptr;
    std::string open_patch_;
    /// The prompt this patch was built from, shown above the explanation.
    std::string last_request_;
    /// A build this shell started and has not yet seen end.
    ///
    /// Not `busy()`, which is `watching_ && outcome == running` — and a log
    /// that has never been written reads as `running`, so `busy()` is true for
    /// a shell that is merely watching and has started nothing. Using it as
    /// the lock refused the FIRST build of every session.
    bool in_flight_ = false;
    /// A run has failed and its report is worth offering. Cleared when the
    /// next build starts: a Copy control that hands over the PREVIOUS run's
    /// failure while a new one is in flight is worse than no control.
    bool failed_report_ = false;
    PatchExplanation* explanation_ = nullptr;
    ModuleSummary* module_summary_ = nullptr;
    void refresh_module_summary();
    void show_for_artifact();
    std::vector<pulp::view::TextButton*> depth_tabs_;
    std::vector<pulp::view::Label*> depth_labels_;
    pulp::view::View* depth_group_ = nullptr;
    /// The preference values currently in force, cached from the settings
    /// file when the sheet asks for choices, then kept by choose_*(): the
    /// file write is asynchronous, so reading it back immediately would
    /// answer from a stale file.
    std::string module_source_ = "prefer_existing";
    std::string auto_download_ = "entitled";
    /// The same default patch.py carries, so the two cannot disagree about
    /// how long a generation is allowed to take.
    int generation_minutes_ = 10;
    /// What the index held when Refresh was pressed, so the row can report a
    /// before and an after rather than a number with nothing to compare to.
    std::string refresh_before_;
    bool refreshing_ = false;
    /// The module a download is running for, so its outcome can be reported
    /// rather than leaving "Downloading…" as the last word on the subject.
    std::string install_pending_;
    /// The status line last pushed to the settings sheet, so the poll tick
    /// only touches the label when the words have actually changed.
    std::string pushed_status_;
    void write_pref(const std::string& key, const std::string& value);
    pulp::view::TextButton* open_button_ = nullptr;
    Launcher launcher_;
    std::vector<std::string> launched_;
    /// Run `command`, or merely record it when nobody installed a launcher.
    void run_detached(const std::string& command);

    pulp::view::Label* rack_pill_ = nullptr;
    std::string rack_phrase_;
    /// When the presence was last probed. `pgrep` on every poll would be a
    /// process spawn several times a second for a state that changes when
    /// somebody launches an application.
    std::chrono::steady_clock::time_point rack_probed_{};
    std::vector<pulp::view::Label*> tab_labels_;
    void refresh_depth_tabs();
    std::string last_random_;
    std::size_t next_random_ = 0;
    pulp::view::TextButton* tab_module_ = nullptr;
    pulp::view::TextButton* tab_patch_ = nullptr;
    EngineClient* engine_ = nullptr;
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);
    Artifact artifact_ = Artifact::module;
    bool has_build_ = false;
    double sample_rate_ = 48000.0;
    int block_size_ = 512;
};

}  // namespace forge_modular
