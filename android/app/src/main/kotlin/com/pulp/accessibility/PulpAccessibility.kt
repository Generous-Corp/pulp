package com.pulp.accessibility

import android.os.Bundle
import android.view.View
import android.view.accessibility.AccessibilityEvent
import android.view.accessibility.AccessibilityNodeInfo
import android.util.Log
import com.pulp.PulpApplication

/**
 * TalkBack accessibility bridge.
 * Maps Pulp's C++ accessibility interfaces to Android's AccessibilityNodeInfo.
 *
 * Pulp Interface → Android Equivalent:
 * - AccessibilityValueInterface → AccessibilityNodeInfo.RangeInfo
 * - AccessibilityTextInterface  → AccessibilityNodeInfo text fields
 * - AccessibilityTableInterface → AccessibilityNodeInfo.CollectionInfo
 * - View::AccessRole           → AccessibilityNodeInfo.setClassName()
 *
 * ⚠️ THIS SURFACE IS INERT. READ BEFORE TRUSTING ANY OF THE MAPPING BELOW.
 *
 * A `View.AccessibilityDelegate` describes ONE view: the host SurfaceView.
 * There is no `getAccessibilityNodeProvider` override here, so Pulp's widgets
 * are not virtual accessibility nodes — TalkBack never asks about them, and
 * `onInitializeAccessibilityNodeInfo` below receives a single
 * AccessibilityNodeInfo and then loops over every Pulp node overwriting that
 * same object's className / contentDescription / text. The result TalkBack
 * actually sees is ONE node carrying the LAST widget's role and label; every
 * other widget is invisible. The role table is correct and matches the C++ wire
 * format, but it is UNREACHABLE today.
 *
 * Making this real means implementing `AccessibilityNodeProvider` (or
 * `ExploreByTouchHelper`) with one virtual view id per accessible node:
 * createAccessibilityNodeInfo(virtualViewId) fills that node's role/name/
 * value/bounds, findFocus/getVirtualViewAt route hit-testing, and
 * performAction(virtualViewId, ...) dispatches back through
 * nativePerformAction with the node index instead of the current -1. No CI lane
 * compiles this file, so nothing here has been executed — do not represent the
 * Android accessibility surface as working until a device/emulator TalkBack
 * run says it does. See docs/guides/modules/view.md and the `android` skill.
 */
class PulpAccessibilityDelegate : View.AccessibilityDelegate() {

    override fun onInitializeAccessibilityNodeInfo(host: View, info: AccessibilityNodeInfo) {
        super.onInitializeAccessibilityNodeInfo(host, info)

        if (!PulpApplication.nativeLoaded) return

        // NOTE (inert): `info` is the HOST SurfaceView's node. This loop
        // overwrites it once per Pulp widget, so only the last one survives.
        // Kept as the role-mapping reference until AccessibilityNodeProvider
        // lands — see the class doc.
        // Query the C++ view hierarchy for accessibility nodes
        val nodeCount = nativeGetAccessibilityNodeCount()
        for (i in 0 until nodeCount) {
            val role = nativeGetNodeRole(i)
            val label = nativeGetNodeLabel(i)
            val value = nativeGetNodeValue(i)

            // Map Pulp roles to Android class names for TalkBack
            when (role) {
                ROLE_SLIDER -> {
                    info.className = "android.widget.SeekBar"
                    val rangeInfo = AccessibilityNodeInfo.RangeInfo.obtain(
                        AccessibilityNodeInfo.RangeInfo.RANGE_TYPE_FLOAT,
                        nativeGetNodeRangeMin(i),
                        nativeGetNodeRangeMax(i),
                        value.toFloatOrNull() ?: 0f
                    )
                    info.rangeInfo = rangeInfo
                }
                ROLE_SCROLL_BAR -> {
                    info.className = "android.widget.SeekBar"
                    val rangeInfo = AccessibilityNodeInfo.RangeInfo.obtain(
                        AccessibilityNodeInfo.RangeInfo.RANGE_TYPE_FLOAT,
                        nativeGetNodeRangeMin(i),
                        nativeGetNodeRangeMax(i),
                        value.toFloatOrNull() ?: 0f
                    )
                    info.rangeInfo = rangeInfo
                }
                ROLE_TOGGLE -> info.className = "android.widget.Switch"
                ROLE_CHECKBOX -> info.className = "android.widget.CheckBox"
                ROLE_RADIO -> info.className = "android.widget.RadioButton"
                ROLE_BUTTON -> info.className = "android.widget.Button"
                // TalkBack has no link class; a Button announces as actionable,
                // which is the closest honest mapping.
                ROLE_LINK -> info.className = "android.widget.Button"
                ROLE_TEXT_FIELD -> {
                    info.className = "android.widget.EditText"
                    info.isEditable = true
                }
                ROLE_COMBO_BOX -> info.className = "android.widget.Spinner"
                ROLE_LABEL, ROLE_HEADING -> info.className = "android.widget.TextView"
                ROLE_GROUP, ROLE_DIALOG, ROLE_TAB_LIST, ROLE_MENU ->
                    info.className = "android.view.ViewGroup"
                ROLE_LIST -> info.className = "android.widget.ListView"
                ROLE_TABLE -> info.className = "android.widget.GridView"
                // Rows/cells/list items are announced as plain text nodes;
                // CollectionInfo / CollectionItemInfo structure is NOT wired yet
                // (that needs per-row index metadata from the C++ side).
                ROLE_LIST_ITEM, ROLE_ROW, ROLE_CELL ->
                    info.className = "android.widget.TextView"
                ROLE_TAB, ROLE_MENU_ITEM -> info.className = "android.widget.Button"
                ROLE_METER, ROLE_PROGRESS_BAR -> {
                    info.className = "android.widget.ProgressBar"
                    val rangeInfo = AccessibilityNodeInfo.RangeInfo.obtain(
                        AccessibilityNodeInfo.RangeInfo.RANGE_TYPE_FLOAT,
                        nativeGetNodeRangeMin(i),
                        nativeGetNodeRangeMax(i),
                        value.toFloatOrNull() ?: 0f
                    )
                    info.rangeInfo = rangeInfo
                }
                ROLE_IMAGE -> info.className = "android.widget.ImageView"
            }

            if (label.isNotEmpty()) {
                info.contentDescription = label
            }
            if (value.isNotEmpty()) {
                info.text = value
            }
        }
    }

    override fun onInitializeAccessibilityEvent(host: View, event: AccessibilityEvent) {
        super.onInitializeAccessibilityEvent(host, event)
        event.className = "com.pulp.render.PulpSurfaceView"
    }

    override fun performAccessibilityAction(host: View, action: Int, args: Bundle?): Boolean {
        if (!PulpApplication.nativeLoaded) return super.performAccessibilityAction(host, action, args)

        return when (action) {
            AccessibilityNodeInfo.ACTION_CLICK -> {
                nativePerformAction(ACTION_CLICK, -1, 0f)
                true
            }
            AccessibilityNodeInfo.ACTION_SCROLL_FORWARD -> {
                nativePerformAction(ACTION_INCREMENT, -1, 0f)
                true
            }
            AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD -> {
                nativePerformAction(ACTION_DECREMENT, -1, 0f)
                true
            }
            else -> super.performAccessibilityAction(host, action, args)
        }
    }

    // Native methods — query C++ view hierarchy
    private external fun nativeGetAccessibilityNodeCount(): Int
    private external fun nativeGetNodeRole(index: Int): Int
    private external fun nativeGetNodeLabel(index: Int): String
    private external fun nativeGetNodeValue(index: Int): String
    private external fun nativeGetNodeRangeMin(index: Int): Float
    private external fun nativeGetNodeRangeMax(index: Int): Float
    private external fun nativeGetNodeRowCount(index: Int): Int
    private external fun nativeGetNodeColumnCount(index: Int): Int
    private external fun nativePerformAction(action: Int, nodeIndex: Int, value: Float)

    companion object {
        private const val TAG = PulpApplication.LOG_TAG

        // Pulp AccessRole ordinals — the JNI bridge
        // (core/view/platform/android/accessibility_android.cpp) marshals the
        // role as static_cast<int>(View::AccessRole), so these integers ARE the
        // wire format. They are append-only on the C++ side and locked by a
        // C++ test ("Android role ordinals are wire format" in
        // test/test_accessibility_tree.cpp); keep this block in lockstep.
        const val ROLE_NONE         = 0
        const val ROLE_SLIDER       = 1
        const val ROLE_TOGGLE       = 2
        const val ROLE_LABEL        = 3
        const val ROLE_GROUP        = 4
        const val ROLE_METER        = 5
        const val ROLE_IMAGE        = 6
        const val ROLE_BUTTON       = 7
        const val ROLE_LINK         = 8
        const val ROLE_CHECKBOX     = 9
        const val ROLE_RADIO        = 10
        const val ROLE_TEXT_FIELD   = 11
        const val ROLE_COMBO_BOX    = 12
        const val ROLE_LIST         = 13
        const val ROLE_LIST_ITEM    = 14
        const val ROLE_TABLE        = 15
        const val ROLE_ROW          = 16
        const val ROLE_CELL         = 17
        const val ROLE_TAB          = 18
        const val ROLE_TAB_LIST     = 19
        const val ROLE_MENU         = 20
        const val ROLE_MENU_ITEM    = 21
        const val ROLE_PROGRESS_BAR = 22
        const val ROLE_DIALOG       = 23
        const val ROLE_HEADING      = 24
        const val ROLE_SCROLL_BAR   = 25

        // Pulp action enum
        const val ACTION_CLICK = 0
        const val ACTION_INCREMENT = 1
        const val ACTION_DECREMENT = 2
    }
}
