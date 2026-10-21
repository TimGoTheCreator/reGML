#include "audio/miniaudio/output.hpp"
#include "game.hpp"
#include "lookup.hpp"
#include <jni.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
struct Runtime {
    std::unique_ptr<regml::GameSession> game;
    std::unique_ptr<regml::MiniAudioOutput> audio;
    regml::KeyboardState previous{};
};
std::unique_ptr<Runtime> runtime;
std::mutex typed_key_mutex;
std::vector<std::pair<int, int>> typed_keys;

std::string from_java(JNIEnv *env, jstring text) {
    if (!text)
        return {};
    const auto *chars = env->GetStringUTFChars(text, nullptr);
    if (!chars)
        return {};
    std::string result(chars);
    env->ReleaseStringUTFChars(text, chars);
    return result;
}

void throw_java(JNIEnv *env, const char *message) {
    if (auto exception = env->FindClass("java/lang/RuntimeException"))
        env->ThrowNew(exception, message);
}
} // namespace

extern "C" JNIEXPORT jboolean JNICALL Java_org_regml_app_GameSurface_nativeCreate(JNIEnv *env,
                                                                                  jobject,
                                                                                  jstring package_path,
                                                                                  jstring save_path) {
    try {
        auto next = std::make_unique<Runtime>();
        next->game = std::make_unique<regml::GameSession>(from_java(env, package_path), std::nullopt,
                                                          from_java(env, save_path));
        next->audio = std::make_unique<regml::MiniAudioOutput>(*next->game, 1.0);
        runtime = std::move(next);
        return JNI_TRUE;
    } catch (const std::exception &error) {
        throw_java(env, error.what());
        return JNI_FALSE;
    }
}

extern "C" JNIEXPORT void JNICALL Java_org_regml_app_GameSurface_nativeFrame(JNIEnv *env,
                                                                             jobject,
                                                                             jint input_mask,
                                                                             jintArray frame) {
    if (!runtime)
        return;
    try {
        constexpr int key_bits[] = {37, 38, 39, 40, 'Z', 'X', 'C', 13};
        regml::KeyboardState held{};
        for (std::size_t bit = 0; bit < std::size(key_bits); ++bit)
            held[static_cast<std::size_t>(key_bits[bit])] = (input_mask & (1 << bit)) != 0;
        regml::KeyboardState pressed{};
        for (std::size_t key = 0; key < held.size(); ++key)
            pressed[key] = held[key] && !runtime->previous[key];
        std::string typed_text;
        {
            std::lock_guard lock(typed_key_mutex);
            for (const auto &[key, character] : typed_keys) {
                if (key >= 0 && key < static_cast<int>(pressed.size()))
                    pressed[static_cast<std::size_t>(key)] = true;
                if (character >= 0 && character <= 255)
                    typed_text.push_back(static_cast<char>(character));
            }
            typed_keys.clear();
        }
        if (!typed_text.empty())
            runtime->game->text_input(typed_text);
        runtime->previous = held;
        runtime->game->tick(held, pressed);

        const auto pixels = runtime->game->canvas().pixels();
        if (!frame || env->GetArrayLength(frame) < static_cast<jsize>(pixels.size())) {
            throw_java(env, "Android frame buffer is too small");
            return;
        }
        env->SetIntArrayRegion(frame, 0, static_cast<jsize>(pixels.size()),
                               reinterpret_cast<const jint *>(pixels.data()));
    } catch (const std::exception &error) {
        throw_java(env, error.what());
    }
}

extern "C" JNIEXPORT jfloat JNICALL Java_org_regml_app_GameSurface_nativeSpeed(JNIEnv *, jobject) {
    return runtime ? static_cast<jfloat>(runtime->game->speed()) : 30.0f;
}

extern "C" JNIEXPORT jint JNICALL Java_org_regml_app_GameSurface_nativeWidth(JNIEnv *, jobject) {
    return runtime ? runtime->game->canvas().width() : 0;
}

extern "C" JNIEXPORT jint JNICALL Java_org_regml_app_GameSurface_nativeHeight(JNIEnv *, jobject) {
    return runtime ? runtime->game->canvas().height() : 0;
}

extern "C" JNIEXPORT void JNICALL Java_org_regml_app_GameSurface_nativeTypedKey(JNIEnv *,
                                                                                jobject,
                                                                                jint key,
                                                                                jint character) {
    if (key < 0 || key >= 256 || character < 0 || character > 255)
        return;
    std::lock_guard lock(typed_key_mutex);
    typed_keys.emplace_back(key, character);
}

extern "C" JNIEXPORT void JNICALL Java_org_regml_app_GameSurface_nativeDestroy(JNIEnv *, jobject) {
    runtime.reset();
}
