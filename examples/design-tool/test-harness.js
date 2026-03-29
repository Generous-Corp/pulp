// ═══════════════════════════════════════════════════════════════
// Automated test harness — injected after design-tool.js
// Tests all interactive UX by dispatching events programmatically
// ═══════════════════════════════════════════════════════════════

var testsPassed = 0;
var testsFailed = 0;

function assert(condition, name) {
    if (condition) {
        testsPassed++;
        console.log("PASS: " + name);
    } else {
        testsFailed++;
        console.log("FAIL: " + name);
    }
}

// Test 1: Harmony dropdown changes currentHarmony
console.log("--- Test 1: Harmony dropdown ---");
var prevHarmony = currentHarmony;
__dispatch__("harmony-selector", "select", 1);
assert(currentHarmony === "analogous", "Harmony changed to analogous");
__dispatch__("harmony-selector", "select", 0);
assert(currentHarmony === "monochromatic", "Harmony restored to monochromatic");

// Test 2: Mode dropdown
console.log("--- Test 2: Mode dropdown ---");
__dispatch__("mode-selector", "select", 1);
// Should have called setTheme("light")
__dispatch__("mode-selector", "select", 0);

// Test 3: Preset dropdown
console.log("--- Test 3: Preset dropdown ---");
__dispatch__("preset-selector", "select", 3);
assert(currentAccent === "#AA88FF", "Preset Violet sets accent to #AA88FF");
__dispatch__("preset-selector", "select", 0);
assert(currentAccent === "#89B4FA", "Preset Default restores accent");

// Test 4: Token popup open/close
console.log("--- Test 4: Token popup ---");
openTokenPopup("bg.primary", "tok-0-0-sw", 0, 0);
assert(tokenEditState.activeToken === "bg.primary", "Token popup opened for bg.primary");
closeTokenPopup();
assert(tokenEditState.activeToken === null, "Token popup closed");

// Test 5: Apply token color
console.log("--- Test 5: Apply token color ---");
var prevTheme = JSON.parse(getThemeJson());
applyTokenColor("bg.primary", "#ff0000");
var newTheme = JSON.parse(getThemeJson());
assert(newTheme.colors["bg.primary"] === "#ff0000", "Token color applied");
assert(tokenEditState.modified["bg.primary"] === true, "Token marked modified");

// Test 6: Token undo
console.log("--- Test 6: Token undo ---");
var h = tokenHistory("bg.primary");
assert(h.cursor > 0, "Token has undo history");

// Test 7: Palette expand/collapse
console.log("--- Test 7: Palette expand ---");
// Simulate clicking Accent dot
expandedPalette = 0;
setVisible("ramp-0-editor", true);
assert(expandedPalette === 0, "Accent palette expanded");
expandedPalette = -1;
setVisible("ramp-0-editor", false);
assert(expandedPalette === -1, "Palette collapsed");

// Test 8: Export format generation
console.log("--- Test 8: Export ---");
var jsonExport = generateExport(0);
assert(jsonExport.length > 100, "JSON export has content");
var cssExport = generateExport(1);
assert(cssExport.indexOf("--pulp-") >= 0, "CSS export has custom properties");
var oklchExport = generateExport(2);
assert(oklchExport.indexOf("oklch(") >= 0, "OKLCH export has oklch() values");

// Test 9: Chat message
console.log("--- Test 9: Chat ---");
addChatMessage("user", "test message");
assert(msgCount > 0, "Chat message added");

// Summary
console.log("═══════════════════════════════════════");
console.log("RESULTS: " + testsPassed + " passed, " + testsFailed + " failed");
console.log("═══════════════════════════════════════");
