#include "client/ui/text_cache.h"

// Native only. The whole file is about caching what the SOFTWARE rasterizer
// produces; the browser build draws text through the page's own text engine,
// which keeps a glyph cache of its own, and none of the device-pixel API this
// leans on exists there.
#ifndef __EMSCRIPTEN__

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "client/ui/text.h"

namespace flix::ui {
namespace {

// Horizontal and vertical subpixel positions kept apart. A run whose pen lands
// at a fractional device pixel is baked at the nearest quarter, which is what
// Chrome's glyph cache quantises to -- the worst case is an eighth of a pixel
// of movement, and the alternative is a cache that misses on every frame a
// label drifts across the screen.
constexpr int kSubpixel = 4;

// Buckets per radian, and rotations are bucketed the way the pen is. Half a
// bucket is under 0.0005 rad, which moves the far end of even a 200-pixel run
// by a tenth of a pixel -- inside the subpixel error the bucketing above
// already accepts. Bucketing at all is what stops a rotated run keying on a
// float that never repeats; and an angle that buckets to zero takes the
// unrotated path unchanged, so every label the client already drew still bakes
// byte for byte as it did before.
constexpr double kAngleBuckets = 1024.0;

// Sizes outside this are not worth a bitmap: below it the direct path is
// already cheap, above it one entry costs more than it saves.
constexpr double kMinDeviceSize = 5.0;
constexpr double kMaxDeviceSize = 220.0;

// A single run wider or taller than this is drawn directly rather than baked.
constexpr int kMaxEntryPixels = 1 << 20;

// Total bitmap budget. Chat, labels, HUD and menu text together sit far under
// this; the cap is what stops a screen full of unique strings growing without
// bound.
constexpr std::size_t kMaxBytes = 24u << 20;

// Frames an entry may go untouched before a sweep may drop it.
constexpr std::uint64_t kStaleFrames = 240;

struct Key {
    std::string text;
    std::int32_t sizeQ = 0;         // device size, sixteenths of a pixel
    std::int32_t strokeQ = 0;       // device stroke width, sixteenths
    std::int32_t angleQ = 0;        // rotation, in kAngleBuckets per radian
    std::uint32_t fill = 0, stroke = 0;
    std::int16_t fillAlphaQ = 0, strokeAlphaQ = 0;
    std::uint8_t bucketX = 0, bucketY = 0;
    bool bold = false, roundJoin = false, fillFirst = false;

    bool operator==(const Key& other) const {
        return text == other.text && sizeQ == other.sizeQ && strokeQ == other.strokeQ &&
               angleQ == other.angleQ && fill == other.fill && stroke == other.stroke &&
               fillAlphaQ == other.fillAlphaQ && strokeAlphaQ == other.strokeAlphaQ &&
               bucketX == other.bucketX && bucketY == other.bucketY && bold == other.bold &&
               roundJoin == other.roundJoin && fillFirst == other.fillFirst;
    }
};

struct KeyHash {
    std::size_t operator()(const Key& k) const {
        std::size_t h = std::hash<std::string>{}(k.text);
        const auto mix = [&h](std::uint64_t v) {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        };
        mix(static_cast<std::uint32_t>(k.sizeQ));
        mix(static_cast<std::uint32_t>(k.strokeQ));
        mix(static_cast<std::uint32_t>(k.angleQ));
        mix(k.fill);
        mix(k.stroke);
        mix(static_cast<std::uint64_t>(static_cast<std::uint16_t>(k.fillAlphaQ)) |
            (static_cast<std::uint64_t>(static_cast<std::uint16_t>(k.strokeAlphaQ)) << 16));
        mix(static_cast<std::uint64_t>(k.bucketX) | (static_cast<std::uint64_t>(k.bucketY) << 8) |
            (static_cast<std::uint64_t>(k.bold) << 16) |
            (static_cast<std::uint64_t>(k.roundJoin) << 17) |
            (static_cast<std::uint64_t>(k.fillFirst) << 18));
        return h;
    }
};

struct Entry {
    std::vector<std::uint8_t> rgba;
    int width = 0, height = 0;
    // Where the run's pen sits inside the bitmap, in whole pixels. The blit
    // subtracts these from the pen's own pixel to place the top-left corner.
    int padLeft = 0, padTop = 0;
    std::uint64_t lastUsed = 0;
};

struct Cache {
    std::unordered_map<Key, Entry, KeyHash> entries;
    std::size_t bytes = 0;
    std::uint64_t clock = 0;
};

Cache& cache() {
    static Cache instance;
    return instance;
}

std::int32_t quantise16(double v) {
    return static_cast<std::int32_t>(std::lround(v * 16.0));
}

std::int16_t quantiseAlpha(double a) {
    return static_cast<std::int16_t>(std::lround(std::clamp(a, 0.0, 1.0) * 255.0));
}

// The tight bounds of a glyph path, in the units it was built in, relative to
// the pen. Curve control points bound their curve, so taking them straight is
// an over-estimate and never a crop. Returns false for any command the glyph
// decoder does not emit, so an unexpected path falls back to direct drawing
// rather than being baked against bounds this does not actually know.
bool glyphBounds(const Path2D& path, double& minX, double& minY, double& maxX, double& maxY) {
    minX = minY = 1e30;
    maxX = maxY = -1e30;
    bool any = false;
    for (const Path2D::Segment& segment : path.segments()) {
        int points = 0;
        switch (segment.command) {
            case Path2D::Command::Move:
            case Path2D::Command::Line: points = 1; break;
            case Path2D::Command::Quadratic: points = 2; break;
            case Path2D::Command::Bezier: points = 3; break;
            case Path2D::Command::Close: points = 0; break;
            default: return false;
        }
        for (int i = 0; i < points; ++i) {
            const double x = segment.v[2 * i], y = segment.v[2 * i + 1];
            if (!std::isfinite(x) || !std::isfinite(y)) return false;
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
            any = true;
        }
    }
    return any;
}

void evictIfNeeded() {
    Cache& c = cache();
    if (c.bytes <= kMaxBytes) return;
    // Stale entries first, which on a normal screen is already enough: the
    // runs actually on it were all touched this frame.
    for (auto it = c.entries.begin(); it != c.entries.end();) {
        if (c.bytes <= kMaxBytes) break;
        if (c.clock - it->second.lastUsed > kStaleFrames) {
            c.bytes -= it->second.rgba.size();
            it = c.entries.erase(it);
        } else {
            ++it;
        }
    }
    // Still over: drop whatever was used longest ago until it fits. Rare, and
    // a full clear here would throw away the runs currently on screen.
    while (c.bytes > kMaxBytes && !c.entries.empty()) {
        auto oldest = c.entries.begin();
        for (auto it = c.entries.begin(); it != c.entries.end(); ++it) {
            if (it->second.lastUsed < oldest->second.lastUsed) oldest = it;
        }
        c.bytes -= oldest->second.rgba.size();
        c.entries.erase(oldest);
    }
}

} // namespace

void paintRunDirect(Canvas& canvas, const std::string& s, double penX, double baseline,
                    const TextStyle& style, double strokeWidth, double strokeAlpha,
                    double fillAlpha, bool fillFirst) {
    Path2D glyphs;
    appendGlyphs(glyphs, s, penX, baseline, style.size, style.bold);
    if (glyphs.empty()) return;

    const auto strokePass = [&] {
        if (strokeWidth <= 0) return;
        canvas.save();
        canvas.setLineJoin(style.roundJoin ? "round" : "miter");
        canvas.setLineCap("butt");
        canvas.setLineWidth(static_cast<float>(strokeWidth));
        setStroke(canvas, style.stroke, strokeAlpha);
        canvas.stroke(glyphs);
        canvas.restore();
    };
    const auto fillPass = [&] {
        setFill(canvas, style.fill, fillAlpha);
        canvas.fill(glyphs, "nonzero");
    };
    // Stroke first, then fill. The other order eats the glyph with its own
    // outline, which is what every hand-rolled attempt at this gets wrong.
    if (fillFirst) { fillPass(); strokePass(); } else { strokePass(); fillPass(); }
}

bool paintRunCached(Canvas& canvas, const std::string& s, double penX, double baseline,
                    const TextStyle& style, double strokeWidth, double strokeAlpha,
                    double fillAlpha, bool fillFirst) {
    // A uniform scale, a rotation and a translation -- a similarity, in other
    // words, which is exactly the family whose ink can be baked once and then
    // copied whole. `[a c; b d]` is one when a == d and c == -b: that leaves
    // no shear to smear the outlines and no reflection to flip them, so the
    // bake can carry the rotation itself and the blit stays a straight copy.
    // Anything else would have to be resampled, which is both slower and
    // softer than rasterising the outlines where they are.
    const std::array<float, 6> m = canvas.currentTransform();
    const double a = m[0], b = m[1], cc = m[2], d = m[3];
    if (std::abs(a - d) > 1e-4 || std::abs(b + cc) > 1e-4) return false;
    const double scale = std::hypot(a, b);
    if (!(scale > 0.0)) return false;
    // Bucketed before anything reads it, so the entry a rotated run lands in
    // is the entry it bakes: two draws a hair apart share one bitmap instead
    // of racing to overwrite each other's.
    const std::int32_t angleQ =
        static_cast<std::int32_t>(std::lround(std::atan2(b, a) * kAngleBuckets));
    const double angle = static_cast<double>(angleQ) / kAngleBuckets;

    const double deviceSize = style.size * scale;
    if (deviceSize < kMinDeviceSize || deviceSize > kMaxDeviceSize) return false;

    // The pen in device pixels, snapped to the subpixel grid the bake uses.
    const double penDeviceX = a * penX + cc * baseline + m[4];
    const double penDeviceY = b * penX + d * baseline + m[5];
    const double snappedX = std::round(penDeviceX * kSubpixel) / kSubpixel;
    const double snappedY = std::round(penDeviceY * kSubpixel) / kSubpixel;
    const double originX = std::floor(snappedX), originY = std::floor(snappedY);
    const int bucketX = static_cast<int>(std::lround((snappedX - originX) * kSubpixel));
    const int bucketY = static_cast<int>(std::lround((snappedY - originY) * kSubpixel));

    Key key;
    key.text = s;
    key.sizeQ = quantise16(deviceSize);
    key.strokeQ = quantise16(std::max(0.0, strokeWidth) * scale);
    key.angleQ = angleQ;
    key.fill = style.fill;
    key.stroke = style.stroke;
    key.fillAlphaQ = quantiseAlpha(fillAlpha);
    key.strokeAlphaQ = quantiseAlpha(strokeAlpha);
    key.bucketX = static_cast<std::uint8_t>(bucketX);
    key.bucketY = static_cast<std::uint8_t>(bucketY);
    key.bold = style.bold;
    key.roundJoin = style.roundJoin;
    key.fillFirst = fillFirst;

    Cache& c = cache();
    auto found = c.entries.find(key);
    if (found == c.entries.end()) {
        // --- bake -----------------------------------------------------------
        // Built at the DEVICE size against an identity transform, so the bitmap
        // is what the rasterizer would have put on the surface.
        const double bakedSize = deviceSize;
        const double bakedStroke = std::max(0.0, strokeWidth) * scale;
        const double subX = static_cast<double>(bucketX) / kSubpixel;
        const double subY = static_cast<double>(bucketY) / kSubpixel;

        Path2D measured;
        appendGlyphs(measured, s, 0.0, 0.0, bakedSize, style.bold);
        if (measured.empty()) return true;   // nothing to draw, and nothing to fall back to
        double minX = 0, minY = 0, maxX = 0, maxY = 0;
        if (!glyphBounds(measured, minX, minY, maxX, maxY)) return false;
        if (angleQ != 0) {
            // The box the ink sits in, turned. Rotating the four corners of an
            // over-estimate is still an over-estimate, so this crops nothing.
            const double cs = std::cos(angle), sn = std::sin(angle);
            const double xs[4] = {minX, maxX, minX, maxX};
            const double ys[4] = {minY, minY, maxY, maxY};
            double rMinX = 1e30, rMinY = 1e30, rMaxX = -1e30, rMaxY = -1e30;
            for (int i = 0; i < 4; ++i) {
                const double rx = xs[i] * cs - ys[i] * sn;
                const double ry = xs[i] * sn + ys[i] * cs;
                rMinX = std::min(rMinX, rx);
                rMaxX = std::max(rMaxX, rx);
                rMinY = std::min(rMinY, ry);
                rMaxY = std::max(rMaxY, ry);
            }
            minX = rMinX; maxX = rMaxX; minY = rMinY; maxY = rMaxY;
        }

        // Half the outline reaches outside the glyph, and a miter join reaches
        // further than half; the join limit is what bounds it, so the margin
        // carries a whole stroke width rather than half of one.
        const double margin = bakedStroke + 2.0;
        const int padLeft = static_cast<int>(std::ceil(margin - std::min(0.0, minX)));
        const int padTop = static_cast<int>(std::ceil(margin - std::min(0.0, minY)));
        const int width =
            padLeft + static_cast<int>(std::ceil(std::max(0.0, maxX) + margin + 1.0));
        const int height =
            padTop + static_cast<int>(std::ceil(std::max(0.0, maxY) + margin + 1.0));
        if (width <= 0 || height <= 0) return true;
        if (static_cast<long long>(width) * height > kMaxEntryPixels) return false;

        Canvas bake = Canvas::createVirtual(width, height);
        TextStyle baked = style;
        baked.size = bakedSize;
        if (angleQ != 0) {
            // Turned about the PEN, which is where the live transform turns it
            // too: translate to the pen first, and the subpixel part of the pen
            // goes into the bitmap ahead of the rotation, exactly as it does on
            // the unrotated path.
            bake.translate(static_cast<float>(padLeft + subX), static_cast<float>(padTop + subY));
            bake.rotate(static_cast<float>(angle));
            paintRunDirect(bake, s, 0.0, 0.0, baked, bakedStroke, strokeAlpha, fillAlpha,
                           fillFirst);
        } else {
            paintRunDirect(bake, s, padLeft + subX, padTop + subY, baked, bakedStroke, strokeAlpha,
                           fillAlpha, fillFirst);
        }

        Entry entry;
        entry.rgba = bake.getImageData(0, 0, width, height);
        entry.width = width;
        entry.height = height;
        entry.padLeft = padLeft;
        entry.padTop = padTop;
        if (entry.rgba.size() != static_cast<std::size_t>(width) * height * 4) return false;

        c.bytes += entry.rgba.size();
        found = c.entries.emplace(std::move(key), std::move(entry)).first;
        evictIfNeeded();
        // evictIfNeeded may have dropped what was just inserted only if it were
        // the least recently used, which it cannot be -- it is stamped below.
    }

    Entry& entry = found->second;
    entry.lastUsed = ++c.clock;

    // Straight onto the device pixels the run was baked for. The subpixel part
    // of the pen is already in the bitmap -- that is what the bucket in the key
    // is -- so what is left is a whole-pixel offset and a one-to-one copy.
    canvas.blitDevice(entry.rgba.data(), entry.width, entry.height,
                      static_cast<int>(originX) - entry.padLeft,
                      static_cast<int>(originY) - entry.padTop);
    return true;
}

void clearTextCache() {
    cache().entries.clear();
    cache().bytes = 0;
}

void textCacheStats(std::size_t& entries, std::size_t& bytes) {
    entries = cache().entries.size();
    bytes = cache().bytes;
}

} // namespace flix::ui

#endif   // __EMSCRIPTEN__
