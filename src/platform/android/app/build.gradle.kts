plugins {
    id("com.android.application")
}

android {
    namespace = "org.regml.app"
    compileSdk = 36
    ndkVersion = "30.0.15729638"

    defaultConfig {
        applicationId = "org.regml.app"
        minSdk = 28
        targetSdk = 35
        versionCode = 1
        versionName = "0.2.1"
        ndk { abiFilters += "arm64-v8a" }

        externalNativeBuild {
            cmake {
                arguments += listOf("-DREGML_ANDROID=ON", "-DREGML_SDL2=OFF", "-DREGML_240P=OFF")
                cppFlags += listOf("-std=c++20")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../../../../CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            externalNativeBuild.cmake { arguments += "-DCMAKE_BUILD_TYPE=Release" }
        }
        debug {
            externalNativeBuild.cmake { arguments += "-DCMAKE_BUILD_TYPE=Debug" }
        }
    }
}
