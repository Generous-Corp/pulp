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
 */
class PulpAccessibilityDelegate : View.AccessibilityDelegate() {

    override fun onInitializeAccessibilityNodeInfo(host: View, info: AccessibilityNodeInfo) {
        super.onInitializeAccessibilityNodeInfo(host, info)

        if (!PulpApplication.nativeLoaded) return

        // Query the C++ view hierarchy for accessibility nodes
        val nodeCount = nativeGetAccessibilityNodeCount()
        for (i in 0 until nodeCount) {
            val role = nativeGetNodeRole(i)
            val label = nativeGetNodeLabel(i)
            val value = nativeGetNodeValue(i)

            // Map Pulp roles to Android class names for TalkBack
            when (role) {
                ROLE_BUTTON -> info.className = "android.widget.Button"
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
                ROLE_TEXT -> info.className = "android.widget.TextView"
                ROLE_TOGGLE -> info.className = "android.widget.ToggleButton"
                ROLE_LIST -> {
                    info.className = "android.widget.ListView"
                    val collectionInfo = AccessibilityNodeInfo.CollectionInfo.obtain(
                        nativeGetNodeRowCount(i), nativeGetNodeColumnCount(i), false
                    )
                    info.collectionInfo = collectionInfo
                }
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

        // Pulp AccessRole enum values (must match C++ View::AccessRole)
        const val ROLE_BUTTON = 0
        const val ROLE_SLIDER = 1
        const val ROLE_TEXT = 2
        const val ROLE_TOGGLE = 3
        const val ROLE_LIST = 4

        // Pulp action enum
        const val ACTION_CLICK = 0
        const val ACTION_INCREMENT = 1
        const val ACTION_DECREMENT = 2
    }
}
