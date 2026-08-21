// web-compat-transient-interaction.js - live gesture state with one committed publication.
//
// Pointer-driven editors often need to publish lightweight visual/native state
// for every delivered sample while deferring an expensive framework snapshot
// until the gesture ends.  A session object makes ownership explicit: starting
// a newer gesture invalidates callbacks retained by the older gesture.

(function() {
    if (typeof window === "undefined") return;
    window.pulp = window.pulp || {};

    window.pulp.createTransientInteraction = function(options) {
        options = options || {};
        var onUpdate = typeof options.onUpdate === "function" ? options.onUpdate : function() {};
        var onCommit = typeof options.onCommit === "function" ? options.onCommit : function() {};
        var onCancel = typeof options.onCancel === "function" ? options.onCancel : function() {};
        var generation = 0;
        var activeGeneration = 0;
        var activeCancel = null;

        function begin(initialValue) {
            var sessionGeneration = ++generation;
            activeGeneration = sessionGeneration;
            var currentValue = initialValue;
            var closed = false;

            function ownsInteraction() {
                return !closed && activeGeneration === sessionGeneration;
            }

            function cancelSession() {
                if (!ownsInteraction()) return false;
                closed = true;
                activeGeneration = 0;
                activeCancel = null;
                onCancel(currentValue);
                return true;
            }

            activeCancel = cancelSession;

            return Object.freeze({
                update: function(value) {
                    if (!ownsInteraction()) return false;
                    currentValue = value;
                    onUpdate(value);
                    return true;
                },
                commit: function(value) {
                    if (!ownsInteraction()) return false;
                    if (arguments.length > 0) {
                        currentValue = value;
                        onUpdate(value);
                    }
                    closed = true;
                    activeGeneration = 0;
                    activeCancel = null;
                    onCommit(currentValue);
                    return true;
                },
                cancel: cancelSession,
                isActive: ownsInteraction
            });
        }

        return Object.freeze({
            begin: begin,
            cancel: function() {
                return activeCancel ? activeCancel() : false;
            },
            isActive: function() { return activeGeneration !== 0; }
        });
    };
})();
