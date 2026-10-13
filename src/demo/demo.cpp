#include "demo.hpp"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
namespace regml {
namespace {
using Expr = std::function<void()>;

class Assembler {
  public:
    Expr n(double value) {
        return [this, value] {
            word(0xc0000000);
            const auto bits = std::bit_cast<std::uint64_t>(value);
            word(static_cast<std::uint32_t>(bits));
            word(static_cast<std::uint32_t>(bits >> 32));
        };
    }
    Expr integer(std::int32_t value) {
        return [this, value] {
            word(0xc0020000);
            word(std::bit_cast<std::uint32_t>(value));
        };
    }
    Expr text(std::string value) {
        return [this, value = std::move(value)] {
            const auto id = static_cast<std::uint32_t>(bindings_.strings.size());
            bindings_.strings.emplace(id, value);
            word(0xc0060000);
            word(id);
        };
    }
    Expr v(std::string name) {
        return [this, name = std::move(name)] {
            bindings_.variables.emplace(bytes_.size(), VariableBinding{VariableScope::self, name});
            word(0xc005ffff);
            word(0xa0000000);
        };
    }
    Expr call(std::string name, std::initializer_list<Expr> input) {
        return [this, name = std::move(name), args = std::vector<Expr>(input)] {
            for (auto it = args.rbegin(); it != args.rend(); ++it)
                (*it)();
            bindings_.functions.emplace(bytes_.size(), name);
            word(0xd9050000 | static_cast<std::uint32_t>(args.size()));
            word(0);
        };
    }
    Expr math(Opcode op, Expr left, Expr right) {
        return [this, op, left = std::move(left), right = std::move(right)] {
            left();
            right();
            word((static_cast<std::uint32_t>(op) << 24) | 0x550000);
        };
    }
    Expr cmp(unsigned kind, Expr left, Expr right) {
        return [this, kind, left = std::move(left), right = std::move(right)] {
            left();
            right();
            word(0x15550000 | (kind << 8));
        };
    }
    Expr either(Expr left, Expr right) {
        return [this, left = std::move(left), right = std::move(right)] {
            left();
            right();
            word(0x0f440000);
        };
    }
    void set(std::string name, const Expr &expression) {
        expression();
        bindings_.variables.emplace(bytes_.size(), VariableBinding{VariableScope::self, std::move(name)});
        word(0x4555ffff);
        word(0xa0000000);
    }
    void statement(const Expr &expression) {
        expression();
        word(0x9e050000);
    }
    void when(const Expr &condition, const std::function<void()> &body) {
        condition();
        const auto branch = bytes_.size();
        word(0xb8000000);
        body();
        const auto displacement = static_cast<std::uint32_t>((bytes_.size() - branch) / 4);
        const auto encoded = 0xb8000000 | displacement;
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes_[branch + shift / 8] = static_cast<std::uint8_t>(encoded >> shift);
    }
    BoundProgram finish() {
        word(0x9d000000);
        return {decode(BinaryView(bytes_), 16), std::move(bindings_)};
    }

  private:
    void word(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    std::vector<std::uint8_t> bytes_;
    VmBindings bindings_;
};
std::int32_t color(unsigned r, unsigned g, unsigned b) {
    return static_cast<std::int32_t>(r | (g << 8) | (b << 16));
}
BoundProgram create_program() {
    Assembler b;
    b.set("x", b.n(320));
    b.set("y", b.n(380));
    b.set("target_x", b.n(120));
    b.set("target_y", b.n(180));
    b.set("score", b.n(0));
    b.set("lives", b.n(3));
    b.set("timer", b.n(0));
    b.set("cooldown", b.n(0));
    b.set("game_over", b.integer(0));
    b.set("hazard_x", b.n(320));
    b.set("speed", b.n(4));
    return b.finish();
}
BoundProgram step_program() {
    Assembler b;
    b.when(b.call("keyboard_check_pressed", {b.integer(82)}), [&] { b.statement(b.call("game_restart", {})); });
    b.when(b.cmp(3, b.v("game_over"), b.n(0)), [&] {
        b.set("speed", b.n(4));
        b.when(b.call("keyboard_check", {b.integer(32)}), [&] { b.set("speed", b.n(2)); });
        auto key = [&](int arrow, int letter) {
            return b.either(b.call("keyboard_check", {b.integer(arrow)}),
                            b.call("keyboard_check", {b.integer(letter)}));
        };
        b.when(key(37, 65), [&] { b.set("x", b.math(Opcode::sub, b.v("x"), b.v("speed"))); });
        b.when(key(39, 68), [&] { b.set("x", b.math(Opcode::add, b.v("x"), b.v("speed"))); });
        b.when(key(38, 87), [&] { b.set("y", b.math(Opcode::sub, b.v("y"), b.v("speed"))); });
        b.when(key(40, 83), [&] { b.set("y", b.math(Opcode::add, b.v("y"), b.v("speed"))); });
        b.set("x", b.call("clamp", {b.v("x"), b.n(48), b.n(592)}));
        b.set("y", b.call("clamp", {b.v("y"), b.n(104), b.n(414)}));
        b.set("timer", b.math(Opcode::add, b.v("timer"), b.n(1)));
        b.set("hazard_x",
              b.math(Opcode::add, b.n(320),
                     b.math(Opcode::mul, b.call("sin", {b.math(Opcode::div, b.v("timer"), b.n(45))}), b.n(220))));
        b.set("cooldown", b.call("max", {b.n(0), b.math(Opcode::sub, b.v("cooldown"), b.n(1))}));
        b.when(b.cmp(3, b.v("cooldown"), b.n(0)), [&] {
            b.when(b.call("point_in_rectangle", {b.v("x"), b.v("y"), b.math(Opcode::sub, b.v("hazard_x"), b.n(48)),
                                                 b.n(234), b.math(Opcode::add, b.v("hazard_x"), b.n(48)), b.n(266)}),
                   [&] {
                       b.set("lives", b.math(Opcode::sub, b.v("lives"), b.n(1)));
                       b.set("cooldown", b.n(90));
                   });
        });
        b.when(b.cmp(1, b.call("point_distance", {b.v("x"), b.v("y"), b.v("target_x"), b.v("target_y")}), b.n(20)),
               [&] {
                   b.set("score", b.math(Opcode::add, b.v("score"), b.n(1)));
                   b.set("target_x", b.math(Opcode::sub, b.n(640), b.v("target_x")));
                   b.set("target_y", b.math(Opcode::sub, b.n(530), b.v("target_y")));
               });
        b.when(b.cmp(2, b.v("lives"), b.n(0)), [&] { b.set("game_over", b.integer(1)); });
    });
    return b.finish();
}
BoundProgram draw_program() {
    Assembler b;
    auto shade = [&](unsigned r, unsigned g, unsigned blue) {
        b.statement(b.call("draw_set_color", {b.integer(color(r, g, blue))}));
    };
    auto font = [&](int id) { b.statement(b.call("draw_set_font", {b.integer(id)})); };
    auto text = [&](double x, double y, std::string message) {
        b.statement(b.call("draw_text", {b.n(x), b.n(y), b.text(std::move(message))}));
    };
    auto rect = [&](double x1, double y1, double x2, double y2, bool outline) {
        b.statement(b.call("draw_rectangle", {b.n(x1), b.n(y1), b.n(x2), b.n(y2), b.integer(outline ? 1 : 0)}));
    };
    b.statement(b.call("draw_clear", {b.integer(color(12, 17, 31))}));
    b.statement(b.call("draw_set_alpha", {b.n(1)}));
    shade(21, 30, 49);
    rect(0, 0, 640, 86, false);
    shade(113, 233, 230);
    font(1);
    text(28, 18, "reGML / VM ARCADE");
    shade(175, 191, 211);
    font(0);
    text(28, 48, "ARROWS / WASD MOVE    SPACE SLOW    R RESTART    ESC QUIT");
    text(28, 66, "COLLECT GOLD. DODGE THE MOVING RED BAR.");
    shade(26, 38, 58);
    rect(36, 96, 604, 426, false);
    shade(40, 56, 80);
    for (int y = 128; y < 420; y += 32)
        for (int x = 68; x < 600; x += 32)
            rect(x, y, x + 1, y + 1, false);
    shade(74, 99, 130);
    rect(36, 96, 604, 426, true);
    shade(255, 198, 75);
    b.statement(b.call("draw_circle", {b.v("target_x"), b.v("target_y"), b.n(11), b.integer(0)}));
    shade(255, 232, 158);
    b.statement(b.call("draw_circle", {b.v("target_x"), b.v("target_y"), b.n(16), b.integer(1)}));
    shade(252, 94, 116);
    b.statement(b.call("draw_rectangle", {b.math(Opcode::sub, b.v("hazard_x"), b.n(40)), b.n(242),
                                          b.math(Opcode::add, b.v("hazard_x"), b.n(40)), b.n(258), b.integer(0)}));
    b.when(b.cmp(5, b.v("cooldown"), b.n(0)), [&] { b.statement(b.call("draw_set_alpha", {b.n(0.45)})); });
    shade(106, 229, 242);
    b.statement(b.call("draw_rectangle",
                       {b.math(Opcode::sub, b.v("x"), b.n(8)), b.math(Opcode::sub, b.v("y"), b.n(8)),
                        b.math(Opcode::add, b.v("x"), b.n(8)), b.math(Opcode::add, b.v("y"), b.n(8)), b.integer(0)}));
    shade(235, 255, 255);
    b.statement(b.call("draw_rectangle",
                       {b.math(Opcode::sub, b.v("x"), b.n(3)), b.math(Opcode::sub, b.v("y"), b.n(3)),
                        b.math(Opcode::add, b.v("x"), b.n(3)), b.math(Opcode::add, b.v("y"), b.n(3)), b.integer(0)}));
    b.statement(b.call("draw_set_alpha", {b.n(1)}));
    font(1);
    shade(229, 238, 251);
    b.statement(b.call("draw_text",
                       {b.n(36), b.n(445), b.math(Opcode::add, b.text("SCORE: "), b.call("string", {b.v("score")}))}));
    shade(255, 161, 163);
    b.statement(b.call("draw_text",
                       {b.n(452), b.n(445), b.math(Opcode::add, b.text("LIVES: "), b.call("string", {b.v("lives")}))}));
    font(0);
    shade(116, 137, 164);
    text(222, 449, "CREATE > STEP > DRAW");
    b.when(b.v("game_over"), [&] {
        shade(12, 17, 31);
        rect(126, 171, 514, 307, false);
        shade(252, 94, 116);
        rect(126, 171, 514, 307, true);
        font(1);
        text(260, 199, "GAME OVER");
        shade(229, 238, 251);
        text(212, 243, "PRESS R TO RESTART");
    });
    return b.finish();
}
void arity(std::span<const Value> args, std::size_t count) {
    if (args.size() != count)
        throw Error(ErrorKind::vm, "incorrect built-in argument count");
}
double number(const Value &value) {
    const auto result = value_number(value);
    if (!std::isfinite(result))
        throw Error(ErrorKind::vm, "built-in numeric argument must be finite");
    return result;
}
std::uint32_t integer(const Value &value, std::uint32_t maximum) {
    const auto result = number(value);
    if (result < 0 || result > maximum || std::floor(result) != result)
        throw Error(ErrorKind::vm, "built-in integer argument is out of range");
    return static_cast<std::uint32_t>(result);
}
} // namespace
DemoSession::DemoSession() : create_(create_program()), step_(step_program()), draw_(draw_program()) {
    run(create_);
    draw();
}
void DemoSession::run(const BoundProgram &code) {
    ExecutionContext context;
    context.bindings = &code.bindings;
    context.self = &self_;
    context.globals = &globals_;
    context.call_builtin = [this](std::string_view name, std::span<const Value> args) { return builtin(name, args); };
    vm_instructions_ += execute(code.program, context).instructions_executed;
}
void DemoSession::tick(const KeyboardState &held, const KeyboardState &pressed_events) {
    held_ = held;
    for (std::size_t i = 0; i < held.size(); ++i)
        pressed_[i] = pressed_events[i] || (held[i] && !previous_[i]);
    previous_ = held;
    run(step_);
    if (restart_requested_) {
        restart_requested_ = false;
        self_.clear();
        globals_.clear();
        run(create_);
    }
    ++frames_;
}
void DemoSession::draw() {
    run(draw_);
}
Value DemoSession::builtin(std::string_view name, std::span<const Value> a) {
    if (name == "keyboard_check" || name == "keyboard_check_pressed") {
        arity(a, 1);
        const auto key = integer(a[0], 255);
        return name == "keyboard_check" ? held_[key] : pressed_[key];
    }
    if (name == "game_restart") {
        arity(a, 0);
        restart_requested_ = true;
        return std::monostate{};
    }
    if (name == "sin") {
        arity(a, 1);
        return std::sin(number(a[0]));
    }
    if (name == "max") {
        arity(a, 2);
        return std::max(number(a[0]), number(a[1]));
    }
    if (name == "clamp") {
        arity(a, 3);
        const auto low = number(a[1]), high = number(a[2]);
        if (low > high)
            throw Error(ErrorKind::vm, "clamp minimum exceeds maximum");
        return std::clamp(number(a[0]), low, high);
    }
    if (name == "point_distance") {
        arity(a, 4);
        return std::hypot(number(a[0]) - number(a[2]), number(a[1]) - number(a[3]));
    }
    if (name == "point_in_rectangle") {
        arity(a, 6);
        const auto x = number(a[0]), y = number(a[1]), x1 = number(a[2]), y1 = number(a[3]), x2 = number(a[4]),
                   y2 = number(a[5]);
        return x >= std::min(x1, x2) && x <= std::max(x1, x2) && y >= std::min(y1, y2) && y <= std::max(y1, y2);
    }
    if (name == "string") {
        arity(a, 1);
        return value_text(a[0]);
    }
    if (name == "draw_clear") {
        arity(a, 1);
        canvas_.clear(integer(a[0], 0xffffff));
    } else if (name == "draw_set_color") {
        arity(a, 1);
        canvas_.color(integer(a[0], 0xffffff));
    } else if (name == "draw_set_alpha") {
        arity(a, 1);
        canvas_.alpha(number(a[0]));
    } else if (name == "draw_set_font") {
        arity(a, 1);
        canvas_.font(static_cast<int>(integer(a[0], 1)));
    } else if (name == "draw_rectangle") {
        arity(a, 5);
        canvas_.rectangle(number(a[0]), number(a[1]), number(a[2]), number(a[3]), number(a[4]) != 0);
    } else if (name == "draw_circle") {
        arity(a, 4);
        canvas_.circle(number(a[0]), number(a[1]), number(a[2]), number(a[3]) != 0);
    } else if (name == "draw_text") {
        arity(a, 3);
        const auto *text = std::get_if<std::string>(&a[2]);
        if (!text)
            throw Error(ErrorKind::vm, "draw_text requires a string");
        canvas_.text(number(a[0]), number(a[1]), *text);
    } else
        throw Error(ErrorKind::unsupported, "built-in is not implemented: " + std::string(name));
    return std::monostate{};
}
KeyboardState demo_autoplay(std::size_t frame) {
    KeyboardState held{};
    if (frame % 160 < 40)
        held[39] = true;
    else if (frame % 160 < 120)
        held[37] = true;
    else
        held[39] = true;
    return held;
}
} // namespace regml
