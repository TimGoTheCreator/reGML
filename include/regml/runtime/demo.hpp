#pragma once
#include "regml/render/canvas.hpp"
#include "regml/vm/interpreter.hpp"
#include <array>
namespace regml {
using KeyboardState = std::array<bool, 256>;
struct BoundProgram { Program program; VmBindings bindings; };
// An original arcade fixture whose Create/Step/Draw programs run on the VM.
// This is not a substituted implementation of a commercial game's logic.
class DemoSession {
public:
    DemoSession();
    void tick(const KeyboardState& held, const KeyboardState& pressed_events = {});
    void draw();
    [[nodiscard]] const Canvas& canvas() const noexcept { return canvas_; }
    [[nodiscard]] const VariableStore& state() const noexcept { return self_; }
    [[nodiscard]] std::size_t frames() const noexcept { return frames_; }
    [[nodiscard]] std::size_t vm_instructions() const noexcept { return vm_instructions_; }
private:
    void run(const BoundProgram& code);
    Value builtin(std::string_view name, std::span<const Value> args);
    Canvas canvas_{640,480};
    VariableStore self_, globals_;
    KeyboardState held_{}, pressed_{}, previous_{};
    BoundProgram create_, step_, draw_;
    bool restart_requested_ = false;
    std::size_t frames_ = 0, vm_instructions_ = 0;
};
KeyboardState demo_autoplay(std::size_t frame);
} // namespace regml
