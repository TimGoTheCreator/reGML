#include "play.hpp"
#if REGML_WII
#include "audio/wii/output.hpp"
#elif REGML_MINIAUDIO
#include "audio/miniaudio/output.hpp"
#elif REGML_OPENAL
#include "audio/openal/output.hpp"
#endif
#if REGML_PS2
#include "audio/ps2/output.hpp"
#endif
#if REGML_WII
#include "platform/wii/host.hpp"
#include "platform/wii/renderer.hpp"
#endif
#if REGML_PS2
#include "platform/ps2/frontend.hpp"
#include "platform/ps2/gpu.hpp"
#endif
#if REGML_PS4
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#endif
#include "error.hpp"
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
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#if REGML_WITH_SDL2
#include <SDL2/SDL.h>
#endif
#if REGML_WEB
#include <emscripten.h>
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
        file_ << "frame,room,samples,frame_ms,tick_ms,present_ms,logic_ms,draw_ms,image_ms,text_ms,image_"
                 "calls,image_quads,glyph_quads,texture_uploads,max_tick_ms,max_present_ms,instructions_per_"
                 "frame,draw_event_ms,draw_builtin_ms,draw_text_builtin_ms,draw_image_builtin_ms,draw_"
                 "primitive_builtin_ms,draw_other_builtin_ms,max_draw_builtin_call_ms,slowest_draw_builtin\n";
        file_ << std::fixed << std::setprecision(3);
    }
    ~FrameProfileLog() { flush(); }
    bool enabled() const noexcept { return file_.is_open(); }
    void sample(std::size_t frame,
                const std::string &room,
                std::size_t instructions,
                double frame_ms,
                double tick_ms,
                double present_ms,
                double logic_ms,
                double draw_ms,
                const GameSession::TickProfile &tick_profile) {
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
        logic_ms_ += logic_ms;
        draw_ms_ += draw_ms;
        draw_event_ms_ += tick_profile.draw_event_ms;
        draw_builtin_ms_ += tick_profile.draw_builtin_ms;
        draw_text_builtin_ms_ += tick_profile.draw_text_builtin_ms;
        draw_image_builtin_ms_ += tick_profile.draw_image_builtin_ms;
        draw_primitive_builtin_ms_ += tick_profile.draw_primitive_builtin_ms;
        draw_other_builtin_ms_ += tick_profile.draw_other_builtin_ms;
        if (tick_profile.max_draw_builtin_call_ms > max_draw_builtin_call_ms_) {
            max_draw_builtin_call_ms_ = tick_profile.max_draw_builtin_call_ms;
            slowest_draw_builtin_ = tick_profile.slowest_draw_builtin;
        }
        image_ms_ += tick_profile.image_ms;
        text_ms_ += tick_profile.text_ms;
        image_calls_ += tick_profile.image_calls;
        image_quads_ += tick_profile.image_quads;
        glyph_quads_ += tick_profile.glyph_quads;
        texture_uploads_ += tick_profile.texture_uploads;
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
              << tick_ms_ / count << ',' << present_ms_ / count << ',' << logic_ms_ / count << ','
              << draw_ms_ / count << ',' << image_ms_ / count << ',' << text_ms_ / count << ','
              << image_calls_ / count << ',' << image_quads_ / count << ',' << glyph_quads_ / count << ','
              << texture_uploads_ / count << ',' << max_tick_ms_ << ',' << max_present_ms_ << ','
              << static_cast<double>(instructions_) / count << ',' << draw_event_ms_ / count << ','
              << draw_builtin_ms_ / count << ',' << draw_text_builtin_ms_ / count << ','
              << draw_image_builtin_ms_ / count << ',' << draw_primitive_builtin_ms_ / count << ','
              << draw_other_builtin_ms_ / count << ',' << max_draw_builtin_call_ms_ << ','
              << std::quoted(slowest_draw_builtin_) << '\n';
        file_.flush();
        samples_ = instructions_ = 0;
        frame_ms_ = tick_ms_ = present_ms_ = logic_ms_ = draw_ms_ = max_tick_ms_ = max_present_ms_ = 0;
        draw_event_ms_ = 0;
        draw_builtin_ms_ = 0;
        draw_text_builtin_ms_ = draw_image_builtin_ms_ = draw_primitive_builtin_ms_ = draw_other_builtin_ms_ =
            0;
        max_draw_builtin_call_ms_ = 0;
        slowest_draw_builtin_.clear();
        image_ms_ = 0;
        text_ms_ = 0;
        image_calls_ = 0;
        image_quads_ = glyph_quads_ = texture_uploads_ = 0;
    }
    std::ofstream file_;
    std::string room_;
    std::size_t frame_ = 0, samples_ = 0, instructions_ = 0;
    double frame_ms_ = 0, tick_ms_ = 0, present_ms_ = 0, logic_ms_ = 0, draw_ms_ = 0, max_tick_ms_ = 0,
           max_present_ms_ = 0, image_ms_ = 0, text_ms_ = 0;
    double draw_event_ms_ = 0;
    double draw_builtin_ms_ = 0;
    double draw_text_builtin_ms_ = 0, draw_image_builtin_ms_ = 0, draw_primitive_builtin_ms_ = 0,
           draw_other_builtin_ms_ = 0;
    double max_draw_builtin_call_ms_ = 0;
    std::string slowest_draw_builtin_;
    std::uint64_t image_calls_ = 0, image_quads_ = 0, glyph_quads_ = 0, texture_uploads_ = 0;
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
        const auto flags =
            static_cast<Uint32>(SDL_WINDOW_FULLSCREEN | (hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN));
        constexpr int window_width = 640, window_height = 480;
#else
        const auto flags = static_cast<Uint32>(SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                               (hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN));
        constexpr int window_width = 640, window_height = 480;
#endif
        window_.reset(SDL_CreateWindow(escape_quits ? "reGML - VM Arcade" : "reGML - Game (experimental)",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, window_width,
                                       window_height, flags));
        if (!window_)
            throw Error(ErrorKind::io, std::string("window creation: ") + SDL_GetError());
        renderer_.reset(SDL_CreateRenderer(window_.get(), -1, SDL_RENDERER_ACCELERATED));
        if (!renderer_)
            renderer_.reset(SDL_CreateRenderer(window_.get(), -1, SDL_RENDERER_SOFTWARE));
        if (!renderer_)
            throw Error(ErrorKind::io, std::string("renderer creation: ") + SDL_GetError());
        sdl_ok(SDL_RenderSetLogicalSize(renderer_.get(), 640, 480), "logical resolution");
        texture_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, render_width, render_height));
        if (!texture_)
            throw Error(ErrorKind::io, std::string("framebuffer texture: ") + SDL_GetError());
        sdl_ok(SDL_SetTextureBlendMode(texture_.get(), SDL_BLENDMODE_NONE), "texture blend mode");
#if REGML_PS4
        OrbisUserServiceInitializeParams params{};
        params.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
        sceUserServiceInitialize(&params);
        int user = -1;
        if (scePadInit() >= 0 && sceUserServiceGetInitialUser(&user) >= 0) {
            pad_ = scePadOpen(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
            poll_ps4_pad();
        }
#else
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) >= 0) {
            controller_subsystem_ = true;
        }
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            if (controller_subsystem_ && SDL_IsGameController(i)) {
                controller_ = SDL_GameControllerOpen(i);
                if (controller_)
                    break;
            }
        }
#endif
    }
    ~VideoWindow() {
        if (controller_)
            SDL_GameControllerClose(controller_);
#if REGML_PS4
        if (pad_ >= 0)
            scePadClose(pad_);
#endif
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
                    *speed_multiplier =
                        std::clamp(*speed_multiplier * (event.key.keysym.sym == SDLK_LEFTBRACKET ? 0.5 : 2.0),
                                   0.01, 1000.0);
                    std::ostringstream title;
                    title << "reGML - Game (" << std::setprecision(3) << *speed_multiplier << "x)";
                    SDL_SetWindowTitle(window_.get(), title.str().c_str());
                    continue;
                }
            }
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                keyboard_held_.fill(false);
                keyboard_pressed_.fill(false);
                controller_buttons_held_.fill(false);
                controller_buttons_pressed_.fill(false);
                controller_axes_held_.fill(false);
                controller_axes_pressed_.fill(false);
                joystick_buttons_held_.fill(false);
                joystick_buttons_pressed_.fill(false);
                joystick_hat_held_.fill(false);
                joystick_hat_pressed_.fill(false);
                joystick_axes_held_.fill(false);
                joystick_axes_pressed_.fill(false);
            }
            if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                const auto key = gml_key(event.key.keysym.sym);
                update_source_key(keyboard_held_, keyboard_pressed_, key, event.type == SDL_KEYDOWN,
                                  !event.key.repeat);
            }
            if (controller_subsystem_ && event.type == SDL_CONTROLLERDEVICEADDED && !controller_ &&
                SDL_IsGameController(event.cdevice.which))
                controller_ = SDL_GameControllerOpen(event.cdevice.which);
            if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller_ &&
                SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller_)) == event.cdevice.which) {
                SDL_GameControllerClose(controller_);
                controller_ = nullptr;
                controller_buttons_held_.fill(false);
                controller_buttons_pressed_.fill(false);
                controller_axes_held_.fill(false);
                controller_axes_pressed_.fill(false);
            }
            if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP) {
                const auto key = gml_button(static_cast<SDL_GameControllerButton>(event.cbutton.button));
                update_source_key(controller_buttons_held_, controller_buttons_pressed_, key,
                                  event.type == SDL_CONTROLLERBUTTONDOWN);
                if (event.type == SDL_CONTROLLERBUTTONDOWN &&
                    event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)
                    return false;
            }
            if (event.type == SDL_CONTROLLERAXISMOTION) {
                update_controller_axis(static_cast<SDL_GameControllerAxis>(event.caxis.axis),
                                       event.caxis.value, controller_axes_held_, controller_axes_pressed_);
            }
        }
#if REGML_PS4
        poll_ps4_pad();
#endif
        rebuild_input_state();
        return true;
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
    void show_error_reporter(std::string_view title, std::string_view report) noexcept {
        try {
            Canvas canvas(640, 480, true);
            std::vector<std::string> lines;
            std::istringstream input{std::string(report)};
            constexpr std::size_t line_width = 98;
            std::string paragraph;
            while (std::getline(input, paragraph)) {
                if (paragraph.empty()) {
                    lines.emplace_back();
                    continue;
                }
                std::size_t start = 0;
                while (start < paragraph.size()) {
                    std::size_t end = std::min(start + line_width, paragraph.size());
                    if (end < paragraph.size()) {
                        const auto space = paragraph.rfind(' ', end);
                        if (space != std::string::npos && space > start)
                            end = space;
                    }
                    lines.emplace_back(paragraph.substr(start, end - start));
                    start = end;
                    while (start < paragraph.size() && paragraph[start] == ' ')
                        ++start;
                    if (lines.size() >= 5000) {
                        lines.emplace_back("(error report truncated)");
                        break;
                    }
                }
                if (lines.size() >= 5000)
                    break;
            }
            if (lines.empty())
                lines.emplace_back("No error details were provided.");

            constexpr int first_line_y = 82, line_height = 9, visible_lines = 36;
            int scroll = 0;
            std::string status = "C copies the report   X clears it   Esc aborts";
            auto draw_button = [&](int left, int right, std::string_view label) {
                canvas.color(0x424242);
                canvas.rectangle(left, 438, right, 466, false);
                canvas.color(0xb0b0b0);
                canvas.rectangle(left, 438, right, 466, true);
                canvas.color(0xffffff);
                canvas.text(left + 7, 448, label);
            };
            auto redraw = [&] {
                canvas.clear(0x171717);
                canvas.color(0x303030);
                canvas.rectangle(8, 8, 632, 472, false);
                canvas.color(0xaaaaaa);
                canvas.rectangle(8, 8, 632, 472, true);
                canvas.color(0x0000a8);
                canvas.rectangle(9, 9, 631, 42, false);
                canvas.color(0xffffff);
                canvas.text(20, 20, title.empty() ? "reGML Runtime Error" : title);
                canvas.color(0xbcbcbc);
                canvas.text(20, 53, "The game stopped while reGML was running it.");
                canvas.color(0x202020);
                canvas.rectangle(16, 72, 624, 412, false);
                canvas.color(0x777777);
                canvas.rectangle(16, 72, 624, 412, true);
                canvas.color(0xffffff);
                for (int row = 0; row < visible_lines; ++row) {
                    const auto index = static_cast<std::size_t>(scroll + row);
                    if (index >= lines.size())
                        break;
                    canvas.text(22, first_line_y + row * line_height, lines[index]);
                }
                canvas.color(0xcccccc);
                canvas.text(20, 421, status);
                draw_button(360, 444, "COPY [C]");
                draw_button(450, 534, "CLEAR [X]");
                draw_button(540, 624, "ABORT [ESC]");
                present(canvas);
            };

            SDL_SetWindowTitle(window_.get(), "reGML - Runtime Error");
            bool open = true;
            redraw();
            while (open) {
                SDL_Event event;
                bool changed = false;
                while (SDL_PollEvent(&event)) {
                    if (event.type == SDL_QUIT) {
                        open = false;
                    } else if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                        const auto key = event.key.keysym.sym;
                        if (key == SDLK_ESCAPE || key == SDLK_RETURN || key == SDLK_KP_ENTER ||
                            key == SDLK_F10) {
                            open = false;
                        } else if (key == SDLK_c) {
                            if (SDL_SetClipboardText(std::string(report).c_str()) == 0)
                                status = "Copied the report to the clipboard.";
                            else
                                status = std::string("Clipboard copy failed: ") + SDL_GetError();
                            changed = true;
                        } else if (key == SDLK_x) {
                            lines.clear();
                            scroll = 0;
                            status = "Error details cleared.";
                            changed = true;
                        } else if (key == SDLK_UP || key == SDLK_PAGEUP) {
                            scroll = std::max(0, scroll - (key == SDLK_PAGEUP ? visible_lines - 1 : 1));
                            changed = true;
                        } else if (key == SDLK_DOWN || key == SDLK_PAGEDOWN) {
                            scroll = std::min(std::max(0, static_cast<int>(lines.size()) - visible_lines),
                                              scroll + (key == SDLK_PAGEDOWN ? visible_lines - 1 : 1));
                            changed = true;
                        }
                    } else if (event.type == SDL_MOUSEWHEEL) {
                        scroll = std::clamp(scroll - event.wheel.y, 0,
                                            std::max(0, static_cast<int>(lines.size()) - visible_lines));
                        changed = true;
                    } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                        int width = 640, height = 480;
                        SDL_GetWindowSize(window_.get(), &width, &height);
                        const int x = width ? event.button.x * 640 / width : event.button.x;
                        const int y = height ? event.button.y * 480 / height : event.button.y;
                        if (y >= 438 && y <= 466 && x >= 360 && x <= 444) {
                            if (SDL_SetClipboardText(std::string(report).c_str()) == 0)
                                status = "Copied the report to the clipboard.";
                            else
                                status = std::string("Clipboard copy failed: ") + SDL_GetError();
                            changed = true;
                        } else if (y >= 438 && y <= 466 && x >= 450 && x <= 534) {
                            lines.clear();
                            scroll = 0;
                            status = "Error details cleared.";
                            changed = true;
                        } else if (y >= 438 && y <= 466 && x >= 540 && x <= 624) {
                            open = false;
                        }
                    }
                }
                if (!open)
                    break;
                if (changed)
                    redraw();
                SDL_Delay(16);
            }
        } catch (...) {
        }
    }
    void tick_game(GameSession &session,
                   const KeyboardState *replay,
                   const KeyboardState *replay_pressed = nullptr) {
#if REGML_PS4
        session.set_gamepad_state(gamepad_);
        gamepad_.pressed.fill(false);
#endif
        session.tick(replay ? *replay : held_,
                     replay ? (replay_pressed ? *replay_pressed : KeyboardState{}) : pressed_);
        pressed_.fill(false);
    }
#if REGML_PS4
    const GamepadState &gamepad_state() const { return gamepad_; }
#endif

  private:
    void update_source_key(KeyboardState &source_held,
                           KeyboardState &source_pressed,
                           int key,
                           bool down,
                           bool detect_press = true) {
        if (key < 0 || key >= static_cast<int>(source_held.size()))
            return;
        const auto index = static_cast<std::size_t>(key);
        if (down && detect_press && !source_held[index])
            source_pressed[index] = true;
        source_held[index] = down;
    }
    void update_axis_pair(int negative_key,
                          int positive_key,
                          Sint16 value,
                          KeyboardState &source_held,
                          KeyboardState &source_pressed) {
        constexpr Sint16 deadzone = 8000;
        update_source_key(source_held, source_pressed, negative_key, value < -deadzone);
        update_source_key(source_held, source_pressed, positive_key, value > deadzone);
    }
    void update_controller_axis(SDL_GameControllerAxis axis,
                                Sint16 value,
                                KeyboardState &source_held,
                                KeyboardState &source_pressed) {
        if (axis == SDL_CONTROLLER_AXIS_LEFTX)
            update_axis_pair(37, 39, value, source_held, source_pressed);
        else if (axis == SDL_CONTROLLER_AXIS_LEFTY)
            update_axis_pair(38, 40, value, source_held, source_pressed);
    }
#if REGML_PS4
    void poll_ps4_pad() {
        OrbisPadData data{};
        const bool connected = pad_ >= 0 && scePadReadState(pad_, &data) >= 0 && data.connected;
        static constexpr std::array<std::uint32_t, 16> masks = {
            ORBIS_PAD_BUTTON_CROSS, ORBIS_PAD_BUTTON_CIRCLE, ORBIS_PAD_BUTTON_SQUARE,
            ORBIS_PAD_BUTTON_TRIANGLE, ORBIS_PAD_BUTTON_L1, ORBIS_PAD_BUTTON_R1,
            ORBIS_PAD_BUTTON_L2, ORBIS_PAD_BUTTON_R2, ORBIS_PAD_BUTTON_TOUCH_PAD,
            ORBIS_PAD_BUTTON_OPTIONS, ORBIS_PAD_BUTTON_L3, ORBIS_PAD_BUTTON_R3,
            ORBIS_PAD_BUTTON_UP, ORBIS_PAD_BUTTON_DOWN, ORBIS_PAD_BUTTON_LEFT, ORBIS_PAD_BUTTON_RIGHT};
        gamepad_.connected = connected;
        for (std::size_t i = 0; i < masks.size(); ++i) {
            const bool down = connected && (data.buttons & masks[i]);
            gamepad_.pressed[i] = gamepad_.pressed[i] || (down && !gamepad_.buttons[i]);
            gamepad_.buttons[i] = down;
        }
        const auto axis = [connected](std::uint8_t value) {
            if (!connected)
                return 0.0;
            const double normalized = (static_cast<int>(value) - 128) / 127.0;
            return std::clamp(normalized, -1.0, 1.0);
        };
        gamepad_.axes = {axis(data.leftStick.x), axis(data.leftStick.y), axis(data.rightStick.x),
                         axis(data.rightStick.y)};
        const std::array<std::pair<int, int>, 7> keys = {{{'Z', 0}, {'X', 1}, {'C', 2}, {13, 9},
                                                           {38, 12}, {40, 13}, {37, 14}}};
        for (const auto &[key, button] : keys)
            update_source_key(joystick_buttons_held_, joystick_buttons_pressed_, key, gamepad_.buttons[button]);
        update_source_key(joystick_buttons_held_, joystick_buttons_pressed_, 39, gamepad_.buttons[15]);
        update_source_key(joystick_axes_held_, joystick_axes_pressed_, 37, gamepad_.axes[0] < -0.35);
        update_source_key(joystick_axes_held_, joystick_axes_pressed_, 39, gamepad_.axes[0] > 0.35);
        update_source_key(joystick_axes_held_, joystick_axes_pressed_, 38, gamepad_.axes[1] < -0.35);
        update_source_key(joystick_axes_held_, joystick_axes_pressed_, 40, gamepad_.axes[1] > 0.35);
    }
#endif
    void rebuild_input_state() {
        for (std::size_t i = 0; i < held_.size(); ++i) {
            const bool was_held = held_[i];
            held_[i] = keyboard_held_[i] || controller_buttons_held_[i] || controller_axes_held_[i] ||
                       joystick_buttons_held_[i] || joystick_hat_held_[i] || joystick_axes_held_[i];
            pressed_[i] = pressed_[i] || keyboard_pressed_[i] || controller_buttons_pressed_[i] ||
                          controller_axes_pressed_[i] || joystick_buttons_pressed_[i] ||
                          joystick_hat_pressed_[i] || joystick_axes_pressed_[i] || (held_[i] && !was_held);
        }
        keyboard_pressed_.fill(false);
        controller_buttons_pressed_.fill(false);
        controller_axes_pressed_.fill(false);
        joystick_buttons_pressed_.fill(false);
        joystick_hat_pressed_.fill(false);
        joystick_axes_pressed_.fill(false);
    }
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
    // SDL needs to outlive the window and renderer
    SdlLifetime lifetime_;
    std::unique_ptr<SDL_Window, WindowDelete> window_;
    std::unique_ptr<SDL_Renderer, RendererDelete> renderer_;
    std::unique_ptr<SDL_Texture, TextureDelete> texture_;
    SDL_GameController *controller_ = nullptr;
#if REGML_PS4
    int pad_ = -1;
    GamepadState gamepad_{};
#endif
    bool controller_subsystem_ = false;
    KeyboardState held_{}, pressed_{};
    KeyboardState keyboard_held_{}, keyboard_pressed_{};
    KeyboardState controller_buttons_held_{}, controller_buttons_pressed_{};
    KeyboardState controller_axes_held_{}, controller_axes_pressed_{};
    KeyboardState joystick_buttons_held_{}, joystick_buttons_pressed_{};
    KeyboardState joystick_hat_held_{}, joystick_hat_pressed_{};
    KeyboardState joystick_axes_held_{}, joystick_axes_pressed_{};
    bool escape_quits_ = true;
    bool speed_controls_ = false;
};
#endif
#endif
#if REGML_PS2
using VideoWindow = Ps2Frontend;
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
    void tick_game(GameSession &session,
                   const KeyboardState *replay,
                   const KeyboardState *replay_pressed = nullptr) {
        session.canvas().gpu_target(true);
        const auto pressed = input_.take_pressed();
        session.tick(replay ? *replay : input_.held(),
                     replay ? (replay_pressed ? *replay_pressed : KeyboardState{}) : pressed);
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
            sdl_ok(SDL_QueueAudio(device_, audio_buffer_.data(),
                                  static_cast<Uint32>(block_samples * sizeof(float))),
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
    metrics << std::fixed << std::setprecision(1) << debug_overlay_fps_label
            << (1000.0 / std::max(frame_ms, 0.1)) << "  " << debug_overlay_frame_label << session.frames()
            << "  " << frame_ms << debug_overlay_milliseconds_label;
    std::ostringstream address;
    address << debug_overlay_address_label << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
            << session.vm_address();
    const auto code = session.vm_code_name();
    std::array<std::string, 4> storage{
        metrics.str(), std::string(debug_overlay_room_label) + session.room_name(),
        std::string(debug_overlay_code_label) + std::string(code.empty() ? "-" : code), address.str()};
    std::array<std::string_view, 4> lines{storage[0], storage[1], storage[2], storage[3]};
    session.canvas().debug_text_overlay(lines);
}
} // namespace
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
#if REGML_WITH_SDL2 || REGML_WII || REGML_PS2
    std::optional<VideoWindow> video_window;
    if (!options.headless) {
#if REGML_PS2
        video_window.emplace(options.hidden, false, options.speed_controls, options.uncapped);
#else
        video_window.emplace(options.hidden, false, options.speed_controls);
#endif
    }
#endif
    auto show_error = [&](std::string_view title, std::string_view report) {
#if REGML_WITH_SDL2 && !REGML_WEB && !REGML_WII && !REGML_PS2
        if (video_window && !options.hidden)
            video_window->show_error_reporter(title, report);
#else
        (void)title;
        (void)report;
#endif
    };
    auto show_reGML_error = [&](const Error &error) {
        std::ostringstream report;
        report << "reGML " << error_kind_name(error.kind()) << " error";
        if (error.offset())
            report << " at 0x" << std::hex << *error.offset() << std::dec;
        report << ":\n\n" << error.what();
        show_error(error.kind() == ErrorKind::vm ? "Runner Error" : "reGML Error", report.str());
    };
    std::unique_ptr<GameSession> game;
    try {
#if REGML_PS4
        game = std::make_unique<GameSession>(package, options.seed, options.save_directory,
                                             video_window ? video_window->gamepad_state() : GamepadState{});
#else
        game = std::make_unique<GameSession>(package, options.seed, options.save_directory);
#endif
    } catch (const Error &error) {
#if !REGML_PS2
        show_reGML_error(error);
#endif
        throw;
    } catch (const std::exception &error) {
#if !REGML_PS2
        show_error("reGML Error", error.what());
#endif
        throw;
    }
    GameSession &session = *game;
    session.profile_tick(profile.enabled());
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
    KeyboardState replay_keys{}, replay_pressed{};
    auto input = [&]() {
        const auto previous = replay_keys;
        if (const auto it = replay.find(session.frames()); it != replay.end())
            replay_keys = it->second;
        for (std::size_t i = 0; i < replay_keys.size(); ++i)
            replay_pressed[i] = replay_keys[i] && !previous[i];
        return replay_keys;
    };
    if (options.headless) {
        std::vector<float> silent_audio;
        for (std::size_t i = 0; i < options.frame_limit; ++i) {
            using Clock = std::chrono::steady_clock;
            const auto start = profile.enabled() ? Clock::now() : Clock::time_point{};
            const auto instructions_begin = session.instructions();
            if (!replay.empty()) {
                input();
                session.tick(replay_keys, replay_pressed);
            } else
                session.tick(KeyboardState{}, {});
            const auto tick_end = profile.enabled() ? Clock::now() : Clock::time_point{};
            silent_audio.resize(static_cast<std::size_t>(96000 / session.speed()));
            session.mix_audio(silent_audio);
            if (profile.enabled()) {
                const auto tick_profile = session.take_tick_profile();
                profile.sample(session.frames(), session.room_name(),
                               session.instructions() - instructions_begin,
                               std::chrono::duration<double, std::milli>(Clock::now() - start).count(),
                               std::chrono::duration<double, std::milli>(tick_end - start).count(), 0,
                               tick_profile.logic_ms, tick_profile.draw_ms, tick_profile);
            }
        }
    } else {
#if REGML_WITH_SDL2 || REGML_WII || REGML_PS2
        trace_stage("video.begin", &session);
        auto &window = *video_window;
        trace_stage("audio.begin", &session);
#if !REGML_PS2
        AudioDevice audio(session, options.speed_multiplier);
#else
        Ps2AudioOutput audio(session);
#endif
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
#if !REGML_PS2
                audio.set_speed(speed_multiplier);
#else
                (void)previous_speed_multiplier;
#endif
            input();
            trace_stage("tick", &session);
            const auto tick_begin = profile.enabled() ? Clock::now() : Clock::time_point{};
            auto tick_end = Clock::time_point{};
            auto present_end = Clock::time_point{};
            try {
                window.tick_game(session, replay.empty() ? nullptr : &replay_keys,
                                 replay.empty() ? nullptr : &replay_pressed);
#if !REGML_PS2
                audio.pump();
#endif
                tick_end = profile.enabled() ? Clock::now() : Clock::time_point{};
                if (options.debug_overlay)
                    draw_debug_overlay(session, debug_frame_ms);
                trace_stage("present", &session);
                window.present(session.canvas());
                present_end = profile.enabled() ? Clock::now() : Clock::time_point{};
#if REGML_WII
                if (!options.trace_log.empty()) {
                    if (session.frames() <= 10 || session.frames() % 30 == 0)
                        report_wii_frame(session.frames(), session.room_name());
                    if (session.frames() == 30)
                        session.canvas().save_bmp(package.parent_path() / "regml-frame.bmp");
                }
#endif
            } catch (const Error &error) {
#if !REGML_PS2
                show_reGML_error(error);
#endif
                throw;
            } catch (const std::exception &error) {
#if !REGML_PS2
                show_error("reGML Error", error.what());
#endif
                throw;
            }
            trace_stage("delay", &session);
            const auto frame_time = std::chrono::duration<double>(1.0 / (session.speed() * speed_multiplier));
            next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(frame_time);
#if !REGML_PS2
            if (!options.hidden) {
                // skip missed frames instead of racing through the backlog
                next = std::max(next, std::chrono::steady_clock::now());
#if REGML_WEB
                const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(
                    next - std::chrono::steady_clock::now());
                if (wait.count() > 0)
                    emscripten_sleep(static_cast<unsigned>(wait.count()));
#else
                std::this_thread::sleep_until(next);
#endif
            }
#endif
            if (profile.enabled()) {
                const auto ms = [](auto duration) {
                    return std::chrono::duration<double, std::milli>(duration).count();
                };
                const auto tick_profile = session.take_tick_profile();
                profile.sample(session.frames(), session.room_name(),
                               session.instructions() - instructions_begin, ms(Clock::now() - frame_begin),
                               ms(tick_end - tick_begin), ms(present_end - tick_end), tick_profile.logic_ms,
                               tick_profile.draw_ms, tick_profile);
            }
        }
#if REGML_PS2
        session.canvas().gpu_target(false);
#endif
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
