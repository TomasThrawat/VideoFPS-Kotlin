# VideoFPS-Kotlin

تطبيق Android Native مكتوب بـ Kotlin يحول الفيديو إلى 60 أو 90 أو 120 FPS باستخدام motion-compensated frame interpolation عبر FFmpeg.

- بدون HTML وبدون WebView.
- اختيار فيديو من Android Storage Access Framework.
- 60 / 90 / 120 FPS.
- حفظ الناتج في Movies/VideoFPS.
- معالجة محلية على الجهاز.
- يسجل Diagnostics في Download/VideoFPS-Diagnostics.log.
- بناء arm64-v8a.

## Build

GitHub Actions يبني FFmpeg وOpenH264 ثم يبني APK Debug للتحقق والتنزيل.

الناتج:
app/build/outputs/apk/debug/app-debug.apk

راجع THIRD_PARTY_LICENSES.md للمكونات الخارجية.
