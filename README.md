# VideoFPS-Kotlin

تطبيق Android Native مكتوب بـ Kotlin يحول الفيديو إلى 60 أو 90 أو 120 FPS باستخدام motion-compensated frame interpolation عبر FFmpeg.

- بدون HTML وبدون WebView.
- اختيار فيديو من Android Storage Access Framework.
- 60 / 90 / 120 FPS.
- حفظ الناتج في Movies/VideoFPS.
- معالجة محلية على الجهاز.
- لا ينشئ logs داخل Download.
- بناء arm64-v8a.

## Build

GitHub Actions يبني FFmpeg وOpenH264 ثم يبني APK Release.

الناتج:
app/build/outputs/apk/release/app-release.apk

راجع THIRD_PARTY_LICENSES.md للمكونات الخارجية.
