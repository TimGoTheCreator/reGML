#include "regml/platform/play.hpp"
#include "regml/runtime/game.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <thread>
#if REGML_WITH_SDL2
#include <SDL2/SDL.h>
#endif
namespace regml {
namespace {
#if REGML_WITH_SDL2
void sdl_ok(int status, std::string_view operation) {
    if (status < 0)
        throw Error(ErrorKind::io, std::string(operation) + ": " + SDL_GetError());
}
struct SdlLifetime {
    SdlLifetime() {
        SDL_SetMainReady();
        sdl_ok(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER), "SDL initialization");
    }
    ~SdlLifetime() { SDL_Quit(); }
    SdlLifetime(const SdlLifetime &) = delete;
    SdlLifetime &operator=(const SdlLifetime &) = delete;
};
struct WindowDelete {
    void operator()(SDL_Window *p) const { SDL_DestroyWindow(p); }
};
struct RendererDelete {
    void operator()(SDL_Renderer *p) const { SDL_DestroyRenderer(p); }
};
struct TextureDelete {
    void operator()(SDL_Texture *p) const { SDL_DestroyTexture(p); }
};
int gml_key(SDL_Keycode key) {
    if (key >= SDLK_F1 && key <= SDLK_F12)
        return 112 + static_cast<int>(key - SDLK_F1);
    switch (key) {
    case SDLK_LEFT:
        return 37;
    case SDLK_UP:
        return 38;
    case SDLK_RIGHT:
        return 39;
    case SDLK_DOWN:
        return 40;
    default:
        if (key >= SDLK_a && key <= SDLK_z)
            return static_cast<int>(key - SDLK_a + 'A');
        return key >= 0 && key < 256 ? static_cast<int>(key) : -1;
    }
}
class VideoWindow {
  public:
    explicit VideoWindow(bool hidden, bool escape_quits = true) : escape_quits_(escape_quits) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        const auto flags = static_cast<Uint32>(SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                               (hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN));
        window_.reset(SDL_CreateWindow(escape_quits ? "reGML - VM Arcade" : "reGML - Game (experimental)",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600, flags));
        if (!window_)
            throw Error(ErrorKind::io, std::string("window creation: ") + SDL_GetError());
        renderer_.reset(SDL_CreateRenderer(window_.get(), -1, SDL_RENDERER_ACCELERATED));
        if (!renderer_)
            renderer_.reset(SDL_CreateRenderer(window_.get(), -1, SDL_RENDERER_SOFTWARE));
        if (!renderer_)
            throw Error(ErrorKind::io, std::string("renderer creation: ") + SDL_GetError());
        sdl_ok(SDL_RenderSetLogicalSize(renderer_.get(), 640, 480), "logical resolution");
        texture_.reset(
            SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 480));
        if (!texture_)
            throw Error(ErrorKind::io, std::string("framebuffer texture: ") + SDL_GetError());
        sdl_ok(SDL_SetTextureBlendMode(texture_.get(), SDL_BLENDMODE_NONE), "texture blend mode");
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) >= 0) {
            controller_subsystem_ = true;
        }
        for (int i = 0; controller_subsystem_ && i < SDL_NumJoysticks(); ++i) {
            if (SDL_IsGameController(i)) {
                controller_ = SDL_GameControllerOpen(i);
                if (controller_)
                    break;
            }
        }
    }
    ~VideoWindow() {
        if (controller_)
            SDL_GameControllerClose(controller_);
        if (controller_subsystem_)
            SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    }
    bool poll() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT)
                return false;
            if (event.type == SDL_KEYDOWN &&
                (event.key.keysym.sym == SDLK_F10 || (escape_quits_ && event.key.keysym.sym == SDLK_ESCAPE)))
                return false;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                held_.fill(false);
                pressed_.fill(false);
            }
            if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                const auto key = gml_key(event.key.keysym.sym);
                if (key >= 0) {
                    const auto index = static_cast<std::size_t>(key);
                    held_[index] = event.type == SDL_KEYDOWN;
                    if (event.type == SDL_KEYDOWN && !event.key.repeat)
                        pressed_[index] = true;
                }
            }
            if (controller_subsystem_ && event.type == SDL_CONTROLLERDEVICEADDED && !controller_ &&
                SDL_IsGameController(event.cdevice.which))
                controller_ = SDL_GameControllerOpen(event.cdevice.which);
            if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller_ &&
                SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller_)) == event.cdevice.which) {
                SDL_GameControllerClose(controller_);
                controller_ = nullptr;
                held_.fill(false);
                pressed_.fill(false);
            }
            if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP) {
                const auto key = gml_button(static_cast<SDL_GameControllerButton>(event.cbutton.button));
                if (key >= 0) {
                    const auto index = static_cast<std::size_t>(key);
                    held_[index] = event.type == SDL_CONTROLLERBUTTONDOWN;
                    if (event.type == SDL_CONTROLLERBUTTONDOWN)
                        pressed_[index] = true;
                }
                if (event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)
                    return false;
            }
        }
        return true;
    }
    template <class Session> void tick(Session &session, bool autoplay) {
        session.tick(autoplay ? demo_autoplay(session.frames()) : held_, autoplay ? KeyboardState{} : pressed_);
        pressed_.fill(false);
    }
    void present(const Canvas &canvas) {
        sdl_ok(SDL_UpdateTexture(texture_.get(), nullptr, canvas.pixels().data(),
                                 canvas.width() * static_cast<int>(sizeof(std::uint32_t))),
               "texture upload");
        sdl_ok(SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 255), "clear color");
        sdl_ok(SDL_RenderClear(renderer_.get()), "renderer clear");
        sdl_ok(SDL_RenderCopy(renderer_.get(), texture_.get(), nullptr, nullptr), "frame presentation");
        SDL_RenderPresent(renderer_.get());
    }
    void tick_game(GameSession &session, const KeyboardState *replay) {
        session.tick(replay ? *replay : held_, replay ? KeyboardState{} : pressed_);
        pressed_.fill(false);
    }

  private:
    static int gml_button(SDL_GameControllerButton button) {
        switch (button) {
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            return 37;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            return 38;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            return 39;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            return 40;
        case SDL_CONTROLLER_BUTTON_A:
            return 'Z';
        case SDL_CONTROLLER_BUTTON_B:
            return 'X';
        case SDL_CONTROLLER_BUTTON_X:
        case SDL_CONTROLLER_BUTTON_Y:
            return 'C';
        case SDL_CONTROLLER_BUTTON_START:
            return 13;
        default:
            return -1;
        }
    }
    // Members destroy in reverse order; SDL lifetime ends after all GPU/window resources.
    SdlLifetime lifetime_;
    std::unique_ptr<SDL_Window, WindowDelete> window_;
    std::unique_ptr<SDL_Renderer, RendererDelete> renderer_;
    std::unique_ptr<SDL_Texture, TextureDelete> texture_;
    SDL_GameController *controller_ = nullptr;
    bool controller_subsystem_ = false;
    KeyboardState held_{}, pressed_{};
    bool escape_quits_ = true;
};
class AudioDevice {
  public:
    explicit AudioDevice(double speed) {
        sdl_ok(SDL_InitSubSystem(SDL_INIT_AUDIO), "audio initialization");
        SDL_AudioSpec want{}, got{};
        // SDL devices commonly top out around 192 kHz. Simulation speed remains
        // unrestricted by the audio device's supported sample-rate range.
        want.freq = static_cast<int>(std::clamp(48000.0 * speed, 8000.0, 192000.0));
        want.format = AUDIO_F32SYS;
        want.channels = 2;
        want.samples = 1024;
        device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
        if (!device_)
            throw Error(ErrorKind::io, std::string("audio device: ") + SDL_GetError());
        SDL_PauseAudioDevice(device_, 0);
    }
    ~AudioDevice() {
        if (device_)
            SDL_CloseAudioDevice(device_);
    }
    void queue(std::span<const float> samples) {
        if (SDL_GetQueuedAudioSize(device_) > 48000 * 2 * sizeof(float))
            SDL_ClearQueuedAudio(device_);
        sdl_ok(SDL_QueueAudio(device_, samples.data(), static_cast<Uint32>(samples.size_bytes())), "audio queue");
    }

  private:
    SDL_AudioDeviceID device_ = 0;
};
#endif
} // namespace
PlayReport play_demo(const PlayOptions &options) {
    if (!std::isfinite(options.speed_multiplier) || options.speed_multiplier < 0.01 ||
        options.speed_multiplier > 1000.0)
        throw Error(ErrorKind::usage, "speed multiplier must be between 0.01 and 1000");
    if ((options.headless || options.hidden) && options.frame_limit == 0)
        throw Error(ErrorKind::usage, "--headless/--hidden requires a positive --frames limit");
    if (options.frame_limit > 1'000'000)
        throw Error(ErrorKind::usage, "frame limit must not exceed 1000000");
    DemoSession session;
    if (options.headless) {
        for (std::size_t i = 0; i < options.frame_limit; ++i) {
            session.tick(options.autoplay ? demo_autoplay(i) : KeyboardState{});
            session.draw();
        }
    } else {
#if REGML_WITH_SDL2
        VideoWindow window(options.hidden);
        using Clock = std::chrono::steady_clock;
        const double step = 1.0 / (60.0 * options.speed_multiplier);
        auto previous = Clock::now();
        double accumulated = step;
        bool running = true;
        window.present(session.canvas());
        while (running && (options.frame_limit == 0 || session.frames() < options.frame_limit)) {
            running = window.poll();
            if (!running)
                break;
            const auto now = Clock::now();
            accumulated += std::min(std::chrono::duration<double>(now - previous).count(), 0.25);
            previous = now;
            // Bounded catch-up preserves per-step input edges without unbounded work after a stall.
            std::size_t steps = 0;
            while (accumulated >= step && steps < 5 &&
                   (options.frame_limit == 0 || session.frames() < options.frame_limit)) {
                window.tick(session, options.autoplay);
                session.draw();
                accumulated -= step;
                ++steps;
            }
            if (steps != 0)
                window.present(session.canvas());
            if (steps == 5)
                accumulated = std::min(accumulated, step);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
#else
        throw Error(ErrorKind::unsupported,
                    "this build has no SDL2 window backend; rebuild with SDL2 or use --headless --frames N");
#endif
    }
    if (!options.screenshot.empty())
        session.canvas().save_bmp(options.screenshot);
    return {session.frames(),
            session.vm_instructions(),
            value_number(session.state().at("x")),
            value_number(session.state().at("y")),
            value_number(session.state().at("score")),
            value_number(session.state().at("lives")),
            session.canvas().hash(),
            {}};
}
PlayReport play_game(const std::filesystem::path &package, const PlayOptions &options) {
    std::ofstream trace;
    if (!options.trace_log.empty()) {
        trace.open(options.trace_log, std::ios::trunc);
        if (!trace)
            throw Error(ErrorKind::io, "runtime trace cannot be opened");
    }
    auto trace_stage = [&](std::string_view stage, const GameSession *session = nullptr) {
        if (!trace.is_open())
            return;
        trace << stage;
        if (session)
            trace << " frame=" << session->frames() << " instructions=" << session->instructions()
                  << " room=" << session->room_name();
        trace << '\n';
        trace.flush();
    };
    if (!std::isfinite(options.speed_multiplier) || options.speed_multiplier < 0.01 ||
        options.speed_multiplier > 1000.0)
        throw Error(ErrorKind::usage, "speed multiplier must be between 0.01 and 1000");
    if ((options.headless || options.hidden) && !options.frame_limit)
        throw Error(ErrorKind::usage, "hidden/headless requires --frames");
    if (options.frame_limit > 1'000'000)
        throw Error(ErrorKind::usage, "frame limit exceeds 1000000");
    trace_stage("load.begin");
    GameSession session(package, options.seed, options.save_directory);
    trace_stage("load.end", &session);
    std::map<std::size_t, KeyboardState> replay;
    if (!options.input_script.empty()) {
        std::ifstream file(options.input_script);
        if (!file)
            throw Error(ErrorKind::io, "input script cannot be opened");
        std::string line;
        std::size_t count = 0;
        while (std::getline(file, line)) {
            if (++count > 100000)
                throw Error(ErrorKind::usage, "input script exceeds line limit");
            if (line.empty() || line[0] == '#')
                continue;
            std::istringstream row(line);
            std::size_t frame = 0;
            if (!(row >> frame) || frame > 1000000)
                throw Error(ErrorKind::usage, "invalid replay frame");
            KeyboardState keys{};
            int key = 0;
            while (row >> key) {
                if (key < 0 || key > 255)
                    throw Error(ErrorKind::usage, "invalid replay key");
                keys[static_cast<std::size_t>(key)] = true;
            }
            if (!row.eof())
                throw Error(ErrorKind::usage, "invalid replay key text");
            replay[frame] = keys;
        }
    }
    KeyboardState replay_keys{};
    auto input = [&]() {
        if (const auto it = replay.find(session.frames()); it != replay.end())
            replay_keys = it->second;
        return replay_keys;
    };
    if (options.headless) {
        for (std::size_t i = 0; i < options.frame_limit; ++i)
            session.tick(!replay.empty() ? input() : options.autoplay ? demo_autoplay(i) : KeyboardState{}, {});
    } else {
#if REGML_WITH_SDL2
        trace_stage("video.begin", &session);
        VideoWindow window(options.hidden, false);
        trace_stage("audio.begin", &session);
        AudioDevice audio(options.speed_multiplier);
        trace_stage("audio.end", &session);
        auto next = std::chrono::steady_clock::now();
        while (options.frame_limit == 0 || session.frames() < options.frame_limit) {
            trace_stage("poll", &session);
            if (!window.poll())
                break;
            input();
            trace_stage("tick", &session);
            window.tick_game(session, replay.empty() ? nullptr : &replay_keys);
            trace_stage("present", &session);
            window.present(session.canvas());
            trace_stage("audio.queue", &session);
            audio.queue(session.audio());
            trace_stage("delay", &session);
            const auto frame_time = std::chrono::duration<double>(1.0 / (session.speed() * options.speed_multiplier));
            next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(frame_time);
            if (!options.hidden)
                std::this_thread::sleep_until(next);
        }
#else
        throw Error(ErrorKind::unsupported, "SDL2 backend is unavailable");
#endif
    }
    if (!options.screenshot.empty())
        session.canvas().save_bmp(options.screenshot);
    PlayReport report;
    report.frames = session.frames();
    report.instructions = session.instructions();
    report.frame_hash = session.canvas().hash();
    report.room = session.room_name();
    return report;
}
} // namespace regml
