package com.hyouka.videofps

object FpsProcessor {
    init {
        System.loadLibrary("videofps")
    }

    external fun process(
        inputFd: Int,
        outputFd: Int,
        targetFps: Int,
        durationUs: Long,
        listener: ProgressListener
    ): String?

    external fun cancel()
}

fun interface ProgressListener {
    fun onProgress(percent: Int)
}
