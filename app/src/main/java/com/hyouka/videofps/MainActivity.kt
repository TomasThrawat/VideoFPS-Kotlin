package com.hyouka.videofps

import android.app.Activity
import android.content.ContentValues
import android.content.Intent
import android.database.Cursor
import android.graphics.Color
import android.media.MediaMetadataRetriever
import android.net.Uri
import android.os.Bundle
import android.os.Environment
import android.os.ParcelFileDescriptor
import android.provider.MediaStore
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

class MainActivity : Activity() {

    companion object {
        private const val PICK_VIDEO = 1001
    }

    private var selectedUri: Uri? = null
    private var selectedName: String = "video"
    private var processRunning = false
    private var currentOutputUri: Uri? = null

    private lateinit var chooseButton: Button
    private lateinit var button60: Button
    private lateinit var button90: Button
    private lateinit var button120: Button
    private lateinit var cancelButton: Button
    private lateinit var openButton: Button
    private lateinit var progressBar: ProgressBar
    private lateinit var statusText: TextView
    private lateinit var infoText: TextView

    private val executor: ExecutorService = Executors.newSingleThreadExecutor()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        AppLogger.init(this)
        AppLogger.i("MainActivity", "onCreate")
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        buildUi()
        updateButtonStates()
    }

    override fun onDestroy() {
        AppLogger.i("MainActivity", "onDestroy")
        if (processRunning) {
            FpsProcessor.cancel()
        }
        executor.shutdownNow()
        super.onDestroy()
    }

    private fun buildUi() {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 32, 32, 32)
            setBackgroundColor(Color.rgb(16, 16, 20))
        }

        val title = TextView(this).apply {
            text = "Video FPS"
            textSize = 30f
            setTextColor(Color.WHITE)
            gravity = Gravity.CENTER
        }
        root.addView(title, lp())

        val subtitle = TextView(this).apply {
            text = "رفع الفيديو إلى 60 / 90 / 120 FPS"
            textSize = 16f
            setTextColor(Color.LTGRAY)
            gravity = Gravity.CENTER
        }
        root.addView(subtitle, lp(top = 8))

        chooseButton = Button(this).apply {
            text = "اختيار فيديو"
            setOnClickListener { openVideoPicker() }
        }
        root.addView(chooseButton, lp(top = 24))

        infoText = TextView(this).apply {
            text = "لم يتم اختيار فيديو"
            textSize = 15f
            setTextColor(Color.WHITE)
            gravity = Gravity.CENTER
        }
        root.addView(infoText, lp(top = 16))

        val fpsRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
        }

        button60 = fpsButton(60)
        button90 = fpsButton(90)
        button120 = fpsButton(120)

        fpsRow.addView(button60, weightLp())
        fpsRow.addView(button90, weightLp())
        fpsRow.addView(button120, weightLp())
        root.addView(fpsRow, lp(top = 18))

        progressBar = ProgressBar(
            this,
            null,
            android.R.attr.progressBarStyleHorizontal
        ).apply {
            max = 100
            progress = 0
        }
        root.addView(progressBar, lp(top = 20))

        statusText = TextView(this).apply {
            text = "اختار فيديو أولاً"
            textSize = 15f
            setTextColor(Color.LTGRAY)
            gravity = Gravity.CENTER
        }
        root.addView(statusText, lp(top = 10))

        cancelButton = Button(this).apply {
            text = "إلغاء"
            visibility = View.GONE
            setOnClickListener {
                if (processRunning) {
                    statusText.text = "جاري الإلغاء..."
                    FpsProcessor.cancel()
                }
            }
        }
        root.addView(cancelButton, lp(top = 8))

        openButton = Button(this).apply {
            text = "فتح الفيديو الناتج"
            visibility = View.GONE
            setOnClickListener { openCurrentOutput() }
        }
        root.addView(openButton, lp(top = 8))

        setContentView(root)
    }

    private fun fpsButton(fps: Int): Button {
        return Button(this).apply {
            text = "$fps FPS"
            setOnClickListener { startConversion(fps) }
        }
    }

    private fun openVideoPicker() {
        AppLogger.i("Picker", "Opening video picker")
        if (processRunning) return

        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "video/*"
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
        }
        startActivityForResult(intent, PICK_VIDEO)
    }

    private fun startConversion(targetFps: Int) {
        AppLogger.i("Conversion", "Requested target FPS=" + targetFps)
        val input = selectedUri ?: run {
            statusText.text = "اختار فيديو أولاً"
            return
        }

        if (processRunning) return

        processRunning = true
        currentOutputUri = null
        progressBar.progress = 0
        statusText.text = "جاري تحويل الفيديو إلى $targetFps FPS..."
        cancelButton.visibility = View.VISIBLE
        openButton.visibility = View.GONE
        updateButtonStates()

        executor.execute {
            val result = runConversion(input, targetFps)
            runOnUiThread {
                processRunning = false
                cancelButton.visibility = View.GONE
                progressBar.progress = if (result.success) 100 else 0
                statusText.text = result.message
                currentOutputUri = result.outputUri
                openButton.visibility =
                    if (result.success && result.outputUri != null) View.VISIBLE else View.GONE
                updateButtonStates()
            }
        }
    }

    private fun runConversion(
        inputUri: Uri,
        targetFps: Int
    ): ConversionResult {
        AppLogger.i("Conversion", "runConversion started targetFps=" + targetFps)
        var inputFd = -1
        var outputFd = -1
        var outputUri: Uri? = null

        try {
            val metadata = readMetadata(inputUri)
            AppLogger.i(
                "Conversion",
                "Metadata width=" + metadata.width +
                    " height=" + metadata.height +
                    " durationUs=" + metadata.durationUs +
                    " durationMs=" + (metadata.durationUs / 1000L) +
                    " inputUri=" + inputUri
            )
            if (metadata.width <= 0 || metadata.height <= 0) {
                AppLogger.e("Conversion", "Invalid metadata dimensions")
                return ConversionResult(false, "تعذر قراءة بيانات الفيديو", null)
            }

            if (metadata.width > 3840 || metadata.height > 2160) {
                AppLogger.e(
                    "Conversion",
                    "Rejected video dimensions=" + metadata.width + "x" + metadata.height
                )
                return ConversionResult(false, "الحد الأقصى حالياً 3840×2160", null)
            }

            val descriptor = contentResolver.openFileDescriptor(inputUri, "r")
                ?: return ConversionResult(false, "تعذر فتح ملف الفيديو", null)
            inputFd = descriptor.detachFd()
            AppLogger.i("Conversion", "Input FD detached=" + inputFd)

            val values = ContentValues().apply {
                put(
                    MediaStore.Video.Media.DISPLAY_NAME,
                    buildOutputName(selectedName, targetFps)
                )
                put(MediaStore.Video.Media.MIME_TYPE, "video/mp4")
                put(
                    MediaStore.Video.Media.RELATIVE_PATH,
                    Environment.DIRECTORY_MOVIES + "/VideoFPS"
                )
                put(MediaStore.Video.Media.IS_PENDING, 1)
            }

            outputUri = contentResolver.insert(
                MediaStore.Video.Media.EXTERNAL_CONTENT_URI,
                values
            ) ?: return ConversionResult(false, "تعذر إنشاء ملف الإخراج", null)

            AppLogger.i(
                "Conversion",
                "Output URI created=" + outputUri +
                    " displayName=" + buildOutputName(selectedName, targetFps)
            )

            val outputDescriptor = contentResolver.openFileDescriptor(outputUri, "w")
            if (outputDescriptor == null) {
                AppLogger.e("Conversion", "Output ParcelFileDescriptor is null")
                contentResolver.delete(outputUri, null, null)
                return ConversionResult(false, "تعذر فتح ملف الإخراج", null)
            }
            outputFd = outputDescriptor.detachFd()
            AppLogger.i("Conversion", "Output FD detached=" + outputFd)

            val listener = object : FpsProcessor.ProgressListener {
                override fun onProgress(percent: Int) {
                    runOnUiThread {
                        progressBar.progress = percent.coerceIn(0, 100)
                        statusText.text = "جاري التحويل... $percent%"
                    }
                }
            }
            AppLogger.i(
                "Native",
                "Preparing listener class=" + listener.javaClass.name +
                    " identity=" + System.identityHashCode(listener)
            )
            AppLogger.i(
                "Native",
                "Calling FpsProcessor.process inputFd=" + inputFd +
                    " outputFd=" + outputFd +
                    " targetFps=" + targetFps +
                    " durationUs=" + metadata.durationUs
            )

            val error = FpsProcessor.process(
                inputFd,
                outputFd,
                targetFps,
                metadata.durationUs,
                listener
            )
            val error = FpsProcessor.process(
                inputFd,
                outputFd,
                targetFps,
                metadata.durationUs,
                object : FpsProcessor.ProgressListener {
                    override fun onProgress(percent: Int) {
                        runOnUiThread {
                            progressBar.progress = percent.coerceIn(0, 100)
                            statusText.text = "جاري التحويل... $percent%"
                        }
                    }
                }
            )

            closeFd(inputFd)
            inputFd = -1
            closeFd(outputFd)
            outputFd = -1

            AppLogger.i(
                "Native",
                "FpsProcessor.process returned=" + (error ?: "success") +
                    " inputFdWas=" + inputFd +
                    " outputFdWas=" + outputFd
            )
            if (error != null) {
                AppLogger.e("Conversion", "Conversion failed: " + error)
                contentResolver.delete(outputUri, null, null)
                outputUri = null
                return ConversionResult(false, error, null)
            }

            contentResolver.update(
                outputUri,
                ContentValues().apply {
                    put(MediaStore.Video.Media.IS_PENDING, 0)
                },
                null,
                null
            )

            return ConversionResult(
                true,
                "تم التحويل إلى $targetFps FPS وحفظه في Movies/VideoFPS",
                outputUri
            )
        } catch (t: Throwable) {
            AppLogger.e(
                "Conversion",
                "runConversion exception type=" + t.javaClass.name +
                    " message=" + (t.message ?: "<null>"),
                t
            )
            if (outputUri != null) {
                try {
                    contentResolver.delete(outputUri, null, null)
                } catch (_: Throwable) {
                }
            }
            return ConversionResult(
                false,
                t.message ?: "حدث خطأ أثناء التحويل",
                null
            )
        } finally {
            closeFd(inputFd)
            closeFd(outputFd)
        }
    }

    private fun closeFd(fd: Int) {
        if (fd < 0) return
        try {
            ParcelFileDescriptor.adoptFd(fd).close()
        } catch (_: Throwable) {
        }
    }

    private fun updateButtonStates() {
        chooseButton.isEnabled = !processRunning

        val canConvert = selectedUri != null && !processRunning
        button60.isEnabled = canConvert
        button90.isEnabled = canConvert
        button120.isEnabled = canConvert

        openButton.isEnabled = !processRunning && currentOutputUri != null
    }

    private fun readMetadata(uri: Uri): VideoMetadata {
        val retriever = MediaMetadataRetriever()
        return try {
            retriever.setDataSource(this, uri)
            VideoMetadata(
                width = retriever.extractMetadata(
                    MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH
                )?.toIntOrNull() ?: 0,
                height = retriever.extractMetadata(
                    MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT
                )?.toIntOrNull() ?: 0,
                durationUs = (
                    retriever.extractMetadata(
                        MediaMetadataRetriever.METADATA_KEY_DURATION
                    )?.toLongOrNull() ?: 0L
                ) * 1000L
            )
        } catch (_: Throwable) {
            VideoMetadata(0, 0, 0)
        } finally {
            retriever.release()
        }
    }

    private fun queryDisplayName(uri: Uri): String? {
        val cursor: Cursor? = contentResolver.query(
            uri,
            arrayOf(MediaStore.Video.Media.DISPLAY_NAME),
            null,
            null,
            null
        )

        cursor?.use {
            if (it.moveToFirst()) {
                return it.getString(0)
            }
        }

        return null
    }

    override fun onActivityResult(
        requestCode: Int,
        resultCode: Int,
        data: Intent?
    ) {
        super.onActivityResult(requestCode, resultCode, data)

        if (requestCode != PICK_VIDEO || resultCode != RESULT_OK) {
            return
        }

        val uri = data?.data ?: return
        selectedUri = uri
        AppLogger.i("Picker", "Selected URI=" + uri)

        try {
            contentResolver.takePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION
            )
        } catch (_: Throwable) {
        }

        selectedName = queryDisplayName(uri) ?: "video"
        AppLogger.i("Picker", "Selected name=" + selectedName)
        val metadata = readMetadata(uri)

        infoText.text = buildString {
            append(selectedName)
            if (metadata.width > 0 && metadata.height > 0) {
                append("\n")
                append(metadata.width)
                append(" × ")
                append(metadata.height)
            }
            if (metadata.durationUs > 0) {
                append("\n")
                append(formatDuration(metadata.durationUs))
            }
        }

        currentOutputUri = null
        openButton.visibility = View.GONE
        progressBar.progress = 0
        statusText.text = "اختار 60 أو 90 أو 120 FPS"
        updateButtonStates()
    }

    private fun openCurrentOutput() {
        val uri = currentOutputUri ?: return

        try {
            startActivity(
                Intent(Intent.ACTION_VIEW).apply {
                    setDataAndType(uri, "video/mp4")
                    addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                }
            )
        } catch (t: Throwable) {
            statusText.text = t.message ?: "تعذر فتح الفيديو الناتج"
        }
    }

    private fun buildOutputName(inputName: String, fps: Int): String {
        val dot = inputName.lastIndexOf('.')
        val base = if (dot > 0) inputName.substring(0, dot) else inputName
        return base + "_" + fps + "fps.mp4"
    }

    private fun formatDuration(durationUs: Long): String {
        val totalSeconds = durationUs / 1_000_000L
        val hours = totalSeconds / 3600L
        val minutes = (totalSeconds % 3600L) / 60L
        val seconds = totalSeconds % 60L
        return String.format("%02d:%02d:%02d", hours, minutes, seconds)
    }

    private fun lp(top: Int = 0): LinearLayout.LayoutParams {
        return LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
        ).also {
            it.topMargin = top
        }
    }

    private fun weightLp(): LinearLayout.LayoutParams {
        return LinearLayout.LayoutParams(
            0,
            LinearLayout.LayoutParams.WRAP_CONTENT,
            1f
        )
    }

    private data class VideoMetadata(
        val width: Int,
        val height: Int,
        val durationUs: Long
    )

    private data class ConversionResult(
        val success: Boolean,
        val message: String,
        val outputUri: Uri?
    )
}
