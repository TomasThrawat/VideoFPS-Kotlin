package com.hyouka.videofps

object FpsProcessor {
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

    external fun process(
        inputFd: Int,
        outputFd: Int,
        targetFps: Int,
        durationUs: Long,
        listener: ProgressListener
    ): String?

    external fun getNativeBuildId(): String

    external fun cancel()

    interface ProgressListener {
        fun onProgress(percent: Int)
    }
}
