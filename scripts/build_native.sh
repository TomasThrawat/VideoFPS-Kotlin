#!/usr/bin/env bash
set -euo pipefail

FFMPEG_VERSION="9.0.2"
OPENH264_VERSION="2.6.0"
ANDROID_API="31"
ABI="arm64-v8a"

ROOT="$PWD"
WORK="${RUNNER_TEMP:-$ROOT/.work}"
mkdir -p "$WORK"

NDK_ROOT="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-}}"
if [[ -z "$NDK_ROOT" ]]; then
  echo "Android NDK is not configured."
  exit 2
fi

TOOLCHAIN="$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64"
CC="$TOOLCHAIN/bin/aarch64-linux-android31-clang"
CXX="$TOOLCHAIN/bin/aarch64-linux-android31-clang++"
AR="$TOOLCHAIN/bin/llvm-ar"
RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
STRIP="$TOOLCHAIN/bin/llvm-strip"

test -x "$CC"
test -x "$CXX"

PREFIX="$WORK/ffmpeg-$ABI"
OPENH264_PREFIX="$WORK/openh264-$ABI"
OPENH264_SRC="$WORK/openh264-src-$OPENH264_VERSION"
FFMPEG_TARBALL="$WORK/ffmpeg-$FFMPEG_VERSION.tar.xz"
FFMPEG_SRC="$WORK/ffmpeg-$FFMPEG_VERSION"

rm -rf "$PREFIX" "$OPENH264_PREFIX"
mkdir -p "$PREFIX" "$OPENH264_PREFIX/include" "$OPENH264_PREFIX/lib/pkgconfig"

if [[ ! -d "$OPENH264_SRC" ]]; then
  git clone --depth 1 --branch "v$OPENH264_VERSION" https://github.com/cisco/openh264.git "$OPENH264_SRC"
fi

cp -a "$OPENH264_SRC/codec/api/wels" "$OPENH264_PREFIX/include/"

OPENH264_SO="$OPENH264_PREFIX/lib/libopenh264.so.8"
if [[ ! -f "$OPENH264_SO" ]]; then
  curl -L --fail --retry 3 "https://ciscobinary.openh264.org/libopenh264-$OPENH264_VERSION-android-arm64.8.so.bz2" -o "$WORK/openh264.bz2"
  bzip2 -dc "$WORK/openh264.bz2" > "$OPENH264_SO"
fi

ln -sf libopenh264.so.8 "$OPENH264_PREFIX/lib/libopenh264.so"

cat > "$OPENH264_PREFIX/lib/pkgconfig/openh264.pc" <<EOF
prefix=$OPENH264_PREFIX
exec_prefix=${prefix}
libdir=${prefix}/lib
includedir=${prefix}/include

Name: openh264
Description: OpenH264 H.264 codec
Version: $OPENH264_VERSION
Libs: -L${libdir} -lopenh264
Cflags: -I${includedir}
EOF

if [[ ! -d "$FFMPEG_SRC" ]]; then
  curl -L --fail --retry 3 "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz" -o "$FFMPEG_TARBALL"
  tar -xf "$FFMPEG_TARBALL" -C "$WORK"
fi

pushd "$FFMPEG_SRC" >/dev/null
export PKG_CONFIG_PATH="$OPENH264_PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$OPENH264_PREFIX/lib/pkgconfig"
export PATH="$TOOLCHAIN/bin:$PATH"

make distclean >/dev/null 2>&1 || true

./configure   --target-os=android   --arch=aarch64   --cpu=armv8-a   --enable-cross-compile   --cc="$CC"   --cxx="$CXX"   --ar="$AR"   --ranlib="$RANLIB"   --strip="$STRIP"   --sysroot="$TOOLCHAIN/sysroot"   --prefix="$PREFIX"   --libdir="$PREFIX/lib"   --incdir="$PREFIX/include"   --enable-shared   --disable-static   --disable-programs   --disable-doc   --disable-debug   --disable-network   --disable-autodetect   --enable-pic   --enable-small   --enable-libopenh264

make -j"$(nproc)"
make install
popd >/dev/null

DEST="$ROOT/app/src/main/jniLibs/$ABI"
INCLUDE_DEST="$ROOT/app/src/main/cpp/ffmpeg/include"

rm -rf "$DEST" "$INCLUDE_DEST"
mkdir -p "$DEST" "$INCLUDE_DEST"

cp -a "$PREFIX/include/." "$INCLUDE_DEST/"
cp -L "$OPENH264_PREFIX/lib/libopenh264.so.8" "$DEST/libopenh264.so.8"
ln -sf libopenh264.so.8 "$DEST/libopenh264.so"

for lib in libavutil libavcodec libavformat libavfilter libswscale libswresample; do
  source="$PREFIX/lib/$lib.so"
  test -f "$source"
  cp -L "$source" "$DEST/$lib.so"
done

LIBCXX_SHARED="$TOOLCHAIN/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
test -s "$LIBCXX_SHARED"
cp -L "$LIBCXX_SHARED" "$DEST/libc++_shared.so"

"$CXX"   -shared   -fPIC   -O3   -std=c++17   -I"$INCLUDE_DEST"   "$ROOT/app/src/main/cpp/video_fps.cpp"   "$PREFIX/lib/libavfilter.so"   "$PREFIX/lib/libavformat.so"   "$PREFIX/lib/libavcodec.so"   "$PREFIX/lib/libswscale.so"   "$PREFIX/lib/libswresample.so"   "$PREFIX/lib/libavutil.so"   "$OPENH264_PREFIX/lib/libopenh264.so.8"   -Wl,-soname,libvideofps.so   -Wl,-rpath-link,"$PREFIX/lib"   -Wl,-rpath-link,"$OPENH264_PREFIX/lib"   -llog -lz -ldl -lm   -o "$DEST/libvideofps.so"

"$STRIP" --strip-unneeded "$DEST/libvideofps.so"
ls -lh "$DEST"
