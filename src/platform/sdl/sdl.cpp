#include "play.hpp"
#if REGML_WII
#include "audio/wii/output.hpp"
#elif REGML_MINIAUDIO
#include "audio/miniaudio/output.hpp"
#elif REGML_OPENAL
#include "audio/openal/output.hpp"
#endif
#if REGML_WII
#include "platform/wii/host.hpp"
#include "platform/wii/renderer.hpp"
#endif
#include "game.hpp"
#include "lookup.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <thread>
#if REGML_WITH_SDL2
#include <SDL2/SDL.h>
#endif
namespace regml {
namespace {
class FrameProfileLog {
  public:
    explicit FrameProfileLog(const std::filesystem::path &path) {
        if (path.empty())
            return;
        file_.open(path, std::ios::trunc);
        if (!file_)
            throw Error(ErrorKind::io, "frame profile cannot be opened");
        file_ << "frame,room,samples,frame_ms,tick_ms,present_ms,max_tick_ms,max_present_ms,instructions_per_frame\n";
        file_ << std::fixed << std::setprecision(3);
    }
    ~FrameProfileLog() { flush(); }
    bool enabled() const noexcept { return file_.is_open(); }
    void sample(std::size_t frame, const std::string &room, std::size_t instructions, double frame_ms, double tick_ms,
                double present_ms) {
        if (room != room_) {
            flush();
            room_ = room;
        }
        frame_ = frame;
        ++samples_;
        instructions_ += instructions;
        frame_ms_ += frame_ms;
        tick_ms_ += tick_ms;
        present_ms_ += present_ms;
        max_tick_ms_ = std::max(max_tick_ms_, tick_ms);
        max_present_ms_ = std::max(max_present_ms_, present_ms);
        if (samples_ == 30)
            flush();
    }

  private:
    void flush() {
        if (!samples_)
            return;
        const auto count = static_cast<double>(samples_);
        file_ << frame_ << ',' << std::quoted(room_) << ',' << samples_ << ',' << frame_ms_ / count << ','
              << tick_ms_ / count << ',' << present_ms_ / count << ',' << max_tick_ms_ << ',' << max_present_ms_ << ','
              << static_cast<double>(instructions_) / count << '\n';
        file_.flush();
        samples_ = instructions_ = 0;
        frame_ms_ = tick_ms_ = present_ms_ = max_tick_ms_ = max_present_ms_ = 0;
    }
    std::ofstream file_;
    std::string room_;
    std::size_t frame_ = 0, samples_ = 0, instructions_ = 0;
    double frame_ms_ = 0, tick_ms_ = 0, present_ms_ = 0, max_tick_ms_ = 0, max_present_ms_ = 0;
};
#if REGML_WITH_SDL2
void sdl_ok(int status, std::string_view operation) {
    if (status < 0)
        throw Error(ErrorKind::io, std::string(operation) + ": " + SDL_GetError());
}
struct SdlLifetime {
    SdlLifetime() {
        SDL_SetMainReady();
#if REGML_WII
        sdl_ok(SDL_Init(SDL_INIT_TIMER), "SDL timer initialization");
#else
        sdl_ok(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER), "SDL initialization");
#endif
    }
    ~SdlLifetime() { SDL_Quit(); }
    SdlLifetime(const SdlLifetime &) = delete;
    SdlLifetime &operator=(const SdlLifetime &) = delete;
};
#if !REGML_WII
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
    explicit VideoWindow(bool hidden, bool escape_quits = true, bool speed_controls = false)
        : escape_quits_(escape_quits), speed_controls_(speed_controls) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
#if REGML_WII
        const auto flags = static_cast<Uint32>(SDL_WINDOW_FULLSCREEN | (hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN));
        constexpr int window_width = 640, window_height = 480;
#else
        const auto flags = static_cast<Uint32>(SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                               (hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN));
        constexpr int window_width = 800, window_height = 600;
#endif
        window_.reset(SDL_CreateWindow(escape_quits ? "reGML - VM Arcade" : "reGML - Game (experimental)",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, window_width, window_height,
                                       flags));
        if (!window_)
            throw Error(ErrorKind::io, std::string("window creation: ") + SDL_GetError());
        renderer_.reset(SDL_CreateRenderer(window_.get(), -1, SDL_RENDERER_ACCELERATED));
        if (!renderer_)
            renderer_.reset(SDL_CreateRenderer(window_.get(), -1, SDL_RENDERER_SOFTWARE));
        if (!renderer_)
            throw Error(ErrorKind::io, std::string("renderer creation: ") + SDL_GetError());
        sdl_ok(SDL_RenderSetLogicalSize(renderer_.get(), 640, 480), "logical resolution");
        texture_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                         render_width, render_height));
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
    bool poll(double *speed_multiplier = nullptr) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT)
                return false;
            if (event.type == SDL_KEYDOWN &&
                (event.key.keysym.sym == SDLK_F10 || (escape_quits_ && event.key.keysym.sym == SDLK_ESCAPE)))
                return false;
            if (speed_controls_ && speed_multiplier && event.type == SDL_KEYDOWN && !event.key.repeat) {
                if (event.key.keysym.sym == SDLK_LEFTBRACKET || event.key.keysym.sym == SDLK_RIGHTBRACKET) {
                    *speed_multiplier = std::clamp(
                        *speed_multiplier * (event.key.keysym.sym == SDLK_LEFTBRACKET ? 0.5 : 2.0), 0.01, 1000.0);
                    std::ostringstream title;
                    title << "reGML - Game (" << std::setprecision(3) << *speed_multiplier << "x)";
                    SDL_SetWindowTitle(window_.get(), title.str().c_str());
                    continue;
                }
            }
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
    // Keep SDL alive until the window and renderer are destroyed.
    SdlLifetime lifetime_;
    std::unique_ptr<SDL_Window, WindowDelete> window_;
    std::unique_ptr<SDL_Renderer, RendererDelete> renderer_;
    std::unique_ptr<SDL_Texture, TextureDelete> texture_;
    SDL_GameController *controller_ = nullptr;
    bool controller_subsystem_ = false;
    KeyboardState held_{}, pressed_{};
    bool escape_quits_ = true;
    bool speed_controls_ = false;
};
#endif
#endif
#if REGML_WII && !REGML_WITH_SDL2
class VideoWindow {
  public:
    explicit VideoWindow(bool hidden, bool escape_quits = true, bool speed_controls = false) {
        (void)hidden;
        (void)escape_quits;
        (void)speed_controls;
    }
    bool poll(double *speed_multiplier = nullptr) {
        (void)speed_multiplier;
        return input_.poll();
    }
    template <class Session> void tick(Session &session, bool autoplay) {
        const auto pressed = input_.take_pressed();
        session.tick(autoplay ? demo_autoplay(session.frames()) : input_.held(), autoplay ? KeyboardState{} : pressed);
    }
    void tick_game(GameSession &session, const KeyboardState *replay) {
        session.canvas().gpu_target(true);
        const auto pressed = input_.take_pressed();
        session.tick(replay ? *replay : input_.held(), replay ? KeyboardState{} : pressed);
    }
    void present(const Canvas &canvas) { renderer_.present(canvas); }

  private:
    WiiRenderer renderer_;
    WiiInput input_;
};
#endif
#if REGML_WITH_SDL2 || REGML_WII
class AudioDevice {
  public:
#if REGML_WII
    AudioDevice(GameSession &session, double speed) : output_(session, speed) {}
    void set_speed(double speed) { output_.set_speed(speed); }
    void pump() { output_.pump(); }

  private:
    WiiAudioOutput output_;
#elif REGML_MINIAUDIO
    AudioDevice(GameSession &session, double speed) : output_(session, speed) {}
    void set_speed(double speed) { output_.set_speed(speed); }
    void pump() {}

  private:
    MiniAudioOutput output_;
#elif REGML_OPENAL
    AudioDevice(GameSession &session, double speed) : output_(session, speed) {}
    void set_speed(double speed) { output_.set_speed(speed); }
    void pump() { output_.pump(); }

  private:
    OpenALOutput output_;
#else
    AudioDevice(GameSession &session, double speed) : session_(session) {
        sdl_ok(SDL_InitSubSystem(SDL_INIT_AUDIO), "audio initialization");
        SDL_AudioSpec want{}, got{};
        want.freq = static_cast<int>(std::clamp(48000.0 * speed, 8000.0, 192000.0));
        want.format = AUDIO_F32SYS;
        want.channels = 2;
        want.samples = 1024;
#if !REGML_WII
        want.callback = &AudioDevice::callback;
        want.userdata = this;
#endif
        device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
        if (!device_)
            throw Error(ErrorKind::io, std::string("audio device: ") + SDL_GetError());
        obtained_frequency_ = got.freq;
        playback_rate_ = speed * 48000.0 / obtained_frequency_;
        SDL_PauseAudioDevice(device_, 0);
    }
    ~AudioDevice() {
        if (device_)
            SDL_CloseAudioDevice(device_);
    }
    void set_speed(double speed) {
        SDL_LockAudioDevice(device_);
        playback_rate_ = speed * 48000.0 / obtained_frequency_;
        SDL_UnlockAudioDevice(device_);
    }
    void pump() {
#if REGML_WII
        constexpr std::size_t block_samples = 2048;
        constexpr std::size_t queued_bytes = 8192 * 2 * sizeof(float);
        while (SDL_GetQueuedAudioSize(device_) < queued_bytes) {
            session_.mix_audio(audio_buffer_, playback_rate_);
            sdl_ok(SDL_QueueAudio(device_, audio_buffer_.data(), static_cast<Uint32>(block_samples * sizeof(float))),
                   "audio queue");
        }
#endif
    }

  private:
    static void callback(void *context, Uint8 *stream, int length) noexcept {
        auto &device = *static_cast<AudioDevice *>(context);
        const auto bytes = static_cast<std::size_t>(std::max(0, length));
        const auto sample_count = bytes / sizeof(float);
        auto output = std::span<float>(reinterpret_cast<float *>(stream), sample_count);
        device.session_.mix_audio(output, device.playback_rate_);
        if (bytes % sizeof(float) != 0)
            stream[bytes - 1] = 0;
    }
    GameSession &session_;
    double playback_rate_ = 1;
    int obtained_frequency_ = 48000;
    SDL_AudioDeviceID device_ = 0;
#if REGML_WII
    std::array<float, 2048> audio_buffer_{};
#endif
#endif
};
#endif
void draw_debug_overlay(GameSession &session, double frame_ms) {
    std::ostringstream metrics;
    metrics << std::fixed << std::setprecision(1) << debug_overlay_fps_label << (1000.0 / std::max(frame_ms, 0.1))
            << "  " << debug_overlay_frame_label << session.frames() << "  " << frame_ms
            << debug_overlay_milliseconds_label;
    std::ostringstream address;
    address << debug_overlay_address_label << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
            << session.vm_address();
    const auto code = session.vm_code_name();
    std::array<std::string, 4> storage{metrics.str(), std::string(debug_overlay_room_label) + session.room_name(),
                                       std::string(debug_overlay_code_label) + std::string(code.empty() ? "-" : code),
                                       address.str()};
    std::array<std::string_view, 4> lines{storage[0], storage[1], storage[2], storage[3]};
    session.canvas().debug_text_overlay(lines);
}
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
#if REGML_WITH_SDL2 || REGML_WII
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
            // Limit catch-up after a stall; retain input edges for each step.
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
    FrameProfileLog profile(options.profile_log);
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
        std::vector<float> silent_audio;
        for (std::size_t i = 0; i < options.frame_limit; ++i) {
            using Clock = std::chrono::steady_clock;
            const auto start = profile.enabled() ? Clock::now() : Clock::time_point{};
            const auto instructions_begin = session.instructions();
            session.tick(!replay.empty() ? input() : options.autoplay ? demo_autoplay(i) : KeyboardState{}, {});
            const auto tick_end = profile.enabled() ? Clock::now() : Clock::time_point{};
            silent_audio.resize(static_cast<std::size_t>(96000 / session.speed()));
            session.mix_audio(silent_audio);
            if (profile.enabled())
                profile.sample(session.frames(), session.room_name(), session.instructions() - instructions_begin,
                               std::chrono::duration<double, std::milli>(Clock::now() - start).count(),
                               std::chrono::duration<double, std::milli>(tick_end - start).count(), 0);
        }
    } else {
#if REGML_WITH_SDL2 || REGML_WII
        trace_stage("video.begin", &session);
        VideoWindow window(options.hidden, false, options.speed_controls);
        trace_stage("audio.begin", &session);
        AudioDevice audio(session, options.speed_multiplier);
        trace_stage("audio.end", &session);
        auto next = std::chrono::steady_clock::now();
        auto previous_frame = next;
        double debug_frame_ms = 1000.0 / std::max(session.speed(), 1.0);
        auto speed_multiplier = options.speed_multiplier;
        while (options.frame_limit == 0 || session.frames() < options.frame_limit) {
            using Clock = std::chrono::steady_clock;
            const auto frame_begin = profile.enabled() ? Clock::now() : Clock::time_point{};
            if (options.debug_overlay) {
                const auto now = Clock::now();
                debug_frame_ms = std::chrono::duration<double, std::milli>(now - previous_frame).count();
                previous_frame = now;
            }
            const auto instructions_begin = session.instructions();
            trace_stage("poll", &session);
            const auto previous_speed_multiplier = speed_multiplier;
            if (!window.poll(&speed_multiplier))
                break;
            if (speed_multiplier != previous_speed_multiplier)
                audio.set_speed(speed_multiplier);
            input();
            trace_stage("tick", &session);
            const auto tick_begin = profile.enabled() ? Clock::now() : Clock::time_point{};
            window.tick_game(session, replay.empty() ? nullptr : &replay_keys);
            audio.pump();
            const auto tick_end = profile.enabled() ? Clock::now() : Clock::time_point{};
            if (options.debug_overlay)
                draw_debug_overlay(session, debug_frame_ms);
            trace_stage("present", &session);
            window.present(session.canvas());
#if REGML_WII
            if (!options.trace_log.empty()) {
                if (session.frames() <= 10 || session.frames() % 30 == 0)
                    report_wii_frame(session.frames(), session.room_name());
                if (session.frames() == 30)
                    session.canvas().save_bmp(package.parent_path() / "regml-frame.bmp");
            }
#endif
            const auto present_end = profile.enabled() ? Clock::now() : Clock::time_point{};
            trace_stage("delay", &session);
            const auto frame_time = std::chrono::duration<double>(1.0 / (session.speed() * speed_multiplier));
            next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(frame_time);
            if (!options.hidden) {
                // Drop missed deadlines instead of replaying the backlog at full speed.
                next = std::max(next, std::chrono::steady_clock::now());
                std::this_thread::sleep_until(next);
            }
            if (profile.enabled()) {
                const auto ms = [](auto duration) {
                    return std::chrono::duration<double, std::milli>(duration).count();
                };
                profile.sample(session.frames(), session.room_name(), session.instructions() - instructions_begin,
                               ms(Clock::now() - frame_begin), ms(tick_end - tick_begin), ms(present_end - tick_end));
            }
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
