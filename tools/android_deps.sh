#!/bin/bash
# Build SDL2 + SDL2_ttf (vendored FreeType/HarfBuzz) for Android arm64-v8a into work/android/prefix.
#   tools/android_deps.sh        (ANDROID_HOME default ~/Library/Android/sdk; NDK 26.1 + CMake 3.22.1 from the SDK)
set -euo pipefail
SDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}
NDK=${ANDROID_NDK:-$SDK/ndk/26.1.10909125}
export PATH=$SDK/cmake/3.22.1/bin:$PATH
W=work/android; P=$PWD/$W/prefix
mkdir -p $W/src
cd $W/src
[ -d SDL2-2.30.9 ] || { curl -sSLO https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz; tar xzf SDL2-2.30.9.tar.gz; }
[ -d SDL2_ttf-2.22.0 ] || { curl -sSLO https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.22.0/SDL2_ttf-2.22.0.tar.gz; tar xzf SDL2_ttf-2.22.0.tar.gz; }
cd ..
COMMON=(-G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a
        -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_FIND_ROOT_PATH=$P)
cmake -S src/SDL2-2.30.9 -B b/sdl "${COMMON[@]}" -DSDL_STATIC=OFF -DSDL_SHARED=ON -DSDL_TEST=OFF
cmake --build b/sdl -j10 && cmake --install b/sdl
cmake -S src/SDL2_ttf-2.22.0 -B b/ttf "${COMMON[@]}" -DSDL2TTF_VENDORED=ON -DSDL2TTF_HARFBUZZ=ON -DSDL2TTF_SAMPLES=OFF \
      -DBUILD_SHARED_LIBS=ON -DSDL2_DIR=$P/lib/cmake/SDL2
cmake --build b/ttf -j10 && cmake --install b/ttf
ls -la $P/lib/*.so
