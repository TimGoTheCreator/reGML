#include "gx2_batcher.hpp"
#include "../glmodern/sprite_shader_gsh.hpp"

#include <coreinit/cache.h>
#include <gx2/draw.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/shaders.h>
#include <gx2/state.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <malloc.h>

namespace regml::gx2 {
namespace {

constexpr float DEG_TO_RAD = 0.017453292519943295769f;

void write_ubo_words(std::uint8_t *dst, std::uint32_t word_offset, const void *src, std::size_t word_count) noexcept {
    auto *out = reinterpret_cast<std::uint32_t *>(dst) + word_offset;
    const auto *in = static_cast<const std::uint32_t *>(src);
    for (std::size_t i = 0; i < word_count; ++i) {
        out[i] = __builtin_bswap32(in[i]);
    }
}

[[nodiscard]] GX2BlendMode map_blend_factor(int gm_factor) noexcept {
    switch (gm_factor) {
    case 1: return GX2_BLEND_MODE_ZERO;
    case 2: return GX2_BLEND_MODE_ONE;
    case 3: return GX2_BLEND_MODE_SRC_COLOR;
    case 4: return GX2_BLEND_MODE_INV_SRC_COLOR;
    case 5: return GX2_BLEND_MODE_SRC_ALPHA;
    case 6: return GX2_BLEND_MODE_INV_SRC_ALPHA;
    case 7: return GX2_BLEND_MODE_DST_ALPHA;
    case 8: return GX2_BLEND_MODE_INV_DST_ALPHA;
    case 9: return GX2_BLEND_MODE_DST_COLOR;
    case 10: return GX2_BLEND_MODE_INV_DST_COLOR;
    case 11: return GX2_BLEND_MODE_SRC_ALPHA_SAT;
    default: return GX2_BLEND_MODE_SRC_ALPHA;
    }
}

[[nodiscard]] GX2BlendCombineMode map_blend_eq(int eq) noexcept {
    switch (eq) {
    case 1: return GX2_BLEND_COMBINE_MODE_SUB;
    case 2: return GX2_BLEND_COMBINE_MODE_REV_SUB;
    case 3: return GX2_BLEND_COMBINE_MODE_MIN;
    case 4: return GX2_BLEND_COMBINE_MODE_MAX;
    case 0:
    default: return GX2_BLEND_COMBINE_MODE_ADD;
    }
}

} // namespace

Gx2Mat4 Gx2Mat4::ortho(float left, float right, float bottom, float top, float near_z, float far_z) noexcept {
    Gx2Mat4 out{};
    const float rl = right - left;
    const float tb = top - bottom;
    const float fn = far_z - near_z;
    out.m[0] = (rl != 0.0f) ? (2.0f / rl) : 0.0f;
    out.m[5] = (tb != 0.0f) ? (2.0f / tb) : 0.0f;
    out.m[10] = (fn != 0.0f) ? (-2.0f / fn) : 0.0f;
    out.m[12] = (rl != 0.0f) ? (-(right + left) / rl) : 0.0f;
    out.m[13] = (tb != 0.0f) ? (-(top + bottom) / tb) : 0.0f;
    out.m[14] = (fn != 0.0f) ? (-(far_z + near_z) / fn) : 0.0f;
    out.m[15] = 1.0f;
    return out;
}

Gx2Batcher::~Gx2Batcher() {
    destroy();
}

bool Gx2Batcher::init() {
    if (shader_loaded_) return true;

    if (!WHBGfxLoadGFDShaderGroup(&shader_group_, 0, SPRITE_SHADER_GSH)) {
        return false;
    }

    WHBGfxInitShaderAttribute(&shader_group_, "in_pos", 0, offsetof(Gx2Vertex, x), GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&shader_group_, "in_uv", 0, offsetof(Gx2Vertex, u), GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&shader_group_, "in_color", 0, offsetof(Gx2Vertex, rgba), GX2_ATTRIB_FORMAT_UNORM_8_8_8_8);
    if (!WHBGfxInitFetchShader(&shader_group_)) {
        WHBGfxFreeShaderGroup(&shader_group_);
        return false;
    }
    shader_loaded_ = true;

    vs_proj_offset_ = 0;
    vs_ubo_block_offset_ = 0;
    vs_ubo_block_size_ = 0;
    vs_has_ubo_ = false;
    vs_proj_found_ = false;
    if (const auto *vs = shader_group_.vertexShader) {
        if (vs->uniformBlocks && vs->uniformBlockCount > 0) {
            vs_has_ubo_ = true;
            vs_ubo_block_offset_ = vs->uniformBlocks[0].offset;
            vs_ubo_block_size_ = vs->uniformBlocks[0].size;
        }
        if (vs->uniformVars && vs->uniformVarCount > 0) {
            for (std::uint32_t i = 0; i < vs->uniformVarCount; ++i) {
                const auto &var = vs->uniformVars[i];
                if (var.name && std::strcmp(var.name, "u_projection") == 0) {
                    vs_proj_offset_ = var.offset;
                    vs_proj_found_ = true;
                    break;
                }
            }
            if (!vs_proj_found_ && !vs_has_ubo_) {
                vs_proj_offset_ = vs->uniformVars[0].offset;
                vs_proj_found_ = true;
            }
        }
    }

    ps_tex_loc_ = 0;
    ps_ubo_block_offset_ = 0;
    ps_ubo_block_size_ = 0;
    ps_use_tex_offset_ = 0;
    ps_alpha_test_offset_ = 0;
    ps_alpha_ref_offset_ = 0;
    ps_has_ubo_ = false;
    ps_use_tex_found_ = false;
    ps_alpha_test_found_ = false;
    ps_alpha_ref_found_ = false;
    if (const auto *ps = shader_group_.pixelShader) {
        if (ps->samplerVars && ps->samplerVarCount > 0) {
            ps_tex_loc_ = ps->samplerVars[0].location;
        }
        if (ps->uniformBlocks && ps->uniformBlockCount > 0) {
            ps_has_ubo_ = true;
            ps_ubo_block_offset_ = ps->uniformBlocks[0].offset;
            ps_ubo_block_size_ = ps->uniformBlocks[0].size;
        }
        if (ps->uniformVars) {
            for (std::uint32_t i = 0; i < ps->uniformVarCount; ++i) {
                const auto &var = ps->uniformVars[i];
                if (!var.name) continue;
                if (ps_has_ubo_ && var.block != static_cast<std::int32_t>(ps_ubo_block_offset_)) continue;
                if (std::strcmp(var.name, "u_use_texture") == 0) {
                    ps_use_tex_offset_ = var.offset;
                    ps_use_tex_found_ = true;
                } else if (std::strcmp(var.name, "u_alpha_test") == 0) {
                    ps_alpha_test_offset_ = var.offset;
                    ps_alpha_test_found_ = true;
                } else if (std::strcmp(var.name, "u_alpha_ref") == 0) {
                    ps_alpha_ref_offset_ = var.offset;
                    ps_alpha_ref_found_ = true;
                }
            }
        }
    }
    use_ubo_ = vs_has_ubo_ || ps_has_ubo_;

    ring_buffer_ = static_cast<Gx2Vertex *>(
        memalign(GX2_VERTEX_BUFFER_ALIGNMENT, RING_CAPACITY_VERTICES * sizeof(Gx2Vertex)));
    if (!ring_buffer_) {
        destroy();
        return false;
    }

    ubo_ring_ = static_cast<std::uint8_t *>(
        memalign(GX2_UNIFORM_BLOCK_ALIGNMENT, UBO_SLOT_SIZE * UBO_SLOT_COUNT));
    if (!ubo_ring_) {
        destroy();
        return false;
    }
    std::memset(ubo_ring_, 0, UBO_SLOT_SIZE * UBO_SLOT_COUNT);

    GX2InitSampler(&sampler_, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);
    GX2InitSampler(&linear_sampler_, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);

    std::memset(&dummy_white_tex_, 0, sizeof(dummy_white_tex_));
    dummy_white_tex_.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    dummy_white_tex_.surface.width = 1;
    dummy_white_tex_.surface.height = 1;
    dummy_white_tex_.surface.depth = 1;
    dummy_white_tex_.surface.mipLevels = 1;
    dummy_white_tex_.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    dummy_white_tex_.surface.use = GX2_SURFACE_USE_TEXTURE;
    dummy_white_tex_.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    dummy_white_tex_.viewNumMips = 1;
    dummy_white_tex_.viewNumSlices = 1;
    dummy_white_tex_.compMap = 0x00010203u;
    GX2CalcSurfaceSizeAndAlignment(&dummy_white_tex_.surface);
    GX2InitTextureRegs(&dummy_white_tex_);

    const std::size_t tex_align = std::max<std::size_t>(dummy_white_tex_.surface.alignment, 64);
    dummy_white_pixels_ = static_cast<std::uint8_t *>(
        memalign(tex_align, dummy_white_tex_.surface.imageSize));
    if (!dummy_white_pixels_) {
        destroy();
        return false;
    }
    std::memset(dummy_white_pixels_, 0xff, dummy_white_tex_.surface.imageSize);
    dummy_white_tex_.surface.image = dummy_white_pixels_;
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, dummy_white_pixels_, dummy_white_tex_.surface.imageSize);

    return true;
}

void Gx2Batcher::destroy() noexcept {
    if (ring_buffer_) {
        free(ring_buffer_);
        ring_buffer_ = nullptr;
    }
    if (ubo_ring_) {
        free(ubo_ring_);
        ubo_ring_ = nullptr;
    }
    if (dummy_white_pixels_) {
        free(dummy_white_pixels_);
        dummy_white_pixels_ = nullptr;
    }
    if (shader_loaded_) {
        WHBGfxFreeShaderGroup(&shader_group_);
        shader_loaded_ = false;
    }
    ring_offset_ = 0;
    ubo_offset_ = 0;
    batch_count_ = 0;
    in_begin_ = false;
}

std::uint8_t *Gx2Batcher::alloc_ubo_slot(std::size_t size) noexcept {
    if (!ubo_ring_ || size > UBO_SLOT_SIZE) return nullptr;
    std::uint8_t *slot = ubo_ring_ + ubo_offset_ * UBO_SLOT_SIZE;
    ubo_offset_ = (ubo_offset_ + 1) % UBO_SLOT_COUNT;
    std::memset(slot, 0, size);
    return slot;
}

void Gx2Batcher::upload_vertex_projection() {
    if (!shader_loaded_) return;

    if (vs_has_ubo_) {
        if (std::uint8_t *slot = alloc_ubo_slot(vs_ubo_block_size_)) {
            if ((vs_proj_offset_ + 16u) * sizeof(std::uint32_t) <= vs_ubo_block_size_) {
                write_ubo_words(slot, vs_proj_offset_, projection_.m.data(), 16);
                DCFlushRange(slot, vs_ubo_block_size_);
                GX2Invalidate(GX2_INVALIDATE_MODE_UNIFORM_BLOCK, slot, vs_ubo_block_size_);
                GX2SetVertexUniformBlock(vs_ubo_block_offset_, vs_ubo_block_size_, slot);
            }
        }
        return;
    }

    if (vs_proj_found_) {
        GX2SetVertexUniformReg(vs_proj_offset_, 16, projection_.m.data());
    }
}

void Gx2Batcher::begin(const Gx2Mat4 &projection) {
    if (!shader_loaded_) return;

    projection_ = projection;
    batch_count_ = 0;
    current_texture_ = nullptr;
    last_bound_texture_ = nullptr;
    last_use_texture_ = -1;
    last_ps_use_tex_ = -1;
    has_applied_blend_ = false;
    blend_dirty_ = true;
    has_applied_color_mask_ = false;
    color_mask_dirty_ = true;
    alpha_test_dirty_ = true;
    in_begin_ = true;

    GX2SetShaderMode(use_ubo_ ? GX2_SHADER_MODE_UNIFORM_BLOCK : GX2_SHADER_MODE_UNIFORM_REGISTER);
    GX2SetFetchShader(&shader_group_.fetchShader);
    GX2SetVertexShader(shader_group_.vertexShader);
    GX2SetPixelShader(shader_group_.pixelShader);
    GX2SetPixelSampler(&sampler_, ps_tex_loc_);
    upload_vertex_projection();

    GX2SetDepthOnlyControl(GX2_DISABLE, GX2_DISABLE, GX2_COMPARE_FUNC_ALWAYS);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, GX2_DISABLE, GX2_DISABLE);

    blend_dirty_ = true;
    color_mask_dirty_ = true;
    alpha_test_dirty_ = true;
}

void Gx2Batcher::set_projection(const Gx2Mat4 &projection) {
    projection_ = projection;
    if (in_begin_ && shader_loaded_) {
        upload_vertex_projection();
    }
}

void Gx2Batcher::ensure_space(std::size_t vertices_needed, const GX2Texture *texture) {
    if (batch_count_ > 0 &&
        (current_texture_ != texture ||
         batch_count_ + vertices_needed > MAX_BATCH_VERTICES ||
         ring_offset_ + batch_count_ + vertices_needed > RING_CAPACITY_VERTICES)) {
        flush();
    }
    if (ring_offset_ + batch_count_ + vertices_needed > RING_CAPACITY_VERTICES) {
        ring_offset_ = 0;
    }
    current_texture_ = texture;
}

void Gx2Batcher::draw_quad(const GX2Texture *texture,
                           float x,
                           float y,
                           float w,
                           float h,
                           float u0,
                           float v0,
                           float u1,
                           float v1,
                           std::uint32_t rgba,
                           float angle_degrees) {
    if (!in_begin_ || !shader_loaded_ || !ring_buffer_) return;
    ensure_space(6, texture);

    float x0 = x, y0 = y;
    float x1 = x + w, y1 = y;
    float x2 = x + w, y2 = y + h;
    float x3 = x, y3 = y + h;

    if (angle_degrees != 0.0f) {
        const float rad = -angle_degrees * DEG_TO_RAD;
        const float c = std::cos(rad);
        const float s = std::sin(rad);
        const float cx = x + w * 0.5f;
        const float cy = y + h * 0.5f;
        auto rot = [&](float &px, float &py) {
            const float dx = px - cx;
            const float dy = py - cy;
            px = cx + dx * c - dy * s;
            py = cy + dx * s + dy * c;
        };
        rot(x0, y0);
        rot(x1, y1);
        rot(x2, y2);
        rot(x3, y3);
    }

    Gx2Vertex *v = ring_buffer_ + ring_offset_ + batch_count_;
    v[0] = {x0, y0, u0, v0, rgba};
    v[1] = {x1, y1, u1, v0, rgba};
    v[2] = {x2, y2, u1, v1, rgba};
    v[3] = {x0, y0, u0, v0, rgba};
    v[4] = {x2, y2, u1, v1, rgba};
    v[5] = {x3, y3, u0, v1, rgba};
    batch_count_ += 6;
}

void Gx2Batcher::draw_triangle(float x1,
                               float y1,
                               float x2,
                               float y2,
                               float x3,
                               float y3,
                               std::uint32_t rgba) {
    if (!in_begin_ || !shader_loaded_ || !ring_buffer_) return;
    ensure_space(3, nullptr);

    Gx2Vertex *v = ring_buffer_ + ring_offset_ + batch_count_;
    v[0] = {x1, y1, 0.0f, 0.0f, rgba};
    v[1] = {x2, y2, 0.0f, 0.0f, rgba};
    v[2] = {x3, y3, 0.0f, 0.0f, rgba};
    batch_count_ += 3;
}

void Gx2Batcher::apply_blend_state() {
    if (!blend_dirty_) return;
    blend_dirty_ = false;
    if (has_applied_blend_ && applied_blend_ == blend_state_) return;
    has_applied_blend_ = true;
    applied_blend_ = blend_state_;

    if (!blend_state_.enabled) {
        GX2SetColorControl(GX2_LOGIC_OP_COPY, 0, GX2_DISABLE, GX2_ENABLE);
        return;
    }

    GX2SetColorControl(GX2_LOGIC_OP_COPY, 1, GX2_DISABLE, GX2_ENABLE);
    GX2SetBlendControl(
        GX2_RENDER_TARGET_0,
        map_blend_factor(blend_state_.src_rgb),
        map_blend_factor(blend_state_.dst_rgb),
        map_blend_eq(blend_state_.eq_rgb),
        GX2_ENABLE,
        map_blend_factor(blend_state_.src_a),
        map_blend_factor(blend_state_.dst_a),
        map_blend_eq(blend_state_.eq_a));
}

void Gx2Batcher::apply_color_mask() {
    if (!color_mask_dirty_) return;
    color_mask_dirty_ = false;
    if (has_applied_color_mask_ && applied_color_mask_ == color_mask_) return;
    has_applied_color_mask_ = true;
    applied_color_mask_ = color_mask_;

    std::uint32_t mask = 0;
    if (color_mask_.r) mask |= GX2_CHANNEL_MASK_R;
    if (color_mask_.g) mask |= GX2_CHANNEL_MASK_G;
    if (color_mask_.b) mask |= GX2_CHANNEL_MASK_B;
    if (color_mask_.a) mask |= GX2_CHANNEL_MASK_A;
    GX2SetTargetChannelMasks(static_cast<GX2ChannelMask>(mask),
                             static_cast<GX2ChannelMask>(0), static_cast<GX2ChannelMask>(0), static_cast<GX2ChannelMask>(0),
                             static_cast<GX2ChannelMask>(0), static_cast<GX2ChannelMask>(0), static_cast<GX2ChannelMask>(0),
                             static_cast<GX2ChannelMask>(0));
}

void Gx2Batcher::apply_alpha_test() {
    if (!alpha_test_dirty_) return;
    alpha_test_dirty_ = false;

    GX2SetAlphaTest(alpha_test_enabled_ ? GX2_ENABLE : GX2_DISABLE,
                    GX2_COMPARE_FUNC_GREATER,
                    alpha_test_ref_);
}

void Gx2Batcher::flush() {
    if (batch_count_ == 0 || !shader_loaded_ || !ring_buffer_) return;

    apply_blend_state();
    apply_color_mask();
    apply_alpha_test();

    const int use_tex = (current_texture_ != nullptr) ? 1 : 0;
    const GX2Texture *tex = current_texture_ ? current_texture_ : &dummy_white_tex_;
    if (last_bound_texture_ != tex) {
        last_bound_texture_ = tex;
        GX2SetPixelTexture(const_cast<GX2Texture *>(tex), ps_tex_loc_);
    }

    if (last_ps_use_tex_ != use_tex || last_ps_alpha_test_ != alpha_test_enabled_ ||
        last_ps_alpha_ref_ != alpha_test_ref_) {
        last_ps_use_tex_ = use_tex;
        last_ps_alpha_test_ = alpha_test_enabled_;
        last_ps_alpha_ref_ = alpha_test_ref_;
        if (ps_has_ubo_) {
            if (std::uint8_t *slot = alloc_ubo_slot(ps_ubo_block_size_)) {
                if (ps_use_tex_found_ && (ps_use_tex_offset_ + 1u) * sizeof(std::uint32_t) <= ps_ubo_block_size_) {
                    const std::int32_t v = use_tex;
                    write_ubo_words(slot, ps_use_tex_offset_, &v, 1);
                }
                if (ps_alpha_test_found_ && (ps_alpha_test_offset_ + 1u) * sizeof(std::uint32_t) <= ps_ubo_block_size_) {
                    const std::int32_t v = alpha_test_enabled_ ? 1 : 0;
                    write_ubo_words(slot, ps_alpha_test_offset_, &v, 1);
                }
                if (ps_alpha_ref_found_ && (ps_alpha_ref_offset_ + 1u) * sizeof(std::uint32_t) <= ps_ubo_block_size_) {
                    const float v = alpha_test_ref_;
                    write_ubo_words(slot, ps_alpha_ref_offset_, &v, 1);
                }
                DCFlushRange(slot, ps_ubo_block_size_);
                GX2Invalidate(GX2_INVALIDATE_MODE_UNIFORM_BLOCK, slot, ps_ubo_block_size_);
                GX2SetPixelUniformBlock(ps_ubo_block_offset_, ps_ubo_block_size_, slot);
            }
        } else {
            if (ps_use_tex_found_) {
                const std::int32_t v[4] = {use_tex, 0, 0, 0};
                GX2SetPixelUniformReg(ps_use_tex_offset_, 4, v);
            }
            if (ps_alpha_test_found_) {
                const std::int32_t v[4] = {alpha_test_enabled_ ? 1 : 0, 0, 0, 0};
                GX2SetPixelUniformReg(ps_alpha_test_offset_, 4, v);
            }
            if (ps_alpha_ref_found_) {
                const float v[4] = {alpha_test_ref_, 0.0f, 0.0f, 0.0f};
                GX2SetPixelUniformReg(ps_alpha_ref_offset_, 4, v);
            }
        }
    }

    Gx2Vertex *dst = ring_buffer_ + ring_offset_;
    const std::size_t byte_size = batch_count_ * sizeof(Gx2Vertex);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, dst, byte_size);

    GX2SetAttribBuffer(0, static_cast<std::uint32_t>(byte_size), sizeof(Gx2Vertex), dst);
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, static_cast<std::uint32_t>(batch_count_), 0, 1);

    // Align ring_offset_ to a multiple of 16 vertices (16 * 20 = 320 bytes,
    // which is a multiple of GX2_VERTEX_BUFFER_ALIGNMENT = 64) so every
    // GX2SetAttribBuffer / GX2Invalidate base pointer stays 64-byte aligned.
    ring_offset_ = (ring_offset_ + batch_count_ + 15u) & ~std::size_t{15u};
    batch_count_ = 0;
    ++draw_call_count_;
}

void Gx2Batcher::end() {
    if (!in_begin_) return;
    flush();
    in_begin_ = false;
}

void Gx2Batcher::set_blend_enable(bool enabled) {
    if (blend_state_.enabled == enabled) return;
    flush();
    blend_state_.enabled = enabled;
    blend_dirty_ = true;
}

void Gx2Batcher::set_blend_mode(int src_rgb, int dst_rgb, int src_a, int dst_a, int eq_rgb, int eq_a) {
    if (blend_state_.src_rgb == src_rgb && blend_state_.dst_rgb == dst_rgb &&
        blend_state_.src_a == src_a && blend_state_.dst_a == dst_a &&
        blend_state_.eq_rgb == eq_rgb && blend_state_.eq_a == eq_a) {
        return;
    }
    flush();
    blend_state_.src_rgb = src_rgb;
    blend_state_.dst_rgb = dst_rgb;
    blend_state_.src_a = src_a;
    blend_state_.dst_a = dst_a;
    blend_state_.eq_rgb = eq_rgb;
    blend_state_.eq_a = eq_a;
    blend_dirty_ = true;
}

void Gx2Batcher::set_alpha_test(bool enabled, float ref) {
    if (alpha_test_enabled_ == enabled && alpha_test_ref_ == ref) return;
    flush();
    alpha_test_enabled_ = enabled;
    alpha_test_ref_ = ref;
    alpha_test_dirty_ = true;
}

void Gx2Batcher::set_color_mask(bool r, bool g, bool b, bool a) {
    if (color_mask_.r == r && color_mask_.g == g && color_mask_.b == b && color_mask_.a == a) return;
    flush();
    color_mask_ = {r, g, b, a};
    color_mask_dirty_ = true;
}

} // namespace regml::gx2
