#pragma once
// Procedural RGBA8 images for the minimap (mask shapes, fallback frame,
// fallback arrow / marker dot). Pure C++, no Unity calls. Pixels are packed as
// Unity Color32 (r, g, b, a bytes => little-endian uint32), row 0 = bottom.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace sr2::modules::minimap::procgen {

struct Rgba {
    float r, g, b, a;  // 0..1, straight alpha
};

inline uint32_t Pack(const Rgba& c) {
    auto b = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return b(c.r) | (b(c.g) << 8) | (b(c.b) << 16) | (b(c.a) << 24);
}

// "over" compositing of src (with coverage) onto dst
inline Rgba Over(const Rgba& dst, const Rgba& src, float coverage) {
    float sa = std::clamp(src.a * coverage, 0.0f, 1.0f);
    float oa = sa + dst.a * (1.0f - sa);
    if (oa <= 0.0f) return {0, 0, 0, 0};
    auto ch = [&](float s, float d) { return (s * sa + d * dst.a * (1.0f - sa)) / oa; };
    return {ch(src.r, dst.r), ch(src.g, dst.g), ch(src.b, dst.b), oa};
}

// coverage of a band [inner, outer] of a signed distance field (pixels, <0 inside)
inline float Band(float d, float inner, float outer) {
    float a = std::clamp(outer - d + 0.5f, 0.0f, 1.0f);
    float b = std::clamp(d - inner + 0.5f, 0.0f, 1.0f);
    return std::min(a, b);
}
inline float Inside(float d) { return std::clamp(0.5f - d, 0.0f, 1.0f); }

inline float SdCircle(float x, float y, float cx, float cy, float r) { return std::hypot(x - cx, y - cy) - r; }

inline float SdRoundRect(float x, float y, float cx, float cy, float hw, float hh, float r) {
    float qx = std::fabs(x - cx) - (hw - r);
    float qy = std::fabs(y - cy) - (hh - r);
    float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::hypot(ox, oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

template <class F>
std::vector<uint32_t> Render(int w, int h, F&& shade) {
    std::vector<uint32_t> px(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) px[static_cast<size_t>(y) * w + x] = Pack(shade(x + 0.5f, y + 0.5f));
    return px;
}

// Palette for the fallback frame / arrow (Slime Rancher-ish cream + teal ink).
inline constexpr Rgba kInk{0.07f, 0.16f, 0.20f, 0.95f};
inline constexpr Rgba kCream{0.98f, 0.94f, 0.82f, 1.0f};
inline constexpr Rgba kWhite{1, 1, 1, 1};

// Filled white disc (mask).
inline std::vector<uint32_t> Disc(int size) {
    float c = size * 0.5f, r = c - 1.0f;
    return Render(size, size, [&](float x, float y) { return Rgba{1, 1, 1, Inside(SdCircle(x, y, c, c, r))}; });
}

// Filled white rounded square (mask), meant to be 9-sliced with border = radius + 2.
inline std::vector<uint32_t> RoundSquare(int size, float radius) {
    float c = size * 0.5f;
    return Render(size, size, [&](float x, float y) {
        return Rgba{1, 1, 1, Inside(SdRoundRect(x, y, c, c, c - 1.0f, c - 1.0f, radius))};
    });
}

// Ring frame: ink outline / cream body / thin ink inner line, along the shape edge.
template <class Sd>
std::vector<uint32_t> Frame(int size, float thickness, Sd&& sd) {
    float ink = std::max(1.5f, thickness * 0.22f);
    return Render(size, size, [&](float x, float y) {
        float d = sd(x, y);  // 0 at the outer edge, <0 inside
        Rgba out{0, 0, 0, 0};
        out = Over(out, kInk, Band(d, -thickness, 0.0f));
        out = Over(out, kCream, Band(d, -thickness + ink, -ink));
        // soft inner shadow
        float sh = std::clamp(1.0f - (-(d + thickness)) / (thickness * 0.8f), 0.0f, 1.0f);
        if (d < -thickness) out = Over(out, Rgba{0, 0, 0, 0.35f}, sh);
        return out;
    });
}

inline std::vector<uint32_t> CircleFrame(int size, float thickness) {
    float c = size * 0.5f, r = c - 1.0f;
    return Frame(size, thickness, [&](float x, float y) { return SdCircle(x, y, c, c, r); });
}

inline std::vector<uint32_t> RoundSquareFrame(int size, float radius, float thickness) {
    float c = size * 0.5f;
    return Frame(size, thickness, [&](float x, float y) { return SdRoundRect(x, y, c, c, c - 1.0f, c - 1.0f, radius); });
}

// Player arrow pointing up (+y): cream chevron with ink outline.
inline std::vector<uint32_t> Arrow(int size) {
    float s = static_cast<float>(size);
    // triangle with a notch: tip (0.5, 0.95), left (0.14, 0.08), notch (0.5, 0.30), right (0.86, 0.08)
    struct P { float x, y; };
    const P poly[4] = {{0.5f * s, 0.95f * s}, {0.86f * s, 0.08f * s}, {0.5f * s, 0.30f * s}, {0.14f * s, 0.08f * s}};
    auto sdPoly = [&](float x, float y) {
        float d = 1e9f;
        bool inside = false;
        for (int i = 0, j = 3; i < 4; j = i++) {
            P a = poly[j], b = poly[i];
            float ex = b.x - a.x, ey = b.y - a.y, wx = x - a.x, wy = y - a.y;
            float t = std::clamp((wx * ex + wy * ey) / (ex * ex + ey * ey), 0.0f, 1.0f);
            d = std::min(d, std::hypot(wx - ex * t, wy - ey * t));
            if (((a.y > y) != (b.y > y)) && (x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)) inside = !inside;
        }
        return inside ? -d : d;
    };
    float outline = std::max(1.5f, s * 0.06f);
    return Render(size, size, [&](float x, float y) {
        float d = sdPoly(x, y);
        Rgba out{0, 0, 0, 0};
        out = Over(out, kInk, Inside(d - outline));
        out = Over(out, kCream, Inside(d));
        return out;
    });
}

// Marker dot: white disc with ink outline (tinted by Image.color).
inline std::vector<uint32_t> Dot(int size) {
    float c = size * 0.5f, r = c - 1.0f, outline = std::max(1.5f, size * 0.12f);
    return Render(size, size, [&](float x, float y) {
        float d = SdCircle(x, y, c, c, r);
        Rgba out{0, 0, 0, 0};
        out = Over(out, kInk, Inside(d));
        out = Over(out, kWhite, Inside(d + outline));
        return out;
    });
}

// Nullifier wall icon: a white brick tile (tinted by Image.color) with an ink outline, two mortar
// lines and a jagged ink crack through it.
inline std::vector<uint32_t> CrackedWall(int size) {
    float s = static_cast<float>(size), c = s * 0.5f;
    float hw = s * 0.40f, hh = s * 0.34f, rad = s * 0.08f, outline = std::max(1.5f, s * 0.08f);
    struct P { float x, y; };
    const P crack[5] = {{0.56f * s, 0.86f * s}, {0.44f * s, 0.62f * s}, {0.58f * s, 0.48f * s},
                        {0.42f * s, 0.32f * s}, {0.52f * s, 0.12f * s}};
    auto sdSeg = [](float x, float y, P a, P b) {
        float ex = b.x - a.x, ey = b.y - a.y, wx = x - a.x, wy = y - a.y;
        float t = std::clamp((wx * ex + wy * ey) / (ex * ex + ey * ey), 0.0f, 1.0f);
        return std::hypot(wx - ex * t, wy - ey * t);
    };
    float crackW = std::max(1.0f, s * 0.045f), mortarW = std::max(0.75f, s * 0.025f);
    return Render(size, size, [&](float x, float y) {
        float d = SdRoundRect(x, y, c, c, hw, hh, rad);
        Rgba out{0, 0, 0, 0};
        out = Over(out, kInk, Inside(d - outline));
        out = Over(out, kWhite, Inside(d));
        if (d < 0) {
            // mortar: one horizontal line, offset vertical joints above / below it
            float mort = std::fabs(y - c) - mortarW;
            if (y > c) mort = std::min(mort, std::fabs(x - (c - hw * 0.45f)) - mortarW);
            else mort = std::min(mort, std::fabs(x - (c + hw * 0.45f)) - mortarW);
            out = Over(out, Rgba{kInk.r, kInk.g, kInk.b, 0.45f}, Inside(mort));
            float dc = 1e9f;
            for (int i = 0; i < 4; ++i) dc = std::min(dc, sdSeg(x, y, crack[i], crack[i + 1]));
            out = Over(out, kInk, Inside(dc - crackW));
        }
        return out;
    });
}

// Drone port icon (fallback when the game's drone-type sprite and the gadget's map icon are both
// unavailable): a white hexagonal pad (tinted by Image.color) with an ink outline and an ink ring
// with a centre dot (the docked drone).
inline std::vector<uint32_t> DronePort(int size) {
    float s = static_cast<float>(size), c = s * 0.5f;
    float r = s * 0.42f, outline = std::max(1.5f, s * 0.08f);
    auto sdHex = [&](float x, float y) {  // flat-top regular hexagon, apothem r
        float px = std::fabs(x - c), py = std::fabs(y - c);
        const float k0 = 0.8660254f, k1 = 0.5f;  // cos30, sin30
        return std::max(py, px * k0 + py * k1) - r;
    };
    return Render(size, size, [&](float x, float y) {
        float d = sdHex(x, y);
        Rgba out{0, 0, 0, 0};
        out = Over(out, kInk, Inside(d - outline));
        out = Over(out, kWhite, Inside(d));
        if (d < 0) {
            float ring = std::fabs(SdCircle(x, y, c, c, s * 0.20f)) - std::max(1.0f, s * 0.045f);
            out = Over(out, kInk, Inside(ring));
            out = Over(out, kInk, Inside(SdCircle(x, y, c, c, s * 0.07f)));
        }
        return out;
    });
}

// Generic placed-gadget icon (fallback when the gadget definition has no icon sprite): a white
// eight-tooth gear (tinted by Image.color) with an ink outline and an ink hub.
inline std::vector<uint32_t> Gadget(int size) {
    float s = static_cast<float>(size), c = s * 0.5f;
    float rBody = s * 0.30f, rTooth = s * 0.42f, outline = std::max(1.5f, s * 0.07f);
    return Render(size, size, [&](float x, float y) {
        float dx = x - c, dy = y - c, r = std::hypot(dx, dy);
        float a = std::atan2(dy, dx);
        // teeth: radius steps between body and tooth radius on 8 sectors (soft edge via the cosine)
        float wave = std::cos(a * 8.0f);
        float rad = wave > 0.2f ? rTooth : rBody;
        float d = r - rad;
        Rgba out{0, 0, 0, 0};
        out = Over(out, kInk, Inside(d - outline));
        out = Over(out, kWhite, Inside(d));
        if (d < 0) out = Over(out, kInk, Inside(r - s * 0.10f));
        return out;
    });
}

}  // namespace sr2::modules::minimap::procgen
