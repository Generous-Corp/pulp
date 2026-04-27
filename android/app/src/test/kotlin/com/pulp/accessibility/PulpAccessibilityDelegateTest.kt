package com.pulp.accessibility

import android.view.View
import android.view.accessibility.AccessibilityEvent
import android.view.accessibility.AccessibilityNodeInfo
import com.pulp.PulpApplication
import org.junit.Assert.assertFalse
import org.junit.Test
import org.mockito.kotlin.mock
import org.mockito.kotlin.verify

class PulpAccessibilityDelegateTest {
    @Test
    fun initializesAccessibilityEventWithSurfaceViewClassName() {
        val delegate = PulpAccessibilityDelegate()
        val host = mock<View>()
        val event = mock<AccessibilityEvent>()

        delegate.onInitializeAccessibilityEvent(host, event)

        verify(event).className = "com.pulp.render.PulpSurfaceView"
    }

    @Test
    fun performAccessibilityActionFallsBackWhenNativeBridgeIsUnavailable() {
        assertFalse(PulpApplication.nativeLoaded)

        val handled = PulpAccessibilityDelegate().performAccessibilityAction(
            mock(),
            AccessibilityNodeInfo.ACTION_CLICK,
            null
        )

        assertFalse(handled)
    }
}
