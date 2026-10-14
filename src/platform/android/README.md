# Android

This is an early Android front end. The Activity and touch controls are Kotlin; the game VM, drawing, and audio stay in the existing C++ engine. The first build targets `arm64-v8a` and uses the software canvas.

Open `src/platform/android` in Android Studio, install Android SDK 36, NDK 30.0.15729638, and CMake 3.22.1, then build and install the `app` configuration. CMake fetches pinned libpng, libogg, and libvorbis sources while configuring the native library.

On launch, choose the folder containing `data.win` and the game's external audio files. Android copies that folder into the app's private storage. The on-screen controls and a connected keyboard or controller can provide movement and confirm/cancel input. Saves go into the app's private storage too.

The native library can also be cross-compiled directly with the NDK toolchain:

```sh
cmake -S . -B build/android \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 \
  -DREGML_ANDROID=ON -DREGML_SDL2=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/android
```
