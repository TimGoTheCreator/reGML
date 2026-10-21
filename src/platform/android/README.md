# Android

Early Android front end
Activity and touch controls are Kotlin, VM, drawing and audio stay in the C++ engine
First build targets `arm64-v8a` and uses the software canvas

Open `src/platform/android` in Android Studio
Install Android SDK 36, NDK 30.0.15729638 and CMake 3.22.1, then build and install the `app` configuration
CMake fetches pinned libpng, libogg and libvorbis sources for the native library

On launch, choose the folder with `data.win` and the game's external audio files
Android copies that folder into app storage
Use the on-screen controls, keyboard or controller for movement and confirm/cancel
Saves go in app storage too

Can cross-compile the native library directly with the NDK:

```sh
cmake -S . -B build/android \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 \
  -DREGML_ANDROID=ON -DREGML_SDL2=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/android
```
