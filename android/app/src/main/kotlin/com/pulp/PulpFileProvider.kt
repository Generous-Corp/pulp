package com.pulp

import android.content.Context
import android.util.Log

/**
 * Bridges Android's scoped storage with the C++ file I/O layer.
 *
 * - Bundled assets → AAssetManager (read-only, zero-copy in C++)
 * - App-private files → context.filesDir (free native I/O)
 * - User files → Storage Access Framework (SAF) via ParcelFileDescriptor → fd
 */
class PulpFileProvider(private val context: Context) {

    fun init() {
        nativeInitAssets(context)
        nativeSetAppDataDir(context.filesDir.absolutePath)
        Log.i(PulpApplication.LOG_TAG, "FileProvider initialized: ${context.filesDir.absolutePath}")
    }

    private external fun nativeInitAssets(context: Context)
    private external fun nativeSetAppDataDir(dir: String)
}
