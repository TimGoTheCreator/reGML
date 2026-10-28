#!/usr/bin/env bash
set -euo pipefail

: "${OO_PS4_TOOLCHAIN:?OpenOrbis toolchain path is required}"
: "${REGML_PS4_DEPS_ROOT:?PS4 dependency install prefix is required}"

workspace="${GITHUB_WORKSPACE:-$(pwd)}"
work="${RUNNER_TEMP:-/tmp}/regml-ps4-deps-src"
build="$work/build"
deps="$REGML_PS4_DEPS_ROOT"
toolchain_file="$workspace/cmake/toolchains/ps4-openorbis.cmake"

if [[ -f "$deps/.regml-ps4-deps-v1" ]]; then
    exit 0
fi

mkdir -p "$work/src" "$build" "$deps"

clone_tag() {
    local repository="$1" tag="$2" destination="$3"
    git clone --depth 1 --branch "$tag" "$repository" "$destination"
}

clone_tag https://github.com/madler/zlib.git v1.3.1 "$work/src/zlib"
clone_tag https://github.com/xiph/ogg.git v1.3.5 "$work/src/ogg"
clone_tag https://github.com/xiph/vorbis.git v1.3.7 "$work/src/vorbis"
clone_tag https://github.com/pnggroup/libpng.git v1.6.50 "$work/src/libpng"

build_cmake_library() {
    local name="$1" source="$2"
    shift 2
    cmake -S "$source" -B "$build/$name" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain_file" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$deps" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_SHARED_LIBS=OFF \
        "$@"
    cmake --build "$build/$name" --target install --parallel
}

build_cmake_library zlib "$work/src/zlib" \
    -DZLIB_BUILD_EXAMPLES=OFF

build_cmake_library ogg "$work/src/ogg" \
    -DBUILD_TESTING=OFF \
    -DINSTALL_DOCS=OFF \
    -DINSTALL_PKG_CONFIG_MODULE=OFF

build_cmake_library vorbis "$work/src/vorbis" \
    -DBUILD_TESTING=OFF \
    -DINSTALL_CMAKE_PACKAGE_MODULE=OFF \
    -DCMAKE_PREFIX_PATH="$deps"

build_cmake_library libpng "$work/src/libpng" \
    -DPNG_SHARED=OFF \
    -DPNG_STATIC=ON \
    -DPNG_TESTS=OFF \
    -DPNG_TOOLS=OFF \
    -DPNG_BUILD_ZLIB=OFF \
    -DZLIB_ROOT="$deps" \
    -DZLIB_INCLUDE_DIR="$deps/include" \
    -DZLIB_LIBRARY_RELEASE="$deps/lib/libz.a" \
    -DZLIB_LIBRARY="$deps/lib/libz.a"

curl -fsSL https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz -o "$work/bzip2.tar.gz"
tar -xzf "$work/bzip2.tar.gz" -C "$work/src"
(
    cd "$work/src/bzip2-1.0.8"
    make clean
    make libbz2.a \
        CC="clang --target=x86_64-pc-freebsd12-elf --sysroot=$OO_PS4_TOOLCHAIN" \
        AR=llvm-ar \
        RANLIB=llvm-ranlib \
        CFLAGS="-O2 -fPIC -funwind-tables -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -isystem $OO_PS4_TOOLCHAIN/include"
    install -D -m 644 bzlib.h "$deps/include/bzlib.h"
    install -D -m 644 libbz2.a "$deps/lib/libbz2.a"
)

touch "$deps/.regml-ps4-deps-v1"
