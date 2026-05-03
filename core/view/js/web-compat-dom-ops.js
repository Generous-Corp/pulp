// DOM manipulation methods (small file for QuickJS compilation limit).
//
// pulp #745 — single source of truth for the prototype methods that
// underpin web-compat DOM mutation. Earlier the same code lived twice:
// in this file (embedded into web_compat_preludes_gen.hpp via embed_js.py)
// AND inline in widget_bridge.cpp as `kDomOpsInit`. The two had drifted
// — the inline copy carried DocumentFragment flatten paths from #468
// Codex P1 that were never backported here, and the inline version was
// the one actually evaluated, so this file was dead code that the build
// happily included anyway. Now the file is the only copy.
//
// Idempotency guard: callers may eval this script more than once
// (constructor + manual reload, or the future #468 deferred-init
// transition). The flag-on-prototype check makes a second eval a no-op
// instead of re-defining the methods (which would also re-publish the
// same logic but re-trigger any global side effects readers attach via
// Object.defineProperty / proxies).

if (!Element.prototype.appendChild ||
    !Element.prototype.appendChild.__pulp_dom_ops__) {

    Element.prototype.appendChild = function(child) {
        if (!(child instanceof Element)) return child;
        // DocumentFragment flatten (pulp #468 Codex P1): a fragment must
        // splice its children into the parent — the fragment node itself
        // never enters the tree. React 18's reconciler stages commits
        // inside fragments; without this they show up as phantom wrappers.
        if (child._isDocumentFragment) {
            var kids = child._children.slice(0);
            child._children.length = 0;
            for (var i = 0; i < kids.length; i++) this.appendChild(kids[i]);
            return child;
        }
        if (child._parentElement) child._parentElement.removeChild(child);
        child._parentElement = this;
        this._children.push(child);
        this._ensureNative();
        __domAppend(this._id, child._id, child.tagName.toLowerCase());
        child._nativeCreated = true;
        if (child._textContent) setText(child._id, child._textContent);
        child.style._flushAll();
        child._reapplyStylesheets();
        // pulp #1323 — `<style>` elements receive CSS via either direct
        // textContent assignment or child Text nodes (React's reconciler
        // takes the second path). When a Text-bearing child lands under a
        // `<style>` parent, route the aggregated text through the CSS
        // translator so `:hover` rules get registered.
        if ((this.tagName === "STYLE" || this._isStyleElement)
                && typeof _processStyleElement === "function") {
            var aggregated = "";
            for (var ci = 0; ci < this._children.length; ci++) {
                aggregated += this._children[ci]._textContent || "";
            }
            this._textContent = aggregated;
            _processStyleElement(this);
        }
        return child;
    };
    Element.prototype.appendChild.__pulp_dom_ops__ = true;

    Element.prototype.removeChild = function(child) {
        var idx = this._children.indexOf(child);
        if (idx < 0) return child;
        this._children.splice(idx, 1);
        child._parentElement = null;
        if (child._nativeCreated) __domRemove(child._id);
        child._nativeCreated = false;
        return child;
    };
    Element.prototype.removeChild.__pulp_dom_ops__ = true;

    Element.prototype.remove = function() {
        if (this._parentElement) this._parentElement.removeChild(this);
    };
    Element.prototype.remove.__pulp_dom_ops__ = true;

    Element.prototype.insertBefore = function(newChild, refChild) {
        if (!refChild) return this.appendChild(newChild);
        // DocumentFragment flatten (pulp #468 Codex P1): each child of
        // the fragment is inserted at the ref position individually.
        if (newChild._isDocumentFragment) {
            var kids = newChild._children.slice(0);
            newChild._children.length = 0;
            for (var i = 0; i < kids.length; i++) this.insertBefore(kids[i], refChild);
            return newChild;
        }
        var idx = this._children.indexOf(refChild);
        if (idx < 0) return this.appendChild(newChild);
        if (newChild._parentElement) newChild._parentElement.removeChild(newChild);
        newChild._parentElement = this;
        this._children.splice(idx, 0, newChild);
        this._ensureNative();
        __domAppend(this._id, newChild._id, newChild.tagName.toLowerCase());
        newChild._nativeCreated = true;
        if (newChild._textContent) setText(newChild._id, newChild._textContent);
        newChild.style._flushAll();
        newChild._reapplyStylesheets();
        return newChild;
    };
    Element.prototype.insertBefore.__pulp_dom_ops__ = true;

    Element.prototype.replaceChild = function(newChild, oldChild) {
        var idx = this._children.indexOf(oldChild);
        if (idx < 0) return oldChild;
        this.removeChild(oldChild);
        this.appendChild(newChild);  // fragment-aware via appendChild above
        return oldChild;
    };
    Element.prototype.replaceChild.__pulp_dom_ops__ = true;
}
