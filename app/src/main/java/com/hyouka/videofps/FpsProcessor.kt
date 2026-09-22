package com.hyouka.videofps

object FpsProcessor {
    @Volatile
    private var progressListener: ProgressListener? = null

    init {
        try {
            AppLogger.i("Native", "Loading libvideofps.so")
            System.loadLibrary("videofps")
            AppLogger.i("Native", "libvideofps.so loaded successfully")
            val nativeBuildId = getNativeBuildId()
            AppLogger.i("Native", "Native build id=" + nativeBuildId)
        } catch (t: Throwable) {
            AppLogger.e("Native", "Failed to load libvideofps.so", t)
            throw t
        }
    }

    fun setProgressListener(listener: ProgressListener?) {
        progressListener = listener
    }

    @JvmStatic
    fun dispatchProgress(percent: Int) {
        progressListener?.onProgress(percent.coerceIn(0, 100))
    }

    external fun process(
        inputFd: Int,
        outputFd: Int,
        targetFps: Int,
        durationUs: Long
    ): String?

    external fun getNativeBuildId(): String

    external fun cancel()

    interface ProgressListener {
        fun onProgress(percent: Int)
    }
}
