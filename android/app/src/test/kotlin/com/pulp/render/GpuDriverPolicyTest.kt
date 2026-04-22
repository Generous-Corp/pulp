package com.pulp.render

import android.content.Context
import android.content.SharedPreferences
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.mockito.kotlin.any
import org.mockito.kotlin.eq
import org.mockito.kotlin.mock
import org.mockito.kotlin.times
import org.mockito.kotlin.verify
import org.mockito.kotlin.whenever

class GpuDriverPolicyTest {
    @Test
    fun shouldUseVulkanReturnsTrueWhenNoFallbackFlagsAreSet() {
        val (context, prefs, _) = mockedContext()
        whenever(prefs.getBoolean("vulkan_crashed", false)).thenReturn(false)
        whenever(prefs.getBoolean("force_gles", false)).thenReturn(false)

        assertTrue(GpuDriverPolicy.shouldUseVulkan(context))
    }

    @Test
    fun shouldUseVulkanReturnsFalseAfterCrash() {
        val (context, prefs, _) = mockedContext()
        whenever(prefs.getBoolean("vulkan_crashed", false)).thenReturn(true)

        assertFalse(GpuDriverPolicy.shouldUseVulkan(context))
    }

    @Test
    fun shouldUseVulkanReturnsFalseWhenGlesIsForced() {
        val (context, prefs, _) = mockedContext()
        whenever(prefs.getBoolean("vulkan_crashed", false)).thenReturn(false)
        whenever(prefs.getBoolean("force_gles", false)).thenReturn(true)

        assertFalse(GpuDriverPolicy.shouldUseVulkan(context))
    }

    @Test
    fun mutatorsPersistTheirPreferenceFlags() {
        val (context, _, editor) = mockedContext()

        GpuDriverPolicy.markVulkanAttempt(context)
        GpuDriverPolicy.markVulkanSuccess(context)
        GpuDriverPolicy.forceOpenGLES(context, true)
        GpuDriverPolicy.resetCrashFlag(context)

        verify(editor, times(1)).putBoolean("vulkan_crashed", true)
        verify(editor, times(2)).putBoolean("vulkan_crashed", false)
        verify(editor, times(1)).putBoolean("force_gles", true)
        verify(editor, times(4)).apply()
    }

    private fun mockedContext(): Triple<Context, SharedPreferences, SharedPreferences.Editor> {
        val context = mock<Context>()
        val prefs = mock<SharedPreferences>()
        val editor = mock<SharedPreferences.Editor>()
        whenever(context.getSharedPreferences(any(), eq(Context.MODE_PRIVATE))).thenReturn(prefs)
        whenever(prefs.edit()).thenReturn(editor)
        whenever(editor.putBoolean(any(), any())).thenReturn(editor)
        return Triple(context, prefs, editor)
    }
}
