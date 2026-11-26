#include "audio/miniaudio/output.hpp"
#include "game.hpp"
#include "lookup.hpp"
#include <algorithm>
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
std::mutex input_mutex;
std::vector<std::pair<int, int>> typed_keys;
regml::KeyboardState physical_held{};
regml::KeyboardState physical_pressed{};
regml::GamepadState gamepad_state{};

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
        {
            std::lock_guard lock(input_mutex);
            typed_keys.clear();
            physical_held.fill(false);
            physical_pressed.fill(false);
            gamepad_state = regml::GamepadState{};
        }
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
        regml::KeyboardState pressed{};
        regml::GamepadState gamepad_snapshot{};
        std::string typed_text;
        {
            std::lock_guard lock(input_mutex);
            held = physical_held;
            for (std::size_t bit = 0; bit < std::size(key_bits); ++bit) {
                if ((input_mask & (1 << bit)) != 0)
                    held[static_cast<std::size_t>(key_bits[bit])] = true;
            }
            held[16] = held[16] || held[160] || held[161];
            held[17] = held[17] || held[162] || held[163];
            held[18] = held[18] || held[164] || held[165];
            for (std::size_t key = 0; key < held.size(); ++key)
                pressed[key] = physical_pressed[key] || (held[key] && !runtime->previous[key]);
            physical_pressed.fill(false);
            for (const auto &[key, character] : typed_keys) {
                if (key >= 0 && key < static_cast<int>(pressed.size()))
                    pressed[static_cast<std::size_t>(key)] = true;
                if (character > 0 && character <= 255)
                    typed_text.push_back(static_cast<char>(character));
            }
            typed_keys.clear();
            gamepad_snapshot = gamepad_state;
            gamepad_state.pressed.fill(false);
        }
        if (!typed_text.empty())
            runtime->game->text_input(typed_text);
        runtime->previous = held;
        runtime->game->set_gamepad_state(gamepad_snapshot);
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
    if (key < 0 || key >= 256 || character > 255)
        return;
    std::lock_guard lock(input_mutex);
    typed_keys.emplace_back(key, character);
}

extern "C" JNIEXPORT void JNICALL Java_org_regml_app_GameSurface_nativeSetKey(JNIEnv *,
                                                                              jobject,
                                                                              jint key,
                                                                              jboolean down) {
    if (key < 0 || key >= 256)
        return;
    const auto index = static_cast<std::size_t>(key);
    const bool is_down = down == JNI_TRUE;
    std::lock_guard lock(input_mutex);
    if (is_down && !physical_held[index])
        physical_pressed[index] = true;
    physical_held[index] = is_down;
}

extern "C" JNIEXPORT void JNICALL Java_org_regml_app_GameSurface_nativeSetGamepadState(JNIEnv *,
                                                                                       jobject,
                                                                                       jboolean connected,
                                                                                       jint button_mask,
                                                                                       jfloat axis_lx,
                                                                                       jfloat axis_ly,
                                                                                       jfloat axis_rx,
                                                                                       jfloat axis_ry) {
    const bool is_connected = connected == JNI_TRUE;
    std::lock_guard lock(input_mutex);
    gamepad_state.connected = is_connected;
    for (std::size_t i = 0; i < gamepad_state.buttons.size(); ++i) {
        const bool down = is_connected && ((button_mask & (1 << i)) != 0);
        if (down && !gamepad_state.buttons[i])
            gamepad_state.pressed[i] = true;
        gamepad_state.buttons[i] = down;
    }
    if (is_connected) {
        gamepad_state.axes = {
            std::clamp(static_cast<double>(axis_lx), -1.0, 1.0),
            std::clamp(static_cast<double>(axis_ly), -1.0, 1.0),
            std::clamp(static_cast<double>(axis_rx), -1.0, 1.0),
            std::clamp(static_cast<double>(axis_ry), -1.0, 1.0),
        };
    } else {
        gamepad_state.axes.fill(0.0);
    }
}

extern "C" JNIEXPORT void JNICALL Java_org_regml_app_GameSurface_nativeDestroy(JNIEnv *, jobject) {
    runtime.reset();
    std::lock_guard lock(input_mutex);
    typed_keys.clear();
    physical_held.fill(false);
    physical_pressed.fill(false);
    gamepad_state = regml::GamepadState{};
}
