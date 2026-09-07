#include "client/render/art_cache.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>

namespace flix {

#ifdef __EMSCRIPTEN__

namespace {

/// A tile smaller than this is not worth a texture: the path work it saves is
/// less than the blit costs to set up.
constexpr int kMinSide = 8;
/// One bitmap's limit. The ground tile is 400 units at up to a 2x display
/// scale and a 1.6x zoom, so ~1300 is the real ceiling; this leaves room and
/// refuses anything that would be a surprise allocation.
constexpr int kMaxSide = 4096;

/// Total texture budget. The live set is one bitmap per biome section plus one
/// per textured tile kind at one size each -- a couple of dozen at ~2.5MB
/// apiece in the worst case. The cap is what stops a window being dragged
/// slowly across a display boundary from keeping every intermediate size.
constexpr std::size_t kMaxBytes = 96u << 20;

struct Key {
    const SvgDocument* art = nullptr;
    int width = 0, height = 0;
    bool operator==(const Key& o) const {
        return art == o.art && width == o.width && height == o.height;
    }
};

struct KeyHash {
    std::size_t operator()(const Key& k) const {
        std::size_t h = std::hash<const void*>{}(k.art);
        h ^= static_cast<std::size_t>(k.width) * 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= static_cast<std::size_t>(k.height) * 0x85ebca6bu + (h << 6) + (h >> 2);
        return h;
    }
};

struct Entry {
    Canvas bitmap;
    std::size_t bytes = 0;
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

void evictIfNeeded() {
    Cache& c = cache();
    // Least recently used, one at a time. The live set is tiny and every
    // member of it was touched this frame, so this only ever reaches sizes
    // nothing is drawing at any more.
    while (c.bytes > kMaxBytes && c.entries.size() > 1) {
        auto oldest = c.entries.begin();
        for (auto it = c.entries.begin(); it != c.entries.end(); ++it) {
            if (it->second.lastUsed < oldest->second.lastUsed) oldest = it;
        }
        c.bytes -= oldest->second.bytes;
        c.entries.erase(oldest);
    }
}

} // namespace

bool drawCachedArt(Canvas& canvas, const SvgDocument& art, double x, double y, double w,
                   double h) {
    // Time-dependent ink cannot be baked, and neither can a box with no area.
    if (art.animated() || !(w > 0.0) || !(h > 0.0)) return false;

    // The scale one user unit is drawn at. The browser owns the transform, so
    // this reads the one part of it the client set itself: the backing store
    // against the user-space size the window declared. Callers pass a box
    // already in that space, and none of them is under a zoom of its own --
    // the world's zoom is folded into `w`/`h` before the call.
    const double logical = canvas.width() > 0 ? static_cast<double>(canvas.width()) : 1.0;
    const double scale = static_cast<double>(canvas.pixelWidth()) / logical;
    if (!(scale > 0.0)) return false;

    const int bakeW = static_cast<int>(std::lround(w * scale));
    const int bakeH = static_cast<int>(std::lround(h * scale));
    if (bakeW < kMinSide || bakeH < kMinSide || bakeW > kMaxSide || bakeH > kMaxSide) {
        return false;
    }

    Cache& c = cache();
    const Key key{&art, bakeW, bakeH};
    auto found = c.entries.find(key);
    if (found == c.entries.end()) {
        // Baked at DEVICE resolution and drawn back into a user-space box of
        // exactly w x h, so the browser samples it one texel to one pixel.
        Canvas bitmap = Canvas::createVirtual(bakeW, bakeH);
        if (!art.renderFitted(bitmap, 0.0f, 0.0f, static_cast<float>(bakeW),
                              static_cast<float>(bakeH), 0.0f)) {
            return false;
        }
        Entry entry{std::move(bitmap), static_cast<std::size_t>(bakeW) * bakeH * 4, 0};
        c.bytes += entry.bytes;
        found = c.entries.emplace(key, std::move(entry)).first;
        evictIfNeeded();
        // evictIfNeeded cannot have dropped what was just inserted: it stops at
        // one entry, and this one is stamped as the most recent below.
        found = c.entries.find(key);
        if (found == c.entries.end()) return false;
    }
    found->second.lastUsed = ++c.clock;

    canvas.drawCanvas(found->second.bitmap, static_cast<float>(x), static_cast<float>(y),
                      static_cast<float>(w), static_cast<float>(h));
    return true;
}

void artCacheStats(std::size_t& entries, std::size_t& bytes) {
    entries = cache().entries.size();
    bytes = cache().bytes;
}

void clearArtCache() {
    cache().entries.clear();
    cache().bytes = 0;
}

#else

bool drawCachedArt(Canvas&, const SvgDocument&, double, double, double, double) { return false; }

void artCacheStats(std::size_t& entries, std::size_t& bytes) { entries = 0; bytes = 0; }

void clearArtCache() {}

#endif   // __EMSCRIPTEN__

} // namespace flix
