buildExportPopup();

// Undo/Redo (snapshot-based)
var themeHistory = [];
var historyIndex = -1;

function pushThemeSnapshot() {
    var snap = getThemeJson();
    if (historyIndex < themeHistory.length - 1) {
        themeHistory = themeHistory.slice(0, historyIndex + 1);
    }
    themeHistory.push(snap);
    historyIndex = themeHistory.length - 1;
}
function buildUndoRedoWiring() {
    pushThemeSnapshot();

    registerClick("undo-btn-pill");
    on("undo-btn-pill", "click", function() {
        if (historyIndex > 0) {
            historyIndex--;
            applyTokenDiff(themeHistory[historyIndex]);
            updateTokenSwatches();
            setText("status-text", "Undo (" + historyIndex + "/" + (themeHistory.length - 1) + ")");
            layout();
        }
    });

    registerClick("redo-btn-pill");
    on("redo-btn-pill", "click", function() {
        if (historyIndex < themeHistory.length - 1) {
            historyIndex++;
            applyTokenDiff(themeHistory[historyIndex]);
            updateTokenSwatches();
            setText("status-text", "Redo (" + historyIndex + "/" + (themeHistory.length - 1) + ")");
            layout();
        }
    });

    layout();
}
buildUndoRedoWiring();
