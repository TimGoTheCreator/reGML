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
#if REGML_GLMODERN
#include "glmodern/gl_gpu.hpp"
#include "glmodern/gl_core.hpp"
#endif
#if REGML_GX2
#include "gx2/gx2_gpu.hpp"
#endif
#if REGML_WIIU
#include <whb/proc.h>
#include <whb/gfx.h>
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
#include <iostream>
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
        file_
            << "frame,room,samples,frame_ms,tick_ms,present_ms,logic_ms,draw_ms,image_ms,text_ms,image_"
               "calls,image_quads,glyph_quads,texture_uploads,max_tick_ms,max_present_ms,instructions_per_"
               "frame,draw_setup_ms,draw_pre_event_ms,draw_world_setup_ms,draw_list_ms,draw_world_ms,"
               "draw_finish_ms,draw_depth_visual_ms,draw_instance_body_ms,draw_object_hotspots,draw_tile_ms,"
               "draw_layer_visual_ms,draw_particle_ms,draw_tile_calls,draw_layer_visual_calls,draw_particle_"
               "calls,draw_layer_hotspots,draw_event_ms,"
               "draw_builtin_ms,draw_text_builtin_ms,draw_image_builtin_ms,draw_"
               "primitive_builtin_ms,draw_other_builtin_ms,max_draw_builtin_call_ms,slowest_draw_builtin,"
               "slowest_draw_builtin_details\n";
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
        draw_setup_ms_ += tick_profile.draw_setup_ms;
        draw_pre_event_ms_ += tick_profile.draw_pre_event_ms;
        draw_world_setup_ms_ += tick_profile.draw_world_setup_ms;
        draw_list_ms_ += tick_profile.draw_list_ms;
        draw_world_ms_ += tick_profile.draw_world_ms;
        draw_finish_ms_ += tick_profile.draw_finish_ms;
        draw_depth_visual_ms_ += tick_profile.draw_depth_visual_ms;
        draw_instance_body_ms_ += tick_profile.draw_instance_body_ms;
        draw_object_hotspots_ = tick_profile.draw_object_hotspots;
        draw_tile_ms_ += tick_profile.draw_tile_ms;
        draw_layer_visual_ms_ += tick_profile.draw_layer_visual_ms;
        draw_particle_ms_ += tick_profile.draw_particle_ms;
        draw_tile_calls_ += tick_profile.draw_tile_calls;
        draw_layer_visual_calls_ += tick_profile.draw_layer_visual_calls;
        draw_particle_calls_ += tick_profile.draw_particle_calls;
        draw_layer_hotspots_ = tick_profile.draw_layer_hotspots;
        draw_event_ms_ += tick_profile.draw_event_ms;
        draw_builtin_ms_ += tick_profile.draw_builtin_ms;
        draw_text_builtin_ms_ += tick_profile.draw_text_builtin_ms;
        draw_image_builtin_ms_ += tick_profile.draw_image_builtin_ms;
        draw_primitive_builtin_ms_ += tick_profile.draw_primitive_builtin_ms;
        draw_other_builtin_ms_ += tick_profile.draw_other_builtin_ms;
        if (tick_profile.max_draw_builtin_call_ms > max_draw_builtin_call_ms_) {
            max_draw_builtin_call_ms_ = tick_profile.max_draw_builtin_call_ms;
            slowest_draw_builtin_ = tick_profile.slowest_draw_builtin;
            slowest_draw_builtin_details_ = tick_profile.slowest_draw_builtin_details;
        }
        image_ms_ += tick_profile.image_ms;
        text_ms_ += tick_profile.text_ms;
        image_calls_ += tick_profile.image_calls;
        image_quads_ += tick_profile.image_quads;
        glyph_quads_ += tick_profile.glyph_quads;
        texture_uploads_ += tick_profile.texture_uploads;
        max_tick_ms_ = std::max(max_tick_ms_, tick_ms);
        max_present_ms_ = std::max(max_present_ms_, present_ms);
        if (samples_ == 30 || frame_ms >= 250.0)
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
              << static_cast<double>(instructions_) / count << ',' << draw_setup_ms_ / count << ','
              << draw_pre_event_ms_ / count << ',' << draw_world_setup_ms_ / count << ','
              << draw_list_ms_ / count << ',' << draw_world_ms_ / count << ',' << draw_finish_ms_ / count
              << ',' << draw_depth_visual_ms_ / count << ',' << draw_instance_body_ms_ / count << ','
              << std::quoted(draw_object_hotspots_) << ',' << draw_tile_ms_ / count << ','
              << draw_layer_visual_ms_ / count << ',' << draw_particle_ms_ / count << ','
              << draw_tile_calls_ / count << ',' << draw_layer_visual_calls_ / count << ','
              << draw_particle_calls_ / count << ',' << std::quoted(draw_layer_hotspots_) << ','
              << draw_event_ms_ / count << ',' << draw_builtin_ms_ / count << ','
              << draw_text_builtin_ms_ / count << ',' << draw_image_builtin_ms_ / count << ','
              << draw_primitive_builtin_ms_ / count << ',' << draw_other_builtin_ms_ / count << ','
              << max_draw_builtin_call_ms_ << ',' << std::quoted(slowest_draw_builtin_) << ','
              << std::quoted(slowest_draw_builtin_details_) << '\n';
        file_.flush();
        samples_ = instructions_ = 0;
        frame_ms_ = tick_ms_ = present_ms_ = logic_ms_ = draw_ms_ = max_tick_ms_ = max_present_ms_ = 0;
        draw_setup_ms_ = draw_pre_event_ms_ = draw_world_setup_ms_ = draw_list_ms_ = draw_world_ms_ =
            draw_finish_ms_ = 0;
        draw_depth_visual_ms_ = draw_instance_body_ms_ = 0;
        draw_object_hotspots_.clear();
        draw_tile_ms_ = draw_layer_visual_ms_ = draw_particle_ms_ = 0;
        draw_tile_calls_ = draw_layer_visual_calls_ = draw_particle_calls_ = 0;
        draw_layer_hotspots_.clear();
        draw_event_ms_ = 0;
        draw_builtin_ms_ = 0;
        draw_text_builtin_ms_ = draw_image_builtin_ms_ = draw_primitive_builtin_ms_ = draw_other_builtin_ms_ =
            0;
        max_draw_builtin_call_ms_ = 0;
        slowest_draw_builtin_.clear();
        slowest_draw_builtin_details_.clear();
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
    double draw_setup_ms_ = 0, draw_pre_event_ms_ = 0, draw_world_setup_ms_ = 0, draw_list_ms_ = 0,
           draw_world_ms_ = 0, draw_finish_ms_ = 0, draw_depth_visual_ms_ = 0, draw_instance_body_ms_ = 0;
    std::string draw_object_hotspots_;
    double draw_tile_ms_ = 0, draw_layer_visual_ms_ = 0, draw_particle_ms_ = 0;
    std::uint64_t draw_tile_calls_ = 0, draw_layer_visual_calls_ = 0, draw_particle_calls_ = 0;
    std::string draw_layer_hotspots_;
    double draw_event_ms_ = 0;
    double draw_builtin_ms_ = 0;
    double draw_text_builtin_ms_ = 0, draw_image_builtin_ms_ = 0, draw_primitive_builtin_ms_ = 0,
           draw_other_builtin_ms_ = 0;
    double max_draw_builtin_call_ms_ = 0;
    std::string slowest_draw_builtin_;
    std::string slowest_draw_builtin_details_;
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
#elif REGML_WIIU
        // On Wii U, GX2GL uses WHBGfx for display buffers.
        // SDL_INIT_VIDEO in wiiu-sdl2 would initialize its own GX2 buffers and conflict with WHBGfx.
        sdl_ok(SDL_Init(SDL_INIT_EVENTS | SDL_INIT_TIMER), "SDL initialization");
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
    if (key >= SDLK_KP_1 && key <= SDLK_KP_9)
        return 97 + static_cast<int>(key - SDLK_KP_1);
    switch (key) {
    case SDLK_BACKSPACE:
        return 8;
    case SDLK_TAB:
        return 9;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        return 13;
    case SDLK_PAUSE:
        return 19;
    case SDLK_CAPSLOCK:
        return 20;
    case SDLK_ESCAPE:
        return 27;
    case SDLK_SPACE:
        return 32;
    case SDLK_PAGEUP:
        return 33;
    case SDLK_PAGEDOWN:
        return 34;
    case SDLK_END:
        return 35;
    case SDLK_HOME:
        return 36;
    case SDLK_LEFT:
        return 37;
    case SDLK_UP:
        return 38;
    case SDLK_RIGHT:
        return 39;
    case SDLK_DOWN:
        return 40;
    case SDLK_PRINTSCREEN:
        return 44;
    case SDLK_INSERT:
        return 45;
    case SDLK_DELETE:
        return 46;
    case SDLK_KP_0:
        return 96;
    case SDLK_KP_MULTIPLY:
        return 106;
    case SDLK_KP_PLUS:
        return 107;
    case SDLK_KP_MINUS:
        return 109;
    case SDLK_KP_PERIOD:
        return 110;
    case SDLK_KP_DIVIDE:
        return 111;
    case SDLK_LSHIFT:
        return 160;
    case SDLK_RSHIFT:
        return 161;
    case SDLK_LCTRL:
        return 162;
    case SDLK_RCTRL:
        return 163;
    case SDLK_LALT:
        return 164;
    case SDLK_RALT:
        return 165;
    case SDLK_SEMICOLON:
        return 186;
    case SDLK_EQUALS:
        return 187;
    case SDLK_COMMA:
        return 188;
    case SDLK_MINUS:
        return 189;
    case SDLK_PERIOD:
        return 190;
    case SDLK_SLASH:
        return 191;
    case SDLK_BACKQUOTE:
        return 192;
    case SDLK_LEFTBRACKET:
        return 219;
    case SDLK_BACKSLASH:
        return 220;
    case SDLK_RIGHTBRACKET:
        return 221;
    case SDLK_QUOTE:
        return 222;
    default:
        if (key >= SDLK_a && key <= SDLK_z)
            return static_cast<int>(key - SDLK_a + 'A');
        return key >= 0 && key < 256 ? static_cast<int>(key) : -1;
    }
}
class VideoWindow {
  public:
    explicit VideoWindow(bool hidden, bool escape_quits = true, bool speed_controls = false, bool opengl = false)
        : escape_quits_(escape_quits), speed_controls_(speed_controls) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
#if !REGML_WIIU
#if REGML_WII
        auto flags =
            static_cast<Uint32>(SDL_WINDOW_FULLSCREEN | (hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN));
        constexpr int window_width = 640, window_height = 480;
#else
        auto flags = static_cast<Uint32>(SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                         (hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN));
        constexpr int window_width = 640, window_height = 480;
#endif
#if REGML_GLMODERN
        if (opengl) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
            SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
            SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
            SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
            SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
            SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
            flags |= SDL_WINDOW_OPENGL;
        }
#else
        (void)opengl;
#endif
        window_.reset(SDL_CreateWindow(escape_quits ? "reGML - VM Arcade" : "reGML - Game (experimental)",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, window_width,
                                       window_height, flags));
        if (!window_ && (flags & SDL_WINDOW_OPENGL)) {
            flags &= ~static_cast<Uint32>(SDL_WINDOW_OPENGL);
            window_.reset(SDL_CreateWindow(escape_quits ? "reGML - VM Arcade" : "reGML - Game (experimental)",
                                           SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, window_width,
                                           window_height, flags));
        }
        if (!window_)
            throw Error(ErrorKind::io, std::string("window creation: ") + SDL_GetError());
#if REGML_GLMODERN
        if (opengl) {
            gl_context_ = SDL_GL_CreateContext(window_.get());
            if (gl_context_) {
                if (SDL_GL_MakeCurrent(window_.get(), gl_context_) == 0 &&
                    regml::glmodern::load_gl_core_3_3(SDL_GL_GetProcAddress)) {
                    int drawable_w = window_width, drawable_h = window_height;
                    SDL_GL_GetDrawableSize(window_.get(), &drawable_w, &drawable_h);
                    glViewport(0, 0, drawable_w, drawable_h);
                    regml::glmodern::gl_gpu_set_viewport(640, 480);
                    regml::glmodern::gl_gpu_begin();
                    use_opengl_ = true;
                    std::cout << "OpenGL Renderer initialized successfully." << std::endl;
                } else {
                    SDL_GL_DeleteContext(gl_context_);
                    gl_context_ = nullptr;
                }
            }
        }
#endif
#else // REGML_WIIU
        if (opengl) {
#if REGML_GX2
            WHBProcInit();
            regml::gx2::gx2_gpu_set_viewport(640, 480);
            use_opengl_ = true;
            std::cout << "Wii U GX2 Renderer initialized successfully." << std::endl;
#endif
        }
        if (!use_opengl_) {
            SDL_InitSubSystem(SDL_INIT_VIDEO);
            constexpr int window_width = 640, window_height = 480;
            window_.reset(SDL_CreateWindow("reGML - Wii U", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                           window_width, window_height, SDL_WINDOW_FULLSCREEN));
            if (!window_)
                throw Error(ErrorKind::io, std::string("window creation: ") + SDL_GetError());
        }
#endif
        if (!use_opengl_) {
            renderer_.reset(
                SDL_CreateRenderer(window_.get(), -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));
            if (!renderer_)
                renderer_.reset(SDL_CreateRenderer(window_.get(), -1, SDL_RENDERER_ACCELERATED));
            if (!renderer_)
                renderer_.reset(
                    SDL_CreateRenderer(window_.get(), -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC));
            if (!renderer_)
                renderer_.reset(SDL_CreateRenderer(window_.get(), -1, SDL_RENDERER_SOFTWARE));
            if (!renderer_)
                throw Error(ErrorKind::io, std::string("renderer creation: ") + SDL_GetError());
            SDL_RendererInfo rinfo{};
            if (SDL_GetRendererInfo(renderer_.get(), &rinfo) == 0)
                std::cout << "SDL Renderer: " << (rinfo.name ? rinfo.name : "unknown") << std::endl;
            sdl_ok(SDL_RenderSetLogicalSize(renderer_.get(), 640, 480), "logical resolution");
            texture_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING, render_width, render_height));
            if (!texture_)
                throw Error(ErrorKind::io, std::string("framebuffer texture: ") + SDL_GetError());
            sdl_ok(SDL_SetTextureBlendMode(texture_.get(), SDL_BLENDMODE_NONE), "texture blend mode");
        }
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
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) >= 0) {
            controller_subsystem_ = true;
        }
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            if (controller_subsystem_ && SDL_IsGameController(i)) {
                controller_ = SDL_GameControllerOpen(i);
                if (controller_) {
                    gamepad_.connected = true;
                    break;
                }
            }
        }
        if (!controller_ && controller_subsystem_) {
            for (int i = 0; i < SDL_NumJoysticks(); ++i) {
                joystick_ = SDL_JoystickOpen(i);
                if (joystick_) {
                    gamepad_.connected = true;
                    break;
                }
            }
        }
#endif
    }
    ~VideoWindow() {
        if (controller_)
            SDL_GameControllerClose(controller_);
        if (joystick_)
            SDL_JoystickClose(joystick_);
#if REGML_PS4
        if (pad_ >= 0)
            scePadClose(pad_);
#endif
        if (controller_subsystem_)
            SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
#if !REGML_WIIU
        if (gl_context_)
            SDL_GL_DeleteContext(gl_context_);
#endif
    }
    [[nodiscard]] bool is_opengl() const noexcept { return use_opengl_; }
    bool poll(double *speed_multiplier = nullptr) {
#if REGML_WIIU
        if (!WHBProcIsRunning()) {
            return false;
        }
#endif
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
                    if (window_) {
                        std::ostringstream title;
                        title << "reGML - Game (" << std::setprecision(3) << *speed_multiplier << "x)";
                        SDL_SetWindowTitle(window_.get(), title.str().c_str());
                    }
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
                gamepad_.buttons.fill(false);
                gamepad_.pressed.fill(false);
                gamepad_.axes.fill(0.0);
            }
            if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                const bool down = event.type == SDL_KEYDOWN;
                const bool detect_press = !event.key.repeat;
                const auto key = gml_key(event.key.keysym.sym);
                update_source_key(keyboard_held_, keyboard_pressed_, key, down, detect_press);
                if (key == 160 || key == 161)
                    update_source_key(keyboard_held_, keyboard_pressed_, 16,
                                      keyboard_held_[160] || keyboard_held_[161], detect_press);
                else if (key == 162 || key == 163)
                    update_source_key(keyboard_held_, keyboard_pressed_, 17,
                                      keyboard_held_[162] || keyboard_held_[163], detect_press);
                else if (key == 164 || key == 165)
                    update_source_key(keyboard_held_, keyboard_pressed_, 18,
                                      keyboard_held_[164] || keyboard_held_[165], detect_press);
            }
            if (controller_subsystem_ && event.type == SDL_CONTROLLERDEVICEADDED && !controller_ &&
                SDL_IsGameController(event.cdevice.which)) {
                controller_ = SDL_GameControllerOpen(event.cdevice.which);
                if (controller_) {
                    if (joystick_) {
                        SDL_JoystickClose(joystick_);
                        joystick_ = nullptr;
                    }
                    gamepad_.connected = true;
                }
            }
            if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller_ &&
                SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller_)) == event.cdevice.which) {
                SDL_GameControllerClose(controller_);
                controller_ = nullptr;
                controller_buttons_held_.fill(false);
                controller_buttons_pressed_.fill(false);
                controller_axes_held_.fill(false);
                controller_axes_pressed_.fill(false);
                if (!joystick_)
                    gamepad_ = GamepadState{};
            }
            if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP) {
                const bool down = event.type == SDL_CONTROLLERBUTTONDOWN;
                const auto btn = static_cast<SDL_GameControllerButton>(event.cbutton.button);
                const auto key = gml_button(btn);
                update_source_key(controller_buttons_held_, controller_buttons_pressed_, key, down);
                update_gamepad_button(gml_gamepad_button(btn), down);
            }
            if (event.type == SDL_CONTROLLERAXISMOTION) {
                const auto axis = static_cast<SDL_GameControllerAxis>(event.caxis.axis);
                update_controller_axis(axis, event.caxis.value, controller_axes_held_, controller_axes_pressed_);
                update_gamepad_axis(axis, event.caxis.value);
            }
            if (controller_subsystem_ && event.type == SDL_JOYDEVICEADDED && !controller_ && !joystick_ &&
                !SDL_IsGameController(event.jdevice.which)) {
                joystick_ = SDL_JoystickOpen(event.jdevice.which);
                if (joystick_)
                    gamepad_.connected = true;
            }
            if (event.type == SDL_JOYDEVICEREMOVED && joystick_ &&
                SDL_JoystickInstanceID(joystick_) == event.jdevice.which) {
                SDL_JoystickClose(joystick_);
                joystick_ = nullptr;
                joystick_buttons_held_.fill(false);
                joystick_buttons_pressed_.fill(false);
                joystick_hat_held_.fill(false);
                joystick_hat_pressed_.fill(false);
                joystick_axes_held_.fill(false);
                joystick_axes_pressed_.fill(false);
                if (!controller_)
                    gamepad_ = GamepadState{};
            }
            if (!controller_ && (event.type == SDL_JOYBUTTONDOWN || event.type == SDL_JOYBUTTONUP)) {
                const bool down = event.type == SDL_JOYBUTTONDOWN;
                static constexpr std::array<int, 4> face_keys = {'Z', 'X', 'C', 'C'};
                if (event.jbutton.button < face_keys.size())
                    update_source_key(joystick_buttons_held_, joystick_buttons_pressed_,
                                      face_keys[event.jbutton.button], down);
                if (event.jbutton.button < 16)
                    update_gamepad_button(static_cast<int>(event.jbutton.button), down);
            }
            if (!controller_ && event.type == SDL_JOYAXISMOTION) {
                if (event.jaxis.axis == 0)
                    update_axis_pair(37, 39, event.jaxis.value, joystick_axes_held_, joystick_axes_pressed_);
                else if (event.jaxis.axis == 1)
                    update_axis_pair(38, 40, event.jaxis.value, joystick_axes_held_, joystick_axes_pressed_);
                if (event.jaxis.axis < 4)
                    gamepad_.axes[event.jaxis.axis] = std::clamp(event.jaxis.value / 32767.0, -1.0, 1.0);
            }
            if (!controller_ && event.type == SDL_JOYHATMOTION) {
                const Uint8 hat = event.jhat.value;
                update_source_key(joystick_hat_held_, joystick_hat_pressed_, 37, (hat & SDL_HAT_LEFT) != 0);
                update_source_key(joystick_hat_held_, joystick_hat_pressed_, 39, (hat & SDL_HAT_RIGHT) != 0);
                update_source_key(joystick_hat_held_, joystick_hat_pressed_, 38, (hat & SDL_HAT_UP) != 0);
                update_source_key(joystick_hat_held_, joystick_hat_pressed_, 40, (hat & SDL_HAT_DOWN) != 0);
                update_gamepad_button(12, (hat & SDL_HAT_UP) != 0);
                update_gamepad_button(13, (hat & SDL_HAT_DOWN) != 0);
                update_gamepad_button(14, (hat & SDL_HAT_LEFT) != 0);
                update_gamepad_button(15, (hat & SDL_HAT_RIGHT) != 0);
            }
        }
#if REGML_PS4
        poll_ps4_pad();
#endif
        rebuild_input_state();
        return true;
    }
    void present(const Canvas &canvas) {
#if REGML_GLMODERN || REGML_GX2
        if (use_opengl_) {
#if REGML_GLMODERN
            if (regml::glmodern::glBindFramebuffer) {
                regml::glmodern::glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }
            if (regml::glmodern::glDrawBuffer) {
                regml::glmodern::glDrawBuffer(GL_BACK);
            }
            if (canvas.gpu_target()) {
                regml::glmodern::gl_gpu_flush();
                regml::glmodern::gl_gpu_presented();
            } else {
                std::span<const std::uint32_t> px = canvas.pixels();
                if (!px.empty()) {
                    regml::glmodern::gl_gpu_begin();
                    regml::glmodern::gl_gpu_image(px, canvas.width(), 0, 0, canvas.width(), canvas.height(),
                                                  0, 0, canvas.width(), canvas.height(), 0, 0xffffff, 1.0, 0);
                    regml::glmodern::gl_gpu_flush();
                    regml::glmodern::gl_gpu_presented();
                }
            }
            SDL_GL_SwapWindow(window_.get());
#elif REGML_GX2
            if (canvas.gpu_target()) {
                regml::gx2::gx2_gpu_flush();
                regml::gx2::gx2_gpu_presented();
            } else {
                std::span<const std::uint32_t> px = canvas.pixels();
                if (!px.empty()) {
                    regml::gx2::gx2_gpu_begin();
                    regml::gx2::gx2_gpu_image(px, canvas.width(), 0, 0, canvas.width(), canvas.height(),
                                              0, 0, canvas.width(), canvas.height(), 0, 0xffffff, 1.0, 0);
                    regml::gx2::gx2_gpu_flush();
                    regml::gx2::gx2_gpu_presented();
                }
            }
#endif
            return;
        }
#endif
        const auto *pixels = canvas.pixels().data();
        int pitch = canvas.width() * static_cast<int>(sizeof(std::uint32_t));
        if (canvas.width() != render_width || canvas.height() != render_height) {
            present_pixels_.resize(static_cast<std::size_t>(render_width) * render_height);
            for (int y = 0; y < render_height; ++y) {
                const int source_y =
                    static_cast<int>(static_cast<std::int64_t>(y) * canvas.height() / render_height);
                const auto *source_row =
                    canvas.pixels().data() + static_cast<std::size_t>(source_y) * canvas.width();
                auto *target_row = present_pixels_.data() + static_cast<std::size_t>(y) * render_width;
                for (int x = 0; x < render_width; ++x) {
                    const int source_x =
                        static_cast<int>(static_cast<std::int64_t>(x) * canvas.width() / render_width);
                    target_row[x] = source_row[source_x];
                }
            }
            pixels = present_pixels_.data();
            pitch = render_width * static_cast<int>(sizeof(std::uint32_t));
        }
        sdl_ok(SDL_UpdateTexture(texture_.get(), nullptr, pixels, pitch), "texture upload");
        sdl_ok(SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 255), "clear color");
        sdl_ok(SDL_RenderClear(renderer_.get()), "renderer clear");
        sdl_ok(SDL_RenderCopy(renderer_.get(), texture_.get(), nullptr, nullptr), "frame presentation");
        SDL_RenderPresent(renderer_.get());
    }
    void show_error_reporter(std::string_view title, std::string_view report) noexcept {
        try {
#if !REGML_PS4 && !REGML_WIIU
            const std::string details(report);
            const SDL_MessageBoxButtonData buttons[] = {
                {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Close"},
                {0, 1, "Copy report"},
            };
            const SDL_MessageBoxData message_box{
                SDL_MESSAGEBOX_ERROR,
                window_.get(),
                title == "Runner Error" ? "reGML Runtime Error" : "reGML Error",
                details.c_str(),
                static_cast<int>(std::size(buttons)),
                buttons,
                nullptr,
            };
            int selected = 0;
            if (SDL_ShowMessageBox(&message_box, &selected) == 0 && selected == 1) {
                if (SDL_SetClipboardText(details.c_str()) == 0)
                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "reGML", "Error report copied.",
                                             window_.get());
                else
                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "reGML", SDL_GetError(), window_.get());
            }
#else
            Canvas canvas(640, 480, true);
            std::vector<std::string> lines;
            std::istringstream input{std::string(report)};
            std::string paragraph;
            constexpr std::size_t line_width = 48;
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
                    if (lines.size() >= 2000)
                        break;
                }
                if (lines.size() >= 2000)
                    break;
            }
            if (lines.empty())
                lines.emplace_back("No error details were provided.");

            constexpr int visible_lines = 24, line_height = 16;
            int scroll = 0;
            bool open = true;
            auto redraw = [&] {
                canvas.clear(0x000000);
                canvas.color(0xffffff);
                canvas.text(16, 12, title == "Runner Error" ? "reGML RUNTIME ERROR" : "reGML ERROR", 2, 2, 0,
                            0, 0);
                canvas.text(16, 38, "The run stopped. Report details:", 1, 1, 0, 0, 0);
                for (int row = 0; row < visible_lines; ++row) {
                    const auto index = static_cast<std::size_t>(scroll + row);
                    if (index >= lines.size())
                        break;
                    canvas.text(16, 62 + row * line_height, lines[index], 2, 2, 0, 0, 0);
                }
                canvas.text(16, 458, "UP/DOWN scroll  |  any button closes", 1, 1, 0, 0, 0);
                present(canvas);
            };
            redraw();
            while (open) {
                SDL_Event event;
                while (SDL_PollEvent(&event)) {
                    if (event.type == SDL_QUIT || event.type == SDL_KEYDOWN ||
                        event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_JOYBUTTONDOWN) {
                        open = false;
                    } else if (event.type == SDL_MOUSEWHEEL) {
                        scroll = std::clamp(scroll - event.wheel.y, 0,
                                            std::max(0, static_cast<int>(lines.size()) - visible_lines));
                        redraw();
                    }
                }
                SDL_Delay(16);
            }
#endif
        } catch (...) {
        }
    }
    void tick_game(GameSession &session,
                   const KeyboardState *replay,
                   const KeyboardState *replay_pressed = nullptr) {
        session.set_gamepad_state(gamepad_);
        gamepad_.pressed.fill(false);
#if REGML_GLMODERN || REGML_GX2
        if (use_opengl_) {
            session.canvas().gpu_target(true);
#if REGML_GLMODERN
            int drawable_w = 640, drawable_h = 480;
            if (window_)
                SDL_GL_GetDrawableSize(window_.get(), &drawable_w, &drawable_h);
            glViewport(0, 0, drawable_w, drawable_h);
            regml::glmodern::gl_gpu_set_viewport(session.canvas().width(), session.canvas().height());
            regml::glmodern::gl_gpu_begin();
#elif REGML_GX2
            regml::gx2::gx2_gpu_set_viewport(session.canvas().width(), session.canvas().height());
            regml::gx2::gx2_gpu_begin();
#endif
        }
#endif
        session.tick(replay ? *replay : held_,
                     replay ? (replay_pressed ? *replay_pressed : KeyboardState{}) : pressed_);
        pressed_.fill(false);
    }
    const GamepadState &gamepad_state() const { return gamepad_; }

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
            ORBIS_PAD_BUTTON_CROSS,    ORBIS_PAD_BUTTON_CIRCLE, ORBIS_PAD_BUTTON_SQUARE,
            ORBIS_PAD_BUTTON_TRIANGLE, ORBIS_PAD_BUTTON_L1,     ORBIS_PAD_BUTTON_R1,
            ORBIS_PAD_BUTTON_L2,       ORBIS_PAD_BUTTON_R2,     ORBIS_PAD_BUTTON_TOUCH_PAD,
            ORBIS_PAD_BUTTON_OPTIONS,  ORBIS_PAD_BUTTON_L3,     ORBIS_PAD_BUTTON_R3,
            ORBIS_PAD_BUTTON_UP,       ORBIS_PAD_BUTTON_DOWN,   ORBIS_PAD_BUTTON_LEFT,
            ORBIS_PAD_BUTTON_RIGHT};
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
        const std::array<std::pair<int, int>, 7> keys = {
            {{'Z', 0}, {'X', 1}, {'C', 2}, {13, 9}, {38, 12}, {40, 13}, {37, 14}}};
        for (const auto &[key, button] : keys)
            update_source_key(joystick_buttons_held_, joystick_buttons_pressed_, key,
                              gamepad_.buttons[button]);
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
    void update_gamepad_button(int index, bool down) {
        if (index < 0 || index >= static_cast<int>(gamepad_.buttons.size()))
            return;
        const auto i = static_cast<std::size_t>(index);
        if (down && !gamepad_.buttons[i])
            gamepad_.pressed[i] = true;
        gamepad_.buttons[i] = down;
    }
    void update_gamepad_axis(SDL_GameControllerAxis axis, Sint16 value) {
        const double norm = std::clamp(value / 32767.0, -1.0, 1.0);
        switch (axis) {
        case SDL_CONTROLLER_AXIS_LEFTX:
            gamepad_.axes[0] = norm;
            break;
        case SDL_CONTROLLER_AXIS_LEFTY:
            gamepad_.axes[1] = norm;
            break;
        case SDL_CONTROLLER_AXIS_RIGHTX:
            gamepad_.axes[2] = norm;
            break;
        case SDL_CONTROLLER_AXIS_RIGHTY:
            gamepad_.axes[3] = norm;
            break;
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
            update_gamepad_button(6, value > 8000);
            break;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
            update_gamepad_button(7, value > 8000);
            break;
        default:
            break;
        }
    }
    static int gml_gamepad_button(SDL_GameControllerButton button) {
        switch (button) {
        case SDL_CONTROLLER_BUTTON_A:
            return 0;
        case SDL_CONTROLLER_BUTTON_B:
            return 1;
        case SDL_CONTROLLER_BUTTON_X:
            return 2;
        case SDL_CONTROLLER_BUTTON_Y:
            return 3;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            return 4;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            return 5;
        case SDL_CONTROLLER_BUTTON_BACK:
            return 8;
        case SDL_CONTROLLER_BUTTON_START:
            return 9;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:
            return 10;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
            return 11;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            return 12;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            return 13;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            return 14;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            return 15;
        default:
            return -1;
        }
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
    SDL_GLContext gl_context_ = nullptr;
    bool use_opengl_ = false;
    std::vector<std::uint32_t> present_pixels_;
    SDL_GameController *controller_ = nullptr;
    SDL_Joystick *joystick_ = nullptr;
    GamepadState gamepad_{};
#if REGML_PS4
    int pad_ = -1;
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
    explicit VideoWindow(bool hidden, bool escape_quits = true, bool speed_controls = false, bool opengl = false) {
        (void)hidden;
        (void)escape_quits;
        (void)speed_controls;
        (void)opengl;
    }
    [[nodiscard]] bool is_opengl() const noexcept { return false; }
    bool poll(double *speed_multiplier = nullptr) {
        (void)speed_multiplier;
        return input_.poll();
    }
    bool poll_error_dismiss() {
        if (!input_.poll())
            return true;
        const auto pressed = input_.take_pressed();
        return std::any_of(pressed.begin(), pressed.end(), [](bool key) { return key; });
    }
    void tick_game(GameSession &session,
                   const KeyboardState *replay,
                   const KeyboardState *replay_pressed = nullptr) {
        session.canvas().gpu_target(true);
        session.set_gamepad_state(input_.take_gamepad());
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
void draw_console_error(Canvas &canvas, std::string_view title, std::string_view report) {
    canvas.clear(0x000000);
    canvas.color(0xffffff);
    canvas.text(16, 12, title == "Runner Error" ? "reGML RUNTIME ERROR" : "reGML ERROR", 2, 2, 0, 0, 0);
    canvas.text(16, 36, "The run stopped. Diagnostic details:");
    std::istringstream input{std::string(report)};
    std::string paragraph;
    constexpr std::size_t line_width = 96;
    std::vector<std::string> lines;
    while (std::getline(input, paragraph) && lines.size() < 44) {
        if (paragraph.empty()) {
            lines.emplace_back();
            continue;
        }
        std::size_t start = 0;
        while (start < paragraph.size() && lines.size() < 44) {
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
        }
    }
    for (std::size_t i = 0; i < lines.size(); ++i)
        canvas.text(16, 52 + static_cast<double>(i) * 9, lines[i]);
    if (input && !input.eof())
        canvas.text(16, 448, "(more details are available in the runtime log)");
    canvas.text(16, 466, "Press any button to close");
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
        video_window.emplace(options.hidden, false, options.speed_controls, options.opengl);
#endif
    }
#endif
    trace_stage("video.ready");
    auto show_error = [&](std::string_view title, std::string_view report) {
#if REGML_WITH_SDL2 && !REGML_WEB && !REGML_WII && !REGML_PS2
        if (video_window && !options.hidden)
            video_window->show_error_reporter(title, report);
#elif REGML_WII || REGML_PS2
        if (video_window && !options.hidden) {
            Canvas error_canvas(640, 480);
            draw_console_error(error_canvas, title, report);
            do {
                video_window->present(error_canvas);
            } while (!video_window->poll_error_dismiss());
        }
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
    trace_stage("session.begin");
    try {
        game = std::make_unique<GameSession>(package, options.seed, options.save_directory,
                                             video_window ? video_window->gamepad_state() : GamepadState{},
                                             options.start_room);
        trace_stage("session.ready", game.get());
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
#if (REGML_WITH_SDL2 || REGML_WII) && !REGML_PS2
    if (video_window && video_window->is_opengl())
        session.canvas().gpu_target(true);
#endif
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
        if (replay.empty())
            return replay_keys;
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
#if REGML_GLMODERN
                regml::glmodern::gl_gpu_set_frame_number(session.frames());
#elif REGML_GX2
                regml::gx2::gx2_gpu_set_frame_number(session.frames());
#endif
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
