package com.hyouka.videofps

import android.content.ContentResolver
import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.os.Process
import android.provider.MediaStore
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

object AppLogger {
    private const val FILE_NAME = "VideoFPS-Diagnostics.log"
    private val lock = Any()
    private val formatter = SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US)
    @Volatile private var resolver: ContentResolver? = null
    @Volatile private var logUri: Uri? = null
    @Volatile private var initialized = false
    @Volatile private var previousHandler: Thread.UncaughtExceptionHandler? = null

    fun init(context: Context) {
        synchronized(lock) {
            if (initialized) return
            resolver = context.applicationContext.contentResolver
            initialized = true
            previousHandler = Thread.getDefaultUncaughtExceptionHandler()
            Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
                e("Crash", "Uncaught exception on thread=" + thread.name, throwable)
                previousHandler?.uncaughtException(thread, throwable)
            }
            i("App", "Logging initialized; api=" + Build.VERSION.SDK_INT +
                " model=" + Build.MODEL +
                " manufacturer=" + Build.MANUFACTURER +
                " abi=" + Build.SUPPORTED_ABIS.joinToString(",") +
                " pid=" + Process.myPid())
        }
    }

    fun i(tag: String, message: String) = write("I", tag, message, null)
    fun w(tag: String, message: String) = write("W", tag, message, null)
    fun e(tag: String, message: String) = write("E", tag, message, null)
    fun e(tag: String, message: String, throwable: Throwable) = write("E", tag, message, throwable)

    private fun write(level: String, tag: String, message: String, throwable: Throwable?) {
        synchronized(lock) {
            val time = synchronized(formatter) { formatter.format(Date()) }
            val thread = Thread.currentThread().name
            val suffix = throwable?.let { "\n" + it.stackTraceToString() } ?: ""
            append("[$time] [$level] [$tag] [thread=$thread] $message$suffix\n")
        }
    }

    private fun append(text: String) {
        val r = resolver ?: return
        try {
            val uri = findOrCreate(r) ?: return
            r.openOutputStream(uri, "wa")?.use {
                it.write(text.toByteArray(Charsets.UTF_8))
                it.flush()
            }
        } catch (_: Throwable) {
            // Logging must never crash the app.
        }
    }

    private fun findOrCreate(r: ContentResolver): Uri? {
        logUri?.let { return it }
        val collection = MediaStore.Downloads.EXTERNAL_CONTENT_URI
        val relativePath = Environment.DIRECTORY_DOWNLOADS + "/"
        r.query(
            collection,
            arrayOf(MediaStore.MediaColumns._ID),
            MediaStore.MediaColumns.DISPLAY_NAME + "=? AND " +
                MediaStore.MediaColumns.RELATIVE_PATH + "=?",
            arrayOf(FILE_NAME, relativePath),
            null
        )?.use { cursor ->
            if (cursor.moveToFirst()) {
                val id = cursor.getLong(0)
                return MediaStore.Downloads.getContentUri(
                    MediaStore.VOLUME_EXTERNAL_PRIMARY,
                    id
                ).also { logUri = it }
            }
        }

        val values = ContentValues().apply {
            put(MediaStore.MediaColumns.DISPLAY_NAME, FILE_NAME)
            put(MediaStore.MediaColumns.MIME_TYPE, "text/plain")
            put(MediaStore.MediaColumns.RELATIVE_PATH, relativePath)
            put(MediaStore.MediaColumns.IS_PENDING, 0)
        }
        return r.insert(collection, values)?.also { logUri = it }
    }
}
