// Issue 2: image upload via file dialog
var uploadedImagePath = "";
var uploadedImageName = "";
var REFERENCE_IMAGE_EXTENSIONS = "png;jpg;jpeg;gif;webp;bmp;tif;tiff;heic;heif";

function clearUploadedImage() {
    uploadedImagePath = "";
    uploadedImageName = "";
    setText("chat-attachment-label", "");
    setVisible("chat-attachment-row", false);
    setFlex("chat-attachment-row", "height", 0);
    layout();
}

function shellQuote(s) {
    return "'" + String(s || "").split("'").join("'\\''") + "'";
}

function buildAiCliCommand(promptFile, model, provider, reasoningEffort) {
    var aiCli = "";
    try { aiCli = getAICli(); } catch (e) {}
    var bridgeDefaultCli = "claude --print --model {model}";
    if (!provider || provider.length === 0) provider = "claude";
    if (!aiCli || aiCli.length === 0 || (provider === "codex" && aiCli === bridgeDefaultCli)) {
        if (provider === "codex") {
            aiCli = "codex exec - --model {model} --skip-git-repo-check --sandbox read-only --color never --ephemeral";
            if (reasoningEffort && reasoningEffort.length > 0) {
                aiCli += " -c model_reasoning_effort={reasoning_effort}";
            }
            aiCli += " -o {output_file} >/dev/null";
        } else {
            aiCli = bridgeDefaultCli;
        }
    }

    var usesPromptFile = aiCli.indexOf("{prompt_file}") >= 0;
    var usesModel = aiCli.indexOf("{model}") >= 0;
    var usesProvider = aiCli.indexOf("{provider}") >= 0;
    var usesReasoningEffort = aiCli.indexOf("{reasoning_effort}") >= 0;
    var usesOutputFile = aiCli.indexOf("{output_file}") >= 0;
    var outputFile = promptFile + ".out.txt";
    var cmd = aiCli;
    if (usesPromptFile) cmd = cmd.split("{prompt_file}").join(shellQuote(promptFile));
    if (usesModel) cmd = cmd.split("{model}").join(shellQuote(model));
    if (usesProvider) cmd = cmd.split("{provider}").join(shellQuote(provider || ""));
    if (usesReasoningEffort) cmd = cmd.split("{reasoning_effort}").join(shellQuote(reasoningEffort || ""));
    if (usesOutputFile) cmd = cmd.split("{output_file}").join(shellQuote(outputFile));
    if (!usesPromptFile) {
        cmd = "cat " + shellQuote(promptFile) + " | " + cmd;
    }
    if (usesOutputFile) {
        return "{ " + cmd + "; __pulp_status=$?; if [ -f " + shellQuote(outputFile) + " ]; then cat " + shellQuote(outputFile) + "; fi; printf '\\n__PULP_AI_EXIT_CODE__:%s\\n' \"$__pulp_status\"; rm -f " + shellQuote(promptFile) + " " + shellQuote(outputFile) + "; } 2>&1";
    }
    return "{ " + cmd + "; __pulp_status=$?; printf '\\n__PULP_AI_EXIT_CODE__:%s\\n' \"$__pulp_status\"; rm -f " + shellQuote(promptFile) + "; } 2>&1";
}

function parseAiCliResponse(response) {
    var text = String(response || "");
    var marker = "__PULP_AI_EXIT_CODE__:";
    var markerIdx = text.lastIndexOf(marker);
    var exitCode = 0;
    if (markerIdx >= 0) {
        var suffix = text.substring(markerIdx + marker.length).trim();
        var firstToken = suffix.split(/\s+/)[0];
        exitCode = parseInt(firstToken, 10);
        if (isNaN(exitCode)) exitCode = 0;
        text = text.substring(0, markerIdx);
    }
    return {
        text: text.replace(/\s+$/, ""),
        exitCode: exitCode
    };
}

function summarizeAiCliFailure(provider, exitCode, responseText) {
    var providerLabel = provider === "codex" ? "Codex" : "Claude";
    var clean = String(responseText || "").trim();
    var firstLine = clean.length > 0 ? clean.split(/\r?\n/)[0] : "";
    var lower = firstLine.toLowerCase();
    if (lower.indexOf("not found") >= 0 || exitCode === 127) {
        return providerLabel + " CLI was not found. Check the configured AI CLI command and try again.";
    }
    if (lower.indexOf("api key") >= 0 || lower.indexOf("authentication") >= 0 || lower.indexOf("unauthorized") >= 0) {
        return providerLabel + " authentication failed. Check the provider login/API key and try again.";
    }
    if (lower.indexOf("rate limit") >= 0 || lower.indexOf("too many requests") >= 0) {
        return providerLabel + " hit a rate limit. Wait a moment and try again.";
    }
    if (firstLine.length > 0) {
        return providerLabel + " request failed: " + firstLine;
    }
    return providerLabel + " request failed with exit code " + exitCode + ".";
}

function handleDesignChatCommandResult(requestId, provider, response) {
    if (chatActiveRequestId !== requestId) return false;
    var outcome = parseAiCliResponse(response);
    if (outcome.exitCode !== 0) {
        failPendingChat(summarizeAiCliFailure(provider, outcome.exitCode, outcome.text), "Chat error");
        return false;
    }
    if (!outcome.text || outcome.text.length === 0) {
        failPendingChat("AI provider returned no output. Check the CLI/provider configuration and try again.", "Chat error");
        return false;
    }
    try {
        applyDesignChatResponse(outcome.text);
        return true;
    } catch (e) {
        failPendingChat("Chat apply failed: " + String(e), "Chat error");
        return false;
    }
}

function setUploadedImage(path) {
    uploadedImagePath = path || "";
    var parts = String(uploadedImagePath).replace(/\\/g, "/").split("/");
    uploadedImageName = parts.length > 0 ? parts[parts.length - 1] : uploadedImagePath;
    if (!uploadedImagePath) {
        clearUploadedImage();
        return;
    }
    setText("chat-attachment-label", uploadedImageName);
    setVisible("chat-attachment-row", true);
    setFlex("chat-attachment-row", "height", 22);
    layout();
}

function isSupportedReferenceImage(path) {
    if (!path || path.length === 0) return false;
    var lower = String(path).toLowerCase();
    var dot = lower.lastIndexOf(".");
    if (dot < 0) return false;
    var ext = lower.slice(dot + 1);
    return ext === "png" || ext === "jpg" || ext === "jpeg" || ext === "gif" ||
        ext === "webp" || ext === "bmp" || ext === "tif" || ext === "tiff" ||
        ext === "heic" || ext === "heif";
}

function getReferenceImageDialogExtensions() {
    return REFERENCE_IMAGE_EXTENSIONS;
}

function wireChatAttachmentClear() {
    on("chat-attachment-clear", "click", function() {
        clearUploadedImage();
    });
}
wireChatAttachmentClear();

function updateChatInputSizing(text) {
    var value = text || "";
    var explicitLines = value.split("\n");
    var wrappedLines = 0;
    for (var li = 0; li < explicitLines.length; li++) {
        wrappedLines += Math.max(1, Math.ceil(explicitLines[li].length / 34));
    }
    var visibleLines = Math.max(1, Math.min(5, wrappedLines));
    var editorHeight = Math.max(32, 14 + visibleLines * 16);
    setFlex("chat-input", "height", editorHeight);
    setFlex("chat-input-row", "height", Math.max(36, editorHeight));
    layout();
}

function setChatPendingUi(pending) {
    setEnabled("chat-input", !pending);
    setEnabled("provider-selector", !pending);
    setEnabled("model-selector", !pending);
    setEnabled("effort-selector", !pending);

    setPointerEvents("upload-btn", pending ? "none" : "auto");
    setOpacity("upload-btn", pending ? 0.45 : 1.0);
    setBackground("upload-btn", pending ? APP_PANEL_RAISED : APP_PANEL);
    setBorder("upload-btn", APP_BORDER, 1, 6);

    setPointerEvents("chat-export-btn", pending ? "none" : "auto");
    setOpacity("chat-export-btn", pending ? 0.45 : 1.0);
    refreshSendButtonPresentation(pending);
}

function buildChatControlsAndToast() {
    registerClick("upload-btn");
    on("upload-btn", "click", function() {
        var path = showOpenDialog("Select Reference Image", "Image Files", getReferenceImageDialogExtensions());
        if (path && path.length > 0) {
            if (!isSupportedReferenceImage(path)) {
                showToast("Unsupported image type");
                return;
            }
            setUploadedImage(path);
            showToast("Attached " + uploadedImageName);
        }
    });

    createTextEditor("chat-input", "chat-input-row");
    setPlaceholder("chat-input", "Describe a style...");
    setMultiLine("chat-input", 1);
    setFlex("chat-input", "flex_grow", 1);
    setFlex("chat-input", "height", 32);
    setTextColor("chat-input", APP_TEXT);
    on("chat-input", "change", function(text) {
        updateChatInputSizing(text);
    });

    // #49: Send button with proper icon sizing
    createCol("send-btn", "chat-input-row");
    setFlex("send-btn", "width", 32);
    setFlex("send-btn", "height", 32);
    setBackground("send-btn", APP_ACCENT);
    setBorder("send-btn", APP_ACCENT, 1, 6);
    setFlex("send-btn", "justify_content", "center");
    setFlex("send-btn", "align_items", "center");
    createIcon("send-icon", "send", "send-btn");
    setFlex("send-icon", "width", 16);
    setFlex("send-icon", "height", 16);
    setPointerEvents("send-icon", "none");
    createIcon("send-cancel-icon", "close", "send-btn");
    setFlex("send-cancel-icon", "width", 14);
    setFlex("send-cancel-icon", "height", 14);
    setPointerEvents("send-cancel-icon", "none");
    setVisible("send-cancel-icon", false);
    // Issue 3: hover state for send button
    registerHover("send-btn");
    on("send-btn", "mouseenter", function() {
        if (chatRequestPending) {
            setBackground("send-btn", APP_SURFACE);
            setBorder("send-btn", APP_TEXT_DIM, 1, 6);
        } else {
            setBackground("send-btn", APP_ACCENT_HOVER);
            setBorder("send-btn", APP_ACCENT_HOVER, 1, 6);
        }
    });
    on("send-btn", "mouseleave", function() { refreshSendButtonPresentation(); });

    // ═══════════════════════════════════════════════════════════════════
    // STATUS BAR (28px, full width)
    // ═══════════════════════════════════════════════════════════════════
    createRow("status-bar");
    setFlex("status-bar", "height", 28);
    setFlex("status-bar", "flex_shrink", 0);
    setFlex("status-bar", "padding_left", 12);
    setFlex("status-bar", "padding_right", 12);
    setFlex("status-bar", "align_items", "center");
    setFlex("status-bar", "justify_content", "space-between");
    setBackground("status-bar", APP_SURFACE);
    setBorder("status-bar", APP_BORDER, 1, 0);

    createLabel("status-text", "0 tokens modified", "status-bar");
    setFontSize("status-text", 10);
    setTextColor("status-text", APP_TEXT_DIM);

    createLabel("status-schema", "pulp-theme/v1", "status-bar");

    // ═══════════════════════════════════════════════════════════════════
    // D7: Toast notification system
    // ═══════════════════════════════════════════════════════════════════
    createCol("toast-overlay", "");
    setPosition("toast-overlay", "absolute");
    setFlex("toast-overlay", "width", 200);
    setFlex("toast-overlay", "height", 30);
    setFlex("toast-overlay", "justify_content", "center");
    setFlex("toast-overlay", "align_items", "center");
    setBackground("toast-overlay", APP_PANEL);
    setBorder("toast-overlay", APP_BORDER, 1, 6);
    setBoxShadow("toast-overlay", 0, 4, 16, 0, "#00000060");
    setZIndex("toast-overlay", 200);
    setOpacity("toast-overlay", 0);
    setVisible("toast-overlay", false);

    createLabel("toast-text", "", "toast-overlay");
    setFontSize("toast-text", 10);

    createModal("help-modal", "");
    setPosition("help-modal", "absolute");
    setFlex("help-modal", "width", 1100);
    setFlex("help-modal", "height", 700);
    setFlex("help-modal", "justify_content", "center");
    setFlex("help-modal", "align_items", "center");
    setBackground("help-modal", "#00000088");
    setZIndex("help-modal", 210);
    setOpacity("help-modal", 0);
    setVisible("help-modal", false);
    setPointerEvents("help-modal", "none");
    registerClick("help-modal");

    createCol("help-card", "help-modal");
    setFlex("help-card", "width", 340);
    setFlex("help-card", "min_height", 144);
    setFlex("help-card", "padding", 16);
    setFlex("help-card", "gap", 10);
    setBackground("help-card", APP_PANEL);
    setBorder("help-card", APP_BORDER, 1, 12);
    setBoxShadow("help-card", 0, 12, 32, 0, "#00000088");

    createRow("help-card-header", "help-card");
    setFlex("help-card-header", "height", 26);
    setFlex("help-card-header", "align_items", "center");
    setFlex("help-card-header", "justify_content", "space-between");
    setFlex("help-card-header", "gap", 8);

    createLabel("help-modal-title", "Help", "help-card-header");
    setFontSize("help-modal-title", 14);
    setTextColor("help-modal-title", APP_TEXT);
    setFlex("help-modal-title", "flex_grow", 1);
    setTextOverflow("help-modal-title", "ellipsis");

    createCol("help-modal-close-btn", "help-card-header");
    setFlex("help-modal-close-btn", "width", 60);
    setFlex("help-modal-close-btn", "height", 26);
    setFlex("help-modal-close-btn", "padding_left", 6);
    setFlex("help-modal-close-btn", "padding_right", 6);
    setFlex("help-modal-close-btn", "justify_content", "center");
    setFlex("help-modal-close-btn", "align_items", "center");
    setBackground("help-modal-close-btn", APP_SURFACE);
    setBorder("help-modal-close-btn", APP_BORDER, 1, 8);
    registerClick("help-modal-close-btn");

    createLabel("help-modal-close-label", "Close", "help-modal-close-btn");
    setFontSize("help-modal-close-label", 10);
    setTextColor("help-modal-close-label", APP_TEXT_DIM);
    setPointerEvents("help-modal-close-label", "none");

    createLabel("help-modal-body", "", "help-card");
    setFontSize("help-modal-body", 11);
    setTextColor("help-modal-body", APP_TEXT_DIM);
    setMultiLine("help-modal-body", 1);
    setFlex("help-modal-body", "flex_grow", 1);

    on("help-modal", "click", function() { hideHelpModal(); });
    on("help-modal", "dismiss", function() { hideHelpModal(); });
    on("help-modal-close-btn", "click", function() { hideHelpModal(); });

    createModal("contrast-modal", "");
    setPosition("contrast-modal", "absolute");
    setFlex("contrast-modal", "width", 1100);
    setFlex("contrast-modal", "height", 700);
    setFlex("contrast-modal", "justify_content", "center");
    setFlex("contrast-modal", "align_items", "center");
    setBackground("contrast-modal", "#00000088");
    setZIndex("contrast-modal", 211);
    setOpacity("contrast-modal", 0);
    setVisible("contrast-modal", false);
    setPointerEvents("contrast-modal", "none");
    registerClick("contrast-modal");

    createCol("contrast-card", "contrast-modal");
    setFlex("contrast-card", "width", 360);
    setFlex("contrast-card", "min_height", 214);
    setFlex("contrast-card", "padding", 16);
    setFlex("contrast-card", "gap", 10);
    setBackground("contrast-card", APP_PANEL);
    setBorder("contrast-card", APP_BORDER, 1, 12);
    setBoxShadow("contrast-card", 0, 12, 32, 0, "#00000088");

    createRow("contrast-card-header", "contrast-card");
    setFlex("contrast-card-header", "height", 26);
    setFlex("contrast-card-header", "align_items", "center");
    setFlex("contrast-card-header", "justify_content", "space-between");
    setFlex("contrast-card-header", "gap", 8);

    createLabel("contrast-title", "Contrast", "contrast-card-header");
    setFontSize("contrast-title", 14);
    setTextColor("contrast-title", APP_TEXT);
    setFlex("contrast-title", "flex_grow", 1);
    setTextOverflow("contrast-title", "ellipsis");

    createCol("contrast-close-btn", "contrast-card-header");
    setFlex("contrast-close-btn", "width", 60);
    setFlex("contrast-close-btn", "height", 26);
    setFlex("contrast-close-btn", "padding_left", 6);
    setFlex("contrast-close-btn", "padding_right", 6);
    setFlex("contrast-close-btn", "justify_content", "center");
    setFlex("contrast-close-btn", "align_items", "center");
    setBackground("contrast-close-btn", APP_SURFACE);
    setBorder("contrast-close-btn", APP_BORDER, 1, 8);
    registerClick("contrast-close-btn");

    createLabel("contrast-close-label", "Close", "contrast-close-btn");
    setFontSize("contrast-close-label", 10);
    setTextColor("contrast-close-label", APP_TEXT_DIM);
    setPointerEvents("contrast-close-label", "none");

    createLabel("contrast-hex", "#000000", "contrast-card");
    setFontSize("contrast-hex", 11);
    setTextColor("contrast-hex", APP_TEXT_DIM);

    createRow("contrast-sample-row", "contrast-card");
    setFlex("contrast-sample-row", "height", 92);
    setFlex("contrast-sample-row", "gap", 10);

    createCol("contrast-white-card", "contrast-sample-row");
    setFlex("contrast-white-card", "flex_grow", 1);
    setFlex("contrast-white-card", "height", 92);
    setFlex("contrast-white-card", "padding", 10);
    setFlex("contrast-white-card", "justify_content", "space-between");
    setBackground("contrast-white-card", "#FFFFFF");
    setBorder("contrast-white-card", "#D8D8E4", 1, 10);

    createLabel("contrast-white-aa", "Aa", "contrast-white-card");
    setFontSize("contrast-white-aa", 24);
    createLabel("contrast-white-ratio", "", "contrast-white-card");
    setFontSize("contrast-white-ratio", 10);
    setTextColor("contrast-white-ratio", "#444444");

    createCol("contrast-black-card", "contrast-sample-row");
    setFlex("contrast-black-card", "flex_grow", 1);
    setFlex("contrast-black-card", "height", 92);
    setFlex("contrast-black-card", "padding", 10);
    setFlex("contrast-black-card", "justify_content", "space-between");
    setBackground("contrast-black-card", "#111111");
    setBorder("contrast-black-card", "#1f1f28", 1, 10);

    createLabel("contrast-black-aa", "Aa", "contrast-black-card");
    setFontSize("contrast-black-aa", 24);
    createLabel("contrast-black-ratio", "", "contrast-black-card");
    setFontSize("contrast-black-ratio", 10);
    setTextColor("contrast-black-ratio", "#CFCFE0");

    createLabel("contrast-note", "", "contrast-card");
    setFontSize("contrast-note", 10);
    setTextColor("contrast-note", APP_TEXT_DIM);
    setMultiLine("contrast-note", 1);
    setFlex("contrast-note", "flex_grow", 1);

    on("contrast-modal", "click", function() { hideContrastModal(); });
    on("contrast-modal", "dismiss", function() { hideContrastModal(); });
    on("contrast-close-btn", "click", function() { hideContrastModal(); });
}
buildChatControlsAndToast();

var toastTimer = 0;
function showToast(msg) {
    setText("toast-text", msg);
    setTop("toast-overlay", 650);
    setLeft("toast-overlay", 450);
    setVisible("toast-overlay", true);
    setOpacity("toast-overlay", 1);
    toastTimer = 60; // ~60 frames at 60fps = 1 second
    function fadeToast() {
        toastTimer--;
        if (toastTimer <= 0) {
            setOpacity("toast-overlay", 0);
            setVisible("toast-overlay", false);
            return;
        }
        if (toastTimer < 15) {
            setOpacity("toast-overlay", toastTimer / 15);
        }
        __requestFrame__(fadeToast);
    }
    __requestFrame__(fadeToast);
}
function styleStatusSchema() {
    setFontSize("status-schema", 10);
    setTextColor("status-schema", APP_TEXT_DIM);
}
styleStatusSchema();

// ═══════════════════════════════════════════════════════════════════
// Inspector: Cmd+click detection
// ═══════════════════════════════════════════════════════════════════
// Issue 6: Cmd+click inspector with chat context scoping
var inspectedComponent = null;

function clearInspectedComponent() {
    inspectedComponent = null;
    setText("context-label", "Editing: All");
    setVisible("context-clear", false);
    setText("insp-bounds-v", "—");
    lastDesignDebugState.target = "all";
    lastDesignDebugState.targetBounds = null;
}

function syncDesignDebugTargetBounds() {
    if (!inspectedComponent) {
        setText("insp-bounds-v", "—");
        lastDesignDebugState.targetBounds = null;
        return;
    }
    var rect = getLayoutRect(inspectedComponent);
    if (!rect || rect.width === undefined) {
        setText("insp-bounds-v", "—");
        lastDesignDebugState.targetBounds = null;
        return;
    }
    var boundsText = Math.round(rect.x) + ", " + Math.round(rect.y) + " · " +
        Math.round(rect.width) + "×" + Math.round(rect.height);
    setText("insp-bounds-v", boundsText);
    lastDesignDebugState.targetBounds = {
        x: rect.x,
        y: rect.y,
        width: rect.width,
        height: rect.height
    };
}

function setDesignDebugTarget(widgetId) {
    setText("insp-type-v", widgetId ? "View" : "—");
    setText("insp-id-v", widgetId || "—");
    setText("insp-bounds-v", "—");
    inspectedComponent = widgetId || null;
    if (inspectedComponent) {
        setText("context-label", "Editing: " + inspectedComponent);
        setVisible("context-clear", true);
        lastDesignDebugState.target = inspectedComponent;
        syncDesignDebugTargetBounds();
    } else {
        clearInspectedComponent();
    }
}

function getDesignDebugStateJson() {
    syncDesignDebugTargetBounds();
    return JSON.stringify(lastDesignDebugState);
}

function wireInspectorAndGlobalKeys() {
    enableInspectClick();
    on("__inspect__", "click", function(widgetId) {
        setDesignDebugTarget(widgetId || null);
        if (activeTab === "inspector") {
            switchTab("inspector");
        }
    });

    // Clear context — click x or the badge itself
    on("context-clear", "click", function() {
        clearInspectedComponent();
    });
    on("context-badge", "click", function() {
        if (inspectedComponent) {
            clearInspectedComponent();
        }
    });

    // ═══════════════════════════════════════════════════════════════════
    // Global keyboard shortcuts (Cmd+Z undo, Cmd+Shift+Z redo)
    // ═══════════════════════════════════════════════════════════════════
    on("__global__", "keydown", function(evt) {
        if (!evt) return;
        if ((evt.key === 274 || evt.key === 27) && chatRequestPending) {
            cancelPendingChat("Chat canceled");
            return;
        }
        if ((evt.key === 274 || evt.key === 27) && tokenEditState.activeToken) {
            closeTokenPopup();
            layout();
            return;
        }
        if ((evt.key === 274 || evt.key === 27) && helpModalOpen) {
            hideHelpModal();
            return;
        }
        if ((evt.key === 274 || evt.key === 27) && contrastModalOpen) {
            hideContrastModal();
            return;
        }
        if ((evt.key === 274 || evt.key === 27) && exportPopupOpen) {
            hideExportPopup();
            return;
        }
        var cmd = (evt.mods & 0x18) !== 0;  // kModMeta | kModCmd
        var shift = (evt.mods & 0x01) !== 0;
        // 'z' key = 122 ASCII or platform key code
        if (cmd && evt.key === 122) {
            if (shift) {
                // Redo
                if (historyIndex < themeHistory.length - 1) {
                    historyIndex++;
                    applyTokenDiff(themeHistory[historyIndex]);
                    updateTokenSwatches();
                    setText("status-text", "Redo (" + historyIndex + "/" + (themeHistory.length - 1) + ")");
                    layout();
                }
            } else {
                // Undo
                if (historyIndex > 0) {
                    historyIndex--;
                    applyTokenDiff(themeHistory[historyIndex]);
                    updateTokenSwatches();
                    setText("status-text", "Undo (" + historyIndex + "/" + (themeHistory.length - 1) + ")");
                    layout();
                }
            }
        }
    });
}
wireInspectorAndGlobalKeys();

// ═══════════════════════════════════════════════════════════════════
// Chat logic
// ═══════════════════════════════════════════════════════════════════

// Issue 8: Track cumulative chat height for proper scroll sizing
var chatTotalHeight = 62; // welcome + hint + gap baseline
var chatTypingVisible = false;
var chatTypingPhase = 0;
var chatHistory = [];

function normalizeChatExportPath(path) {
    if (!path || path.length === 0) return "/tmp/pulp-design-chat.md";
    var lower = path.toLowerCase();
    if (lower.slice(-3) === ".md" || lower.slice(-5) === ".json") return path;
    return path + ".md";
}

function serializeChatHistory(format) {
    format = format || "markdown";
    var target = inspectedComponent || "all";
    if (format === "json") {
        return JSON.stringify({
            target: target,
            provider: getSelectedAIProvider(),
            model: getSelectedAIModel(),
            messages: chatHistory
        }, null, 2);
    }

    var out = "# Pulp Style Designer Chat Export\n\n";
    out += "- Target: `" + target + "`\n";
    out += "- Provider: `" + getSelectedAIProvider() + "`\n";
    out += "- Model: `" + getSelectedAIModel() + "`\n\n";
    if (chatHistory.length === 0) {
        out += "_No chat messages yet._\n";
        return out;
    }
    for (var i = 0; i < chatHistory.length; i++) {
        var msg = chatHistory[i];
        out += "## " + (msg.role === "user" ? "User" : "Assistant") + "\n";
        out += msg.text + "\n\n";
    }
    return out;
}

function exportChatHistory() {
    var path = normalizeChatExportPath(showSaveDialog("Export Chat History", "Markdown", "md"));
    writeTextFile(path, serializeChatHistory("markdown"));
    showToast("Chat exported to " + path);
}

function tickChatTypingIndicator() {
    if (!chatTypingVisible) return;
    var dots = ["● ○ ○", "○ ● ○", "○ ○ ●"];
    setText("chat-typing-label", "Designer is thinking  " + dots[chatTypingPhase % dots.length]);
    chatTypingPhase++;
    __requestFrame__(tickChatTypingIndicator);
}

function showChatTypingIndicator() {
    if (chatTypingVisible) return;
    chatTypingVisible = true;
    chatTypingPhase = 0;
    setVisible("chat-typing-row", true);
    setFlex("chat-typing-row", "height", 22);
    tickChatTypingIndicator();
    layout();
}

function hideChatTypingIndicator() {
    chatTypingVisible = false;
    setText("chat-typing-label", "");
    setVisible("chat-typing-row", false);
    setFlex("chat-typing-row", "height", 0);
    layout();
}

function refreshSendButtonPresentation(pending) {
    if (pending === undefined) pending = Boolean(chatRequestPending);
    setPointerEvents("send-btn", "auto");
    setOpacity("send-btn", 1.0);
    setBackground("send-btn", pending ? APP_PANEL_RAISED : APP_ACCENT);
    setBorder("send-btn", pending ? APP_BORDER : APP_ACCENT, 1, 6);
    setVisible("send-icon", !pending);
    setVisible("send-cancel-icon", pending);
}

function clearChatPendingState() {
    chatRequestPending = false;
    chatActiveRequestId = 0;
    hideChatTypingIndicator();
    setChatPendingUi(false);
}

function cancelPendingChat(reason) {
    if (!chatRequestPending) return false;
    clearChatPendingState();
    setText("status-text", reason || "Chat canceled");
    layout();
    return true;
}

function failPendingChat(message, statusText) {
    clearChatPendingState();
    if (message && message.length > 0) addChatMessage("assistant", message);
    setText("status-text", statusText || "Chat error");
    layout();
}

function handleChatRequestTimeout(requestId) {
    if (!chatRequestPending || chatActiveRequestId !== requestId) return false;
    failPendingChat("AI request timed out. Check the CLI/provider configuration and try again.", "Chat timeout");
    return true;
}

function armChatRequestWatchdog(requestId) {
    function tick() {
        if (!chatRequestPending || chatActiveRequestId !== requestId) return;
        if (performance.now() - chatPendingStartedAt >= chatPendingTimeoutMs) {
            handleChatRequestTimeout(requestId);
            return;
        }
        __requestFrame__(tick);
    }
    __requestFrame__(tick);
}

function addChatMessage(role, text) {
    var id = "msg-" + (msgCount++);
    var snapshot = getThemeJson();
    var hasRestore = (role === "assistant");
    chatHistory.push({ role: role, text: text });

    // Issue 8: Better height estimation — wider chars-per-line for 230px width
    var charsPerLine = 25;
    var lineCount = Math.max(1, Math.ceil(text.length / charsPerLine));
    var msgHeight = 16 + lineCount * 16 + (hasRestore ? 24 : 0) + 20;

    createCol(id, "chat-thread");
    setFlex(id, "height", msgHeight);
    setFlex(id, "flex_shrink", 0);  // Issue 8: prevent squishing
    setFlex(id, "padding", 10);
    setFlex(id, "padding_right", 16);  // Issue 8: clear scrollbar
    setFlex(id, "gap", 4);
    setBorder(id, APP_BORDER, 1, 8);
    if (role === "user") {
        setBackground(id, "#2a2a3c");
    } else {
        setBackground(id, APP_PANEL);
    }

    // Role label row
    createRow(id + "-header", id);
    setFlex(id + "-header", "height", 16);
    setFlex(id + "-header", "flex_shrink", 0);
    setFlex(id + "-header", "align_items", "center");
    setFlex(id + "-header", "justify_content", "space-between");

    createLabel(id + "-role", role === "user" ? "You" : "Designer", id + "-header");
    setFontSize(id + "-role", 9);
    setTextColor(id + "-role", APP_TEXT_DIM);

    if (hasRestore) {
        var restoreId = id + "-restore";
        createLabel(restoreId, "Restore", id + "-header");
        setFontSize(restoreId, 9);
        setTextColor(restoreId, APP_ACCENT);
        registerClick(restoreId);
        (function(snap, rid) {
            on(rid, "click", function() {
                applyTokenDiff(snap);
                updateTokenSwatches();
                buildShadeRamps();
                showToast("Restored snapshot");
                layout();
            });
        })(snapshot, restoreId);
    }

    // Issue 1: multi-line label for wrapping
    createLabel(id + "-text", text, id);
    setFontSize(id + "-text", 12);
    setFlex(id + "-text", "flex_grow", 1);
    setMultiLine(id + "-text", 1);

    // Update scroll to fit all messages
    chatTotalHeight += msgHeight + 8;
    setScrollContentSize("chat-messages", 224, chatTotalHeight);
    layout();
}

function wireChatExportButton() {
    on("chat-export-btn", "click", function() {
        exportChatHistory();
    });
}
wireChatExportButton();

var chatRequestPending = false;
var chatRequestCounter = 0;
var chatActiveRequestId = 0;
var chatPendingStartedAt = 0;
var chatPendingTimeoutMs = 25000;
var widgetLookState = {};
var lastChatRequestText = "";
var lastDesignDebugState = {
    target: "all",
    targetBounds: null,
    provider: "claude",
    model: "claude-sonnet-4-6",
    reasoningEffort: "",
    requestText: "",
    responseLength: 0,
    changedColors: [],
    changedDimensions: [],
    widgetLookIds: [],
    widgetLookCount: 0,
    summary: "",
    status: "idle",
    error: "",
    promptLength: 0
};
var widgetKindById = {
    k1: "knob",
    k2: "knob",
    k3: "knob",
    k4: "knob",
    slider1: "fader",
    t1: "toggle",
    t2: "toggle"
};
var aiProviderOptions = [
    { id: "claude", label: "Claude" },
    { id: "codex", label: "Codex" }
];
var aiModelOptions = {
    claude: [
        { id: "claude-sonnet-4-6", label: "Sonnet 4.6" },
        { id: "claude-opus-4-6", label: "Opus 4.6" }
    ],
    codex: [
        { id: "gpt-5.4", label: "GPT-5.4" },
        { id: "gpt-5.4-pro", label: "GPT-5.4 Pro" },
        { id: "gpt-5.3-codex", label: "GPT-5.3 Codex" },
        { id: "gpt-5.2-codex", label: "GPT-5.2 Codex" }
    ]
};
var aiProviderIndex = 0;
var aiModelIndexByProvider = { claude: 0, codex: 0 };
var aiReasoningEffortIndex = 0;
var aiReasoningEffortValues = ["", "low", "medium", "high", "xhigh"];
function clamp01(value) {
    return Math.max(0, Math.min(1, Number(value)));
}

function getSelectedAIProvider() {
    var idx = Math.max(0, Math.min(aiProviderOptions.length - 1, Math.round(aiProviderIndex)));
    return aiProviderOptions[idx].id;
}

function getSelectedAIModel() {
    var provider = getSelectedAIProvider();
    var options = aiModelOptions[provider] || aiModelOptions.claude;
    var idx = Math.max(0, Math.min(options.length - 1, Math.round(aiModelIndexByProvider[provider] || 0)));
    return options[idx].id;
}

function getSelectedAIReasoningEffort() {
    var idx = Math.max(0, Math.min(aiReasoningEffortValues.length - 1, Math.round(aiReasoningEffortIndex)));
    return aiReasoningEffortValues[idx];
}

function refreshAISelectors() {
    var provider = aiProviderOptions[aiProviderIndex].id;
    setValue("provider-selector", aiProviderIndex);
    var modelOptions = aiModelOptions[provider] || aiModelOptions.claude;
    var modelLabels = [];
    for (var i = 0; i < modelOptions.length; i++) modelLabels.push(modelOptions[i].label);
    setItems("model-selector", modelLabels);
    setValue("model-selector", aiModelIndexByProvider[provider] || 0);
    var showEffort = provider === "codex";
    setVisible("effort-selector", showEffort);
    setValue("effort-selector", aiReasoningEffortIndex);
    lastDesignDebugState.provider = provider;
    lastDesignDebugState.model = modelOptions[Math.max(0, aiModelIndexByProvider[provider] || 0)].id;
    lastDesignDebugState.reasoningEffort = showEffort ? aiReasoningEffortValues[aiReasoningEffortIndex] : "";
}

function setDesignDebugAIConfig(providerId, modelId, reasoningEffort) {
    for (var i = 0; i < aiProviderOptions.length; i++) {
        if (aiProviderOptions[i].id === providerId) {
            aiProviderIndex = i;
            break;
        }
    }
    var provider = aiProviderOptions[aiProviderIndex].id;
    var modelOptions = aiModelOptions[provider] || aiModelOptions.claude;
    for (var mi = 0; mi < modelOptions.length; mi++) {
        if (modelOptions[mi].id === modelId) {
            aiModelIndexByProvider[provider] = mi;
            break;
        }
    }
    for (var ei = 0; ei < aiReasoningEffortValues.length; ei++) {
        if (aiReasoningEffortValues[ei] === (reasoningEffort || "")) {
            aiReasoningEffortIndex = ei;
            break;
        }
    }
    refreshAISelectors();
}

function wireAISelectors() {
    on("provider-selector", "select", function(idx) {
        aiProviderIndex = Math.max(0, Math.min(aiProviderOptions.length - 1, Math.round(idx)));
        refreshAISelectors();
        layout();
    });

    on("model-selector", "select", function(idx) {
        var provider = getSelectedAIProvider();
        aiModelIndexByProvider[provider] = Math.max(0, Math.round(idx));
        lastDesignDebugState.model = getSelectedAIModel();
    });

    on("effort-selector", "select", function(idx) {
        aiReasoningEffortIndex = Math.max(0, Math.min(aiReasoningEffortValues.length - 1, Math.round(idx)));
        lastDesignDebugState.reasoningEffort = getSelectedAIReasoningEffort();
    });

    refreshAISelectors();
}
wireAISelectors();

function numericParam(params, key, fallback, minValue, maxValue) {
    var raw = params && params[key] !== undefined ? Number(params[key]) : fallback;
    if (!isFinite(raw)) raw = fallback;
    if (minValue !== undefined) raw = Math.max(minValue, raw);
    if (maxValue !== undefined) raw = Math.min(maxValue, raw);
    return raw;
}

function shaderFloat(value) {
    var rounded = Math.round(Number(value) * 10000) / 10000;
    if (!isFinite(rounded)) rounded = 0;
    return rounded.toFixed(4);
}

function mergeParams(base, overrides) {
    var merged = {};
    var key;
    if (base) {
        for (key in base) merged[key] = base[key];
    }
    if (overrides) {
        for (key in overrides) merged[key] = overrides[key];
    }
    return merged;
}

function mergeWidgetMaterialSpec(spec) {
    if (!spec || !spec.material) return spec;
    var material = spec.material;
    var merged = {};
    for (var key in spec) merged[key] = spec[key];
    delete merged.material;
    merged.params = merged.params || {};

    if (typeof material === "string") {
        merged.preset = merged.preset || material;
        return merged;
    }

    if (material.preset && !merged.preset) merged.preset = material.preset;
    if (material.family && !merged.family) merged.family = material.family;
    if (material.styleFamily && !merged.family) merged.family = material.styleFamily;
    if (material.params) {
        for (var paramKey in material.params) {
            merged.params[paramKey] = material.params[paramKey];
        }
    }
    if (material.body && material.body.bevel && material.body.bevel.width !== undefined) {
        merged.params.bevel = material.body.bevel.width;
    }
    if (material.body && material.body.rim && material.body.rim.opacity !== undefined) {
        merged.params.rim = material.body.rim.opacity;
    }
    if (material.effects && material.effects.highlight && material.effects.highlight.intensity !== undefined) {
        merged.params.gloss = material.effects.highlight.intensity;
    }
    if (material.effects && material.effects.noise && material.effects.noise.intensity !== undefined) {
        merged.params.noise = material.effects.noise.intensity;
    }
    return merged;
}

function buildMacos7KnobShader(params) {
    var gloss = shaderFloat(numericParam(params, "gloss", 0.82, 0.05, 1.0));
    var metalness = shaderFloat(numericParam(params, "metalness", 0.72, 0.0, 1.0));
    var rim = shaderFloat(numericParam(params, "rim", 0.22, 0.0, 1.0));
    var noise = shaderFloat(numericParam(params, "noise", 0.022, 0.0, 0.08));
    var bevel = shaderFloat(numericParam(params, "bevel", 0.90, 0.0, 1.5));
    var bodyRadius = shaderFloat(numericParam(params, "bodyRadius", 0.34, 0.28, 0.40));
    var trackWidth = shaderFloat(numericParam(params, "trackWidth", 0.028, 0.012, 0.060));
    var trackInner = shaderFloat(Number(bodyRadius) + 0.040);
    var trackOuter = shaderFloat(Number(trackInner) + Number(trackWidth));
    return [
        "uniform float2 resolution;",
        "uniform float value;",
        "uniform float time;",
        "layout(color) uniform float4 accentColor;",
        "layout(color) uniform float4 bgColor;",
        "layout(color) uniform float4 trackColor;",
        "layout(color) uniform float4 fillColor;",
        "layout(color) uniform float4 thumbColor;",
        "float hash21(float2 p) { return fract(sin(dot(p, float2(127.1, 311.7))) * 43758.5453123); }",
        "float ringMask(float2 p, float innerR, float outerR, float aa) {",
        "  float d = length(p);",
        "  float outer = 1.0 - smoothstep(outerR - aa, outerR + aa, d);",
        "  float inner = 1.0 - smoothstep(innerR - aa, innerR + aa, d);",
        "  return clamp(outer - inner, 0.0, 1.0);",
        "}",
        "float3 toLinear(float3 c) {",
        "  return pow(clamp(c, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0)), float3(2.2, 2.2, 2.2));",
        "}",
        "float3 toSrgb(float3 c) {",
        "  return pow(max(c, float3(0.0, 0.0, 0.0)), float3(0.454545, 0.454545, 0.454545));",
        "}",
        "half4 main(float2 coord) {",
        "  float size = max(min(resolution.x, resolution.y), 1.0);",
        "  float2 p = (coord - resolution * 0.5) / size;",
        "  float r = length(p);",
        "  float aa = max(1.5 / size, 0.0025);",
        "  float bodyRadius = " + bodyRadius + ";",
        "  float body = 1.0 - smoothstep(bodyRadius - aa, bodyRadius + aa, r);",
        "  float ring = ringMask(p, " + trackInner + ", " + trackOuter + ", aa * 1.2);",
        "  float shadow = (1.0 - smoothstep(bodyRadius + 0.02, bodyRadius + 0.10, length(p - float2(0.0, 0.025)))) * 0.30;",
        "  float normR = clamp(r / max(bodyRadius, 0.0001), 0.0, 1.0);",
        "  float nz = sqrt(max(1.0 - normR * normR, 0.0));",
        "  float2 nxy = p / max(bodyRadius, 0.0001);",
        "  float3 N = normalize(float3(nxy.x, nxy.y, nz));",
        "  float3 L = normalize(float3(-0.42, -0.70, 0.58));",
        "  float3 V = float3(0.0, 0.0, 1.0);",
        "  float3 H = normalize(L + V);",
        "  float gloss = " + gloss + ";",
        "  float diff = max(dot(N, L), 0.0);",
        "  float spec = pow(max(dot(N, H), 0.0), mix(18.0, 140.0, gloss));",
        "  float fres = pow(1.0 - max(dot(N, V), 0.0), 3.0) * " + rim + ";",
        "  float bevelMask = smoothstep(bodyRadius * 0.68, bodyRadius, r) * " + bevel + ";",
        "  float bevelLight = clamp(0.65 - 0.55 * nxy.y - 0.32 * nxy.x, 0.0, 1.0);",
        "  float centerLift = 1.0 - smoothstep(0.0, bodyRadius * 0.96, r);",
        "  float brushed = sin((atan(p.y, p.x) + 3.14159265) * 42.0 + time * 0.05);",
        "  float grain = (hash21(coord + float2(time * 7.0, time * 13.0)) - 0.5) * " + noise + ";",
        "  float metalness = " + metalness + ";",
        "  float3 bgLin = toLinear(bgColor.rgb);",
        "  float3 trackLin = toLinear(trackColor.rgb);",
        "  float3 fillLin = toLinear(fillColor.rgb);",
        "  float3 accentLin = toLinear(accentColor.rgb);",
        "  float3 thumbLin = toLinear(thumbColor.rgb);",
        "  float3 faceBase = mix(bgLin * 0.74 + fillLin * 0.04, float3(0.88, 0.90, 0.95), metalness * 0.78);",
        "  float3 face = faceBase * (0.34 + diff * 0.52 + centerLift * 0.16);",
        "  face += spec * mix(float3(0.45, 0.47, 0.52), float3(1.08, 1.08, 1.04), metalness);",
        "  face += fres * float3(0.22, 0.24, 0.30);",
        "  face = mix(face, face + (bevelLight - 0.5) * 0.36, clamp(bevelMask, 0.0, 1.0));",
        "  face *= 1.0 + grain + brushed * " + noise + " * metalness * 0.65;",
        "  float angle = atan(-p.y, p.x);",
        "  float start = 2.35619449;",
        "  float sweep = 4.71238898;",
        "  if (angle < start) angle += 6.283185307;",
        "  float t = clamp((angle - start) / sweep, 0.0, 1.0);",
        "  float active = step(t, clamp(value, 0.0, 1.0));",
        "  float grooveShade = clamp(0.52 + 0.32 * (-nxy.y * 0.7 - nxy.x * 0.25), 0.0, 1.0);",
        "  float3 grooveColor = mix(trackLin * 0.42, bgLin * 0.28, 0.35) * grooveShade;",
        "  float3 activeColor = mix(fillLin * 0.84, accentLin * 1.06, 0.35) * (0.82 + spec * 0.30);",
        "  float indicatorAngle = start + sweep * clamp(value, 0.0, 1.0);",
        "  float2 dir = float2(cos(indicatorAngle), -sin(indicatorAngle));",
        "  float2 perp = float2(-dir.y, dir.x);",
        "  float2 rel = p - dir * (bodyRadius * 0.54);",
        "  float notchLen = bodyRadius * 0.18;",
        "  float notchHalfWidth = max(aa * 2.0, 0.010);",
        "  float notchDist = max(abs(dot(rel, dir)) - notchLen * 0.5, abs(dot(rel, perp)) - notchHalfWidth);",
        "  float notch = body * (1.0 - smoothstep(0.0, aa * 2.2, notchDist));",
        "  float3 notchColor = mix(float3(0.96, 0.97, 1.00), thumbLin * 1.04, 0.55);",
        "  float alpha = max(shadow, max(body, ring));",
        "  float3 colorLin = bgLin * shadow * 0.35;",
        "  colorLin = mix(colorLin, face, body);",
        "  colorLin = mix(colorLin, grooveColor, ring * 0.94);",
        "  colorLin = mix(colorLin, activeColor, ring * active);",
        "  colorLin = mix(colorLin, notchColor, notch);",
        "  float3 outColor = toSrgb(colorLin);",
        "  return half4(half3(outColor) * half(alpha), half(alpha));",
        "}"
    ].join("\n");
}

function buildGlassFaderShader(params) {
    var gloss = shaderFloat(numericParam(params, "gloss", 0.68, 0.0, 1.0));
    var noise = shaderFloat(numericParam(params, "noise", 0.012, 0.0, 0.05));
    var depth = shaderFloat(numericParam(params, "depth", 0.32, 0.0, 1.0));
    return [
        "uniform float2 resolution;",
        "uniform float value;",
        "uniform float time;",
        "layout(color) uniform float4 trackColor;",
        "layout(color) uniform float4 fillColor;",
        "layout(color) uniform float4 thumbColor;",
        "float hash21(float2 p) { return fract(sin(dot(p, float2(91.7, 173.3))) * 47453.5453); }",
        "float3 toLinear(float3 c) { return pow(clamp(c, float3(0.0,0.0,0.0), float3(1.0,1.0,1.0)), float3(2.2,2.2,2.2)); }",
        "float3 toSrgb(float3 c) { return pow(max(c, float3(0.0,0.0,0.0)), float3(0.454545,0.454545,0.454545)); }",
        "half4 main(float2 coord) {",
        "  float2 uv = coord / resolution;",
        "  float aa = max(1.25 / max(resolution.y, 1.0), 0.003);",
        "  float capsule = 1.0 - smoothstep(0.23 - aa, 0.23 + aa, abs(uv.y - 0.5));",
        "  float fill = step(uv.x, clamp(value, 0.0, 1.0)) * capsule;",
        "  float thumb = 1.0 - smoothstep(0.055, 0.085, length(uv - float2(clamp(value, 0.0, 1.0), 0.5)));",
        "  float highlight = pow(max(1.0 - abs(uv.y - 0.33) * 4.2, 0.0), mix(1.3, 3.2, " + gloss + "));",
        "  float grain = (hash21(coord + float2(time * 5.0, 0.0)) - 0.5) * " + noise + ";",
        "  float3 trackLin = toLinear(trackColor.rgb);",
        "  float3 fillLin = toLinear(fillColor.rgb);",
        "  float3 thumbLin = toLinear(thumbColor.rgb);",
        "  float3 base = trackLin * (0.58 + " + depth + " * 0.22);",
        "  base += highlight * mix(trackLin * 0.15, float3(0.22, 0.24, 0.28), " + gloss + ");",
        "  base *= 1.0 + grain;",
        "  float3 active = mix(fillLin * 0.75, fillLin * 1.08, highlight * 0.75);",
        "  float3 thumbColorLin = mix(thumbLin * 0.82, float3(0.96, 0.97, 1.0), highlight * 0.45);",
        "  float3 colorLin = base;",
        "  colorLin = mix(colorLin, active, fill);",
        "  colorLin = mix(colorLin, thumbColorLin, thumb);",
        "  float alpha = max(capsule, thumb);",
        "  return half4(half3(toSrgb(colorLin)) * half(alpha), half(alpha));",
        "}"
    ].join("\n");
}

function buildCapsuleToggleShader(params) {
    var gloss = shaderFloat(numericParam(params, "gloss", 0.56, 0.0, 1.0));
    var rim = shaderFloat(numericParam(params, "rim", 0.16, 0.0, 1.0));
    var noise = shaderFloat(numericParam(params, "noise", 0.010, 0.0, 0.04));
    return [
        "uniform float2 resolution;",
        "uniform float value;",
        "uniform float time;",
        "layout(color) uniform float4 accentColor;",
        "layout(color) uniform float4 trackColor;",
        "layout(color) uniform float4 thumbColor;",
        "float hash21(float2 p) { return fract(sin(dot(p, float2(53.1, 127.9))) * 15153.5453); }",
        "float3 toLinear(float3 c) { return pow(clamp(c, float3(0.0,0.0,0.0), float3(1.0,1.0,1.0)), float3(2.2,2.2,2.2)); }",
        "float3 toSrgb(float3 c) { return pow(max(c, float3(0.0,0.0,0.0)), float3(0.454545,0.454545,0.454545)); }",
        "half4 main(float2 coord) {",
        "  float2 uv = coord / resolution;",
        "  float2 p = uv - float2(0.5, 0.5);",
        "  float2 q = abs(p) - float2(0.22, 0.18);",
        "  float dist = length(max(q, float2(0.0, 0.0))) + min(max(q.x, q.y), 0.0);",
        "  float aa = max(1.25 / max(min(resolution.x, resolution.y), 1.0), 0.003);",
        "  float capsule = 1.0 - smoothstep(0.0, aa * 2.4, dist);",
        "  float t = clamp(value, 0.0, 1.0);",
        "  float thumbX = mix(0.28, 0.72, t);",
        "  float thumb = 1.0 - smoothstep(0.10, 0.145, length(uv - float2(thumbX, 0.5)));",
        "  float highlight = pow(max(1.0 - abs(uv.y - 0.34) * 4.4, 0.0), mix(1.2, 3.0, " + gloss + "));",
        "  float edge = smoothstep(0.08, 0.24, abs(p.x)) * " + rim + ";",
        "  float grain = (hash21(coord + float2(time * 3.0, time * 9.0)) - 0.5) * " + noise + ";",
        "  float3 trackLin = toLinear(trackColor.rgb);",
        "  float3 accentLin = toLinear(accentColor.rgb);",
        "  float3 thumbLin = toLinear(thumbColor.rgb);",
        "  float3 base = mix(trackLin * 0.72, accentLin * 0.92, t);",
        "  base += highlight * mix(trackLin * 0.12, float3(0.14, 0.15, 0.18), " + gloss + ");",
        "  base += edge * float3(0.10, 0.10, 0.12);",
        "  base *= 1.0 + grain;",
        "  float3 thumbColorLin = mix(thumbLin * 0.82, float3(0.96, 0.97, 0.99), highlight * 0.5);",
        "  float alpha = max(capsule, thumb);",
        "  float3 colorLin = mix(base, thumbColorLin, thumb);",
        "  return half4(half3(toSrgb(colorLin)) * half(alpha), half(alpha));",
        "}"
    ].join("\n");
}

function buildBakeliteKnobShader(params) {
    return buildMacos7KnobShader(mergeParams({
        gloss: 0.28,
        metalness: 0.10,
        rim: 0.10,
        noise: 0.035,
        bevel: 0.62,
        bodyRadius: 0.345,
        trackWidth: 0.024
    }, params || {}));
}

function buildLedRingKnobShader(params) {
    return buildMacos7KnobShader(mergeParams({
        gloss: 0.96,
        metalness: 0.35,
        rim: 0.42,
        noise: 0.010,
        bevel: 0.48,
        bodyRadius: 0.325,
        trackWidth: 0.040
    }, params || {}));
}

function buildAnalogSliderShader(params) {
    return buildGlassFaderShader(mergeParams({
        gloss: 0.24,
        noise: 0.024,
        depth: 0.18
    }, params || {}));
}

function buildIlluminatedToggleShader(params) {
    return buildCapsuleToggleShader(mergeParams({
        gloss: 0.82,
        rim: 0.34,
        noise: 0.008
    }, params || {}));
}

function buildPrecisionKnobShader(params) {
    return buildMacos7KnobShader(mergeParams({
        gloss: 0.58,
        metalness: 0.46,
        rim: 0.07,
        noise: 0.006,
        bevel: 0.44,
        bodyRadius: 0.332,
        trackWidth: 0.022
    }, params || {}));
}

function buildHeritageKnobShader(params) {
    return buildMacos7KnobShader(mergeParams({
        gloss: 0.78,
        metalness: 0.68,
        rim: 0.18,
        noise: 0.020,
        bevel: 1.02,
        bodyRadius: 0.346,
        trackWidth: 0.028
    }, params || {}));
}

function buildRetroKnobShader(params) {
    return buildBakeliteKnobShader(mergeParams({
        gloss: 0.34,
        metalness: 0.08,
        rim: 0.08,
        noise: 0.040,
        bevel: 0.54,
        bodyRadius: 0.350,
        trackWidth: 0.022
    }, params || {}));
}

function buildMasteringKnobShader(params) {
    return buildMacos7KnobShader(mergeParams({
        gloss: 0.46,
        metalness: 0.30,
        rim: 0.05,
        noise: 0.004,
        bevel: 0.30,
        bodyRadius: 0.324,
        trackWidth: 0.020
    }, params || {}));
}

function buildPrecisionFaderShader(params) {
    return buildGlassFaderShader(mergeParams({
        gloss: 0.28,
        noise: 0.004,
        depth: 0.10
    }, params || {}));
}

function buildModularFaderShader(params) {
    return buildGlassFaderShader(mergeParams({
        gloss: 0.88,
        noise: 0.010,
        depth: 0.40
    }, params || {}));
}

function buildConsoleSliderShader(params) {
    return buildAnalogSliderShader(mergeParams({
        gloss: 0.30,
        noise: 0.020,
        depth: 0.24
    }, params || {}));
}

function buildPrecisionToggleShader(params) {
    return buildCapsuleToggleShader(mergeParams({
        gloss: 0.30,
        rim: 0.06,
        noise: 0.004
    }, params || {}));
}

function buildHeritageToggleShader(params) {
    return buildCapsuleToggleShader(mergeParams({
        gloss: 0.56,
        rim: 0.14,
        noise: 0.015
    }, params || {}));
}

var shaderPresetLibrary = {
    macos7_knob: function(widgetId, params) { return buildMacos7KnobShader(params || {}); },
    bakelite_knob: function(widgetId, params) { return buildBakeliteKnobShader(params || {}); },
    led_ring_knob: function(widgetId, params) { return buildLedRingKnobShader(params || {}); },
    precision_knob: function(widgetId, params) { return buildPrecisionKnobShader(params || {}); },
    heritage_knob: function(widgetId, params) { return buildHeritageKnobShader(params || {}); },
    retro_knob: function(widgetId, params) { return buildRetroKnobShader(params || {}); },
    mastering_knob: function(widgetId, params) { return buildMasteringKnobShader(params || {}); },
    glass_fader: function(widgetId, params) { return buildGlassFaderShader(params || {}); },
    analog_slider: function(widgetId, params) { return buildAnalogSliderShader(params || {}); },
    precision_fader: function(widgetId, params) { return buildPrecisionFaderShader(params || {}); },
    modular_fader: function(widgetId, params) { return buildModularFaderShader(params || {}); },
    console_slider: function(widgetId, params) { return buildConsoleSliderShader(params || {}); },
    capsule_toggle: function(widgetId, params) { return buildCapsuleToggleShader(params || {}); },
    illuminated_toggle: function(widgetId, params) { return buildIlluminatedToggleShader(params || {}); },
    precision_toggle: function(widgetId, params) { return buildPrecisionToggleShader(params || {}); },
    heritage_toggle: function(widgetId, params) { return buildHeritageToggleShader(params || {}); }
};

var presetAliasMap = {
    "mac_os_7": "macos7_knob",
    "macos7": "macos7_knob",
    "system7_knob": "macos7_knob",
    "classic_mac_knob": "macos7_knob",
    "vintage_bakelite_knob": "bakelite_knob",
    "bakelite": "bakelite_knob",
    "cyberpunk_knob": "led_ring_knob",
    "neon_knob": "led_ring_knob",
    "led_knob": "led_ring_knob",
    "fabfilter_knob": "precision_knob",
    "precision_eq_knob": "precision_knob",
    "hardware_knob": "heritage_knob",
    "mastering_knob": "mastering_knob",
    "eventide_knob": "retro_knob",
    "soundtoys_knob": "retro_knob",
    "analog_fader": "analog_slider",
    "vintage_slider": "analog_slider",
    "precision_fader": "precision_fader",
    "neon_fader": "modular_fader",
    "console_fader": "console_slider",
    "neon_toggle": "illuminated_toggle",
    "led_toggle": "illuminated_toggle",
    "glow_toggle": "illuminated_toggle",
    "precision_toggle": "precision_toggle",
    "hardware_toggle": "heritage_toggle"
};

var audioPluginStyleFamilies = {
    precision_analyzer: {
        description: "FabFilter-style precision, disciplined chrome, analyzer-centric clarity",
        keywords: ["fabfilter", "pro-q", "precision", "analyzer", "surgical", "clean", "transparent", "metering"],
        presets: { knob: "precision_knob", fader: "precision_fader", toggle: "precision_toggle" }
    },
    heritage_hardware: {
        description: "UA / Softube / Brainworx / Moog hardware with metal, bakelite, and premium studio heft",
        keywords: ["universal audio", "uad", "softube", "brainworx", "moog", "hardware", "studio hardware", "metal", "platinum"],
        presets: { knob: "heritage_knob", fader: "analog_slider", toggle: "heritage_toggle" }
    },
    retro_character: {
        description: "Soundtoys / Eventide character, chunky labels, playful retro personality",
        keywords: ["soundtoys", "eventide", "retro", "character", "playful", "chunky", "vintage fx", "warm vintage"],
        presets: { knob: "retro_knob", fader: "analog_slider", toggle: "capsule_toggle" }
    },
    modular_neon: {
        description: "Arturia Pigments / Massive X neon modular with vivid accents and futuristic gloss",
        keywords: ["arturia", "pigments", "massive x", "native instruments", "cyberpunk", "neon", "modular", "sci-fi", "glow", "led"],
        presets: { knob: "led_ring_knob", fader: "modular_fader", toggle: "illuminated_toggle" }
    },
    mastering_lab: {
        description: "Voxengo / mastering utility aesthetic with restrained surfaces and technical focus",
        keywords: ["voxengo", "mastering", "lab", "utility", "technical", "dense", "analysis", "measurement"],
        presets: { knob: "mastering_knob", fader: "precision_fader", toggle: "precision_toggle" }
    },
    console_strip: {
        description: "Waves / Slate / console-strip workflow with vertical signal-flow pragmatism",
        keywords: ["waves", "slate", "console", "channel strip", "strip", "mixer", "desk", "rack"],
        presets: { knob: "heritage_knob", fader: "console_slider", toggle: "heritage_toggle" }
    }
};

var styleFamilyAliasMap = {
    "precision": "precision_analyzer",
    "fabfilter": "precision_analyzer",
    "clean_precision": "precision_analyzer",
    "heritage": "heritage_hardware",
    "hardware": "heritage_hardware",
    "analog_hardware": "heritage_hardware",
    "retro": "retro_character",
    "character": "retro_character",
    "modular": "modular_neon",
    "cyberpunk": "modular_neon",
    "neon": "modular_neon",
    "mastering": "mastering_lab",
    "lab": "mastering_lab",
    "console": "console_strip",
    "channel_strip": "console_strip"
};

function normalizeStyleFamily(familyId) {
    var raw = String(familyId || "").trim();
    if (!raw) return "";
    if (audioPluginStyleFamilies[raw]) return raw;
    var normalized = raw.toLowerCase().replace(/[\s\-]+/g, "_");
    return styleFamilyAliasMap[normalized] || normalized;
}

function scoreStyleFamily(text, family) {
    var score = 0;
    var keywords = family.keywords || [];
    for (var i = 0; i < keywords.length; i++) {
        if (text.indexOf(keywords[i]) >= 0) score += keywords[i].length > 8 ? 2 : 1;
    }
    return score;
}

function detectStyleFamily(requestText) {
    var text = String(requestText || "").toLowerCase();
    var bestFamily = "";
    var bestScore = 0;
    for (var familyId in audioPluginStyleFamilies) {
        var score = scoreStyleFamily(text, audioPluginStyleFamilies[familyId]);
        if (score > bestScore) {
            bestScore = score;
            bestFamily = familyId;
        }
    }
    return bestFamily;
}

function presetForFamily(familyId, widgetId) {
    var normalizedFamily = normalizeStyleFamily(familyId);
    if (!normalizedFamily || !audioPluginStyleFamilies[normalizedFamily]) return "";
    var kind = widgetKindForId(widgetId);
    var presets = audioPluginStyleFamilies[normalizedFamily].presets || {};
    return presets[kind] || "";
}

function normalizePresetId(presetId) {
    var raw = String(presetId || "").trim();
    if (!raw) return "";
    if (shaderPresetLibrary[raw]) return raw;
    var normalized = raw.toLowerCase().replace(/[\s\-]+/g, "_");
    return presetAliasMap[normalized] || normalized;
}

function buildShaderPreset(presetId, widgetId, params) {
    var normalizedPresetId = normalizePresetId(presetId);
    var preset = shaderPresetLibrary[normalizedPresetId];
    if (!preset) return "";
    if (typeof preset === "function") return preset(widgetId, params || {});
    return preset;
}
var schemaPresetLibrary = {
    notched_knob: JSON.stringify({
        type: "knob",
        elements: [
            { type: "arc", radius: "42%", width: 4, startAngle: -135, sweepAngle: 270, color: "control.track" },
            { type: "arc", radius: "42%", width: 4, startAngle: -135, sweepAngle: { bind: "value", range: [0, 270] }, color: "accent.primary" },
            { type: "circle", radius: "30%", color: "bg.surface" },
            { type: "line", innerRadius: "14%", outerRadius: "34%", width: 3, angle: { bind: "value", range: [-135, 135] }, color: "control.thumb" }
        ]
    }),
    minimal_toggle: JSON.stringify({
        type: "toggle",
        elements: [
            { type: "rect", cornerRadius: "18", color: "control.track" },
            { type: "circle", radius: "18%", color: "control.thumb" }
        ]
    })
};

function widgetKindForId(widgetId) {
    return widgetKindById[widgetId] || "generic";
}

function compactShaderError(error) {
    if (!error) return "Unknown shader error";
    var singleLine = String(error).replace(/\s+/g, " ").trim();
    if (singleLine.length > 220) singleLine = singleLine.substring(0, 217) + "...";
    return singleLine;
}

function shaderCompilerUnavailable(result) {
    if (!result || result.success) return false;
    var error = String(result.error || "").toLowerCase();
    return error.indexOf("skia not available") >= 0 ||
        error.indexOf("shader compilation requires gpu build") >= 0;
}

function applyShaderState(widgetId, shaderSource, state) {
    clearWidgetSchema(widgetId);
    setWidgetShader(widgetId, shaderSource);
    widgetLookState[widgetId] = state;
}

function inferFallbackPreset(widgetId, requestText) {
    var kind = widgetKindForId(widgetId);
    var text = (requestText || "").toLowerCase();
    var familyPreset = presetForFamily(detectStyleFamily(text), widgetId);
    if (familyPreset) return familyPreset;

    if (kind === "knob") {
        if (text.indexOf("mac") >= 0 || text.indexOf("os 7") >= 0 || text.indexOf("classic") >= 0) return "macos7_knob";
        if (text.indexOf("bakelite") >= 0 || text.indexOf("analog") >= 0 || text.indexOf("warm") >= 0) return "bakelite_knob";
        if (text.indexOf("vintage") >= 0) return "retro_knob";
        if (text.indexOf("gloss") >= 0 || text.indexOf("glass") >= 0) return "heritage_knob";
        return "macos7_knob";
    }
    if (kind === "fader") {
        if (text.indexOf("glass") >= 0 || text.indexOf("modern") >= 0) return "glass_fader";
        if (text.indexOf("analog") >= 0 || text.indexOf("vintage") >= 0 || text.indexOf("warm") >= 0) return "analog_slider";
        return "precision_fader";
    }
    if (kind === "toggle") {
        if (text.indexOf("illuminated") >= 0 || text.indexOf("glow") >= 0 || text.indexOf("led") >= 0) return "illuminated_toggle";
        if (text.indexOf("hardware") >= 0 || text.indexOf("console") >= 0) return "heritage_toggle";
        return "capsule_toggle";
    }
    return familyPreset;
}

function buildWidgetShaderFromBody(widgetId, shaderBody) {
    var body = String(shaderBody || "").trim();
    if (!body) return "";
    var kind = widgetKindForId(widgetId);
    if (kind === "knob") {
        return [
            "uniform float2 resolution;",
            "uniform float value;",
            "uniform float time;",
            "layout(color) uniform float4 accentColor;",
            "layout(color) uniform float4 bgColor;",
            "layout(color) uniform float4 trackColor;",
            "layout(color) uniform float4 fillColor;",
            "layout(color) uniform float4 thumbColor;",
            "float ringMask(float2 p, float r0, float r1) {",
            "  float d = length(p);",
            "  float outer = 1.0 - smoothstep(r1 - 0.012, r1 + 0.012, d);",
            "  float inner = 1.0 - smoothstep(r0 - 0.012, r0 + 0.012, d);",
            "  return clamp(outer - inner, 0.0, 1.0);",
            "}",
            "float sdDiamond(float2 p, float s) {",
            "  float2 q = abs(p);",
            "  return (q.x + q.y - s) * 0.7071;",
            "}",
            "half4 main(float2 coord) {",
            "  float2 uv = coord / resolution;",
            "  float2 p = uv - float2(0.5);",
            "  p.y *= resolution.y / max(resolution.x, 1.0);",
            "  float r = length(p);",
            body,
            "}"
        ].join("\n");
    }
    if (kind === "fader") {
        return [
            "uniform float2 resolution;",
            "uniform float value;",
            "uniform float time;",
            "layout(color) uniform float4 accentColor;",
            "layout(color) uniform float4 bgColor;",
            "layout(color) uniform float4 trackColor;",
            "layout(color) uniform float4 fillColor;",
            "layout(color) uniform float4 thumbColor;",
            "half4 main(float2 coord) {",
            "  float2 uv = coord / resolution;",
            "  float2 p = uv - float2(0.5);",
            body,
            "}"
        ].join("\n");
    }
    if (kind === "toggle") {
        return [
            "uniform float2 resolution;",
            "uniform float value;",
            "uniform float time;",
            "layout(color) uniform float4 accentColor;",
            "layout(color) uniform float4 bgColor;",
            "layout(color) uniform float4 trackColor;",
            "layout(color) uniform float4 fillColor;",
            "layout(color) uniform float4 thumbColor;",
            "float sdDiamond(float2 p, float s) {",
            "  float2 q = abs(p);",
            "  return (q.x + q.y - s) * 0.7071;",
            "}",
            "half4 main(float2 coord) {",
            "  float2 uv = coord / resolution;",
            "  float2 p = uv - float2(0.5);",
            body,
            "}"
        ].join("\n");
    }
    return [
        "uniform float2 resolution;",
        "uniform float value;",
        "uniform float time;",
        "layout(color) uniform float4 accentColor;",
        "layout(color) uniform float4 bgColor;",
        "layout(color) uniform float4 trackColor;",
        "layout(color) uniform float4 fillColor;",
        "layout(color) uniform float4 thumbColor;",
        "half4 main(float2 coord) {",
        "  float2 uv = coord / resolution;",
        body,
        "}"
    ].join("\n");
}

function extractShaderBody(shaderText) {
    var shader = String(shaderText || "");
    var match = shader.match(/main\s*\(\s*float2\s+\w+\s*\)\s*\{([\s\S]*)\}\s*$/);
    return match ? match[1].trim() : "";
}

function tryApplyCompiledShader(widgetId, shaderSource, statePreset) {
    if (!shaderSource || shaderSource.length === 0) return false;
    var compiled = compileShader(shaderSource);
    if (compiled && (compiled.success || shaderCompilerUnavailable(compiled))) {
        applyShaderState(widgetId, shaderSource, {
            kind: "shader",
            preset: statePreset || "custom",
            unvalidated: Boolean(compiled && !compiled.success)
        });
        return true;
    }
    return compiled || { success: false, error: "Shader compilation failed" };
}

function applyPresetFallback(widgetId, fallbackPreset, compileError) {
    var normalizedFallback = normalizePresetId(fallbackPreset);
    if (!normalizedFallback || !shaderPresetLibrary[normalizedFallback]) return false;
    var presetShader = buildShaderPreset(normalizedFallback, widgetId, {});
    var compiled = compileShader(presetShader);
    if (!compiled || (!compiled.success && !shaderCompilerUnavailable(compiled))) return false;
    applyShaderState(widgetId, presetShader, {
        kind: "shader",
        preset: normalizedFallback,
        unvalidated: Boolean(compiled && !compiled.success)
    });
    var note = "Custom shader failed for " + widgetId + "; applied " + normalizedFallback + " fallback.";
    if (compileError) note += " " + compactShaderError(compileError);
    addChatMessage("assistant", note);
    return true;
}

function clearWidgetLook(widgetId) {
    clearWidgetShader(widgetId);
    clearWidgetSchema(widgetId);
    delete widgetLookState[widgetId];
}

function applyWidgetLook(widgetId, spec) {
    if (!widgetId || !spec) return false;
    if (spec === "default") {
        clearWidgetLook(widgetId);
        return true;
    }

    if (typeof spec === "string") {
        spec = { preset: spec };
    }

    spec = mergeWidgetMaterialSpec(spec);
    var normalizedPreset = normalizePresetId(spec.preset);
    var normalizedFamily = normalizeStyleFamily(spec.family || spec.styleFamily);
    var familyPreset = presetForFamily(normalizedFamily, widgetId);

    if (spec.reset) {
        clearWidgetLook(widgetId);
        return true;
    }

    if (spec.schemaPreset && schemaPresetLibrary[spec.schemaPreset]) {
        clearWidgetShader(widgetId);
        setWidgetSchema(widgetId, schemaPresetLibrary[spec.schemaPreset]);
        widgetLookState[widgetId] = { kind: "schema", preset: spec.schemaPreset };
        return true;
    }

    var fallbackPreset = spec.fallbackPreset || inferFallbackPreset(widgetId, lastChatRequestText);
    if (!normalizedPreset && familyPreset) normalizedPreset = familyPreset;

    if (normalizedPreset && shaderPresetLibrary[normalizedPreset]) {
        var presetShader = buildShaderPreset(normalizedPreset, widgetId, spec.params || {});
        var compiledPreset = compileShader(presetShader);
        if (compiledPreset && (compiledPreset.success || shaderCompilerUnavailable(compiledPreset))) {
            applyShaderState(widgetId, presetShader, {
                kind: "shader",
                preset: normalizedPreset,
                family: normalizedFamily,
                params: spec.params || {},
                unvalidated: Boolean(!compiledPreset.success)
            });
            return true;
        }
        return false;
    }

    if (spec.shaderBody && spec.shaderBody.length > 0) {
        var wrappedShader = buildWidgetShaderFromBody(widgetId, spec.shaderBody);
        var wrappedResult = tryApplyCompiledShader(widgetId, wrappedShader, "custom-body");
        if (wrappedResult === true) return true;
        if (applyPresetFallback(widgetId, fallbackPreset, wrappedResult.error)) return true;
        addChatMessage("assistant", "Shader compile failed for " + widgetId + ": " + compactShaderError(wrappedResult.error));
        return false;
    }

    if (spec.shader && spec.shader.length > 0) {
        var compiled = tryApplyCompiledShader(widgetId, spec.shader, "custom");
        if (compiled === true) return true;

        var extractedBody = extractShaderBody(spec.shader);
        if (extractedBody && extractedBody.length > 0) {
            var recoveredShader = buildWidgetShaderFromBody(widgetId, extractedBody);
            var recoveredResult = tryApplyCompiledShader(widgetId, recoveredShader, "custom-body");
            if (recoveredResult === true) return true;
            compiled = recoveredResult;
        }

        if (applyPresetFallback(widgetId, fallbackPreset, compiled.error)) return true;
        if (compiled && compiled.error) addChatMessage("assistant", "Shader compile failed for " + widgetId + ": " + compactShaderError(compiled.error));
        return false;
    }

    if (spec.schema) {
        clearWidgetShader(widgetId);
        setWidgetSchema(widgetId, JSON.stringify(spec.schema));
        widgetLookState[widgetId] = { kind: "schema", preset: "custom" };
        return true;
    }

    return false;
}

function applyDesignChatResponse(response) {
    clearChatPendingState();
    lastDesignDebugState.requestText = lastChatRequestText || "";
    lastDesignDebugState.target = inspectedComponent || "all";
    lastDesignDebugState.responseLength = response ? response.length : 0;
    lastDesignDebugState.changedColors = [];
    lastDesignDebugState.changedDimensions = [];
    lastDesignDebugState.widgetLookIds = [];
    lastDesignDebugState.widgetLookCount = 0;
    lastDesignDebugState.summary = "";
    lastDesignDebugState.status = "error";
    lastDesignDebugState.error = "";

    if (!response || response.length === 0) {
        addChatMessage("assistant", "No response from Claude");
        setText("status-text", "Error");
        layout();
        lastDesignDebugState.error = "No response from Claude";
        return "No response from Claude";
    }

    var jsonStart = response.indexOf("{");
    var jsonEnd = response.lastIndexOf("}");
    if (jsonStart < 0 || jsonEnd < 0) {
        addChatMessage("assistant", "No JSON in response");
        setText("status-text", "Error");
        layout();
        lastDesignDebugState.error = "No JSON in response";
        return "No JSON in response";
    }

    var jsonDiff = response.substring(jsonStart, jsonEnd + 1);
    var diffObj = {};
    try { diffObj = JSON.parse(jsonDiff); } catch(e) {
        addChatMessage("assistant", "Invalid JSON in response");
        setText("status-text", "Error");
        layout();
        lastDesignDebugState.error = "Invalid JSON in response";
        return "Invalid JSON in response";
    }

    if (diffObj.colors) {
        applyTokenDiff(JSON.stringify({ colors: diffObj.colors }));
    }
    pushThemeSnapshot();

    var dimChanges = diffObj.dimensions || {};
    var dimCount = 0;
    var dimNames = [];
    for (var dk in dimChanges) {
        dimCount++;
        dimNames.push(dk);
        var dv = dimChanges[dk];
        if (dk === "cornerRadius") {
            setBorder("btn-normal", APP_BORDER, 1, dv);
            setBorder("btn-hover", APP_BORDER, 1, dv);
            setBorder("btn-action", APP_ACCENT, 0, dv);
            setBorder("btn-disabled", APP_BORDER, 1, dv);
        } else if (dk === "headingSize") {
            setFontSize("heading-text", dv);
        } else if (dk === "bodySize") {
            setFontSize("body-text", dv);
        } else if (dk === "labelSize") {
            setFontSize("caption-text", dv);
        }
    }

    var widgetLookCount = 0;
    var widgetLookIds = [];
    if (diffObj.widgetLooks) {
        for (var widgetId in diffObj.widgetLooks) {
            if (applyWidgetLook(widgetId, diffObj.widgetLooks[widgetId])) {
                widgetLookCount++;
                widgetLookIds.push(widgetId);
            }
        }
    }

    if (diffObj.widgetShader && inspectedComponent) {
        if (applyWidgetLook(inspectedComponent, { shader: diffObj.widgetShader })) {
            widgetLookCount++;
            widgetLookIds.push(inspectedComponent);
        }
    }

    if (diffObj.widgetSchema && inspectedComponent) {
        if (applyWidgetLook(inspectedComponent, { schema: diffObj.widgetSchema })) {
            widgetLookCount++;
            widgetLookIds.push(inspectedComponent);
        }
    }

    var diffColors = diffObj.colors || {};
    var changedNames = [];
    for (var ck in diffColors) changedNames.push(ck);
    var summary = "Applied " + changedNames.length + " colors";
    if (dimCount > 0) summary += " + " + dimCount + " styles";
    if (widgetLookCount > 0) summary += " + " + widgetLookCount + " widget looks";
    if (changedNames.length > 0 && changedNames.length <= 6) {
        summary += ": " + changedNames.join(", ");
    }
    addChatMessage("assistant", summary);
    updateTokenSwatches();
    updateModifiedCount();
    setText("status-text", (changedNames.length + dimCount + widgetLookCount) + " changes by AI");
    lastDesignDebugState.changedColors = changedNames;
    lastDesignDebugState.changedDimensions = dimNames;
    lastDesignDebugState.widgetLookIds = widgetLookIds;
    lastDesignDebugState.widgetLookCount = widgetLookCount;
    lastDesignDebugState.summary = summary;
    lastDesignDebugState.status = "ok";
    lastDesignDebugState.error = "";
    layout();
    return summary;
}

function buildDesignChatPrompt(text) {
    var themeJson = getThemeJson();
    var scope = inspectedComponent ? "\nScope: ONLY modify tokens related to '" + inspectedComponent + "'" : "";
    var provider = getSelectedAIProvider();
    var model = getSelectedAIModel();
    var reasoningEffort = getSelectedAIReasoningEffort();
    var prompt = "You are a design system expert for audio plugin UIs.\n";
    prompt += "Modify the theme to achieve the requested look. Be creative and bold.\n";
    prompt += "You can change colors, style properties, and widget looks using built-in rendering presets plus material parameters.\n\n";
    prompt += "## Current Theme\n" + themeJson + "\n\n";
    prompt += "## Available Color Tokens (hex values)\n";
    for (var gi = 0; gi < tokenGroups.length; gi++) {
        prompt += tokenGroups[gi].name + ": " + tokenGroups[gi].tokens.join(", ") + "\n";
    }
    prompt += "\n## Available Style Properties (numeric values)\n";
    prompt += "dimensions.cornerRadius (0-24): border radius for panels/buttons\n";
    prompt += "dimensions.borderWidth (0-4): border thickness\n";
    prompt += "dimensions.shadowBlur (0-20): shadow blur radius\n";
    prompt += "dimensions.shadowAlpha (0-1): shadow opacity\n";
    prompt += "dimensions.knobArcWidth (2-8): knob arc stroke width\n";
    prompt += "dimensions.knobSize (32-80): knob diameter\n";
    prompt += "dimensions.headingSize (12-36): heading font size\n";
    prompt += "dimensions.bodySize (10-16): body font size\n";
    prompt += "dimensions.labelSize (8-12): label font size\n";
    prompt += "\n## Widget Targets For Material Restyling\n";
    prompt += "k1, k2, k3, k4 = knobs\n";
    prompt += "slider1 = fader\n";
    prompt += "t1, t2 = toggles\n";
    if (inspectedComponent) {
        prompt += "Current target = " + inspectedComponent + "\n";
    }
    prompt += "\n## Audio Plugin Style Families\n";
    prompt += "precision_analyzer = FabFilter-style precision, restrained chrome, analyzer-forward clarity\n";
    prompt += "heritage_hardware = UA / Softube / Brainworx / Moog metal-and-bakelite studio hardware\n";
    prompt += "retro_character = Soundtoys / Eventide chunky retro character and playful personality\n";
    prompt += "modular_neon = Arturia Pigments / Massive X vivid modular neon and futuristic gloss\n";
    prompt += "mastering_lab = Voxengo-style technical mastering utility with restrained surfaces\n";
    prompt += "console_strip = Waves / Slate console-strip pragmatism and channel-strip grouping\n";
    prompt += "\n## Built-in Material Look Presets\n";
    prompt += "precision_knob, heritage_knob, retro_knob, mastering_knob, macos7_knob, bakelite_knob, led_ring_knob\n";
    prompt += "precision_fader, glass_fader, analog_slider, modular_fader, console_slider\n";
    prompt += "precision_toggle, heritage_toggle, capsule_toggle, illuminated_toggle\n";
    prompt += "notched_knob = declarative schema knob with bolder indicator\n";
    prompt += "minimal_toggle = declarative schema toggle simplification\n";
    prompt += "default = clear any custom shader/schema for a widget\n";
    prompt += "Prefer family + preset for brand/style requests: FabFilter -> precision_analyzer, Arturia/NI cyberpunk -> modular_neon, Soundtoys/Eventide -> retro_character, UA/Softube/Moog/Brainworx -> heritage_hardware, Waves/Slate -> console_strip, Voxengo/mastering -> mastering_lab.\n";
    prompt += "\n## Widget Look Contract\n";
    prompt += "Use widgetLooks when the request asks for a material or physical look change, not just color token changes.\n";
    prompt += "Prefer preset + params. The renderer is deterministic and higher quality than free-form shader code.\n";
    prompt += "You may use family to express the high-level audio-plugin aesthetic, and the runtime will map that deterministically per widget kind.\n";
    prompt += "Available params for preset/material overrides: gloss (0-1), metalness (0-1), rim (0-1), noise (0-0.08), bevel (0-1.5), trackWidth (0.012-0.06), bodyRadius (0.28-0.40), depth (0-1).\n";
    prompt += "Use schemaPreset only when a simple declarative look is enough.\n";
    prompt += "Make the change materially different: body treatment, gloss, bevel, depth, rim, notch, track, or thumb treatment.\n";
    prompt += "Do NOT output raw SkSL shader code unless the user explicitly asks for shader source.\n";
    prompt += "\nUse widgetLooks ONLY when the request asks for a material change in shape, gloss, body treatment, track treatment, or other non-color styling.\n";
    prompt += "\n## RULES\n";
    prompt += "1. Output ONLY valid JSON. No markdown, no explanation.\n";
    prompt += '2. Format: {"colors": {"token.name": "#hex", ...}, "dimensions": {"prop": number, ...}, "widgetLooks": {"widgetId": {"family": "precision_analyzer"|"heritage_hardware"|"retro_character"|"modular_neon"|"mastering_lab"|"console_strip", "preset": "precision_knob"|"heritage_knob"|"retro_knob"|"mastering_knob"|"macos7_knob"|"bakelite_knob"|"led_ring_knob"|"precision_fader"|"glass_fader"|"analog_slider"|"modular_fader"|"console_slider"|"precision_toggle"|"heritage_toggle"|"capsule_toggle"|"illuminated_toggle"|"default", "params": {"gloss": 0.8, "metalness": 0.7, "rim": 0.2, "noise": 0.02, "bevel": 0.9}, "material": {"family": "heritage_hardware", "preset": "heritage_knob", "params": {...}}, "schemaPreset": "notched_knob"|"minimal_toggle"}}}\n';
    prompt += "3. Change 5-30 tokens. Include BOTH colors and dimensions for dramatic effect.\n";
    prompt += "4. Use the exact token/property names listed above.\n";
    prompt += "5. Be bold — if asked for 'cyberpunk', make it look like cyberpunk. If 'warm analog', make it warm.\n";
    prompt += "6. When a specific control is targeted and the request is about its physical look, include widgetLooks for that control.\n";
    prompt += "7. Prefer family + preset first, then material params, then schemaPreset.\n";
    prompt += "8. Never return empty widgetLooks for a targeted control restyle request.\n";
    prompt += "9. Never emit shaderBody or shader unless explicitly asked for shader source by the user.\n";
    prompt += scope + "\n\n";
    if (uploadedImagePath && uploadedImagePath.length > 0) {
        prompt += "## Reference Image\nThe user uploaded a reference image at: " + uploadedImagePath + "\n";
        prompt += "Extract the visual mood, colors, and style from this image and apply them.\n\n";
    }
    prompt += '## Request\n"' + text + '"\n\n## JSON Output\n';
    lastChatRequestText = text || "";
    lastDesignDebugState.provider = provider;
    lastDesignDebugState.model = model;
    lastDesignDebugState.reasoningEffort = reasoningEffort;
    lastDesignDebugState.requestText = text || "";
    lastDesignDebugState.target = inspectedComponent || "all";
    lastDesignDebugState.promptLength = prompt.length;
    return prompt;
}

function submitChat(text) {
    if (chatRequestPending) return;
    if (!text || text.length === 0) {
        if (uploadedImagePath && uploadedImagePath.length > 0) {
            showToast("Add a message before sending the image");
        }
        return;
    }

    var userText = uploadedImageName ? (text + " [image attached: " + uploadedImageName + "]") : text;
    lastChatRequestText = text;
    addChatMessage("user", userText);
    setText("chat-input", "");
    setText("status-text", "Generating...");
    chatRequestPending = true;
    setChatPendingUi(true);
    showChatTypingIndicator();
    layout();

    var provider = getSelectedAIProvider();
    var model = getSelectedAIModel();
    var reasoningEffort = getSelectedAIReasoningEffort();
    var prompt = buildDesignChatPrompt(text);

    var requestId = chatRequestCounter++;
    var tmpFile = "/tmp/pulp-design-prompt-" + requestId + ".txt";
    exec("cat > " + tmpFile + " << 'PULPEOF'\n" + prompt + "\nPULPEOF");
    chatActiveRequestId = requestId;
    chatPendingStartedAt = performance.now();
    var callbackId = "__design-chat__-" + requestId;
    on(callbackId, "result", function(response) {
        handleDesignChatCommandResult(requestId, provider, response);
    });
    execAsync(buildAiCliCommand(tmpFile, model, provider, reasoningEffort), callbackId);
    armChatRequestWatchdog(requestId);
    clearUploadedImage();
    updateChatInputSizing("");
}

function wireChatInputAndPresetSelector() {
    on("chat-input", "return", function(text) {
        submitChat(text);
    });

    // Send button triggers same as return key
    registerClick("send-btn");
    on("send-btn", "click", function() {
        if (chatRequestPending) {
            cancelPendingChat("Chat canceled");
            return;
        }
        var text = getText("chat-input");
        submitChat(text);
    });

    setChatPendingUi(false);

    // Preset handler
    on("preset-selector", "select", function(idx) {
        var presets = [
            { title: "Default Dark", theme: "dark", accent: "#89B4FA", harmony: "monochromatic", presetIndex: 0 },
            { title: "Light", theme: "light", accent: "#2563EB", harmony: "complementary", presetIndex: 1 },
            { title: "Pro Audio", theme: "pro_audio", accent: "#89B4FA", harmony: "monochromatic", presetIndex: 2 },
            { title: "Violet", theme: "dark", accent: "#AA88FF", harmony: "monochromatic", presetIndex: 3 },
            { title: "Amber", theme: "dark", accent: "#D4A017", harmony: "monochromatic", presetIndex: 4 },
            { title: "Ocean", theme: "dark", accent: "#0EA5E9", harmony: "analogous", presetIndex: 5 },
            { title: "Neon", theme: "dark", accent: "#FF00FF", harmony: "complementary", presetIndex: 6 }
        ];
        applyPaletteConfiguration(presets[idx] || presets[0]);
    });
}
wireChatInputAndPresetSelector();

// ═══════════════════════════════════════════════════════════════════
// Export/Import buttons
// ═══════════════════════════════════════════════════════════════════
// D4: Multi-format export
// #57: expanded export formats including W3C tokens and style preset payloads
var exportFormats = ["JSON", "CSS Vars", "OKLCH", "C++ Header", "C++ Palette", "W3C Tokens", "Style Preset"];
var activeExportFormat = 0;
var exportPopupOpen = false;

function getExportFileExtension(formatIdx) {
    var extensions = [".json", ".css", ".css", ".hpp", ".cpp", ".json", ".json"];
    return extensions[formatIdx] || ".txt";
}

function hideExportPopup() {
    exportPopupOpen = false;
    setVisible("export-popup", false);
    setVisible("export-backdrop", false);
    setPointerEvents("export-backdrop", "none");
    layout();
}

function positionExportPopup() {
    var size = getRootSize();
    var viewportW = size && size.width ? size.width : 1100;
    var viewportH = size && size.height ? size.height : 700;
    var popupW = 480;
    var popupH = 400;
    var margin = 16;
    var left = Math.floor((viewportW - popupW) * 0.5);
    var top = Math.floor((viewportH - popupH) * 0.5);
    if (left < margin) left = margin;
    if (top < 44) top = 44;
    if (left + popupW > viewportW - margin) left = Math.max(margin, viewportW - popupW - margin);
    if (top + popupH > viewportH - margin) top = Math.max(44, viewportH - popupH - margin);
    setLeft("export-popup", left);
    setTop("export-popup", top);
}

function captureStylePresetPayload() {
    var paletteConfig = {};
    try {
        paletteConfig = JSON.parse(serializePaletteConfiguration());
    } catch (e) {
        paletteConfig = {};
    }
    return {
        version: 1,
        target: inspectedComponent || "all",
        palette: paletteConfig,
        theme: JSON.parse(getThemeJson()),
        widgetLooks: JSON.parse(JSON.stringify(widgetLookState || {})),
        debug: JSON.parse(JSON.stringify(lastDesignDebugState || {}))
    };
}

function generateExport(formatIdx) {
    var json = getThemeJson();
    var theme = JSON.parse(json);
    var colors = theme.colors || {};
    if (formatIdx === 0) return json;
    if (formatIdx === 1) {
        var css = ":root {\n";
        for (var k in colors) css += "  --pulp-" + k.replace(/\./g, "-") + ": " + colors[k] + ";\n";
        css += "}\n";
        return css;
    }
    if (formatIdx === 2) {
        var oklch = "/* OKLCH Color Tokens */\n:root {\n";
        for (var k in colors) {
            var o = OklchEngine.hexToOklch(colors[k]);
            oklch += "  --pulp-" + k.replace(/\./g, "-") + ": oklch(" + (o.L*100).toFixed(1) + "% " + o.C.toFixed(3) + " " + o.H.toFixed(1) + ");\n";
        }
        oklch += "}\n";
        return oklch;
    }
    if (formatIdx === 3) {
        // C++ Header — PULP_THEME_COLOR macros
        var cpp = "#pragma once\n// Generated by Pulp Style Designer\n\n";
        cpp += "namespace pulp_theme {\n\n";
        for (var k in colors) {
            var hex = colors[k].replace("#", "");
            var name = k.replace(/\./g, "_");
            cpp += "PULP_THEME_COLOR(" + name + ", 0xFF" + hex.toUpperCase() + ")\n";
        }
        cpp += "\n} // namespace pulp_theme\n";
        return cpp;
    }
    if (formatIdx === 4) {
        // C++ Palette — palette.setColor() calls
        var pal = "// Generated by Pulp Style Designer\n";
        pal += "inline void initThemePalette(pulp::Palette& palette) {\n";
        for (var k in colors) {
            var hex = colors[k].replace("#", "");
            var name = k.replace(/\./g, "_");
            pal += "    palette.setColor(pulp::" + name + ", pulp::Color(0xFF" + hex.toUpperCase() + "));\n";
        }
        pal += "}\n";
        return pal;
    }
    if (formatIdx === 5) {
        return exportDesignTokens();
    }
    if (formatIdx === 6) {
        return JSON.stringify(captureStylePresetPayload(), null, 2) + "\n";
    }
    return json;
}

function showExportPopup() {
    var size = getRootSize();
    setFlex("export-backdrop", "width", size.width);
    setFlex("export-backdrop", "height", size.height);
    setTop("export-backdrop", 0);
    setLeft("export-backdrop", 0);
    positionExportPopup();
    exportPopupOpen = true;
    setVisible("export-backdrop", true);
    setPointerEvents("export-backdrop", "auto");
    setVisible("export-popup", true);
    layout();
}

function buildExportPopup() {
    createCol("export-backdrop", "");
    setPosition("export-backdrop", "absolute");
    setFlex("export-backdrop", "width", 1100);
    setFlex("export-backdrop", "height", 700);
    setTop("export-backdrop", 0);
    setLeft("export-backdrop", 0);
    setBackground("export-backdrop", "#00000088");
    setZIndex("export-backdrop", 149);
    setVisible("export-backdrop", false);
    setPointerEvents("export-backdrop", "none");
    registerClick("export-backdrop");
    on("export-backdrop", "click", function() { hideExportPopup(); });

    // Export popup overlay
    createCol("export-popup", "");
    setPosition("export-popup", "absolute");
    setFlex("export-popup", "width", 480);
    setFlex("export-popup", "height", 400);
    setFlex("export-popup", "padding", 16);
    setFlex("export-popup", "gap", 10);
    setBackground("export-popup", APP_PANEL);
    setBorder("export-popup", APP_BORDER, 1, 10);
    setBoxShadow("export-popup", 0, 16, 48, 0, "#000000c0");
    setZIndex("export-popup", 150);
    setVisible("export-popup", false);

    // Export header
    createRow("exp-header", "export-popup");
    setFlex("exp-header", "height", 24);
    setFlex("exp-header", "align_items", "center");
    createLabel("exp-title", "Export Theme", "exp-header");
    setFontSize("exp-title", 14);
    setFlex("exp-title", "flex_grow", 1);

    createCol("exp-close", "exp-header");
    setFlex("exp-close", "width", 22);
    setFlex("exp-close", "height", 22);
    setFlex("exp-close", "justify_content", "center");
    setFlex("exp-close", "align_items", "center");
    createLabel("exp-close-lbl", "x", "exp-close");
    setFontSize("exp-close-lbl", 12);
    registerClick("exp-close");
    on("exp-close", "click", function() { hideExportPopup(); });

    // Format tabs
    createRow("exp-tabs", "export-popup");
    setFlex("exp-tabs", "height", 26);
    setFlex("exp-tabs", "gap", 2);
    for (var ef = 0; ef < exportFormats.length; ef++) {
        var efId = "exp-tab-" + ef;
        createCol(efId, "exp-tabs");
        setFlex(efId, "flex_grow", 1);
        setFlex(efId, "height", 26);
        setFlex(efId, "justify_content", "center");
        setFlex(efId, "align_items", "center");
        setBorder(efId, ef === 0 ? APP_ACCENT : APP_BORDER, 1, 4);
        createLabel(efId + "-lbl", exportFormats[ef], efId);
        setFontSize(efId + "-lbl", 10);
        setTextColor(efId + "-lbl", ef === 0 ? APP_ACCENT : APP_TEXT_DIM);
        registerClick(efId);
        (function(idx) {
            on("exp-tab-" + idx, "click", function() {
                activeExportFormat = idx;
                for (var i = 0; i < exportFormats.length; i++) {
                    setBorder("exp-tab-" + i, i === idx ? APP_ACCENT : APP_BORDER, 1, 4);
                    setTextColor("exp-tab-" + i + "-lbl", i === idx ? APP_ACCENT : APP_TEXT_DIM);
                }
                setText("exp-code", generateExport(idx));
            });
        })(ef);
    }

    // Code preview (scrollable text)
    createScrollView("exp-code-scroll", "export-popup");
    setFlex("exp-code-scroll", "flex_grow", 1);
    setBackground("exp-code-scroll", APP_BG);
    setBorder("exp-code-scroll", APP_BORDER, 1, 4);
    setScrollContentSize("exp-code-scroll", 440, 1200);

    createLabel("exp-code", "", "exp-code-scroll");
    setFontSize("exp-code", 10);
    setFlex("exp-code", "padding", 8);
    setFlex("exp-code", "width", 440);

    // Action buttons
    createRow("exp-actions", "export-popup");
    setFlex("exp-actions", "height", 28);
    setFlex("exp-actions", "gap", 8);
    setFlex("exp-actions", "justify_content", "flex-end");

    createCol("exp-copy-btn", "exp-actions");
    setFlex("exp-copy-btn", "width", 80);
    setFlex("exp-copy-btn", "height", 28);
    setFlex("exp-copy-btn", "justify_content", "center");
    setFlex("exp-copy-btn", "align_items", "center");
    setBorder("exp-copy-btn", APP_BORDER, 1, 4);
    createLabel("exp-copy-lbl", "Copy", "exp-copy-btn");
    setFontSize("exp-copy-lbl", 10);
    registerClick("exp-copy-btn");
    on("exp-copy-btn", "click", function() {
        var code = generateExport(activeExportFormat);
        writeClipboard(code);
        showToast("Copied to clipboard");
    });

    createCol("exp-save-btn", "exp-actions");
    setFlex("exp-save-btn", "width", 80);
    setFlex("exp-save-btn", "height", 28);
    setFlex("exp-save-btn", "justify_content", "center");
    setFlex("exp-save-btn", "align_items", "center");
    setBackground("exp-save-btn", APP_ACCENT);
    setBorder("exp-save-btn", APP_ACCENT, 0, 4);
    createLabel("exp-save-lbl", "Save", "exp-save-btn");
    setFontSize("exp-save-lbl", 10);
    registerClick("exp-save-btn");
    on("exp-save-btn", "click", function() {
        var code = generateExport(activeExportFormat);
        var ext = getExportFileExtension(activeExportFormat);
        var path = "/tmp/pulp-theme" + ext;
        exec("cat > " + path + " << 'PULPEOF'\n" + code + "\nPULPEOF");
        showToast("Saved to " + path);
    });

    registerClick("export-btn-pill");
    on("export-btn-pill", "click", function() {
        setText("exp-code", generateExport(activeExportFormat));
        showExportPopup();
    });

    registerClick("import-btn-pill");
    on("import-btn-pill", "click", function() {
        var path = "/tmp/pulp-theme-export.json";
        var json = exec("cat " + path + " 2>/dev/null");
        if (json && json.length > 10) {
            applyTokenDiff(json);
            updateTokenSwatches();
            setText("status-text", "Imported from " + path);
        } else {
            setText("status-text", "No theme file found");
        }
        layout();
    });
}
