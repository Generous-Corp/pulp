# juce-dev Plugin Audit

**Subject:** Comprehensive audit of the juce-dev Claude Code plugin
**Source:** `/Users/danielraffel/Code/generous-corp-marketplace/plugins/juce-dev`
**Date:** 2026-03-24

---

## Table of Contents

1. [Plugin Architecture](#plugin-architecture)
2. [Complete Command Inventory](#complete-command-inventory)
3. [Skills Inventory](#skills-inventory)
4. [Template System](#template-system)
5. [Audited Framework Coupling Points](#audited-framework-coupling-points)
6. [Ergonomics Analysis](#ergonomics-analysis)
7. [What Must Be Generalized for Pulp](#what-must-be-generalized-for-pulp)

---

## Plugin Architecture

The juce-dev plugin is a Claude Code marketplace plugin that provides an AI-powered development workflow for audio plugin creation. It is notable for being **entirely composed of Markdown instruction files** — there is no executable code. All logic is expressed as natural language instructions that Claude interprets at runtime.

### Directory Structure

```
juce-dev/
├── README.md               # Plugin description and metadata
├── image.png               # Marketplace icon
├── index.html              # Marketplace landing page
├── commands/               # 11 command definition files
│   ├── build.md
│   ├── ci.md
│   ├── create.md
│   ├── port.md
│   ├── setup-ios.md
│   ├── setup-updates.md
│   ├── setup-visage.md
│   ├── status.md
│   ├── theme.md
│   ├── vm.md
│   └── website.md
└── skills/                 # 3 contextual knowledge bundles
    ├── juce-starter/
    │   ├── SKILL.md
    │   └── references/
    ├── juce-visage/
    │   ├── README.md
    │   ├── SKILL.md
    │   └── references/
    └── visage-theme/
        ├── SKILL.md
        └── references/
```

### How Commands Work

Each command is a `.md` file in the `commands/` directory with YAML frontmatter defining:

| Field | Purpose | Example |
|-------|---------|---------|
| `description` | Marketplace listing and help text | "Build, test, sign, or publish a JUCE plugin project" |
| `argument-hint` | Usage hint displayed in CLI autocomplete | "[target...] [action] [--help]" |
| `allowed-tools` | Claude Code tools the command can invoke | Read, Write, Edit, Bash, Glob, Grep, AskUserQuestion, Agent |

The body of each command file is structured natural language instructions that Claude follows step-by-step. This includes:
- Conditional logic expressed as "if X, then Y" prose
- Tables defining valid inputs and their effects
- Code blocks with exact shell commands to execute
- AskUserQuestion prompts with exact option text
- Error handling instructions

### How Skills Work

Skills are contextual knowledge bundles in the `skills/` directory. Each contains:
- `SKILL.md`: Frontmatter with `name`, `description` (containing trigger phrases), and `version`, followed by detailed reference documentation
- `references/`: Supporting files (code examples, troubleshooting guides, API references)

Skills activate **contextually** — when the user's message matches trigger phrases in the skill's description. They are not invoked by explicit commands. For example, the juce-visage skill activates when the user mentions "Metal rendering in JUCE", "GPU UI in audio plugins", or "bridge layers between JUCE and a GPU framework".

### Architectural Significance

This Markdown-based architecture is **framework-agnostic by nature**. The commands and skills are instruction sets, not code libraries. They can be adapted for any framework by changing the instructions while preserving the interaction patterns, staged workflows, and ergonomic conventions. This is directly relevant to Pulp's goal of creating a similar AI-assisted development experience.

---

## Complete Command Inventory

### 1. /create — Full Project Scaffolding

**Trigger:** `/juce-dev:create <plugin-name> [--visage] [--no-github]`
**Tools:** AskUserQuestion, Read, Write, Edit, Bash, Glob, Grep

**Purpose:** Creates a new audio plugin project from a starter template with full environment setup, feature selection, and GitHub integration.

**Stages:**

| Stage | Name | Description |
|-------|------|-------------|
| 0 | Environment Check | Verifies required dev tools per platform |
| 1 | Template Location | Finds or clones the starter template, checks framework version |
| 2 | Collect Settings | Multi-step interactive configuration |
| 3 | Execute Creation | File operations, placeholder replacement, feature setup |
| 4 | Summary | Reports results and next steps |

**Stage 0 — Environment Check (per platform):**

| Platform | Required Tools | Check Method |
|----------|---------------|--------------|
| macOS | Xcode CLT, Homebrew, CMake, gh (optional) | `xcode-select -p`, `command -v brew/cmake/gh` |
| Windows | Visual Studio/Build Tools, CMake, Ninja, Git, gh (optional) | `where`/`Get-Command` |
| Linux | CMake, Ninja, Clang, Git, pkg-config, gh (optional) | `command -v` |

Missing tools are offered for automatic installation (brew/winget/apt). GitHub CLI authentication is verified if GitHub integration is desired.

**Stage 1 — Template Location:**
- Searches `~/Code/JUCE-Plugin-Starter` and sibling directories
- If not found, offers to clone from GitHub
- Checks framework version via `gh api` against latest release
- Offers version update (template-wide, project-only, or keep current)
- Extracts 7 reusable developer settings from template `.env` with placeholder detection

**Stage 2 — Collect Settings (6 sub-stages):**

| Sub-stage | Collects | Method |
|-----------|----------|--------|
| 2a | Plugin name | Parse from arguments or AskUserQuestion |
| 2b | Developer settings (7 fields) | Reuse from template or walk through individually |
| 2c | Feature flags (Visage, DiagnosticKit) | Multi-select AskUserQuestion |
| 2d | Auto-updates (Sparkle/WinSparkle) | Yes/No AskUserQuestion |
| 2e | GitHub repository (private/public/skip) | AskUserQuestion |
| 2f | Confirmation | Summary table with "Create" or "Start over" |

**Stage 3 — Execution (10 sub-steps):**

| Step | Action |
|------|--------|
| 3.1 | Generate derived values (class name, bundle ID, 4-letter codes) |
| 3.2 | Copy template via rsync (excluding .git, integrate/, todo/) |
| 3.3 | Replace 9 placeholders via sed across all source files |
| 3.3b | Replace README with project-specific content |
| 3.4 | Visage setup if enabled (run setup_visage.sh, copy editor templates) |
| 3.5 | Generate .env with all collected values (6 sections) |
| 3.6 | Make scripts executable |
| 3.7 | Git init + initial commit |
| 3.8 | GitHub repo creation via gh CLI |
| 3.9 | DiagnosticKit setup (create diagnostic repo, guide PAT creation) |
| 3.10 | Auto-update setup (Sparkle/WinSparkle download, EdDSA key generation) |

**4-Letter Code Generation Algorithm:**
1. Remove special characters (keep alphanumeric and spaces)
2. 0 valid chars: "XXXX"
3. 1-4 chars: uppercase and pad with "X"
4. 5+ chars with multiple words: first 2 of word 1 + first 2 of word 2, uppercased
5. 5+ chars single word: char[0] + char[1] + char[len/2] + char[last], uppercased

**Stage 4 — Summary:**
- Table of all project settings
- Build command
- Next steps with links to related commands

---

### 2. /build — Build Wrapper

**Trigger:** `/juce-dev:build [target...] [action] [--help]`
**Tools:** Read, Bash, Glob, Grep, AskUserQuestion

**Purpose:** Wraps the project's build script with intelligent CMake regeneration detection.

**Targets:**

| Target | Description | Platform |
|--------|-------------|----------|
| all | All formats (default) | All |
| au | Audio Unit v2 | macOS only |
| auv3 | Audio Unit v3 | macOS only |
| vst3 | VST3 | All |
| clap | CLAP | All |
| standalone | Standalone app | All |

**Actions:**

| Action | Description | Confirms? |
|--------|-------------|-----------|
| local | Build without signing (default) | No |
| test | Build + Catch2 + PluginVal | No |
| sign | Build + code sign | No |
| notarize | Build + sign + notarize | No |
| pkg | Build + sign + notarize + package | No |
| publish | Full release pipeline | Yes |
| unsigned | Unsigned installer package | No |
| uninstall | Remove all installed plugins | Yes |

**Key Feature — Intelligent CMake Regeneration Detection:**

The command examines `git diff` to determine whether CMake regeneration is needed:

| Changed File Pattern | Regen Needed? |
|---------------------|---------------|
| `CMakeLists.txt`, `*.cmake` | Yes |
| `.env` | Yes |
| New/deleted `.cpp`, `.h`, `.mm` files | Yes |
| `build/` directory missing | Yes |
| Only source file content changes | No — sets `SKIP_CMAKE_REGEN=1 SKIP_VERSION_BUMP=1` |

If regeneration is skipped but the build fails, the command retries with full regeneration.

**Platform Dispatch:**
- macOS/Linux: `./scripts/build.sh {targets} {action}`
- Windows: `.\scripts\build.ps1 {targets} {action}`

---

### 3. /ci — GitHub Actions CI/CD

**Trigger:** `/juce-dev:ci [platforms] [mode] [options] [--help]`
**Tools:** Read, Bash, Glob, Grep, AskUserQuestion, Edit

**Purpose:** Manages GitHub Actions CI/CD workflows: trigger builds, check status, view logs, and sync signing secrets.

**Sub-commands:**

| Sub-command | Description |
|-------------|-------------|
| status | `gh run list --workflow=build.yml --limit=5` — show recent runs |
| logs [run-id] | `gh run view <id> --log` — show build logs |
| secrets | Compare .env secrets vs GitHub Secrets, offer sync |

**CI Modes:**

| Mode | What It Does |
|------|-------------|
| verify (default) | Build + test only |
| sign | Build + code sign (no release) |
| publish | Build + sign + GitHub Release + website update |

**Secrets Management (16 secrets across 4 categories):**

| Category | Secrets |
|----------|---------|
| macOS Notarization | APPLE_ID, APP_SPECIFIC_PASSWORD, TEAM_ID |
| macOS Code Signing | APPLE_DEVELOPER_CERTIFICATE_P12_BASE64, APPLE_DEVELOPER_CERTIFICATE_PASSWORD, APPLE_INSTALLER_CERTIFICATE_P12_BASE64, APPLE_INSTALLER_CERTIFICATE_PASSWORD |
| Windows (PFX) | WINDOWS_CERT_PFX, WINDOWS_CERT_PASSWORD |
| Windows (Azure Trusted Signing) | AZURE_TENANT_ID, AZURE_CLIENT_ID, AZURE_CLIENT_SECRET, AZURE_SIGNING_ACCOUNT, AZURE_SIGNING_PROFILE, AZURE_SIGNING_ENDPOINT |

Secrets are synced by piping values through stdin (`echo -n "$VALUE" | gh secret set NAME`) to avoid exposing them in process listings. GitHub cannot return secret values, so the command can only verify existence and timestamps.

**Workflow Dispatch:**
```bash
gh workflow run build.yml --ref <branch> \
  -f platforms=<platforms> \
  -f mode=<verify|sign|publish> \
  -f sign_macos=<true|false> \
  -f sign_windows=<true|false> \
  -f create_release=<true|false>
```

---

### 4. /port — Cross-Platform Porting

**Trigger:** `/juce-dev:port <platform> [--audit-only] [--vm <alias>] [--test-ci]`
**Tools:** Read, Write, Edit, Bash, Glob, Grep, AskUserQuestion, Agent

**Purpose:** Port a plugin project between macOS, Windows, and Linux. Scans for platform-specific code, generates a plan, applies changes, and optionally tests.

**Stages:**

| Stage | Description |
|-------|-------------|
| 0 | Detect source platform from project indicators (`.mm` files, `build.ps1`, CMake guards) |
| 1 | Audit source code for platform-specific patterns with severity tables |
| 2 | Generate porting plan |
| 3 | Execute changes on `port/<platform>` branch |

**Source Platform Detection Indicators:**

| Indicator | Platform |
|-----------|----------|
| `.mm` files, Xcode scripts | macOS-origin |
| `build.ps1`, Azure signing vars, Inno Setup | Windows-origin |
| `UNIX AND NOT APPLE` CMake guards, ALSA deps | Linux-origin |
| Multiple indicators | Hybrid (partially cross-platform) |

**Audit Tables:**
The audit stage produces severity-rated tables of platform-specific patterns found in the codebase, categorized by porting direction (e.g., macOS-specific code that needs Windows equivalents).

---

### 5. /setup-visage — GPU UI Integration

**Trigger:** `/juce-dev:setup-visage`
**Tools:** AskUserQuestion, Read, Write, Edit, Bash, Glob, Grep

**Purpose:** Adds GPU-accelerated UI to an existing project using the Visage framework.

**Implementation:**
1. Runs `setup_visage.sh` which:
   - Clones the Visage fork into `external/visage/`
   - Applies 7 patches for plugin compatibility
   - Copies `JuceVisageBridge` files into `Source/Visage/`
   - Appends Visage CMake configuration to `CMakeLists.txt`
   - Sets `USE_VISAGE_UI=TRUE` in `.env`
2. Copies Visage-aware editor templates (replacing standard editor files)
3. Verifies setup succeeded (Visage clone, bridge files, CMake integration)

---

### 6. /setup-ios — iOS/iPadOS Target

**Trigger:** `/juce-dev:setup-ios`
**Tools:** AskUserQuestion, Read, Write, Edit, Bash, Glob, Grep

**Purpose:** Adds an iOS/iPadOS app target (standalone app + AUv3 extension) to an existing project.

**Key Actions:**
- Creates `App/` directory for the iOS app target
- Adds CMake target with iOS-specific conditionals
- Handles Visage-on-iOS bridge differences (touch events vs. mouse events, safe area insets)
- Configures signing for iOS distribution

---

### 7. /setup-updates — Auto-Update Support

**Trigger:** `/juce-dev:setup-updates [--doctor] [--help]`
**Tools:** AskUserQuestion, Read, Write, Edit, Bash, Glob, Grep

**Purpose:** Adds Sparkle 2.x (macOS) and WinSparkle (Windows) auto-update support to standalone app targets.

**Key Actions:**
1. Downloads and configures Sparkle/WinSparkle
2. Generates EdDSA key pair (private key stored in macOS Keychain)
3. Updates `.env` with auto-update configuration:
   - `ENABLE_AUTO_UPDATE=true`
   - `AUTO_UPDATE_MODE=public`
   - `AUTO_UPDATE_EDDSA_PUBLIC_KEY=<key>`
   - Feed URLs pointing to raw GitHub content
4. Verifies source files exist (`AutoUpdater.h`, `AutoUpdater_Mac.mm`, `StandaloneApp.cpp`)

**Doctor Mode (`--doctor`):**
Runs a 20-point validation of the existing auto-update configuration, checking:
- .env variables are set and valid
- EdDSA keys exist and match
- Sparkle/WinSparkle frameworks are present
- Source files are properly integrated
- Feed URLs are accessible
- CMake configuration is correct

---

### 8. /status — Project Status Dashboard

**Trigger:** `/juce-dev:status [--verbose]`
**Tools:** Read, Bash, Glob, Grep

**Purpose:** Displays comprehensive project state as formatted tables.

**Displayed Information:**
- Project configuration (name, bundle ID, developer, namespace)
- Build targets and formats
- Feature flags (Visage, DiagnosticKit, auto-updates)
- Auto-update details (keys, feed URLs, framework versions)
- VM list (from local configuration)
- Build state (last build date, CMake cache status)

---

### 9. /vm — Cross-Platform VM Management

**Trigger:** `/juce-dev:vm <action> [name] [ssh-alias] [platform]`
**Tools:** AskUserQuestion, Read, Write, Edit, Bash, Glob

**Purpose:** Manages SSH-accessible virtual machines for cross-platform build testing.

**Actions:**

| Action | Description |
|--------|-------------|
| add | Register a new VM with SSH alias and platform |
| remove | Unregister a VM |
| list | Show all configured VMs with connectivity status |
| test | SSH to a VM and verify build tools are available |

**Storage:** VM configuration is stored in `.claude/juce-dev.local.md` as YAML blocks within the local configuration file.

---

### 10. /website — GitHub Pages Download Page

**Trigger:** `/juce-dev:website [--regenerate] [--no-github-link] [--help]`
**Tools:** Read, Bash, Glob, Grep, AskUserQuestion, Edit, Write

**Purpose:** Creates a GitHub Pages download page for plugin distribution.

**Key Actions:**
1. Creates an orphan `gh-pages` branch
2. Generates `index.html` with plugin name, description, and download buttons
3. Template interpolation with project settings
4. Enables GitHub Pages via the GitHub API
5. Download links are automatically updated when publishing via `/build publish` or `/ci publish`

---

### 11. /theme — Interactive Theme Designer

**Trigger:** `/juce-dev:theme [--open | --generate <theme.json> | --new <name>]`
**Tools:** AskUserQuestion, Read, Write, Edit, Bash, Glob, Grep

**Purpose:** Interactive Visage theme design workflow with HTML preview and C++ code generation.

**Workflow:**
1. Opens an HTML theme designer in the browser
2. User adjusts colors, typography, spacing via visual interface
3. Exports theme as JSON
4. Python script generates C++ code: `Theme.h` + `ThemeLookAndFeel.h`
5. Generated code integrates with both Visage tokens and the audited framework's LookAndFeel ColourId system

---

## Skills Inventory

### 1. juce-starter — Template Reference Knowledge

**Trigger Phrases:** "JUCE-Plugin-Starter", "starter template", "init plugin project", "plugin template", "juce project setup", ".env configuration", "plugin codes", "bundle ID", "VST3 MIDI generator", "IS_SYNTH/IS_MIDI_EFFECT"

**Content:**
- Complete template structure documentation
- 9-placeholder system with derivation rules
- `.env` schema with all 6 sections:
  1. Project Configuration
  2. Plugin Codes
  3. Version Information
  4. Apple Developer Settings
  5. GitHub Settings
  6. Build Configuration
- VST3 MIDI generator patterns for cross-DAW compatibility
- Ableton Live VST3 MIDI routing workarounds
- `.clang-format` settings documentation
- Test infrastructure (Catch2 with MessageManager initialization)

### 2. juce-visage — Deep GPU UI Integration Reference

**Trigger Phrases:** "Metal rendering", "GPU UI in audio plugins", "embedded MTKView", "bridge layers between JUCE and a GPU framework", "Visage"

**Content:**

*Architecture:*
- Two-framework bridge: the audited framework owns the plugin window, Visage owns a Metal-based render loop via MTKView
- `JuceVisageBridge` component bridges events between the two frameworks
- ApplicationWindow in plugin mode (no NSWindow, MTKView as subview of peer NSView)

*API Reference (Visage Framework):*
- Frame: Layout/input container (similar to HTML div)
- Canvas: 2D drawing commands (paths, gradients, images)
- Theme: Token-based styling system
- Widget: Pre-built controls (buttons, sliders, meters)
- Event: Input event system (mouse, keyboard, touch)
- Dimension: Responsive sizing units
- PostEffect: GPU post-processing (blur, bloom, glow)

*Platform Patterns:*
- macOS: NSView embedding, mouse event forwarding, keyboard/clipboard bridging, focus management
- iOS: UIView embedding, touch event handling (Visage handles natively), safe area insets
- DAW host compatibility: focus management, keyboard event routing in Logic Pro/Ableton/Reaper

*Troubleshooting — 11-Step Destruction Order:*
A specific destruction sequence required to avoid crashes when tearing down the bridge:
1. Stop Visage render loop
2. Release Visage frame tree
3. Release ApplicationWindow
4. Remove MTKView from NSView hierarchy
5. Release MTKView
6. ... (continues for 11 steps)

*UI Patterns:*
- Popups, modals, and dropdown menus spanning the bridge boundary
- Secondary windows
- Resize handling

### 3. visage-theme — Theme Design Workflow

**Trigger Phrases:** "theme design", "color theme", "Visage theme", "ThemeLookAndFeel"

**Content:**
- JSON theme schema with token catalog
- Token categories: colors (primary, secondary, background, surface, accent), typography (font sizes, weights, line heights), spacing (padding, margins, gaps), borders (radius, width, color)
- C++ code generation: Python script transforms JSON to `Theme.h` (token constants) and `ThemeLookAndFeel.h` (LookAndFeel V4 subclass with ColourId mappings)
- Integration with both Visage's token system and the audited framework's LookAndFeel system

---

## Template System

### External Template Repository

The juce-dev plugin depends on an external template repository (`JUCE-Plugin-Starter`) that is NOT bundled with the plugin. The template must be cloned separately and lives at a known path (default: `~/Code/JUCE-Plugin-Starter`).

**Template Structure:**
```
JUCE-Plugin-Starter/
├── Source/
│   ├── PluginProcessor.h/cpp    # Audio processing
│   └── PluginEditor.h/cpp       # UI
├── templates/visage/            # Visage-enabled editor templates
├── scripts/                     # Build, sign, package, setup scripts
├── tests/                       # Catch2 test files
├── CMakeLists.txt               # Build configuration
├── .env                         # Developer + project settings
└── .env.example                 # Settings reference
```

### 9-Placeholder System

Project creation works by copying the template and performing find-and-replace on 9 placeholders:

| Placeholder | Derived From | Example Input | Example Output |
|-------------|-------------|---------------|----------------|
| `PLUGIN_NAME_PLACEHOLDER` | User input | "My Cool Synth" | "My Cool Synth" |
| `CLASS_NAME_PLACEHOLDER` | Plugin name, alphanumeric only | "My Cool Synth" | "MyCoolSynth" |
| `PROJECT_FOLDER_PLACEHOLDER` | Plugin name, hyphens for non-alnum | "My Cool Synth" | "My-Cool-Synth" |
| `BUNDLE_ID_PLACEHOLDER` | com.{namespace}.{folder} | — | "com.generouscorp.My-Cool-Synth" |
| `DEVELOPER_NAME_PLACEHOLDER` | User input | "Generous Corp" | "Generous Corp" |
| `PLUGIN_MANUFACTURER_PLACEHOLDER` | Same as developer name | — | "Generous Corp" |
| `NAMESPACE_PLACEHOLDER` | Developer name, lowercase alnum | "Generous Corp" | "generouscorp" |
| `PLUGIN_CODE_PLACEHOLDER` | 4-letter algorithm from plugin name | "My Cool Synth" | "MYCO" |
| `PLUGIN_MANUFACTURER_CODE_PLACEHOLDER` | 4-letter algorithm from developer name | "Generous Corp" | "GECO" |

Replacement is performed via `sed` across all `.cpp`, `.h`, `.cmake`, `.txt`, `.md`, and `.env*` files.

### .env as Central Configuration

The `.env` file serves as the single source of truth for project configuration. It contains 6 sections:

| Section | Variables |
|---------|-----------|
| Project Configuration | PROJECT_NAME, PRODUCT_NAME, PROJECT_BUNDLE_ID, DEVELOPER_NAME, PROJECT_PATH |
| Plugin Codes | PLUGIN_CODE, PLUGIN_MANUFACTURER_CODE |
| Version Information | VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_BUILD |
| Apple Developer Settings | APPLE_ID, TEAM_ID, APP_CERT, INSTALLER_CERT, APP_SPECIFIC_PASSWORD |
| GitHub Settings | GITHUB_USER, GITHUB_REPO |
| Build Configuration | CMAKE_BUILD_TYPE, BUILD_DIR, JUCE_REPO, JUCE_TAG, USE_VISAGE_UI, ENABLE_DIAGNOSTICS, CI_PLATFORMS |

Additional sections are appended by feature setup commands:
- Auto-updates: ENABLE_AUTO_UPDATE, AUTO_UPDATE_MODE, AUTO_UPDATE_EDDSA_PUBLIC_KEY, feed URLs
- DiagnosticKit: DIAGNOSTIC_GITHUB_REPO, DIAGNOSTIC_GITHUB_PAT, DIAGNOSTIC_SUPPORT_EMAIL

---

## Audited Framework Coupling Points

The juce-dev plugin has extensive coupling to the audited framework's specific conventions, APIs, and ecosystem. Every coupling point listed below must be parameterized or redesigned for Pulp.

### Class and Naming Conventions

| Coupling | Where Used |
|----------|-----------|
| `AudioProcessor` / `AudioProcessorEditor` base classes | Template source files, placeholder system |
| `PluginProcessor.h/cpp` + `PluginEditor.h/cpp` naming | Template structure, Visage template overlay |
| `juce::` namespace prefix | Template source, skill references |
| LookAndFeel V4 subclass pattern | Theme codegen, visage-theme skill |
| ColourId enum system | Theme codegen, skill references |

### CMake Macros

| Macro | Purpose | Where Used |
|-------|---------|-----------|
| `juce_add_plugin()` | Defines plugin target with format list, codes, company | CMakeLists.txt template |
| `juce_add_gui_app()` | Defines standalone app target | iOS setup |
| `target_link_libraries(... juce::juce_audio_processors)` | Link framework modules | CMakeLists.txt template |
| `FetchContent` with `JUCE_TAG` | Download framework source | CMakeLists.txt, .env |

### Plugin Format Names

| Format | How Referenced |
|--------|---------------|
| AU (Audio Unit v2) | Build targets, CI config, format list |
| AUv3 (Audio Unit v3) | Build targets, CI config, iOS setup |
| VST3 | Build targets, CI config, format list |
| CLAP | Build targets, CI config, format list |
| Standalone | Build targets, CI config, format list |

### Build and Validation Tools

| Tool | Coupling Point |
|------|---------------|
| PluginVal | Used in `/build test` for plugin format validation |
| `auval` | Apple's Audio Unit validation tool, invoked during testing |
| `scripts/build.sh` / `build.ps1` | Shell scripts that invoke CMake and framework-specific build logic |
| `scripts/generate_and_open_xcode.sh` | Xcode project generation from CMake |

### Framework Version Management

| Coupling | Details |
|----------|---------|
| `JUCE_TAG` in .env | Specific framework release tag for FetchContent |
| `JUCE_REPO` in .env | Git URL for framework source |
| `gh api repos/juce-framework/JUCE/releases/latest` | Version check against framework releases |
| `~/.juce_cache/juce-src` | FetchContent cache directory |

### Visage Bridge

| Coupling | Details |
|----------|---------|
| `juce::Component` subclass for bridge | `JuceVisageBridge` inherits from framework Component |
| `juce::AudioProcessorEditor` parent | Bridge lives inside the framework's editor |
| `juce::Timer` for render loop | Uses framework timer for Visage redraw scheduling |
| NSView obtained from framework peer | `getPeer()->getNativeHandle()` to get NSView for MTKView embedding |

### Auto-Update Frameworks

| Framework | Platform | Integration Point |
|-----------|----------|------------------|
| Sparkle 2.x | macOS | Linked to Standalone target, `SUUpdater` invoked from app |
| WinSparkle | Windows | Linked to Standalone target, `winsparkle_init()` from app |

### DiagnosticKit

| Coupling | Details |
|----------|---------|
| GitHub Issues API | Diagnostic reports filed as GitHub issues |
| Fine-grained PAT | Token scoped to diagnostic repo for issue creation |
| Framework-specific diagnostic data | System info, plugin state, crash logs in framework format |

---

## Ergonomics Analysis

### Strengths

| Strength | Description |
|----------|-------------|
| **Natural language configuration** | Settings are collected through conversational AskUserQuestion prompts with sensible defaults, not config files or CLI flags |
| **Intelligent CMake skip** | Build command detects when regeneration is unnecessary, saving significant time on iterative builds |
| **Staged create workflow** | 5-stage creation breaks a complex process into verifiable steps with clear progress |
| **Comprehensive port audit tables** | The /port command produces detailed severity-rated tables of platform-specific patterns before making any changes |
| **Doctor mode** | The /setup-updates --doctor provides 20-point validation of existing configuration |
| **Secrets safety** | GitHub secrets are synced via stdin piping, never exposed in command arguments or process listings |
| **Template reuse** | Developer settings (Apple ID, Team ID, certs) are extracted from template .env and reused across projects |
| **Confirmation gates** | Destructive actions (publish, uninstall) require explicit confirmation |
| **Contextual skills** | Knowledge activates automatically based on conversation context, not explicit invocation |
| **Help system** | Every command supports `--help` with complete usage reference |

### Weaknesses

| Weakness | Description | Impact |
|----------|-------------|--------|
| **External template dependency** | Template must be cloned separately; if not at `~/Code/JUCE-Plugin-Starter`, user must specify path | First-run friction |
| **`sed -i ''` macOS-ism** | Placeholder replacement uses `sed -i ''` which is macOS-specific (GNU sed uses `sed -i` without the empty string) | Breaks on Linux without modification |
| **Hardcoded `~/Code/` path** | Template search defaults to `~/Code/` which is a convention, not a universal standard | Path mismatch for users with different directory structures |
| **CMake regen heuristic false negatives** | Git diff-based detection can miss cases where regeneration is needed (e.g., changes to FetchContent sources, toolchain files) | Build failures that require manual intervention |
| **Windows PATH refresh fragility** | After installing tools via winget, PATH may not update in the current shell; the PowerShell env refresh command is fragile | Tool-not-found errors immediately after installation |
| **No rollback** | If project creation fails mid-way (e.g., GitHub repo created but git push fails), there's no automatic cleanup | Orphaned repos and partial project state |
| **Skills cannot be explicitly invoked** | Users cannot say "activate the visage skill" — they must use trigger phrases, which may not be intuitive | Knowledge may not activate when needed |
| **Large command files** | Some commands (create.md, ci.md) are 400+ lines of instructions, pushing the context window | Potential for Claude to lose track of steps |
| **Single-user assumption** | Developer settings are extracted from a single template; no multi-user or team workflow support | Unsuitable for team development |

---

## What Must Be Generalized for Pulp

### Architecture That Transfers Directly

The following aspects of the juce-dev plugin are **framework-agnostic** and can be adopted by Pulp with minimal modification:

| Component | Why It Transfers |
|-----------|-----------------|
| **Commands as Markdown** | Instruction format is framework-independent |
| **YAML frontmatter convention** | description, argument-hint, allowed-tools — universal |
| **Staged workflows** | Environment check, collect settings, execute, summarize — universal pattern |
| **AskUserQuestion interaction patterns** | Multi-select, confirmation gates, path-A/path-B branching |
| **Skills as contextual knowledge** | Trigger-phrase activation, reference directories |
| **.env as central config** | Key-value configuration is framework-agnostic |
| **Git + GitHub workflow** | Init, commit, repo create, secrets sync, CI trigger |
| **Doctor/validation mode** | N-point configuration validation pattern |
| **VM management** | SSH-based cross-platform testing |

### Components That Require Full Redesign

| Component | Current Coupling | Pulp Redesign Needed |
|-----------|-----------------|---------------------|
| **Template repository** | JUCE-Plugin-Starter with framework-specific structure | New Pulp template(s) with Pulp project structure |
| **Placeholder system** | 9 sed placeholders for framework naming conventions | New placeholders for Pulp naming conventions |
| **CMake macros** | `juce_add_plugin()`, `target_link_libraries(juce::...)` | Pulp CMake macros or SPM targets |
| **Format list** | AU, AUv3, VST3, CLAP, Standalone | Same formats but different build integration |
| **Build scripts** | `build.sh` / `build.ps1` calling framework CMake | New Pulp build scripts |
| **Plugin codes** | 4-letter codes (framework convention) | Keep if targeting same formats (they're format-level, not framework-level) |
| **PluginVal** | Framework-specific plugin validation | Replace with format-specific validators (auval, pluginval, clap-validator) |
| **FetchContent/JUCE_TAG** | Framework source download | Pulp dependency management (FetchContent, SPM, or vcpkg) |
| **LookAndFeel system** | V4 subclass with ColourId mappings | Pulp theming system (protocol-based or SwiftUI) |
| **Visage bridge** | Bridge through framework Component and AudioProcessorEditor | Pulp's own UI/editor architecture |
| **Auto-update frameworks** | Sparkle/WinSparkle for standalone apps | Same frameworks but different integration points |
| **DiagnosticKit** | Framework-specific diagnostic data collection | Pulp-specific diagnostic data |

### Parameterization Strategy

For the Pulp version of this plugin, the following should be parameterized (not hardcoded):

1. **Framework name and source URL** — Pulp repo URL instead of JUCE GitHub
2. **Module/target names** — Pulp module names instead of `juce_audio_processors`, etc.
3. **CMake integration pattern** — Pulp's CMake setup instead of FetchContent/JUCE
4. **Plugin base class names** — Pulp's plugin base class instead of AudioProcessor
5. **Editor class pattern** — Pulp's editor architecture instead of AudioProcessorEditor
6. **Template repository URL** — Pulp starter template
7. **Supported formats per platform** — May differ from current template (e.g., Pulp may add CLAP as a first-class format)
8. **Feature flags** — Pulp-specific features instead of Visage/DiagnosticKit
9. **Build script names and structure** — Pulp build system
10. **Version check API** — Pulp release channel instead of JUCE GitHub releases
