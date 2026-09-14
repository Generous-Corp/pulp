package com.pulp.render

import android.content.Context
import android.content.SharedPreferences
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.Assert.assertEquals
import org.mockito.kotlin.any
import org.mockito.kotlin.anyOrNull
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
    fun blocklistedAdapterIsDeniedVulkan() {
        assertTrue(
            GpuDriverPolicy.isVulkanBlocked(
                GpuDriverPolicy.GpuAdapterIdentity(
                    name = "Mali-G72",
                    vendor = "ARM",
                    driver = "Mali-G72 r16p0",
                ),
            ),
        )
    }

    @Test
    fun unlistedAdapterIsAllowedVulkan() {
        assertFalse(
            GpuDriverPolicy.isVulkanBlocked(
                GpuDriverPolicy.GpuAdapterIdentity(
                    name = "Adreno (TM) 740",
                    vendor = "Qualcomm",
                    driver = "Adreno (TM) 740 512.744",
                ),
            ),
        )
    }

    @Test
    fun blocklistMatchesTheDriverDescriptionWhenTheNameIsSynthetic() {
        // Dawn's native path reports a synthetic adapter name, so the device
        // string is only reachable through the description.
        assertTrue(
            GpuDriverPolicy.isVulkanBlocked(
                GpuDriverPolicy.GpuAdapterIdentity(
                    name = "Native Dawn Adapter (Vulkan)",
                    vendor = "Dawn",
                    driver = "Mali-G72 r16p0",
                ),
            ),
        )
    }

    @Test
    fun shouldUseVulkanReturnsFalseForARememberedBlocklistedAdapter() {
        val (context, prefs, _) = mockedContext()
        whenever(prefs.getBoolean("vulkan_crashed", false)).thenReturn(false)
        whenever(prefs.getBoolean("force_gles", false)).thenReturn(false)
        whenever(prefs.getString("adapter_name", null)).thenReturn("Mali-G72")
        whenever(prefs.getString("adapter_vendor", null)).thenReturn("ARM")
        whenever(prefs.getString("adapter_driver", null)).thenReturn("Mali-G72 r16p0")

        assertFalse(GpuDriverPolicy.shouldUseVulkan(context))
    }

    @Test
    fun shouldUseVulkanReturnsTrueForARememberedAllowedAdapter() {
        val (context, prefs, _) = mockedContext()
        whenever(prefs.getBoolean("vulkan_crashed", false)).thenReturn(false)
        whenever(prefs.getBoolean("force_gles", false)).thenReturn(false)
        whenever(prefs.getString("adapter_name", null)).thenReturn("Adreno (TM) 740")
        whenever(prefs.getString("adapter_vendor", null)).thenReturn("Qualcomm")
        whenever(prefs.getString("adapter_driver", null)).thenReturn("Adreno (TM) 740 512.744")

        assertTrue(GpuDriverPolicy.shouldUseVulkan(context))
    }

    @Test
    fun rememberAdapterPersistsEveryReportedField() {
        val (context, _, editor) = mockedContext()

        GpuDriverPolicy.rememberAdapter(
            context,
            GpuDriverPolicy.GpuAdapterIdentity("Mali-G72", "ARM", "Mali-G72 r16p0"),
        )

        verify(editor, times(1)).putString("adapter_name", "Mali-G72")
        verify(editor, times(1)).putString("adapter_vendor", "ARM")
        verify(editor, times(1)).putString("adapter_driver", "Mali-G72 r16p0")
        verify(editor, times(1)).apply()
    }

    @Test
    fun lastKnownAdapterIsNullBeforeAnAdapterHasBeenSeen() {
        val (context, prefs, _) = mockedContext()
        whenever(prefs.getString("adapter_name", null)).thenReturn(null)

        assertEquals(null, GpuDriverPolicy.lastKnownAdapter(context))
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
        whenever(editor.putString(any(), anyOrNull())).thenReturn(editor)
        return Triple(context, prefs, editor)
    }
}
