package com.pulp.render

import android.app.Activity
import android.content.ClipData
import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.HandlerThread
import android.provider.OpenableColumns
import android.util.Log
import android.view.DragEvent
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.core.content.FileProvider
import java.io.File
import java.util.concurrent.CountDownLatch
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.pulp.PulpApplication
import com.pulp.accessibility.PulpAccessibilityDelegate

/**
 * SurfaceView that hosts the Vulkan/Dawn rendering surface.
 *
 * Key lifecycle contract:
 * - surfaceCreated: pass ANativeWindow to C++ for Dawn/Vulkan surface creation
 * - surfaceDestroyed: SYNCHRONOUSLY block until C++ render thread confirms stop
 *   (returning early while C++ still renders → SIGSEGV)
 * - Touch events dispatched to C++ view hierarchy
 */
class PulpSurfaceView(context: Context) : SurfaceView(context), SurfaceHolder.Callback {

    init {
        holder.addCallback(this)
        // TalkBack: route accessibility queries into the C++ view
        // hierarchy via PulpAccessibilityDelegate's JNI bridge (#87).
        isFocusable = true
        importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_YES
        accessibilityDelegate = PulpAccessibilityDelegate()

        // Pass system bar insets (status bar, nav bar) to C++ for safe area padding
        ViewCompat.setOnApplyWindowInsetsListener(this) { _, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            val density = resources.displayMetrics.density
            // Convert px insets to dp for the C++ view system
            safeAreaInsets = floatArrayOf(
                bars.top / density, bars.bottom / density,
                bars.left / density, bars.right / density
            )
            if (PulpApplication.nativeLoaded) {
                val safe = safeAreaInsets
                renderHandler?.post {
                    nativeSetSafeAreaInsets(safe[0], safe[1], safe[2], safe[3])
                }
                Log.i(TAG, "Safe area insets (dp): top=${bars.top/density} bottom=${bars.bottom/density}")
            }
            insets
        }

        // Native file drag-and-drop (inbound): accept dropped files and route
        // their resolved paths into the C++ view tree's dispatch_drop core —
        // the same core the mac/win/linux/iOS hosts use.
        setOnDragListener { _, event -> handleDragEvent(event) }
    }

    // ── Surface Lifecycle ─────────────────────────────────────────────────

    private var renderThread: HandlerThread? = null
    private var renderHandler: Handler? = null
    // Surface callbacks can overlap during Activity transitions. Keep the
    // thread/handler pair owned by exactly one callback at a time so a new
    // surface cannot overwrite the pair while the old one is joining.
    private val lifecycleLock = Any()
    @Volatile private var safeAreaInsets = FloatArray(4)

    override fun surfaceCreated(holder: SurfaceHolder) {
        synchronized(lifecycleLock) {
            Log.i(TAG, "surfaceCreated — launching GPU init on render thread")
            if (renderThread?.isAlive == true) {
                Log.w(TAG, "surfaceCreated while render thread is still alive; ignoring duplicate")
                return
            }
            if (PulpApplication.nativeLoaded) {
                // Run Dawn/Skia initialization on a dedicated thread to avoid ANR.
                // Dawn shader compilation takes 10-15 seconds on the emulator.
                // The render thread becomes the owner of the GPU context and
                // AChoreographer render loop.
                val thread = HandlerThread("PulpRenderThread").also { it.start() }
                val handler = Handler(thread.looper)
                renderThread = thread
                renderHandler = handler
                val surface = holder.surface
                val density = resources.displayMetrics.density
                val safe = safeAreaInsets
                handler.post {
                    Log.i(TAG, "Render thread started")
                    nativeSetDisplayDensity(density)
                    nativeSetSafeAreaInsets(safe[0], safe[1], safe[2], safe[3])
                    nativeOnSurfaceCreated(surface)
                    recordGpuAdapterIdentity()
                    Log.i(TAG, "Dawn init complete, servicing choreographer callbacks")
                }
            }
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        Log.i(TAG, "surfaceChanged: ${width}x${height} format=$format")
        if (PulpApplication.nativeLoaded) {
            renderHandler?.post { nativeOnSurfaceResized(width, height) }
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        synchronized(lifecycleLock) {
            Log.i(TAG, "surfaceDestroyed — waiting for render-thread teardown")
            val thread = renderThread ?: return
            val handler = renderHandler ?: return
            renderHandler = null
            // The destroy command follows any in-progress initialization, resize,
            // or input dispatch on the same Looper as Choreographer. Do not return
            // while that owner can still use SurfaceHolder's native window.
            val stopped = CountDownLatch(1)
            val destroyOnOwner = Runnable {
                try {
                    nativeOnSurfaceDestroyed()
                } finally {
                    thread.quit()
                    stopped.countDown()
                }
            }
            if (!handler.post(destroyOnOwner)) {
                // A render Looper can disappear after an uncaught native exception.
                // There can be no queued Choreographer callback once its Looper is
                // gone, so finish the native barrier directly instead of throwing
                // from SurfaceHolder.Callback on the UI thread.
                try {
                    nativeOnSurfaceDestroyed()
                } finally {
                    thread.quitSafely()
                    stopped.countDown()
                }
            }
            var interrupted = false
            while (true) {
                try {
                    stopped.await()
                    thread.join()
                    break
                } catch (_: InterruptedException) {
                    interrupted = true
                }
            }
            if (interrupted) Thread.currentThread().interrupt()
            renderThread = null
            Log.i(TAG, "surfaceDestroyed: render thread stopped")
        }
    }

    // ── Touch Input ───────────────────────────────────────────────────────

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (!PulpApplication.nativeLoaded) return super.onTouchEvent(event)
        val handler = renderHandler ?: return true
        // Android recycles MotionEvent after this call. Only immutable copied
        // pointer values may cross to the owner of the C++ view/script tree.
        val action = event.actionMasked
        val first = if (action == MotionEvent.ACTION_MOVE ||
            action == MotionEvent.ACTION_CANCEL) 0 else event.actionIndex
        val last = if (action == MotionEvent.ACTION_MOVE ||
            action == MotionEvent.ACTION_CANCEL) event.pointerCount else first + 1

        for (i in first until last) {
            val id = event.getPointerId(i)
            val x = event.getX(i)
            val y = event.getY(i)
            val pressure = event.getPressure(i)
            handler.post {
                when (action) {
                    MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN ->
                        nativeOnTouchDown(id, x, y, pressure)
                    MotionEvent.ACTION_MOVE -> nativeOnTouchMove(id, x, y, pressure)
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP ->
                        nativeOnTouchUp(id, x, y)
                    MotionEvent.ACTION_CANCEL -> nativeOnTouchCancel(id, x, y)
                }
            }
        }
        return true
    }

    // ── Drag-and-Drop (inbound) ───────────────────────────────────────────

    private fun handleDragEvent(event: DragEvent): Boolean {
        return when (event.action) {
            // Accept drags that carry at least one item; URIs are resolved at
            // drop time (we can't read them until we hold drop permission).
            DragEvent.ACTION_DRAG_STARTED ->
                (event.clipDescription?.mimeTypeCount ?: 0) > 0
            DragEvent.ACTION_DROP -> onDrop(event)
            DragEvent.ACTION_DRAG_ENTERED,
            DragEvent.ACTION_DRAG_EXITED,
            DragEvent.ACTION_DRAG_LOCATION,
            DragEvent.ACTION_DRAG_ENDED -> true
            else -> false
        }
    }

    private fun onDrop(event: DragEvent): Boolean {
        if (!PulpApplication.nativeLoaded) return false
        val clip = event.clipData ?: return false
        // Reading another app's content:// URIs requires drop permission (API 24+).
        val perms = (context as? Activity)?.requestDragAndDropPermissions(event)
        try {
            val paths = ArrayList<String>(clip.itemCount)
            for (i in 0 until clip.itemCount) {
                val uri = clip.getItemAt(i).uri ?: continue
                resolveUriToCacheFile(uri)?.let { paths.add(it) }
            }
            if (paths.isEmpty()) return false
            val droppedPaths = paths.toTypedArray()
            val x = event.x
            val y = event.y
            return renderHandler?.post { nativeOnDrop(droppedPaths, x, y) } ?: false
        } finally {
            perms?.release()
        }
    }

    // Copy a dropped content URI into the app cache and return its absolute
    // path: the C++ side consumes filesystem paths, and content:// URIs are not
    // openable by path. Best-effort — returns null on any failure.
    private fun resolveUriToCacheFile(uri: Uri): String? {
        return try {
            // The display name comes from an EXTERNAL (source-app) content URI, so
            // it is attacker-controlled: a name like "../databases/x" would escape
            // cacheDir/dropped and let a malicious drag overwrite our private files.
            // File(raw).name strips every path segment (and "..") to a bare leaf.
            val raw = queryDisplayName(uri) ?: "drop_${System.nanoTime()}"
            val name = File(raw).name.ifBlank { "drop_${System.nanoTime()}" }
            val droppedDir = File(context.cacheDir, "dropped").apply { mkdirs() }
            val out = File(droppedDir, name)
            // Defence in depth: refuse anything that still resolves outside the dir.
            if (!out.canonicalPath.startsWith(droppedDir.canonicalPath + File.separator)) {
                Log.w(TAG, "Refusing dropped file outside cache: $raw")
                return null
            }
            context.contentResolver.openInputStream(uri)?.use { input ->
                out.outputStream().use { input.copyTo(it) }
            } ?: return null
            out.absolutePath
        } catch (e: Exception) {
            Log.w(TAG, "Failed to resolve dropped URI $uri: ${e.message}")
            null
        }
    }

    private fun queryDisplayName(uri: Uri): String? {
        if (uri.scheme == "file") return uri.path?.let { File(it).name }
        return try {
            context.contentResolver.query(
                uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null
            )?.use { c ->
                if (c.moveToFirst()) {
                    val idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    if (idx >= 0) c.getString(idx) else null
                } else null
            }
        } catch (e: Exception) {
            null
        }
    }

    // ── Drag-and-Drop (outbound) ──────────────────────────────────────────

    // Invoked from C++ (JNI up-call via the registered global file-drag backend)
    // when View::start_file_drag runs on hostless Android. Builds a ClipData of
    // FileProvider content URIs and runs startDragAndDrop. Returns true if the
    // drag started (off the UI thread it posts and reports optimistic success).
    @Suppress("unused")  // called via JNI
    fun startFileDrag(paths: Array<String>): Boolean {
        if (paths.isEmpty()) return false
        val clip = buildDragClip(paths) ?: return false
        val flags = DRAG_FLAG_GLOBAL or DRAG_FLAG_GLOBAL_URI_READ
        return if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper()) {
            startDragAndDrop(clip, DragShadowBuilder(this), null, flags)
        } else {
            post { startDragAndDrop(clip, DragShadowBuilder(this), null, flags) }
            true
        }
    }

    private fun buildDragClip(paths: Array<String>): ClipData? {
        val authority = "${context.packageName}.fileprovider"
        var clip: ClipData? = null
        for (p in paths) {
            val file = File(p)
            if (!file.exists()) continue
            val uri: Uri = try {
                FileProvider.getUriForFile(context, authority, file)
            } catch (e: IllegalArgumentException) {
                // File lives outside the FileProvider's configured roots.
                Log.w(TAG, "Not shareable via FileProvider: $p (${e.message})")
                continue
            }
            val item = ClipData.Item(uri)
            if (clip == null) {
                clip = ClipData(file.name, arrayOf("application/octet-stream"), item)
            } else {
                clip.addItem(item)
            }
        }
        return clip
    }

    /**
     * Hand the adapter Dawn just initialized to the driver policy, so the next
     * launch can check it against the Vulkan blocklist. The policy decision is
     * made before any adapter exists, so a persisted identity is the only thing
     * it can consult.
     */
    private fun recordGpuAdapterIdentity() {
        val info = nativeGetGpuAdapterInfo()
        if (info == null || info.size < 3) {
            Log.w(TAG, "No GPU adapter identity reported; blocklist stays unevaluated")
            return
        }
        val identity = GpuDriverPolicy.GpuAdapterIdentity(info[0], info[1], info[2])
        Log.i(TAG, "GPU adapter: ${identity.name} / ${identity.vendor} / ${identity.driver}")
        GpuDriverPolicy.rememberAdapter(context, identity)
    }

    // ── Native Methods ────────────────────────────────────────────────────

    // Display density — called once in init, before surface lifecycle
    private external fun nativeSetDisplayDensity(density: Float)
    // Safe area insets (dp) — status bar, nav bar, notch
    private external fun nativeSetSafeAreaInsets(top: Float, bottom: Float, left: Float, right: Float)

    // Surface lifecycle — called on the same render Looper as Choreographer
    private external fun nativeOnSurfaceCreated(surface: Surface)
    private external fun nativeOnSurfaceResized(width: Int, height: Int)
    private external fun nativeOnSurfaceDestroyed()

    // Adapter identity — valid only after nativeOnSurfaceCreated has initialized
    // Dawn; null before that and after the surface is destroyed
    private external fun nativeGetGpuAdapterInfo(): Array<String>?

    // Touch events — called on main thread
    private external fun nativeOnTouchDown(pointerId: Int, x: Float, y: Float, pressure: Float)
    private external fun nativeOnTouchMove(pointerId: Int, x: Float, y: Float, pressure: Float)
    private external fun nativeOnTouchUp(pointerId: Int, x: Float, y: Float)
    private external fun nativeOnTouchCancel(pointerId: Int, x: Float, y: Float)

    // File drop — absolute cache-file paths resolved from the drag's ClipData
    private external fun nativeOnDrop(paths: Array<String>, x: Float, y: Float)

    companion object {
        private const val TAG = PulpApplication.LOG_TAG
    }
}
