#!/usr/bin/env bash
# Builds third-party libraries for 64-bit Windows with MinGW-w64, as static
# libraries in a prefix, for the cross builds on Linux (CI and local):
#
#   ci/mingw-deps.sh <prefix> zlib openssl ...
#
# Each library is built once: a prefix that already has it is left alone.
# Sources are downloaded from their projects and checked against pinned
# SHA-256 sums (or, for git checkouts, pinned commit ids). JOBS sets the
# number of parallel compile jobs; the default is one per processor.
set -euo pipefail

PREFIX=$(realpath -m "$1")
shift
HOST=x86_64-w64-mingw32
JOBS=${JOBS:-$(nproc)}
WORK=${MINGW_DEPS_WORK:-$(mktemp -d)}
mkdir -p "$PREFIX" "$WORK"

fetch() {  # url sha256 file
    local url=$1 sum=$2 file=$WORK/$3
    if [ ! -f "$file" ]; then curl -sSLf --retry 3 -o "$file" "$url"; fi
    echo "$sum  $file" | sha256sum -c --quiet -
}

fetch_git() {  # url tag commit dir
    local url=$1 tag=$2 commit=$3 dir=$WORK/$4
    rm -rf "$dir"
    git -c advice.detachedHead=false clone -q --depth 1 --branch "$tag" "$url" "$dir"
    if [ "$(git -C "$dir" rev-parse HEAD)" != "$commit" ]; then
        echo "$url $tag is not commit $commit" >&2
        exit 1
    fi
}

have() { [ -f "$PREFIX/.built-$1" ]; }
done_() { touch "$PREFIX/.built-$1"; }

build_zlib() {
    have zlib && return
    fetch https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz \
        9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23 zlib.tar.gz
    rm -rf "$WORK/zlib" && mkdir -p "$WORK/zlib" && tar -xzf "$WORK/zlib.tar.gz" -C "$WORK/zlib" --strip-components=1
    (cd "$WORK/zlib" && make -f win32/Makefile.gcc PREFIX=$HOST- -j"$JOBS" libz.a &&
        make -f win32/Makefile.gcc PREFIX=$HOST- \
            INCLUDE_PATH="$PREFIX/include" LIBRARY_PATH="$PREFIX/lib" BINARY_PATH="$PREFIX/bin" install)
    done_ zlib
}

build_openssl() {
    have openssl && return
    fetch https://github.com/openssl/openssl/releases/download/openssl-3.5.4/openssl-3.5.4.tar.gz \
        967311f84955316969bdb1d8d4b983718ef42338639c621ec4c34fddef355e99 openssl.tar.gz
    rm -rf "$WORK/openssl" && mkdir -p "$WORK/openssl" && tar -xzf "$WORK/openssl.tar.gz" -C "$WORK/openssl" --strip-components=1
    (cd "$WORK/openssl" &&
        ./Configure mingw64 --cross-compile-prefix=$HOST- --prefix="$PREFIX" --libdir=lib \
            no-shared no-tests no-docs no-apps no-module &&
        make -j"$JOBS" build_libs >/dev/null && make install_dev >/dev/null)
    rm -rf "$WORK/openssl"
    done_ openssl
}

# Opus for the audio stream. Its release tarballs live on a mirror whose
# certificate chain not every CA store accepts, so the tagged release is
# taken from git and checked against its commit id.
build_opus() {
    have opus && return
    fetch_git https://github.com/xiph/opus v1.6.1 22244de5a79bd1d6d623c32e72bf1954b56235be opus
    cmake -S "$WORK/opus" -B "$WORK/opus/build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
        -DCMAKE_C_COMPILER=$HOST-gcc -DCMAKE_RC_COMPILER=$HOST-windres \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
        -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF \
        -DOPUS_STACK_PROTECTOR=OFF -DOPUS_FORTIFY_SOURCE=OFF
    cmake --build "$WORK/opus/build" -j "$JOBS" >/dev/null
    cmake --install "$WORK/opus/build" >/dev/null
    rm -rf "$WORK/opus"
    done_ opus
}

# FFmpeg's decoding libraries, LGPL only (no --enable-gpl): H.264 and HEVC
# decoders with D3D11VA hardware decoding, and swscale for the software
# path's colour conversion. Everything else is left out.
build_ffmpeg() {
    have ffmpeg && return
    fetch https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
        8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e ffmpeg.tar.xz
    rm -rf "$WORK/ffmpeg" && mkdir -p "$WORK/ffmpeg" && tar -xJf "$WORK/ffmpeg.tar.xz" -C "$WORK/ffmpeg" --strip-components=1
    (cd "$WORK/ffmpeg" &&
        ./configure --prefix="$PREFIX" --enable-cross-compile --target-os=mingw32 --arch=x86_64 \
            --cross-prefix=$HOST- --pkg-config=pkg-config \
            --enable-static --disable-shared --disable-programs --disable-doc --disable-debug \
            --disable-autodetect --disable-everything --disable-network \
            --disable-avdevice --disable-avformat --disable-avfilter --disable-swresample \
            --enable-w32threads --enable-d3d11va \
            --enable-decoder=h264,hevc --enable-parser=h264,hevc \
            --enable-hwaccel=h264_d3d11va,h264_d3d11va2,hevc_d3d11va,hevc_d3d11va2 &&
        make -j"$JOBS" >/dev/null && make install >/dev/null)
    rm -rf "$WORK/ffmpeg"
    done_ ffmpeg
}

for lib in "$@"; do
    "build_$lib"
done
