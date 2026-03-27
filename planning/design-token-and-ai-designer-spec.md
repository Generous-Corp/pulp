# Design Token & AI Designer Spec

**Date:** 2026-03-26
**Type:** Product specification
**Depends on:** pulp-ui-architecture-spec.md, ui-pattern-extraction-from-visage.md
**Implements:** Stream C from ai-designer-workstream-plan.md

---

## 1. Overview

`pulp design` is a CLI command that lets developers restyle Pulp plugin UIs using natural language. It invokes a local AI tool (Claude CLI) to generate structured design token diffs, previews the result via headless screenshot, and saves accepted styles as reusable style packs.

The system is local-first: no cloud services required, works offline with saved style packs, and treats the AI as a structured-diff generator — not the owner of design state.

---

## 2. Token Schema

### Current Tokens (35 total)

The schema is defined by `Theme::dark()` in `core/view/src/theme.cpp`. All tokens use dot-separated naming: `category.name`.

**Colors (16):**
```
bg.primary        bg.secondary      bg.surface        bg.elevated
text.primary      text.secondary    text.disabled
accent.primary    accent.secondary  accent.success    accent.warning    accent.error
control.track     control.fill      control.thumb     control.border
```

**Dimensions (17):**
```
spacing.xs (2)    spacing.sm (4)    spacing.md (8)    spacing.lg (16)   spacing.xl (24)
radius.sm (4)     radius.md (8)     radius.lg (12)    radius.full (9999)
font.xs (10)      font.sm (12)      font.md (14)      font.lg (18)      font.xl (24)
control.knob_size (48)  control.fader_width (24)  control.meter_width (12)
```

**Strings (2):**
```
font.family ("Inter")    font.mono ("JetBrains Mono")
```

### Extensibility

The token schema is open — users can add custom tokens (e.g., `plugin.header_color`, `plugin.logo_size`). Custom tokens are stored in style packs and passed through `Theme::apply_overrides()`. Widgets must explicitly read custom tokens via `resolve_color()` or `theme_.dimension()`.

The AI prompt includes only the known tokens above, but the diff parser accepts unknown tokens with a warning.

### Token Categories (for AI prompt context)

| Category | Purpose | Tokens |
|----------|---------|--------|
| bg.* | Background layers, darkest to lightest | 4 colors |
| text.* | Typography colors | 3 colors |
| accent.* | Brand colors, semantic status | 5 colors |
| control.* | Interactive widget chrome | 4 colors, 3 dimensions |
| spacing.* | Layout spacing scale | 5 dimensions |
| radius.* | Corner rounding scale | 4 dimensions |
| font.* | Typography sizes and families | 5 dimensions, 2 strings |

---

## 3. AI Interaction Model

### Subprocess Architecture

```
pulp design "warm vintage look"
  │
  ├─ 1. Load base theme (dark/light/pro_audio)
  ├─ 2. Serialize to JSON via Theme::to_json()
  ├─ 3. Build prompt (theme JSON + valid tokens + user request)
  ├─ 4. Invoke: echo '<prompt>' | claude --print
  ├─ 5. Extract JSON from response (find { ... } boundaries)
  ├─ 6. Parse via Theme::from_json(), validate token names
  ├─ 7. Apply via Theme::apply_overrides() on copy of base
  ├─ 8. Render headless screenshot → temp PNG
  ├─ 9. Open preview (macOS: `open`, or print path)
  └─ 10. Interactive: [A]ccept / [R]eject / [T]weak / [S]ave
```

### Claude CLI Invocation

```bash
echo "$PROMPT" | claude --print
```

The `--print` flag runs Claude in non-interactive single-turn mode. The prompt is piped via stdin and the response comes on stdout.

If `claude` is not found on PATH:
- Print error: "Claude CLI not found. Install from https://docs.anthropic.com/claude-code"
- Offer fallback: `pulp design --load <file.json>` for manual editing

### AI Prompt Template

```
You are a design token expert for audio plugin UIs. You modify structured design tokens to achieve a requested visual style.

## Current Theme
<JSON from Theme::to_json()>

## Valid Token Names

Colors (hex format #rrggbb or #rrggbbaa):
  bg.primary, bg.secondary, bg.surface, bg.elevated
  text.primary, text.secondary, text.disabled
  accent.primary, accent.secondary, accent.success, accent.warning, accent.error
  control.track, control.fill, control.thumb, control.border

Dimensions (numbers, in logical pixels):
  spacing.xs, spacing.sm, spacing.md, spacing.lg, spacing.xl
  radius.sm, radius.md, radius.lg, radius.full
  font.xs, font.sm, font.md, font.lg, font.xl
  control.knob_size, control.fader_width, control.meter_width

Strings:
  font.family, font.mono

## Rules
- Output ONLY a JSON object. No markdown, no explanation, no code fences.
- Use the exact same format as the theme above.
- Only include tokens you want to CHANGE — omit unchanged tokens.
- Colors must be hex strings starting with #.
- Dimensions must be positive numbers.
- Maintain visual hierarchy: bg.primary should be darkest background, bg.elevated lightest.
- Maintain readability: text colors must contrast sufficiently with background colors.
- Maintain semantic meaning: accent.error should feel like an error/warning color.

## Request
"<user's natural language request>"

## Output
```

### Response Parsing

1. Find first `{` and last `}` in the response (handles markdown fences or preamble)
2. Parse the JSON via `Theme::from_json()`
3. Validate: check each key against the known token list
4. Unknown keys → warning (still applied, user might have custom tokens)
5. Invalid values (non-hex colors, negative dimensions) → error, token skipped

---

## 4. Style Pack Format

### File Format

```json
{
  "name": "Vintage Amber",
  "description": "Warm vintage look with amber accents and soft rounding",
  "base": "dark",
  "overrides": {
    "colors": {
      "accent.primary": "#d4a017",
      "accent.secondary": "#c4956a",
      "bg.primary": "#1a1510",
      "bg.secondary": "#2a2218"
    },
    "dimensions": {
      "radius.md": 6.0,
      "radius.lg": 10.0
    },
    "strings": {}
  }
}
```

### Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `name` | string | yes | Human-readable style name |
| `description` | string | no | How the style was generated or what it looks like |
| `base` | string | no | Base theme name: "dark", "light", "pro_audio". Default: "dark" |
| `overrides` | Theme JSON | yes | Only the tokens that differ from base |

### Resolution

```
StylePack::resolve()
  1. Look up base theme by name → Theme::dark() / light() / pro_audio()
  2. Apply overrides via Theme::apply_overrides()
  3. Return the resulting complete Theme
```

### Storage Locations

Search order (first match wins for a given name):
1. `<project_root>/styles/` — project-specific packs
2. `~/.pulp/styles/` — global user packs

Built-in themes are always available by name ("dark", "light", "pro_audio") without files.

### File Discovery

`StylePack::list_packs()` scans all search paths for `*.json` files, parses each, returns a list sorted by name. Duplicates: project-local overrides global.

---

## 5. `pulp design` CLI Interface

### Usage

```
pulp design [options] [prompt...]

AI-powered style design for Pulp plugin UIs.

Options:
  --base <theme>       Base theme: dark (default), light, pro_audio
  --target <name>      Render target: showcase (default), or plugin name
  --load <file>        Apply a style pack file (no AI)
  --save <file>        Save the result as a style pack
  --list               List available style packs and exit
  --preview-only       Render screenshot of current theme, no AI
  --output <file.png>  Screenshot output path (default: temp file)
  --no-open            Don't auto-open the screenshot
  --json               Output resulting theme JSON to stdout
  --diff               Output only the changed tokens as JSON
  --dry-run            Show the AI prompt without invoking Claude
  --name <name>        Name for saved style pack
  --desc <text>        Description for saved style pack

Examples:
  pulp design "warm vintage with amber accents"
  pulp design --base pro_audio "neon cyberpunk"
  pulp design --load styles/vintage.json --preview-only
  pulp design --list
  pulp design --save styles/my-theme.json "dark moody analog"
```

### Interactive Mode

After AI generates a diff and the preview opens:

```
Preview: /tmp/pulp-design-preview.png

Changed tokens:
  accent.primary: #89B4FA → #d4a017
  accent.secondary: #F5C2E7 → #c4956a
  bg.primary: #1E1E2E → #1a1510
  bg.secondary: #2A2A3C → #2a2218
  radius.md: 8.0 → 6.0

[A]ccept  [R]eject  [T]weak "..."  [S]ave as...  [Q]uit
>
```

- **Accept**: Apply the theme (write to project/plugin config)
- **Reject**: Discard, exit
- **Tweak**: Enter a follow-up prompt (re-invokes Claude with current state + tweak)
- **Save**: Prompt for filename and save as style pack
- **Quit**: Discard, exit

### Non-Interactive Mode

With `--json` or `--diff`, output goes to stdout with no interactive prompt. Useful for scripting and MCP integration.

```bash
# Scriptable: generate a theme, pipe to another tool
pulp design --json "ocean blue" > my-theme.json

# Dry run: see the prompt
pulp design --dry-run "vintage warm" > prompt.txt
```

---

## 6. Token Diff API

### New Header: `core/view/include/pulp/view/token_diff.hpp`

```cpp
namespace pulp::view {

struct TokenDiffResult {
    Theme diff;                          // Parsed tokens (only changed ones)
    std::vector<std::string> warnings;   // Unknown token names, parse issues
    bool valid = false;                  // Whether parsing succeeded
};

// Parse JSON into a token diff, validate against known tokens in reference theme
TokenDiffResult parse_token_diff(const std::string& json, const Theme& reference);

// List all known token names from a reference theme
std::vector<std::string> known_color_tokens(const Theme& ref);
std::vector<std::string> known_dimension_tokens(const Theme& ref);
std::vector<std::string> known_string_tokens(const Theme& ref);

// Build the AI prompt for Claude CLI
std::string generate_design_prompt(const Theme& base, const std::string& user_request);

// Extract JSON object from a potentially noisy AI response
// Finds first '{' to last '}', strips markdown fences
std::string extract_json_from_response(const std::string& response);

} // namespace pulp::view
```

---

## 7. Style Pack API

### New Header: `core/view/include/pulp/view/style_pack.hpp`

```cpp
namespace pulp::view {

struct StylePack {
    std::string name;
    std::string description;
    std::string base_theme;    // "dark", "light", "pro_audio", or empty (= dark)
    Theme overrides;           // Only tokens that differ from base

    // Resolve to a full Theme (load base + apply overrides)
    Theme resolve() const;

    // Serialization
    std::string to_json() const;
    static StylePack from_json(const std::string& json);

    // File I/O
    static StylePack load(const std::filesystem::path& path);
    bool save(const std::filesystem::path& path) const;

    // Discovery
    static std::vector<StylePack> list_packs();
    static std::vector<std::filesystem::path> search_paths();
};

} // namespace pulp::view
```

---

## 8. MCP Tool Additions

Two new tools in `tools/mcp/pulp_mcp.cpp`:

### pulp_design_preview
- **Input:** `theme_json` (full or diff), `target` (showcase/plugin name), `base` (theme name)
- **Output:** base64 PNG screenshot
- **Use:** AI agents can preview theme changes without the interactive CLI

### pulp_design_apply
- **Input:** `style_pack_path` or `theme_json`
- **Output:** resulting full theme JSON
- **Use:** AI agents can apply styles programmatically

---

## 9. CLI Integration

In `tools/cli/pulp_cli.cpp`, add a `design` command that delegates to `pulp-design` binary (same pattern as `inspect` delegates to `pulp-screenshot`):

```cpp
if (command == "design") {
    auto root = find_project_root();
    auto design_bin = root / "build" / "tools" / "design" / "pulp-design";
    // delegate with all args
}
```

Add to `print_usage()`:
```
  design   AI-powered style design (natural language → token diffs → preview)
```

---

## 10. Build Integration

### New CMake target: `tools/design/CMakeLists.txt`

```cmake
add_executable(pulp-design pulp_design.cpp)
target_link_libraries(pulp-design PRIVATE pulp::view pulp::canvas pulp::state)
target_compile_features(pulp-design PRIVATE cxx_std_20)
```

### Root CMakeLists.txt
Add `add_subdirectory(tools/design)` alongside existing tool subdirectories.

### Built-in Style Packs
Export the three built-in themes as style pack JSON files in `styles/`:
- `styles/dark.json`
- `styles/light.json`
- `styles/pro_audio.json`

These are generated at build time or checked in as static files.

---

## 11. Test Plan

### test_token_diff.cpp (6 tests)
1. Parse a valid diff JSON → correct Theme with only specified tokens
2. Parse a diff with unknown token names → valid=true, warnings list populated
3. Parse invalid JSON (malformed) → valid=false
4. `generate_design_prompt()` output includes all 35 token names and user request string
5. `known_color_tokens()` returns 16 names from dark theme
6. `extract_json_from_response()` handles markdown fences, preamble text, trailing text

### test_style_pack.cpp (5 tests)
1. Round-trip: create → to_json() → from_json() → compare fields
2. `resolve()` with base="dark" applies overrides correctly
3. `resolve()` with base="light" uses light theme as base
4. `from_json()` with missing optional fields (no description, no base) → defaults
5. `save()` → `load()` round-trip via temp file

### test_showcase.cpp (3 tests)
1. `build_showcase(Theme::dark())` returns non-null root with expected widget count
2. Showcase renders to PNG without crash under all 3 built-in themes
3. All widgets in showcase have non-empty IDs

---

## 12. Security Considerations

- The AI prompt is constructed from trusted local data (theme JSON + hardcoded token names)
- AI responses are parsed as JSON only — no code execution
- Unknown tokens produce warnings, not errors — no injection path
- `claude --print` runs in non-interactive mode — no shell escapes
- Style pack files are JSON only — no executable content

---

## 13. Acceptance Criteria

The AI Designer MVP is done when:

1. `pulp design "warm vintage"` generates a diff, shows preview, offers accept/reject
2. `pulp design --list` shows available style packs
3. `pulp design --load styles/dark.json --preview-only` renders a screenshot
4. `pulp design --save styles/my.json "ocean theme"` saves a reusable pack
5. `pulp design --dry-run "neon"` shows the prompt without invoking AI
6. Saved style packs round-trip: save → load → resolve → identical Theme
7. All 14+ new tests pass
8. Existing 647 tests still pass
9. check-docs.sh passes
10. STATUS.md updated to reflect the feature
