package com.pulp.render

import android.content.Context
import android.content.SharedPreferences
import android.util.Log
import com.pulp.PulpApplication

/**
 * GPU driver blocklist and crash recovery for Vulkan.
 *
 * Android Vulkan drivers (especially older Mali and Adreno) can crash during
 * vkCreateDevice or shader compilation. Strategy:
 * 1. Check blocklist before init
 * 2. Set crash flag BEFORE Vulkan init attempt
 * 3. Clear flag after first successful frame
 * 4. If app crashes → next launch sees flag → permanent OpenGL ES fallback
 *
 * The blocklist is checked against the adapter a PREVIOUS run reported, not the
 * current one: the decision is made before Dawn exists, so there is no adapter
 * to interrogate yet. [rememberAdapter] records the identity once Dawn has
 * initialized, and the entry is consulted from the next launch onward.
 */
object GpuDriverPolicy {

    private const val PREFS_NAME = "pulp_gpu"
    private const val KEY_VULKAN_CRASHED = "vulkan_crashed"
    private const val KEY_FORCE_GLES = "force_gles"
    private const val KEY_ADAPTER_NAME = "adapter_name"
    private const val KEY_ADAPTER_VENDOR = "adapter_vendor"
    private const val KEY_ADAPTER_DRIVER = "adapter_driver"

    /**
     * What the GPU adapter reports about itself, as surfaced by
     * `PulpSurfaceView.nativeGetGpuAdapterInfo()`.
     *
     * [driver] is the adapter's free-form description string. It is the only
     * driver-build detail available: neither Pulp's `GpuSurface::AdapterInfo`
     * nor Dawn's `wgpu::AdapterInfo` reports a numeric driver version.
     */
    data class GpuAdapterIdentity(
        val name: String,
        val vendor: String,
        val driver: String,
    )

    /**
     * One known-bad GPU. [gpuName] is matched as a case-insensitive fragment of
     * the adapter name or of its driver description, because which of the two
     * carries the device string depends on the backend.
     *
     * There is deliberately no driver-version field: no adapter Pulp can reach
     * reports one, so an entry could only ever block a whole GPU. Narrowing an
     * entry to a driver build is worth adding the day a blocklist row needs it
     * and can be tested against real field data.
     */
    data class GpuDriverEntry(val gpuName: String)

    // Known-bad GPUs — updated via app update as field data arrives
    private val VULKAN_BLOCKLIST = listOf(
        GpuDriverEntry("Mali-G72"),
        GpuDriverEntry("Adreno (TM) 505"),
    )

    /**
     * Whether [adapter] matches the Vulkan blocklist. Pure: no Context, no JNI,
     * so the blocklist itself is checkable without a device or a loaded
     * native library.
     */
    fun isVulkanBlocked(adapter: GpuAdapterIdentity): Boolean =
        VULKAN_BLOCKLIST.any { it.matches(adapter) }

    private fun GpuDriverEntry.matches(adapter: GpuAdapterIdentity): Boolean =
        adapter.name.contains(gpuName, ignoreCase = true) ||
            adapter.driver.contains(gpuName, ignoreCase = true)

    fun shouldUseVulkan(context: Context): Boolean {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

        if (prefs.getBoolean(KEY_VULKAN_CRASHED, false)) {
            Log.w(TAG, "Vulkan previously crashed — using OpenGL ES fallback")
            return false
        }
        if (prefs.getBoolean(KEY_FORCE_GLES, false)) {
            Log.i(TAG, "OpenGL ES forced by user preference")
            return false
        }

        val adapter = lastKnownAdapter(prefs)
        if (adapter != null && isVulkanBlocked(adapter)) {
            Log.w(TAG, "GPU ${adapter.name} is blocklisted for Vulkan, using OpenGL ES")
            return false
        }
        return true
    }

    /**
     * Record the adapter Dawn actually initialized, so the next launch can
     * check it against the blocklist.
     */
    fun rememberAdapter(context: Context, adapter: GpuAdapterIdentity) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(KEY_ADAPTER_NAME, adapter.name)
            .putString(KEY_ADAPTER_VENDOR, adapter.vendor)
            .putString(KEY_ADAPTER_DRIVER, adapter.driver)
            .apply()
    }

    /** The adapter a previous run reported, or null before one has been seen. */
    fun lastKnownAdapter(context: Context): GpuAdapterIdentity? =
        lastKnownAdapter(context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE))

    private fun lastKnownAdapter(prefs: SharedPreferences): GpuAdapterIdentity? {
        val name = prefs.getString(KEY_ADAPTER_NAME, null) ?: return null
        return GpuAdapterIdentity(
            name = name,
            vendor = prefs.getString(KEY_ADAPTER_VENDOR, null).orEmpty(),
            driver = prefs.getString(KEY_ADAPTER_DRIVER, null).orEmpty(),
        )
    }

    /**
     * Set crash flag BEFORE attempting Vulkan init.
     * If the app crashes during init, the flag persists → next launch uses GLES.
     */
    fun markVulkanAttempt(context: Context) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit().putBoolean(KEY_VULKAN_CRASHED, true).apply()
    }

    /**
     * Clear crash flag after first successful Vulkan frame.
     */
    fun markVulkanSuccess(context: Context) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit().putBoolean(KEY_VULKAN_CRASHED, false).apply()
    }

    /**
     * Force OpenGL ES for this device (user choice).
     */
    fun forceOpenGLES(context: Context, force: Boolean) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit().putBoolean(KEY_FORCE_GLES, force).apply()
    }

    /**
     * Reset crash flag — allows retrying Vulkan after an OS update.
     */
    fun resetCrashFlag(context: Context) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit().putBoolean(KEY_VULKAN_CRASHED, false).apply()
    }

    private const val TAG = PulpApplication.LOG_TAG
}
