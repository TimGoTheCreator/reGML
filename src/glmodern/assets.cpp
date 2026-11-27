#include "assets.hpp"
#include "canvas.hpp"
#include "gl_common.hpp"
#include "gl_core.hpp"
#include "rendersoftware/assets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace regml::glmodern {

GLModernAssets *GLModernAssets::s_active_ = nullptr;

namespace {

constexpr double PI = 3.14159265358979323846;

} // namespace

GLModernAssets::GLModernAssets() noexcept {
    if (!s_active_) {
        s_active_ = this;
    }
}

GLModernAssets::GLModernAssets(const regml::Package &package)
    : owned_source_assets_(std::make_unique<regml::GameAssets>(package)),
      source_assets_(owned_source_assets_.get()) {
    if (!s_active_) {
        s_active_ = this;
    }
}

GLModernAssets::GLModernAssets(const regml::Package &package, const regml::GmrtProject &project)
    : owned_source_assets_(std::make_unique<regml::GameAssets>(package, project)),
      source_assets_(owned_source_assets_.get()) {
    if (!s_active_) {
        s_active_ = this;
    }
}

GLModernAssets::GLModernAssets(const regml::GameAssets &source)
    : source_assets_(&source) {
    if (!s_active_) {
        s_active_ = this;
    }
}

GLModernAssets::~GLModernAssets() {
    release_gl_resources();
    if (s_active_ == this) {
        s_active_ = nullptr;
    }
}

GLModernAssets::GLModernAssets(GLModernAssets &&other) noexcept
    : view_x_(other.view_x_),
      view_y_(other.view_y_),
      view_sx_(other.view_sx_),
      view_sy_(other.view_sy_),
      port_x_(other.port_x_),
      port_y_(other.port_y_),
      sprites_(std::move(other.sprites_)),
      sprite_frames_storage_(std::move(other.sprite_frames_storage_)),
      regions_(std::move(other.regions_)),
      pages_(std::move(other.pages_)),
      backgrounds_(std::move(other.backgrounds_)),
      fonts_(std::move(other.fonts_)),
      owned_source_assets_(std::move(other.owned_source_assets_)),
      source_assets_(other.source_assets_) {
    if (s_active_ == &other) {
        s_active_ = this;
    }
}

GLModernAssets &GLModernAssets::operator=(GLModernAssets &&other) noexcept {
    if (this != &other) {
        release_gl_resources();
        view_x_ = other.view_x_;
        view_y_ = other.view_y_;
        view_sx_ = other.view_sx_;
        view_sy_ = other.view_sy_;
        port_x_ = other.port_x_;
        port_y_ = other.port_y_;
        sprites_ = std::move(other.sprites_);
        sprite_frames_storage_ = std::move(other.sprite_frames_storage_);
        regions_ = std::move(other.regions_);
        pages_ = std::move(other.pages_);
        backgrounds_ = std::move(other.backgrounds_);
        fonts_ = std::move(other.fonts_);
        source_assets_ = other.source_assets_;
        owned_source_assets_ = std::move(other.owned_source_assets_);
        if (s_active_ == &other) {
            s_active_ = this;
        }
    }
    return *this;
}

GLModernAssets *GLModernAssets::active() noexcept {
    return s_active_;
}

std::size_t GLModernAssets::frame_index(double frame, std::size_t count) noexcept {
    if (count == 0) return 0;
    const double wrapped = std::fmod(std::floor(frame), static_cast<double>(count));
    const double positive = wrapped < 0.0 ? wrapped + static_cast<double>(count) : wrapped;
    return std::min(static_cast<std::size_t>(positive), count - 1);
}

bool GLModernAssets::sprite_exists(int id) const noexcept {
    if (source_assets_) {
        return source_assets_->sprite_exists(id);
    }
    if (id < 0 || static_cast<std::size_t>(id) >= sprites_.size()) {
        return false;
    }
    return sprites_[static_cast<std::size_t>(id)].valid;
}

const GLModernAssets::Sprite &GLModernAssets::sprite(int id) const {
    if (source_assets_) {
        const auto &src = source_assets_->sprite(id);
        thread_local Sprite converted{};
        converted.name = src.name;
        converted.width = src.width;
        converted.height = src.height;
        converted.ox = src.ox;
        converted.oy = src.oy;
        converted.left = src.left;
        converted.right = src.right;
        converted.top = src.top;
        converted.bottom = src.bottom;
        converted.bbox_mode = src.bbox_mode;
        converted.auto_left = src.auto_left;
        converted.auto_right = src.auto_right;
        converted.auto_top = src.auto_top;
        converted.auto_bottom = src.auto_bottom;
        converted.frames = src.frames;
        converted.precise = src.precise;
        converted.dynamic = src.dynamic;
        converted.valid = src.valid;
        converted.mask_offset = src.mask_offset;
        converted.mask_count = src.mask_count;
        return converted;
    }
    if (id < 0 || static_cast<std::size_t>(id) >= sprites_.size()) {
        static const Sprite empty{};
        return empty;
    }
    return sprites_[static_cast<std::size_t>(id)];
}

std::string_view GLModernAssets::sprite_name(int id) const {
    if (source_assets_) {
        return source_assets_->sprite_name(id);
    }
    return sprite(id).name;
}

std::size_t GLModernAssets::sprite_count() const noexcept {
    if (source_assets_) {
        return source_assets_->sprite_count();
    }
    return sprites_.size();
}

std::optional<GLModernAssets::SpriteFrameInfo> GLModernAssets::sprite_frame_info(int id, std::size_t frame) const {
    if (source_assets_) {
        const auto info = source_assets_->sprite_frame_info(id, frame);
        if (!info) return std::nullopt;
        SpriteFrameInfo converted{};
        converted.x = info->x;
        converted.y = info->y;
        converted.width = info->width;
        converted.height = info->height;
        converted.texture = info->texture;
        converted.original_width = info->original_width;
        converted.original_height = info->original_height;
        converted.crop_width = info->crop_width;
        converted.crop_height = info->crop_height;
        converted.x_offset = info->x_offset;
        converted.y_offset = info->y_offset;
        return converted;
    }
    if (!sprite_exists(id)) return std::nullopt;
    const auto &spr = sprite(id);
    if (spr.frames.empty() || frame >= spr.frames.size()) return std::nullopt;
    const auto region_index = spr.frames[frame];
    if (region_index >= regions_.size()) return std::nullopt;
    const auto &r = regions_[region_index];
    SpriteFrameInfo info{};
    info.x = r.x;
    info.y = r.y;
    info.width = r.w;
    info.height = r.h;
    info.texture = r.page;
    info.original_width = spr.width;
    info.original_height = spr.height;
    info.crop_width = r.tw;
    info.crop_height = r.th;
    info.x_offset = r.tx;
    info.y_offset = r.ty;
    return info;
}

std::array<double, 8> GLModernAssets::sprite_uvs(int id, double frame) const {
    if (source_assets_) {
        return const_cast<regml::GameAssets *>(source_assets_)->sprite_uvs(id, frame);
    }
    if (!sprite_exists(id)) return {};
    const auto &spr = sprite(id);
    if (spr.frames.empty() || !std::isfinite(frame)) return {};

    const auto idx = frame_index(frame, spr.frames.size());
    const auto region_index = spr.frames[idx];
    if (region_index >= regions_.size()) return {};
    const auto &r = regions_[region_index];
    int pw = 2048, ph = 2048;
    if (r.page >= 0 && static_cast<std::size_t>(r.page) < pages_.size()) {
        if (pages_[r.page].width > 0) pw = pages_[r.page].width;
        if (pages_[r.page].height > 0) ph = pages_[r.page].height;
    }

    return {
        static_cast<double>(r.x) / pw,
        static_cast<double>(r.y) / ph,
        static_cast<double>(r.x + r.w) / pw,
        static_cast<double>(r.y + r.h) / ph,
        static_cast<double>(r.tx),
        static_cast<double>(r.ty),
        spr.width > 0 ? static_cast<double>(r.tw) / spr.width : 0.0,
        spr.height > 0 ? static_cast<double>(r.th) / spr.height : 0.0
    };
}

void GLModernAssets::texture_texel_size(int id, double frame, double &width, double &height) const {
    width = height = 0.0;
    if (source_assets_) {
        const_cast<regml::GameAssets *>(source_assets_)->texture_texel_size(id, frame, width, height);
        return;
    }
    if (!sprite_exists(id)) return;
    const auto &spr = sprite(id);
    if (spr.frames.empty() || !std::isfinite(frame)) return;
    const auto idx = frame_index(frame, spr.frames.size());
    const auto region_index = spr.frames[idx];
    if (region_index >= regions_.size()) return;
    const auto &r = regions_[region_index];
    if (r.page >= 0 && static_cast<std::size_t>(r.page) < pages_.size()) {
        const auto &p = pages_[r.page];
        if (p.width > 0 && p.height > 0) {
            width = 1.0 / p.width;
            height = 1.0 / p.height;
        }
    }
}

std::pair<int, int> GLModernAssets::background_size(int id) const {
    if (source_assets_) {
        return source_assets_->background_size(id);
    }
    if (id < 0 || static_cast<std::size_t>(id) >= backgrounds_.size()) {
        return {0, 0};
    }
    const auto region_index = backgrounds_[static_cast<std::size_t>(id)];
    if (region_index < 0 || static_cast<std::size_t>(region_index) >= regions_.size()) {
        return {0, 0};
    }
    const auto &r = regions_[static_cast<std::size_t>(region_index)];
    return {r.tw, r.th};
}

void GLModernAssets::view(double x, double y, double sx, double sy, double px, double py) noexcept {
    view_x_ = x;
    view_y_ = y;
    view_sx_ = sx;
    view_sy_ = sy;
    port_x_ = px;
    port_y_ = py;
}

std::array<double, 2> GLModernAssets::transform_point(double x, double y) const noexcept {
    return {(x - view_x_) * view_sx_ + port_x_, (y - view_y_) * view_sy_ + port_y_};
}

int GLModernAssets::add_sprite(Sprite spr, std::vector<std::uint32_t> frame_regions) {
    const int id = static_cast<int>(sprites_.size());
    sprite_frames_storage_.push_back(std::move(frame_regions));
    spr.frames = sprite_frames_storage_.back();
    spr.valid = true;
    sprites_.push_back(spr);
    return id;
}

int GLModernAssets::add_region(Region region) {
    const int index = static_cast<int>(regions_.size());
    regions_.push_back(region);
    return index;
}

int GLModernAssets::add_page(int width, int height, std::span<const std::uint32_t> pixels, GLuint texture_id) {
    const int index = static_cast<int>(pages_.size());
    Page page;
    page.width = width;
    page.height = height;
    page.texture_id = texture_id;
    page.owned_texture = (texture_id == 0);
    if (!pixels.empty()) {
        page.pixels.assign(pixels.begin(), pixels.end());
    }
    pages_.push_back(std::move(page));
    return index;
}

int GLModernAssets::add_background(int region_index) {
    const int id = static_cast<int>(backgrounds_.size());
    backgrounds_.push_back(region_index);
    return id;
}

GLuint GLModernAssets::page_texture(int page_index) {
    if (page_index < 0 || static_cast<std::size_t>(page_index) >= pages_.size()) {
        return 0;
    }
    auto &p = pages_[static_cast<std::size_t>(page_index)];
    if (p.texture_id != 0) {
        return p.texture_id;
    }

    if (!p.pixels.empty() && p.width > 0 && p.height > 0) {
        if (glGenTextures && glBindTexture && glTexParameteri && glTexImage2D) {
            glGenTextures(1, &p.texture_id);
            glBindTexture(GL_TEXTURE_2D, p.texture_id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, p.width, p.height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, p.pixels.data());
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }
    return p.texture_id;
}

void GLModernAssets::release_gl_resources() noexcept {
    for (auto &p : pages_) {
        if (p.texture_id != 0 && p.owned_texture && glDeleteTextures) {
            glDeleteTextures(1, &p.texture_id);
            p.texture_id = 0;
        }
    }
}

void GLModernAssets::blit(GLModernCanvas &canvas,
                          const Region &r,
                          double x,
                          double y,
                          double sx,
                          double sy,
                          double angle,
                          unsigned color,
                          double alpha) {
    x = (x - view_x_) * view_sx_ + port_x_;
    y = (y - view_y_) * view_sy_ + port_y_;
    x *= canvas.render_scale();
    y *= canvas.render_scale();
    sx *= view_sx_ * canvas.render_scale();
    sy *= view_sy_ * canvas.render_scale();

    if (!std::isfinite(x + y + sx + sy + angle + alpha)) {
        throw Error(ErrorKind::vm, "nonfinite sprite transform");
    }
    if (sx == 0.0 || sy == 0.0 || r.w == 0 || r.h == 0 || alpha <= 0.0) {
        return;
    }

    const GLuint tex_id = page_texture(r.page);
    int pw = 2048, ph = 2048;
    if (r.page >= 0 && static_cast<std::size_t>(r.page) < pages_.size()) {
        if (pages_[r.page].width > 0) pw = pages_[r.page].width;
        if (pages_[r.page].height > 0) ph = pages_[r.page].height;
    }

    const float inv_pw = 1.0f / static_cast<float>(pw);
    const float inv_ph = 1.0f / static_cast<float>(ph);

    const float u0 = static_cast<float>(r.x) * inv_pw;
    const float v0 = static_cast<float>(r.y) * inv_ph;
    const float u1 = static_cast<float>(r.x + r.w) * inv_pw;
    const float v1 = static_cast<float>(r.y + r.h) * inv_ph;

    const float dw = static_cast<float>(r.tw * sx);
    const float dh = static_cast<float>(r.th * sy);
    const std::uint32_t c = gml_to_rgba(color, alpha);

    auto &batcher = canvas.batcher();

    if (angle == 0.0) {
        batcher.draw_quad(tex_id, static_cast<float>(x), static_cast<float>(y),
                          dw, dh, u0, v0, u1, v1, c, 0.0f);
    } else {
        const double rad = angle * PI / 180.0;
        const double cs = std::cos(rad), sn = std::sin(rad);
        const double wx = dw * cs, wy = -dw * sn;
        const double hx = dh * sn, hy = dh * cs;

        const Vertex v_tl{static_cast<float>(x), static_cast<float>(y), u0, v0, c};
        const Vertex v_tr{static_cast<float>(x + wx), static_cast<float>(y + wy), u1, v0, c};
        const Vertex v_br{static_cast<float>(x + wx + hx), static_cast<float>(y + wy + hy), u1, v1, c};
        const Vertex v_bl{static_cast<float>(x + hx), static_cast<float>(y + hy), u0, v1, c};

        batcher.draw_quad_corners(tex_id, {v_tl, v_tr, v_br, v_bl});
    }
}

void GLModernAssets::draw_sprite(GLModernCanvas &canvas,
                                 int id,
                                 double frame,
                                 double x,
                                 double y,
                                 double sx,
                                 double sy,
                                 double angle,
                                 unsigned color,
                                 double alpha) {
    if (!sprite_exists(id)) return;
    const auto &spr = sprite(id);
    if (spr.frames.empty()) return;
    if (!std::isfinite(frame)) {
        throw Error(ErrorKind::vm, "nonfinite sprite frame");
    }

    const auto index = frame_index(frame, spr.frames.size());
    const auto region_index = spr.frames[index];
    if (region_index == std::numeric_limits<std::uint32_t>::max()) {
        return;
    }

    if (source_assets_) {
        const auto info = sprite_frame_info(id, index);
        if (!info) return;

        Region r;
        r.x = info->x;
        r.y = info->y;
        r.w = info->width;
        r.h = info->height;
        r.tx = info->x_offset;
        r.ty = info->y_offset;
        r.tw = info->crop_width;
        r.th = info->crop_height;
        r.page = info->texture;

        const double ox = (r.tx - spr.ox) * sx;
        const double oy = (r.ty - spr.oy) * sy;

        if (angle == 0.0) {
            blit(canvas, r, x + ox, y + oy, sx, sy, 0.0, color, alpha);
        } else {
            const double rad = angle * PI / 180.0;
            blit(canvas, r,
                 x + ox * std::cos(rad) + oy * std::sin(rad),
                 y - ox * std::sin(rad) + oy * std::cos(rad),
                 sx, sy, angle, color, alpha);
        }
        return;
    }

    if (region_index >= regions_.size()) return;
    const auto &r = regions_[region_index];
    const double ox = (r.tx - spr.ox) * sx;
    const double oy = (r.ty - spr.oy) * sy;

    if (angle == 0.0) {
        blit(canvas, r, x + ox, y + oy, sx, sy, 0.0, color, alpha);
    } else {
        const double rad = angle * PI / 180.0;
        blit(canvas, r,
             x + ox * std::cos(rad) + oy * std::sin(rad),
             y - ox * std::sin(rad) + oy * std::cos(rad),
             sx, sy, angle, color, alpha);
    }
}

void GLModernAssets::draw_sprite_ext(GLModernCanvas &canvas,
                                     int id,
                                     double frame,
                                     double x,
                                     double y,
                                     double sx,
                                     double sy,
                                     double angle,
                                     unsigned color,
                                     double alpha) {
    draw_sprite(canvas, id, frame, x, y, sx, sy, angle, color, alpha);
}

void GLModernAssets::draw_sprite_part(GLModernCanvas &canvas,
                                      int id,
                                      double frame,
                                      double left,
                                      double top,
                                      double width,
                                      double height,
                                      double x,
                                      double y,
                                      double sx,
                                      double sy,
                                      unsigned color,
                                      double alpha) {
    if (!sprite_exists(id)) return;
    const auto &spr = sprite(id);
    if (spr.frames.empty()) return;
    if (!std::isfinite(frame + left + top + width + height + x + y + sx + sy + alpha)) {
        throw Error(ErrorKind::vm, "nonfinite sprite-part argument");
    }
    if (width <= 0.0 || height <= 0.0 || sx == 0.0 || sy == 0.0 || alpha <= 0.0) {
        return;
    }

    const auto index = frame_index(frame, spr.frames.size());
    const auto region_index = spr.frames[index];
    if (region_index == std::numeric_limits<std::uint32_t>::max()) {
        return;
    }

    Region r;
    if (source_assets_) {
        const auto info = sprite_frame_info(id, index);
        if (!info) return;
        r.x = info->x;
        r.y = info->y;
        r.w = info->width;
        r.h = info->height;
        r.tx = info->x_offset;
        r.ty = info->y_offset;
        r.tw = info->crop_width;
        r.th = info->crop_height;
        r.page = info->texture;
    } else {
        if (region_index >= regions_.size()) return;
        r = regions_[region_index];
    }

    const double l = std::max({left, 0.0, static_cast<double>(r.tx)});
    const double t = std::max({top, 0.0, static_cast<double>(r.ty)});
    const double rr = std::min({left + width, static_cast<double>(spr.width), static_cast<double>(r.tx + r.tw)});
    const double bb = std::min({top + height, static_cast<double>(spr.height), static_cast<double>(r.ty + r.th)});

    if (rr <= l || bb <= t || r.tw <= 0 || r.th <= 0) {
        return;
    }

    const int ix = static_cast<int>(std::floor(l));
    const int iy = static_cast<int>(std::floor(t));
    const int ir = static_cast<int>(std::ceil(rr));
    const int ib = static_cast<int>(std::ceil(bb));

    const Region crop{
        r.x + static_cast<int>((ix - r.tx) * r.w / r.tw),
        r.y + static_cast<int>((iy - r.ty) * r.h / r.th),
        static_cast<int>((ir - ix) * r.w / r.tw),
        static_cast<int>((ib - iy) * r.h / r.th),
        0,
        0,
        ir - ix,
        ib - iy,
        r.page,
        r.alpha_mode
    };

    blit(canvas, crop, x + (ix - left) * sx, y + (iy - top) * sy, sx, sy, 0.0, color, alpha);
}

void GLModernAssets::draw_sprite_pos(GLModernCanvas &canvas,
                                     int id,
                                     double frame,
                                     const std::array<double, 8> &corners,
                                     double alpha) {
    if (!sprite_exists(id)) return;
    const auto &spr = sprite(id);
    if (spr.frames.empty() || !std::isfinite(frame + alpha) ||
        !std::all_of(corners.begin(), corners.end(), [](double v) { return std::isfinite(v); })) {
        return;
    }
    if (alpha <= 0.0 || spr.width <= 0 || spr.height <= 0) return;

    const auto index = frame_index(frame, spr.frames.size());
    const auto region_index = spr.frames[index];

    Region r;
    if (source_assets_) {
        const auto info = sprite_frame_info(id, index);
        if (!info) return;
        r.x = info->x;
        r.y = info->y;
        r.w = info->width;
        r.h = info->height;
        r.tx = info->x_offset;
        r.ty = info->y_offset;
        r.tw = info->crop_width;
        r.th = info->crop_height;
        r.page = info->texture;
    } else {
        if (region_index >= regions_.size()) return;
        r = regions_[region_index];
    }

    const GLuint tex_id = page_texture(r.page);
    int pw = 2048, ph = 2048;
    if (r.page >= 0 && static_cast<std::size_t>(r.page) < pages_.size()) {
        if (pages_[r.page].width > 0) pw = pages_[r.page].width;
        if (pages_[r.page].height > 0) ph = pages_[r.page].height;
    }

    const float u0 = static_cast<float>(r.x) / static_cast<float>(pw);
    const float v0 = static_cast<float>(r.y) / static_cast<float>(ph);
    const float u1 = static_cast<float>(r.x + r.w) / static_cast<float>(pw);
    const float v1 = static_cast<float>(r.y + r.h) / static_cast<float>(ph);

    const std::uint32_t c = make_rgba(255, 255, 255, static_cast<std::uint8_t>(std::clamp(alpha, 0.0, 1.0) * 255.0 + 0.5));

    const auto t = [&](double cx, double cy) {
        const double tx = (cx - view_x_) * view_sx_ + port_x_;
        const double ty = (cy - view_y_) * view_sy_ + port_y_;
        return std::pair<float, float>{
            static_cast<float>(tx * canvas.render_scale()),
            static_cast<float>(ty * canvas.render_scale())
        };
    };

    const auto p0 = t(corners[0], corners[1]);
    const auto p1 = t(corners[2], corners[3]);
    const auto p2 = t(corners[4], corners[5]);
    const auto p3 = t(corners[6], corners[7]);

    const Vertex vert0{p0.first, p0.second, u0, v0, c};
    const Vertex vert1{p1.first, p1.second, u1, v0, c};
    const Vertex vert2{p2.first, p2.second, u1, v1, c};
    const Vertex vert3{p3.first, p3.second, u0, v1, c};

    canvas.batcher().draw_quad_corners(tex_id, {vert0, vert1, vert2, vert3});
}

void GLModernAssets::draw_sprite_general(GLModernCanvas &canvas,
                                         int id,
                                         double frame,
                                         double left,
                                         double top,
                                         double width,
                                         double height,
                                         double x,
                                         double y,
                                         double sx,
                                         double sy,
                                         double angle,
                                         const std::array<unsigned, 4> &colors,
                                         double alpha) {
    if (!sprite_exists(id)) return;
    const auto &spr = sprite(id);
    if (spr.frames.empty()) return;
    if (!std::isfinite(frame + left + top + width + height + x + y + sx + sy + angle + alpha)) {
        throw Error(ErrorKind::vm, "nonfinite sprite-general argument");
    }
    if (width <= 0.0 || height <= 0.0 || sx == 0.0 || sy == 0.0 || alpha <= 0.0) return;

    const auto index = frame_index(frame, spr.frames.size());
    const auto region_index = spr.frames[index];

    Region r;
    if (source_assets_) {
        const auto info = sprite_frame_info(id, index);
        if (!info) return;
        r.x = info->x;
        r.y = info->y;
        r.w = info->width;
        r.h = info->height;
        r.tx = info->x_offset;
        r.ty = info->y_offset;
        r.tw = info->crop_width;
        r.th = info->crop_height;
        r.page = info->texture;
    } else {
        if (region_index >= regions_.size()) return;
        r = regions_[region_index];
    }

    const double l = std::max({left, 0.0, static_cast<double>(r.tx)});
    const double t = std::max({top, 0.0, static_cast<double>(r.ty)});
    const double rr = std::min({left + width, static_cast<double>(spr.width), static_cast<double>(r.tx + r.tw)});
    const double bb = std::min({top + height, static_cast<double>(spr.height), static_cast<double>(r.ty + r.th)});

    if (rr <= l || bb <= t || r.tw <= 0 || r.th <= 0 || r.w <= 0 || r.h <= 0) return;

    const int source_left = static_cast<int>(std::floor(l));
    const int source_top = static_cast<int>(std::floor(t));
    const int source_right = static_cast<int>(std::ceil(rr));
    const int source_bottom = static_cast<int>(std::ceil(bb));

    const int crop_x = r.x + static_cast<int>((source_left - r.tx) * r.w / r.tw);
    const int crop_y = r.y + static_cast<int>((source_top - r.ty) * r.h / r.th);
    const int crop_w = static_cast<int>((source_right - source_left) * r.w / r.tw);
    const int crop_h = static_cast<int>((source_bottom - source_top) * r.h / r.th);

    const GLuint tex_id = page_texture(r.page);
    int pw = 2048, ph = 2048;
    if (r.page >= 0 && static_cast<std::size_t>(r.page) < pages_.size()) {
        if (pages_[r.page].width > 0) pw = pages_[r.page].width;
        if (pages_[r.page].height > 0) ph = pages_[r.page].height;
    }

    const float u0 = static_cast<float>(crop_x) / static_cast<float>(pw);
    const float v0 = static_cast<float>(crop_y) / static_cast<float>(ph);
    const float u1 = static_cast<float>(crop_x + crop_w) / static_cast<float>(pw);
    const float v1 = static_cast<float>(crop_y + crop_h) / static_cast<float>(ph);

    double draw_x = x + (source_left - left) * sx;
    double draw_y = y + (source_top - top) * sy;
    draw_x = (draw_x - view_x_) * view_sx_ + port_x_;
    draw_y = (draw_y - view_y_) * view_sy_ + port_y_;
    draw_x *= canvas.render_scale();
    draw_y *= canvas.render_scale();

    const double screen_sx = sx * view_sx_ * canvas.render_scale();
    const double screen_sy = sy * view_sy_ * canvas.render_scale();
    const double dw = (source_right - source_left) * screen_sx;
    const double dh = (source_bottom - source_top) * screen_sy;

    const std::uint32_t c0 = gml_to_rgba(colors[0], alpha);
    const std::uint32_t c1 = gml_to_rgba(colors[1], alpha);
    const std::uint32_t c2 = gml_to_rgba(colors[2], alpha);
    const std::uint32_t c3 = gml_to_rgba(colors[3], alpha);

    if (angle == 0.0) {
        const Vertex vert0{static_cast<float>(draw_x), static_cast<float>(draw_y), u0, v0, c0};
        const Vertex vert1{static_cast<float>(draw_x + dw), static_cast<float>(draw_y), u1, v0, c1};
        const Vertex vert2{static_cast<float>(draw_x + dw), static_cast<float>(draw_y + dh), u1, v1, c2};
        const Vertex vert3{static_cast<float>(draw_x), static_cast<float>(draw_y + dh), u0, v1, c3};
        canvas.batcher().draw_quad_corners(tex_id, {vert0, vert1, vert2, vert3});
    } else {
        const double rad = angle * PI / 180.0;
        const double cs = std::cos(rad), sn = std::sin(rad);
        const double wx = dw * cs, wy = -dw * sn;
        const double hx = dh * sn, hy = dh * cs;

        const Vertex vert0{static_cast<float>(draw_x), static_cast<float>(draw_y), u0, v0, c0};
        const Vertex vert1{static_cast<float>(draw_x + wx), static_cast<float>(draw_y + wy), u1, v0, c1};
        const Vertex vert2{static_cast<float>(draw_x + wx + hx), static_cast<float>(draw_y + wy + hy), u1, v1, c2};
        const Vertex vert3{static_cast<float>(draw_x + hx), static_cast<float>(draw_y + hy), u0, v1, c3};
        canvas.batcher().draw_quad_corners(tex_id, {vert0, vert1, vert2, vert3});
    }
}

void GLModernAssets::draw_sprite_stretched(GLModernCanvas &canvas,
                                           int id,
                                           double frame,
                                           double x,
                                           double y,
                                           double width,
                                           double height,
                                           unsigned color,
                                           double alpha) {
    if (!sprite_exists(id)) return;
    const auto &spr = sprite(id);
    const double sx = spr.width > 0 ? width / spr.width : 1.0;
    const double sy = spr.height > 0 ? height / spr.height : 1.0;
    draw_sprite(canvas, id, frame, x, y, sx, sy, 0.0, color, alpha);
}

void GLModernAssets::draw_background(GLModernCanvas &canvas, int id, double x, double y) {
    if (source_assets_) {
        const auto size = background_size(id);
        if (size.first <= 0 || size.second <= 0) return;
        Region r;
        r.x = 0; r.y = 0; r.w = size.first; r.h = size.second;
        r.tx = 0; r.ty = 0; r.tw = size.first; r.th = size.second;
        r.page = id;
        blit(canvas, r, x, y, 1.0, 1.0, 0.0, 0xffffff, 1.0);
        return;
    }
    if (id < 0 || static_cast<std::size_t>(id) >= backgrounds_.size()) {
        throw Error(ErrorKind::vm, "background ID out of range");
    }
    const auto region_index = backgrounds_[static_cast<std::size_t>(id)];
    if (region_index < 0 || static_cast<std::size_t>(region_index) >= regions_.size()) return;
    const auto &r = regions_[static_cast<std::size_t>(region_index)];
    blit(canvas, r, x + r.tx, y + r.ty, 1.0, 1.0, 0.0, 0xffffff, 1.0);
}

void GLModernAssets::draw_background_stretched(GLModernCanvas &canvas, int id, double x, double y, double width, double height) {
    if (source_assets_) {
        const auto size = background_size(id);
        if (size.first <= 0 || size.second <= 0) return;
        Region r;
        r.x = 0; r.y = 0; r.w = size.first; r.h = size.second;
        r.tx = 0; r.ty = 0; r.tw = size.first; r.th = size.second;
        r.page = id;
        blit(canvas, r, x, y, width / size.first, height / size.second, 0.0, 0xffffff, 1.0);
        return;
    }
    if (id < 0 || static_cast<std::size_t>(id) >= backgrounds_.size()) {
        throw Error(ErrorKind::vm, "background ID out of range");
    }
    const auto region_index = backgrounds_[static_cast<std::size_t>(id)];
    if (region_index < 0 || static_cast<std::size_t>(region_index) >= regions_.size()) return;
    const auto &r = regions_[static_cast<std::size_t>(region_index)];
    if (r.tw > 0 && r.th > 0) {
        blit(canvas, r, x, y, width / r.tw, height / r.th, 0.0, 0xffffff, 1.0);
    }
}

void GLModernAssets::draw_background_part_ext(GLModernCanvas &canvas,
                                              int id,
                                              double left,
                                              double top,
                                              double width,
                                              double height,
                                              double x,
                                              double y,
                                              double sx,
                                              double sy,
                                              unsigned color,
                                              double alpha) {
    Region r;
    if (source_assets_) {
        const auto size = background_size(id);
        if (size.first <= 0 || size.second <= 0) return;
        r.x = 0; r.y = 0; r.w = size.first; r.h = size.second;
        r.tx = 0; r.ty = 0; r.tw = size.first; r.th = size.second;
        r.page = id;
    } else {
        if (id < 0 || static_cast<std::size_t>(id) >= backgrounds_.size()) {
            throw Error(ErrorKind::vm, "background ID out of range");
        }
        const auto region_index = backgrounds_[static_cast<std::size_t>(id)];
        if (region_index < 0 || static_cast<std::size_t>(region_index) >= regions_.size()) return;
        r = regions_[static_cast<std::size_t>(region_index)];
    }

    const double l = std::max(0.0, left);
    const double t = std::max(0.0, top);
    const double rr = std::min(static_cast<double>(r.tw), left + width);
    const double bb = std::min(static_cast<double>(r.th), top + height);
    if (rr <= l || bb <= t || r.tw <= 0 || r.th <= 0) return;

    const Region crop{
        r.x + static_cast<int>((l - r.tx) * r.w / r.tw),
        r.y + static_cast<int>((t - r.ty) * r.h / r.th),
        static_cast<int>((rr - l) * r.w / r.tw),
        static_cast<int>((bb - t) * r.h / r.th),
        0, 0,
        static_cast<int>(rr - l),
        static_cast<int>(bb - t),
        r.page,
        r.alpha_mode
    };

    blit(canvas, crop, x, y, sx, sy, 0.0, color, alpha);
}

void GLModernAssets::draw_tile(GLModernCanvas &canvas,
                              int id,
                              int source_x,
                              int source_y,
                              int width,
                              int height,
                              double x,
                              double y,
                              double sx,
                              double sy,
                              unsigned color,
                              std::uint32_t tile_data) {
    Region r;
    if (source_assets_) {
        const auto size = background_size(id);
        if (size.first <= 0 || size.second <= 0) return;
        r.x = 0; r.y = 0; r.w = size.first; r.h = size.second;
        r.tx = 0; r.ty = 0; r.tw = size.first; r.th = size.second;
        r.page = id;
    } else {
        if (id < 0 || static_cast<std::size_t>(id) >= backgrounds_.size()) {
            throw Error(ErrorKind::vm, "tile background ID out of range");
        }
        const auto region_index = backgrounds_[static_cast<std::size_t>(id)];
        if (region_index < 0 || static_cast<std::size_t>(region_index) >= regions_.size()) return;
        r = regions_[static_cast<std::size_t>(region_index)];
    }

    const int left = std::max(source_x, r.tx);
    const int top = std::max(source_y, r.ty);
    const int right = std::min(source_x + width, r.tx + r.tw);
    const int bottom = std::min(source_y + height, r.ty + r.th);
    if (right <= left || bottom <= top || r.tw <= 0 || r.th <= 0) return;

    const int crop_x = r.x + static_cast<int>((left - r.tx) * r.w / r.tw);
    const int crop_y = r.y + static_cast<int>((top - r.ty) * r.h / r.th);
    const int crop_w = static_cast<int>((right - left) * r.w / r.tw);
    const int crop_h = static_cast<int>((bottom - top) * r.h / r.th);

    const GLuint tex_id = page_texture(r.page);
    int pw = 2048, ph = 2048;
    if (r.page >= 0 && static_cast<std::size_t>(r.page) < pages_.size()) {
        if (pages_[r.page].width > 0) pw = pages_[r.page].width;
        if (pages_[r.page].height > 0) ph = pages_[r.page].height;
    }

    float u0 = static_cast<float>(crop_x) / static_cast<float>(pw);
    float v0 = static_cast<float>(crop_y) / static_cast<float>(ph);
    float u1 = static_cast<float>(crop_x + crop_w) / static_cast<float>(pw);
    float v1 = static_cast<float>(crop_y + crop_h) / static_cast<float>(ph);

    if (tile_data & 0x10000000U) {
        std::swap(u0, u1);
    }
    if (tile_data & 0x20000000U) {
        std::swap(v0, v1);
    }

    const double dest_x = ((x + (left - source_x) * sx - view_x_) * view_sx_ + port_x_) * canvas.render_scale();
    const double dest_y = ((y + (top - source_y) * sy - view_y_) * view_sy_ + port_y_) * canvas.render_scale();
    const double dw = (right - left) * sx * view_sx_ * canvas.render_scale();
    const double dh = (bottom - top) * sy * view_sy_ * canvas.render_scale();

    const double alpha = static_cast<double>(color >> 24) / 255.0;
    const std::uint32_t c = gml_to_rgba(color, alpha);
    const float fx = static_cast<float>(dest_x);
    const float fy = static_cast<float>(dest_y);
    const float fw = static_cast<float>(dw);
    const float fh = static_cast<float>(dh);

    if (tile_data & 0x40000000U) {
        const std::array<Vertex, 4> v{{
            {fx, fy, u0, v1, c},
            {fx + fw, fy, u0, v0, c},
            {fx + fw, fy + fh, u1, v0, c},
            {fx, fy + fh, u1, v1, c}
        }};
        canvas.batcher().draw_quad_corners(tex_id, v);
    } else {
        canvas.batcher().draw_quad(tex_id, fx, fy, fw, fh, u0, v0, u1, v1, c, 0.0f);
    }
}

void GLModernAssets::draw_text(GLModernCanvas &canvas,
                               int font,
                               double x,
                               double y,
                               std::string_view text,
                               unsigned color,
                               double alpha,
                               int halign,
                               int valign,
                               double sx,
                               double sy,
                               double angle) {
    if (text.empty() || alpha <= 0.0 || sx == 0.0 || sy == 0.0) return;
    if (font < 0 || static_cast<std::size_t>(font) >= fonts_.size()) return;

    const auto &f = fonts_[static_cast<std::size_t>(font)];
    const std::uint32_t c = gml_to_rgba(color, alpha);
    const double rad = angle * PI / 180.0;
    const double cs = std::cos(rad), sn = std::sin(rad);

    double cursor_x = x;
    double cursor_y = y;

    for (unsigned char ch : text) {
        if (!f.has_glyph[ch]) {
            cursor_x += f.size * sx;
            continue;
        }
        const auto &g = f.glyphs[ch];
        const GLuint tex_id = page_texture(g.page);
        int pw = 2048, ph = 2048;
        if (g.page < pages_.size() && pages_[g.page].width > 0) {
            pw = pages_[g.page].width;
            ph = pages_[g.page].height;
        }

        const float u0 = static_cast<float>(g.x) / static_cast<float>(pw);
        const float v0 = static_cast<float>(g.y) / static_cast<float>(ph);
        const float u1 = static_cast<float>(g.x + g.w) / static_cast<float>(pw);
        const float v1 = static_cast<float>(g.y + g.h) / static_cast<float>(ph);

        const float gw = static_cast<float>(g.w * sx * canvas.render_scale());
        const float gh = static_cast<float>(g.h * sy * canvas.render_scale());
        const float gx = static_cast<float>((cursor_x + g.offset * sx) * canvas.render_scale());
        const float gy = static_cast<float>(cursor_y * canvas.render_scale());

        if (angle == 0.0) {
            canvas.batcher().draw_quad(tex_id, gx, gy, gw, gh, u0, v0, u1, v1, c, 0.0f);
        } else {
            const float wx = gw * static_cast<float>(cs), wy = -gw * static_cast<float>(sn);
            const float hx = gh * static_cast<float>(sn), hy = gh * static_cast<float>(cs);
            const Vertex vert0{gx, gy, u0, v0, c};
            const Vertex vert1{gx + wx, gy + wy, u1, v0, c};
            const Vertex vert2{gx + wx + hx, gy + wy + hy, u1, v1, c};
            const Vertex vert3{gx + hx, gy + hy, u0, v1, c};
            canvas.batcher().draw_quad_corners(tex_id, {vert0, vert1, vert2, vert3});
        }
        cursor_x += g.shift * sx;
    }
}

void draw_sprite(GLModernCanvas &canvas,
                 int id,
                 double frame,
                 double x,
                 double y,
                 double sx,
                 double sy,
                 double angle,
                 unsigned color,
                 double alpha) {
    auto *a = GLModernAssets::active();
    if (a) {
        a->draw_sprite(canvas, id, frame, x, y, sx, sy, angle, color, alpha);
    }
}

void draw_sprite_ext(GLModernCanvas &canvas,
                     int id,
                     double frame,
                     double x,
                     double y,
                     double sx,
                     double sy,
                     double angle,
                     unsigned color,
                     double alpha) {
    auto *a = GLModernAssets::active();
    if (a) {
        a->draw_sprite_ext(canvas, id, frame, x, y, sx, sy, angle, color, alpha);
    }
}

void draw_sprite_part(GLModernCanvas &canvas,
                      int id,
                      double frame,
                      double left,
                      double top,
                      double width,
                      double height,
                      double x,
                      double y,
                      double sx,
                      double sy,
                      unsigned color,
                      double alpha) {
    auto *a = GLModernAssets::active();
    if (a) {
        a->draw_sprite_part(canvas, id, frame, left, top, width, height, x, y, sx, sy, color, alpha);
    }
}

void draw_sprite_pos(GLModernCanvas &canvas,
                     int id,
                     double frame,
                     const std::array<double, 8> &corners,
                     double alpha) {
    auto *a = GLModernAssets::active();
    if (a) {
        a->draw_sprite_pos(canvas, id, frame, corners, alpha);
    }
}

void draw_sprite_general(GLModernCanvas &canvas,
                         int id,
                         double frame,
                         double left,
                         double top,
                         double width,
                         double height,
                         double x,
                         double y,
                         double sx,
                         double sy,
                         double angle,
                         const std::array<unsigned, 4> &colors,
                         double alpha) {
    auto *a = GLModernAssets::active();
    if (a) {
        a->draw_sprite_general(canvas, id, frame, left, top, width, height, x, y, sx, sy, angle, colors, alpha);
    }
}

void draw_sprite_stretched(GLModernCanvas &canvas,
                           int id,
                           double frame,
                           double x,
                           double y,
                           double width,
                           double height,
                           unsigned color,
                           double alpha) {
    auto *a = GLModernAssets::active();
    if (a) {
        a->draw_sprite_stretched(canvas, id, frame, x, y, width, height, color, alpha);
    }
}

void draw_background(GLModernCanvas &canvas, int id, double x, double y) {
    auto *a = GLModernAssets::active();
    if (a) {
        a->draw_background(canvas, id, x, y);
    }
}

void draw_background_stretched(GLModernCanvas &canvas, int id, double x, double y, double width, double height) {
    auto *a = GLModernAssets::active();
    if (a) {
        a->draw_background_stretched(canvas, id, x, y, width, height);
    }
}

void draw_background_part_ext(GLModernCanvas &canvas,
                              int id,
                              double left,
                              double top,
                              double width,
                              double height,
                              double x,
                              double y,
                              double sx,
                              double sy,
                              unsigned color,
                              double alpha) {
    auto *a = GLModernAssets::active();
    if (a) {
        a->draw_background_part_ext(canvas, id, left, top, width, height, x, y, sx, sy, color, alpha);
    }
}

void draw_tile(GLModernCanvas &canvas,
               int id,
               int source_x,
               int source_y,
               int width,
               int height,
               double x,
               double y,
               double sx,
               double sy,
               unsigned color,
               std::uint32_t tile_data) {
    auto *a = GLModernAssets::active();
    if (a) {
        a->draw_tile(canvas, id, source_x, source_y, width, height, x, y, sx, sy, color, tile_data);
    }
}

} // namespace regml::glmodern
