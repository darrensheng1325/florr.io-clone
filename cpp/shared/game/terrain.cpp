#include "shared/game/terrain.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>

namespace flix {
namespace {

constexpr int kAxis = kTilesPerAxis;
constexpr int kTotalTiles = kAxis * kAxis;

constexpr int kNeighborDx[4] = {1, -1, 0, 0};
constexpr int kNeighborDy[4] = {0, 0, 1, -1};

/// Value noise on a coarse lattice, sampled in TILE units.
///
/// Lattice noise rather than a per-tile hash because the map needs blobs, not
/// static: a hash gives every tile an independent roll, which reads as gravel
/// and walls nothing off in an interesting shape.
class ValueNoise {
public:
    ValueNoise(Rng& rng, int cellTiles)
        : cell_(std::max(1, cellTiles)), dim_(kAxis / std::max(1, cellTiles) + 3) {
        values_.resize(static_cast<std::size_t>(dim_) * static_cast<std::size_t>(dim_));
        for (double& v : values_) v = rng.unit();
    }

    double at(double tx, double ty) const {
        const double fx = tx / cell_;
        const double fy = ty / cell_;
        const double bx = std::floor(fx);
        const double by = std::floor(fy);
        // Weights come from the unclamped fraction, indices from the clamped
        // lattice cell, so a sample past the edge stays continuous.
        const double sx = smoothstep(fx - bx);
        const double sy = smoothstep(fy - by);
        const int x0 = clamp(static_cast<int>(bx), 0, dim_ - 2);
        const int y0 = clamp(static_cast<int>(by), 0, dim_ - 2);
        const double a = value(x0, y0);
        const double b = value(x0 + 1, y0);
        const double c = value(x0, y0 + 1);
        const double d = value(x0 + 1, y0 + 1);
        return lerp(lerp(a, b, sx), lerp(c, d, sx), sy);
    }

private:
    static double smoothstep(double t) { return t * t * (3.0 - 2.0 * t); }
    double value(int x, int y) const {
        return values_[static_cast<std::size_t>(y) * static_cast<std::size_t>(dim_) + static_cast<std::size_t>(x)];
    }

    int cell_;
    int dim_;
    std::vector<double> values_;
};

/// The noise the whole map is cut from. One shared set rather than one per
/// biome, so features line up across a section boundary instead of stopping
/// dead on it -- a river runs out of the Garden and into the Ocean.
struct NoiseSet {
    explicit NoiseSet(Rng& rng)
        : coarse(rng, 24), medium(rng, 10), fine(rng, 4), altCoarse(rng, 14), altMedium(rng, 6) {}

    double fbm(double tx, double ty) const {
        return 0.55 * medium.at(tx, ty) + 0.30 * fine.at(tx, ty) + 0.15 * coarse.at(tx, ty);
    }

    ValueNoise coarse, medium, fine, altCoarse, altMedium;
};

/// Distance from a noise field's 0.5 level set, which draws winding lines
/// (rivers, ridges, streams) instead of blobs.
inline double ridge(double n) { return std::fabs(n - 0.5); }

inline int wrapMod(int v, int m) { return ((v % m) + m) % m; }

// ---------------------------------------------------------------------------
// TypeScript wall-edge geometry
// ---------------------------------------------------------------------------

constexpr double kJaggedMaxOffset = kJaggedMaxProtrusion;
constexpr double kWallResolveEpsilon = 0.01;

double maxJaggedOffset(const JaggedEdge& points, double minT, double maxT) {
    double result = 0.0;
    for (const JaggedEdgePoint& point : points) {
        if (point.t >= minT && point.t <= maxT) result = std::max(result, point.offset);
    }
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const JaggedEdgePoint& a = points[i];
        const JaggedEdgePoint& b = points[i + 1];
        if (b.t < minT || a.t > maxT) continue;
        if (a.t < minT && b.t > minT) {
            const double f = (minT - a.t) / (b.t - a.t);
            result = std::max(result, a.offset + f * (b.offset - a.offset));
        }
        if (a.t < maxT && b.t > maxT) {
            const double f = (maxT - a.t) / (b.t - a.t);
            result = std::max(result, a.offset + f * (b.offset - a.offset));
        }
    }
    return result;
}

struct JaggedCollision {
    double left = 0.0;
    double right = 0.0;
    double top = 0.0;
    double bottom = 0.0;
    double nearDx = 0.0;
    double nearDy = 0.0;
};

std::optional<JaggedCollision> findJaggedCollision(const Terrain& terrain,
                                                   Vec2 position, double radius) {
    const double reach = radius + kJaggedMaxOffset + kCollisionScanBuffer;
    const int minX = std::max(0, Terrain::toTileCoord(position.x - reach));
    const int maxX = std::min(kTilesPerAxis - 1, Terrain::toTileCoord(position.x + reach));
    const int minY = std::max(0, Terrain::toTileCoord(position.y - reach));
    const int maxY = std::min(kTilesPerAxis - 1, Terrain::toTileCoord(position.y + reach));
    const double entityLeft = position.x - radius;
    const double entityRight = position.x + radius;
    const double entityTop = position.y - radius;
    const double entityBottom = position.y + radius;
    std::optional<JaggedCollision> corner;

    for (int tileY = minY; tileY <= maxY; ++tileY) {
        for (int tileX = minX; tileX <= maxX; ++tileX) {
            const Tile tile = terrain.atTile(tileX, tileY);
            if (!tileBlocks(tile)) continue;

            JaggedCollision hit;
            hit.left = tileX * kTileSize;
            hit.right = hit.left + kTileSize;
            hit.top = tileY * kTileSize;
            hit.bottom = hit.top + kTileSize;

            if (tile == Tile::Wall || tile == Tile::Water) {
                if (jaggedEdgeExposed(terrain, tileX, tileY, 0)) {
                    const double lo = std::max(0.0, entityLeft - tileX * kTileSize);
                    const double hi = std::min(kTileSize, entityRight - tileX * kTileSize);
                    if (hi > lo) hit.top -= maxJaggedOffset(jaggedEdge(tileX, tileY, 0), lo, hi);
                }
                if (jaggedEdgeExposed(terrain, tileX, tileY, 1)) {
                    const double lo = std::max(0.0, entityLeft - tileX * kTileSize);
                    const double hi = std::min(kTileSize, entityRight - tileX * kTileSize);
                    if (hi > lo) hit.bottom += maxJaggedOffset(jaggedEdge(tileX, tileY, 1), lo, hi);
                }
                if (jaggedEdgeExposed(terrain, tileX, tileY, 2)) {
                    const double lo = std::max(0.0, entityTop - tileY * kTileSize);
                    const double hi = std::min(kTileSize, entityBottom - tileY * kTileSize);
                    if (hi > lo) hit.left -= maxJaggedOffset(jaggedEdge(tileX, tileY, 2), lo, hi);
                }
                if (jaggedEdgeExposed(terrain, tileX, tileY, 3)) {
                    const double lo = std::max(0.0, entityTop - tileY * kTileSize);
                    const double hi = std::min(kTileSize, entityBottom - tileY * kTileSize);
                    if (hi > lo) hit.right += maxJaggedOffset(jaggedEdge(tileX, tileY, 3), lo, hi);
                }
            }

            const double nearX = clamp(position.x, hit.left, hit.right);
            const double nearY = clamp(position.y, hit.top, hit.bottom);
            hit.nearDx = position.x - nearX;
            hit.nearDy = position.y - nearY;
            const bool inside = hit.nearDx == 0.0 && hit.nearDy == 0.0;
            if (!inside && hit.nearDx * hit.nearDx + hit.nearDy * hit.nearDy >= radius * radius) {
                continue;
            }
            // Prefer a flat-face hit over an adjacent tile's interior seam.
            if (hit.nearDx == 0.0 || hit.nearDy == 0.0) return hit;
            if (!corner) corner = hit;
        }
    }
    return corner;
}

Vec2 resolveJaggedCollision(Vec2 position, double radius, const JaggedCollision& hit) {
    const double r = radius + kWallResolveEpsilon;
    const bool insideX = position.x > hit.left && position.x < hit.right;
    const bool insideY = position.y > hit.top && position.y < hit.bottom;
    if (insideX && insideY) {
        const double left = position.x - hit.left;
        const double right = hit.right - position.x;
        const double top = position.y - hit.top;
        const double bottom = hit.bottom - position.y;
        const double least = std::min(std::min(left, right), std::min(top, bottom));
        if (least == left) return {hit.left - r, position.y};
        if (least == right) return {hit.right + r, position.y};
        if (least == top) return {position.x, hit.top - r};
        return {position.x, hit.bottom + r};
    }
    if (insideY) return {position.x < hit.left ? hit.left - r : hit.right + r, position.y};
    if (insideX) return {position.x, position.y < hit.top ? hit.top - r : hit.bottom + r};

    const double cornerX = position.x < hit.left ? hit.left : hit.right;
    const double cornerY = position.y < hit.top ? hit.top : hit.bottom;
    Vec2 away = position - Vec2{cornerX, cornerY};
    double distance = away.length();
    if (!(distance > 0.0)) { away = {1.0, 0.0}; distance = 1.0; }
    return {cornerX + away.x * r / distance, cornerY + away.y * r / distance};
}

/// Liang-Barsky: does the segment touch the axis-aligned rect at all?
///
/// Parametric clipping rather than four edge intersections, because a segment
/// that lies wholly inside the rect crosses no edge and still touches it.
bool segmentTouchesRect(Vec2 a, Vec2 b, double left, double top, double right, double bottom) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    double t0 = 0.0;
    double t1 = 1.0;
    // Narrows [t0, t1] to the part of the segment on the inside of one edge.
    // A segment parallel to the edge cannot be clipped by it, so it is inside
    // that edge exactly when it starts inside.
    const auto clip = [&](double p, double q) -> bool {
        if (p == 0.0) return q >= 0.0;
        const double r = q / p;
        if (p < 0.0) {
            if (r > t1) return false;
            if (r > t0) t0 = r;
        } else {
            if (r < t0) return false;
            if (r < t1) t1 = r;
        }
        return true;
    };
    return clip(-dx, a.x - left) && clip(dx, right - a.x) && clip(-dy, a.y - top) &&
           clip(dy, bottom - a.y) && t0 <= t1;
}

int base64Value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool decodeBase64(const std::string& encoded, std::vector<std::uint8_t>& out) {
    out.clear();
    out.reserve(encoded.size() * 3 / 4);
    std::uint32_t bits = 0;
    int bitCount = 0;
    for (const unsigned char c : encoded) {
        if (c == '=') break;
        const int value = base64Value(c);
        if (value < 0) return false;
        bits = (bits << 6) | static_cast<std::uint32_t>(value);
        bitCount += 6;
        if (bitCount >= 8) {
            bitCount -= 8;
            out.push_back(static_cast<std::uint8_t>((bits >> bitCount) & 0xFFu));
        }
    }
    return true;
}

Tile classifyGarden(int tx, int ty, const NoiseSet& n) {
    if (n.altMedium.at(tx, ty) > 0.84) return Tile::Wall;      // boulders
    if (n.medium.at(tx, ty) > 0.80) return Tile::Water;        // ponds
    if (n.fine.at(tx, ty) > 0.74) return Tile::Sand;           // worn paths
    return Tile::Ground;
}

Tile classifyDesert(int tx, int ty, const NoiseSet& n) {
    if (n.medium.at(tx, ty) > 0.80) return Tile::Wall;         // mesas
    if (n.altCoarse.at(tx, ty) < 0.07) return Tile::Water;     // oases
    if (n.fine.at(tx, ty) > 0.80) return Tile::Stone;
    return Tile::Sand;
}

Tile classifyHel(int tx, int ty, const NoiseSet& n) {
    if (ridge(n.medium.at(tx, ty)) < 0.035) return Tile::Wall; // basalt ridges
    if (n.altCoarse.at(tx, ty) > 0.78) return Tile::Water;     // impassable lava
    return Tile::Ground;
}

Tile classifyOcean(int tx, int ty, const NoiseSet& n) {
    const double height = 0.6 * n.coarse.at(tx, ty) + 0.4 * n.medium.at(tx, ty);
    if (height > 0.70) return Tile::Ground;                    // island interior
    if (height > 0.60) return Tile::Sand;                      // beach
    if (height < 0.20 && n.altMedium.at(tx, ty) > 0.86) return Tile::Wall;  // spires
    return Tile::Water;
}

Tile classifyJungle(int tx, int ty, const NoiseSet& n) {
    const double canopy = 0.55 * n.medium.at(tx, ty) + 0.45 * n.fine.at(tx, ty);
    if (canopy > 0.66) return Tile::Wall;                      // dense trees
    if (ridge(n.altMedium.at(tx, ty)) < 0.025) return Tile::Water;  // streams
    return Tile::Ground;
}

Tile classifySewers(int tx, int ty, const NoiseSet& n) {
    // A rectilinear lane grid, deliberately unlike everything around it. The
    // lanes are laid out by construction rather than by noise, so the section
    // is connected before the repair pass ever looks at it.
    const int mx = wrapMod(tx, 7);
    const int my = wrapMod(ty, 7);
    const bool lane = mx < 3 || my < 3;
    if (!lane) return Tile::Wall;
    if (n.fine.at(tx, ty) > 0.90) return Tile::Wall;           // collapsed rubble
    if (mx == 1 || my == 1) return Tile::Water;                // the channel itself
    return Tile::Ground;                                       // the ledges beside it
}

Tile classifyComputer(int tx, int ty, const NoiseSet&) {
    // Circuit board: a lattice of trace lanes with board substrate between.
    // Chips (walls) are stamped later, inside the cells, never on a lane.
    if (wrapMod(tx, 9) == 0 || wrapMod(ty, 9) == 0) return Tile::Ground;
    return Tile::Stone;
}

Tile classifyUnknown(int tx, int ty, const NoiseSet& n) {
    const double v = n.fbm(tx, ty);
    if (v > 0.70) return Tile::Wall;
    if (v < 0.24) return Tile::Water;                          // voids
    if (n.altCoarse.at(tx, ty) > 0.62) return Tile::Stone;
    return Tile::Ground;
}

Tile classifyTile(int section, int tx, int ty, const NoiseSet& n) {
    switch (section) {
        case 0: return classifyGarden(tx, ty, n);
        case 1: return classifyDesert(tx, ty, n);
        case 2: return classifyHel(tx, ty, n);
        case 3: return classifyOcean(tx, ty, n);
        // Ant Hell starts solid; carveAntHell() digs the chambers out of it.
        case 4: return Tile::Wall;
        case 5: return classifyJungle(tx, ty, n);
        case 6: return classifySewers(tx, ty, n);
        case 7: return classifyComputer(tx, ty, n);
        case 8: return classifyUnknown(tx, ty, n);
        default: return Tile::Wall;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// The shared jagged outline
// ---------------------------------------------------------------------------

bool jaggedEdgeExposed(const Terrain& terrain, int tileX, int tileY, int edge) {
    int adjacentX = tileX;
    int adjacentY = tileY;
    if (edge == 0) --adjacentY;
    else if (edge == 1) ++adjacentY;
    else if (edge == 2) --adjacentX;
    else ++adjacentX;

    if (adjacentX < 0 || adjacentY < 0 ||
        adjacentX >= kTilesPerAxis || adjacentY >= kTilesPerAxis) return true;
    const Tile adjacent = terrain.atTile(adjacentX, adjacentY);
    if (adjacent == Tile::Ground) return true;
    const Tile current = terrain.atTile(tileX, tileY);
    return current == Tile::Wall && adjacent == Tile::Water;
}

const JaggedEdge& jaggedEdge(int tileX, int tileY, int edge) {
    // Depends on the coordinates and the edge alone -- never on which
    // neighbours happen to be exposed -- so the cache stays valid while a map
    // editor or a test changes tiles around it.
    static std::unordered_map<std::uint64_t, JaggedEdge> cache;
    const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(tileY)) << 34) |
                              (static_cast<std::uint64_t>(static_cast<std::uint32_t>(tileX)) << 2) |
                              static_cast<std::uint64_t>(edge & 3);
    const auto found = cache.find(key);
    if (found != cache.end()) return found->second;

    // TypeScript's `seededRandom`, over an int64 seed. The JavaScript this
    // mirrors does the additions below in doubles, where `baseSeed + i` cannot
    // wrap; doing them in int32 would be signed overflow AND would hand back a
    // different sequence for the handful of tiles whose mixed hash lands
    // within a few thousand of INT_MAX.
    const auto seededRandom = [](std::int64_t seed) {
        const double x = std::sin(static_cast<double>(seed)) * 10000.0;
        return x - std::floor(x);
    };

    JaggedEdge points{};
    points.front() = {0.0, 0.0};
    points.back() = {kTileSize, 0.0};
    const double segmentLength = kTileSize / (kJaggedSegmentCount + 1.0);
    // JavaScript's `^` converts both products to SIGNED 32-bit first. Multiply
    // unsigned (which wraps exactly as ToInt32 does), xor, then widen with the
    // sign it would have had over there.
    const std::uint32_t mixed = static_cast<std::uint32_t>(tileX) * 73856093u ^
                                static_cast<std::uint32_t>(tileY) * 19349669u;
    const std::int64_t signedMixed = mixed <= 0x7fffffffu
        ? static_cast<std::int64_t>(mixed)
        : static_cast<std::int64_t>(mixed) - 4294967296ll;
    const std::int64_t baseSeed = signedMixed + edge * 1000;
    for (int i = 1; i <= kJaggedSegmentCount; ++i) {
        const std::int64_t seed = baseSeed + i;
        const double jitter = (seededRandom(seed) - 0.5) * segmentLength * 0.4;
        points[static_cast<std::size_t>(i)] = {
            clamp(i * segmentLength + jitter, 1.0, kTileSize - 1.0),
            seededRandom(seed + 100) * kJaggedMaxProtrusion,
        };
    }
    std::sort(points.begin(), points.end(),
              [](const JaggedEdgePoint& a, const JaggedEdgePoint& b) { return a.t < b.t; });
    return cache.emplace(key, points).first->second;
}

// ---------------------------------------------------------------------------
// Construction and generation
// ---------------------------------------------------------------------------

Terrain::Terrain()
    : tiles_(static_cast<std::size_t>(kTotalTiles), static_cast<std::uint8_t>(Tile::Ground)),
      spawnTile_(index(kAxis / 2, kAxis / 2)) {}

Vec2 Terrain::spawnPoint() const {
    return tileCenter(spawnTile_ % kAxis, spawnTile_ / kAxis);
}

void Terrain::setTile(int tx, int ty, Tile t) {
    if (tx < 0 || ty < 0 || tx >= kAxis || ty >= kAxis) return;
    tiles_[static_cast<std::size_t>(index(tx, ty))] = static_cast<std::uint8_t>(t);
}

void Terrain::fill(Tile t) {
    std::fill(tiles_.begin(), tiles_.end(), static_cast<std::uint8_t>(t));
}

void Terrain::generate(std::uint64_t seed) {
    seed_ = seed;
    Rng rng(seed);

    // Draw order is the reproducibility contract: every rng consumer below
    // runs exactly once, in this order, for any seed.
    generateSections(rng);
    carveAntHell(rng);
    placeCircuitChips(rng);

    // Players start in the Garden, not at the world centre.
    //
    // The centre section is the Ant Hell, which generates solid and is dug out
    // into a tunnel network -- an interesting place to raid and a hostile place
    // to be dropped into with a single Basic petal. The Garden is where the
    // starter mobs live (bee and ladybug both list section 0 in mobs.json), so
    // that is where a new flower belongs.
    spawnTile_ = chooseGardenSpawn();
    connectAll();
    assert(isConnected());
}

bool Terrain::loadMapBundle(const std::string& path, std::string& errorOut) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        errorOut = "could not open TypeScript map bundle: " + path;
        return false;
    }
    const std::string source((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
    constexpr const char* kMarker = "export const MAP_TILE_RLE = \"";
    const std::size_t beginMarker = source.find(kMarker);
    if (beginMarker == std::string::npos) {
        errorOut = "MAP_TILE_RLE is missing from " + path;
        return false;
    }
    const std::size_t begin = beginMarker + std::char_traits<char>::length(kMarker);
    const std::size_t end = source.find('"', begin);
    if (end == std::string::npos) {
        errorOut = "MAP_TILE_RLE is unterminated in " + path;
        return false;
    }

    std::vector<std::uint8_t> compressed;
    if (!decodeBase64(source.substr(begin, end - begin), compressed)) {
        errorOut = "MAP_TILE_RLE is not valid base64 in " + path;
        return false;
    }

    std::vector<std::uint8_t> decoded;
    decoded.reserve(kTotalTiles);
    std::size_t at = 0;
    while (at < compressed.size()) {
        const std::uint8_t header = compressed[at++];
        std::size_t count = header >> 1;
        if (header & 1u) {
            if (at + 2 > compressed.size()) {
                errorOut = "MAP_TILE_RLE has a truncated extended run";
                return false;
            }
            count += (static_cast<std::size_t>(compressed[at]) << 8) |
                     static_cast<std::size_t>(compressed[at + 1]);
            at += 2;
        }
        if (at >= compressed.size() || count == 0 ||
            decoded.size() + count > static_cast<std::size_t>(kTotalTiles)) {
            errorOut = "MAP_TILE_RLE contains an invalid run";
            return false;
        }
        const std::uint8_t tile = compressed[at++];
        if (tile > static_cast<std::uint8_t>(Tile::Block)) {
            errorOut = "MAP_TILE_RLE contains an unsupported tile id";
            return false;
        }
        decoded.insert(decoded.end(), count, tile);
    }
    if (decoded.size() != static_cast<std::size_t>(kTotalTiles)) {
        errorOut = "MAP_TILE_RLE decoded to " + std::to_string(decoded.size()) +
                   " tiles; expected " + std::to_string(kTotalTiles);
        return false;
    }
    if (!setTiles(decoded)) {
        errorOut = "could not install decoded TypeScript wall grid";
        return false;
    }
    seed_ = 0;
    return true;
}

bool Terrain::setTiles(const std::vector<std::uint8_t>& tiles) {
    if (tiles.size() != static_cast<std::size_t>(kTotalTiles)) return false;
    for (const std::uint8_t tile : tiles) {
        if (tile > static_cast<std::uint8_t>(Tile::Block)) return false;
    }
    tiles_ = tiles;
    spawnTile_ = chooseGardenSpawn();
    return !tileBlocks(atTile(spawnTile_ % kAxis, spawnTile_ / kAxis));
}

void Terrain::generateSections(Rng& rng) {
    const NoiseSet noise(rng);
    for (int ty = 0; ty < kAxis; ++ty) {
        for (int tx = 0; tx < kAxis; ++tx) {
            const int section = flix::sectionAt(tileCenter(tx, ty));
            tiles_[static_cast<std::size_t>(index(tx, ty))] =
                static_cast<std::uint8_t>(classifyTile(section, tx, ty, noise));
        }
    }
}

/// An open tile near the middle of the Garden section, searched outward so the
/// result is the closest walkable spot to the section's centre rather than the
/// first one in scan order.
int Terrain::chooseGardenSpawn() const {
    const int perSection = kAxis / kSectionsPerAxis;
    const int centreTx = perSection / 2;
    const int centreTy = perSection / 2;

    for (int radius = 0; radius < perSection; ++radius) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                // Only the ring at this radius; the interior was covered already.
                if (std::max(std::abs(dx), std::abs(dy)) != radius) continue;
                const int tx = centreTx + dx;
                const int ty = centreTy + dy;
                if (tx < 0 || ty < 0 || tx >= perSection || ty >= perSection) continue;
                if (atTile(tx, ty) == Tile::Ground) return index(tx, ty);
            }
        }
    }
    // The Garden is noise-generated and always has ground, but if it somehow
    // did not, the centre is still a defined tile and connectAll() will open it.
    return index(centreTx, centreTy);
}

void Terrain::carveDisc(Vec2 center, double radius, Tile t) {
    const int x0 = clamp(toTileCoord(center.x - radius), 0, kAxis - 1);
    const int x1 = clamp(toTileCoord(center.x + radius), 0, kAxis - 1);
    const int y0 = clamp(toTileCoord(center.y - radius), 0, kAxis - 1);
    const int y1 = clamp(toTileCoord(center.y + radius), 0, kAxis - 1);
    const double r2 = radius * radius;
    for (int ty = y0; ty <= y1; ++ty) {
        for (int tx = x0; tx <= x1; ++tx) {
            if (distanceSq(tileCenter(tx, ty), center) <= r2) setTile(tx, ty, t);
        }
    }
}

void Terrain::carveCorridor(int fromTx, int fromTy, int toTx, int toTy, int halfWidth, Tile t) {
    const int stepX = toTx >= fromTx ? 1 : -1;
    const int stepY = toTy >= fromTy ? 1 : -1;
    auto brush = [&](int cx, int cy) {
        for (int dy = -halfWidth; dy <= halfWidth; ++dy) {
            for (int dx = -halfWidth; dx <= halfWidth; ++dx) setTile(cx + dx, cy + dy, t);
        }
    };
    // An L, not a diagonal: a diagonal staircase leaves single-tile pinch
    // points that a body wider than a tile cannot squeeze through.
    for (int x = fromTx; x != toTx + stepX; x += stepX) brush(x, fromTy);
    for (int y = fromTy; y != toTy + stepY; y += stepY) brush(toTx, y);
}

void Terrain::carveAntHell(Rng& rng) {
    const Vec2 center{kWorldHalf, kWorldHalf};
    const int centerTx = toTileCoord(center.x);
    const int centerTy = toTileCoord(center.y);

    // The spawn plaza. Everything in the map hangs off this being open.
    carveDisc(center, kTileSize * 5.0, Tile::Ground);

    // Four tunnels out of the section, ending a couple of tiles beyond its
    // edge so they meet whatever the neighbouring biome generated. Without
    // these the repair pass would still connect the hill, but by one ragged
    // corridor instead of four deliberate gates.
    const int reach = static_cast<int>(kSectionSize / kTileSize) / 2 + 3;
    carveCorridor(centerTx, centerTy, centerTx - reach, centerTy, 1, Tile::Ground);
    carveCorridor(centerTx, centerTy, centerTx + reach, centerTy, 1, Tile::Ground);
    carveCorridor(centerTx, centerTy, centerTx, centerTy - reach, 1, Tile::Ground);
    carveCorridor(centerTx, centerTy, centerTx, centerTy + reach, 1, Tile::Ground);

    const int inset = 6;
    const int lo = centerTx - reach + inset;
    const int hi = centerTx + reach - inset;

    int prevTx = centerTx;
    int prevTy = centerTy;
    for (int i = 0; i < 16; ++i) {
        const int cx = rng.rangeInt(lo, hi);
        const int cy = rng.rangeInt(lo, hi);
        const double radius = kTileSize * rng.range(2.0, 5.0);
        carveDisc(tileCenter(cx, cy), radius, Tile::Ground);
        carveCorridor(prevTx, prevTy, cx, cy, 1, Tile::Ground);
        prevTx = cx;
        prevTy = cy;
    }
}

void Terrain::placeCircuitChips(Rng& rng) {
    // Chips sit strictly inside a lattice cell, so the trace lanes stay clear
    // and the section is connected without any repair.
    for (int cellY = 0; cellY + 9 <= kAxis; cellY += 9) {
        for (int cellX = 0; cellX + 9 <= kAxis; cellX += 9) {
            if (flix::sectionAt(tileCenter(cellX + 4, cellY + 4)) != 7) continue;
            if (!rng.chance(0.55)) continue;
            const int size = rng.rangeInt(3, 5);
            const int ox = cellX + 2;
            const int oy = cellY + 2;
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) setTile(ox + x, oy + y, Tile::Wall);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Connectivity
// ---------------------------------------------------------------------------

int Terrain::openTileCount() const {
    int n = 0;
    for (int i = 0; i < kTotalTiles; ++i) {
        if (passableIndex(i)) ++n;
    }
    return n;
}

bool Terrain::isConnected() const {
    const int open = openTileCount();
    if (!passableIndex(spawnTile_)) return open == 0;

    std::vector<std::uint8_t> seen(static_cast<std::size_t>(kTotalTiles), 0);
    std::vector<int> stack;
    stack.reserve(256);
    stack.push_back(spawnTile_);
    seen[static_cast<std::size_t>(spawnTile_)] = 1;
    int reached = 0;
    while (!stack.empty()) {
        const int cur = stack.back();
        stack.pop_back();
        ++reached;
        const int cx = cur % kAxis;
        const int cy = cur / kAxis;
        for (int d = 0; d < 4; ++d) {
            const int nx = cx + kNeighborDx[d];
            const int ny = cy + kNeighborDy[d];
            if (nx < 0 || ny < 0 || nx >= kAxis || ny >= kAxis) continue;
            const int ni = index(nx, ny);
            if (seen[static_cast<std::size_t>(ni)] || !passableIndex(ni)) continue;
            seen[static_cast<std::size_t>(ni)] = 1;
            stack.push_back(ni);
        }
    }
    return reached == open;
}

void Terrain::connectAll() {
    // 0-1 BFS from the spawn over the WHOLE grid: stepping onto an open tile
    // costs nothing, stepping into a wall costs one. dist == 0 therefore means
    // "reachable without digging", and the parent chain of any other tile is
    // the cheapest route to dig for it.
    constexpr std::int32_t kUnreached = std::numeric_limits<std::int32_t>::max();
    std::vector<std::int32_t> dist(static_cast<std::size_t>(kTotalTiles), kUnreached);
    std::vector<std::int32_t> parent(static_cast<std::size_t>(kTotalTiles), -1);
    std::vector<std::uint8_t> settled(static_cast<std::size_t>(kTotalTiles), 0);

    std::deque<std::int32_t> queue;
    dist[static_cast<std::size_t>(spawnTile_)] = 0;
    queue.push_back(spawnTile_);
    while (!queue.empty()) {
        const std::int32_t cur = queue.front();
        queue.pop_front();
        if (settled[static_cast<std::size_t>(cur)]) continue;
        settled[static_cast<std::size_t>(cur)] = 1;
        const int cx = cur % kAxis;
        const int cy = cur / kAxis;
        for (int d = 0; d < 4; ++d) {
            const int nx = cx + kNeighborDx[d];
            const int ny = cy + kNeighborDy[d];
            if (nx < 0 || ny < 0 || nx >= kAxis || ny >= kAxis) continue;
            const std::int32_t ni = index(nx, ny);
            if (settled[static_cast<std::size_t>(ni)]) continue;
            const std::int32_t cost = passableIndex(ni) ? 0 : 1;
            const std::int32_t candidate = dist[static_cast<std::size_t>(cur)] + cost;
            if (candidate < dist[static_cast<std::size_t>(ni)]) {
                dist[static_cast<std::size_t>(ni)] = candidate;
                parent[static_cast<std::size_t>(ni)] = cur;
                if (cost == 0) queue.push_front(ni);
                else queue.push_back(ni);
            }
        }
    }

    std::vector<std::int32_t> stack;
    stack.reserve(256);
    for (std::int32_t t = 0; t < kTotalTiles; ++t) {
        if (!passableIndex(t) || dist[static_cast<std::size_t>(t)] == 0) continue;

        for (std::int32_t cur = t; cur >= 0 && dist[static_cast<std::size_t>(cur)] != 0;
             cur = parent[static_cast<std::size_t>(cur)]) {
            if (!passableIndex(cur)) tiles_[static_cast<std::size_t>(cur)] = static_cast<std::uint8_t>(Tile::Ground);
        }

        // The corridor joined t's whole region to the spawn's, so flood the
        // region and mark it reached. Without this every tile of a walled-off
        // lake would dig its own corridor, and the repair would be quadratic.
        stack.clear();
        stack.push_back(t);
        dist[static_cast<std::size_t>(t)] = 0;
        while (!stack.empty()) {
            const std::int32_t cur = stack.back();
            stack.pop_back();
            const int cx = cur % kAxis;
            const int cy = cur / kAxis;
            for (int d = 0; d < 4; ++d) {
                const int nx = cx + kNeighborDx[d];
                const int ny = cy + kNeighborDy[d];
                if (nx < 0 || ny < 0 || nx >= kAxis || ny >= kAxis) continue;
                const std::int32_t ni = index(nx, ny);
                if (dist[static_cast<std::size_t>(ni)] == 0 || !passableIndex(ni)) continue;
                dist[static_cast<std::size_t>(ni)] = 0;
                stack.push_back(ni);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Collision
// ---------------------------------------------------------------------------

bool Terrain::nearestOpenTile(Vec2 p, int& outTx, int& outTy) const {
    const int px = toTileCoord(p.x);
    const int py = toTileCoord(p.y);
    for (int ring = 0; ring <= kNearestOpenSearchTiles; ++ring) {
        bool found = false;
        double best = 0;
        for (int dy = -ring; dy <= ring; ++dy) {
            for (int dx = -ring; dx <= ring; ++dx) {
                // Only the ring itself; the interior was searched already.
                if (std::abs(dx) != ring && std::abs(dy) != ring) continue;
                const int tx = px + dx;
                const int ty = py + dy;
                if (tileBlocks(atTile(tx, ty))) continue;
                const double d2 = distanceSq(tileCenter(tx, ty), p);
                if (!found || d2 < best) {
                    found = true;
                    best = d2;
                    outTx = tx;
                    outTy = ty;
                }
            }
        }
        if (found) return true;
    }
    return false;
}

Terrain::WallResolution Terrain::resolveWall(Vec2 position, double radius, Realm realm) const {
    WallResolution result;

    // Garbage in must not become an unbounded loop or a NaN out. A teleport
    // bug upstream costs the body a shove, never the tick.
    if (!std::isfinite(position.x) || !std::isfinite(position.y)) {
        position = realm == Realm::Overworld ? spawnPoint()
                 : realm == Realm::Arena     ? kArenaSpawn
                                             : activeMaze().spawn();
    }
    if (!std::isfinite(radius) || radius < 0.0) radius = 0.0;
    radius = std::min(radius, kMaxResolveRadius);

    // The maze and the arena are worlds of their own with their own walls,
    // and the tile grid has nothing to say about either. Each answers for
    // itself, closure included: a body is kept inside its realm's square or
    // ring here, never dragged towards the tile map's edge.
    if (realm == Realm::Maze) {
        const Maze& maze = activeMaze();
        result.position = maze.resolveCircle(position, radius, &result.collided);
        const Vec2 closed = clampInside(result.position, radius, realm);
        if (closed.x != result.position.x || closed.y != result.position.y) result.collided = true;
        result.position = closed;
        // Deep inside the wall mass no face is within one push. Reported, as
        // the tile resolver reports its own failure; resolveCircle rescues.
        result.unresolved = maze.blocksPoint(result.position);
        return result;
    }
    if (realm == Realm::Arena) {
        result.position = clampInside(position, radius, realm);
        result.collided = result.position.x != position.x || result.position.y != position.y;
        return result;
    }

    // Bound the scan before any tile arithmetic: a coordinate of 1e30 makes
    // the tile loop below run for the rest of the universe.
    position.x = clamp(position.x, -kTileSize, kWorldSize + kTileSize);
    position.y = clamp(position.y, -kTileSize, kWorldSize + kTileSize);

    // This is the same four-pass collision solver used by
    // resolveEntityWallCollisions() in constants.ts. In particular, wall and
    // water faces use the deterministic jagged outline players see; treating
    // them as plain 300px rectangles changes both the contact point and the
    // slide direction.
    bool cleared = true;
    for (int pass = 0; pass < kResolvePasses; ++pass) {
        const std::optional<JaggedCollision> hit = findJaggedCollision(*this, position, radius);
        if (!hit) {
            cleared = true;
            break;
        }
        position = resolveJaggedCollision(position, radius, *hit);
        result.collided = true;
        cleared = false;
    }

    // All four passes pushed, so the last push was never re-checked. This one
    // extra check -- reached only on deep multi-tile overlap, never on
    // ordinary wall contact -- is what decides whether the body actually came
    // out clear, and it is the only thing `unresolved` says.
    if (!cleared) cleared = !findJaggedCollision(*this, position, radius);

    result.position = position;
    result.unresolved = !cleared;
    return result;
}

Vec2 Terrain::resolveCircle(Vec2 position, double radius, Realm realm) const {
    if (!std::isfinite(radius) || radius < 0.0) radius = 0.0;
    radius = std::min(radius, kMaxResolveRadius);

    const WallResolution wall = resolveWall(position, radius, realm);
    position = wall.position;
    // The maze and the arena answer for themselves, rescue and closure
    // included; everything below is tile arithmetic.
    if (realm == Realm::Maze && wall.unresolved) return activeMaze().nearestFloor(position);
    if (realm != Realm::Overworld) return position;

    // Spawners and admin teleports can place a centre deep inside several
    // blocking tiles. TypeScript's per-movement caller refuses an unresolved
    // four-pass result, but these non-movement callers need a usable point.
    // Fall back only when the exact solver is still embedded; ordinary contact
    // and sliding keep the TypeScript result above.
    if (wall.unresolved) {
        int tx = 0;
        int ty = 0;
        if (!nearestOpenTile(position, tx, ty)) position = spawnPoint();
        else {
            const Rect open = tileRect(tx, ty);
            const double inset = std::min(radius + kWallResolveEpsilon, kTileSize * 0.49);
            position.x = clamp(position.x, open.left() + inset, open.right() - inset);
            position.y = clamp(position.y, open.top() + inset, open.bottom() - inset);
        }
        for (int pass = 0; pass < kResolvePasses; ++pass) {
            const std::optional<JaggedCollision> hit = findJaggedCollision(*this, position, radius);
            if (!hit) break;
            position = resolveJaggedCollision(position, radius, *hit);
        }
    }

    // Last-resort closure. The out-of-bounds-is-wall rule already keeps a body
    // inside; this makes it true even when the push-out could not converge.
    const double margin = std::min(radius, kWorldHalf * 0.5);
    position.x = clamp(position.x, margin, kWorldSize - margin);
    position.y = clamp(position.y, margin, kWorldSize - margin);
    return position;
}

bool Terrain::blocked(Vec2 p, Realm realm) const {
    switch (realm) {
        case Realm::Maze:  return activeMaze().blocksPoint(p);
        case Realm::Arena: return !insideArena(p);
        case Realm::Overworld: break;
    }
    return tileBlocks(at(p));
}

double Terrain::realmSize(Realm realm) {
    switch (realm) {
        case Realm::Maze:  return activeMaze().worldSize();
        case Realm::Arena: return kArenaWorldSize;
        case Realm::Overworld: break;
    }
    return kWorldSize;
}

Vec2 Terrain::clampInside(Vec2 p, double radius, Realm realm) {
    if (!std::isfinite(radius) || radius < 0.0) radius = 0.0;
    if (realm == Realm::Arena) {
        // Radially, onto the ring's inside face: the reference's PVP clamp
        // (src/server/playerState.ts:1989-1996) with the body's own radius
        // where it wrote PLAYER_SIZE / 2.
        const double maxR = std::max(0.0, kArenaRadius - radius);
        const Vec2 offset = p - kArenaCentre;
        const double distSq = offset.lengthSq();
        if (!std::isfinite(distSq)) return kArenaCentre;
        if (distSq <= maxR * maxR) return p;
        const double dist = std::sqrt(distSq);
        return kArenaCentre + offset * (maxR / dist);
    }
    const double size = realmSize(realm);
    const double margin = std::min(radius, size * 0.25);
    return {clamp(p.x, margin, size - margin), clamp(p.y, margin, size - margin)};
}

bool Terrain::outside(Vec2 p, Realm realm) {
    if (realm == Realm::Arena) return !insideArena(p);
    const double size = realmSize(realm);
    return p.x < 0.0 || p.x >= size || p.y < 0.0 || p.y >= size;
}

bool Terrain::segmentBlocked(Vec2 a, Vec2 b, Realm realm) const {
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y)) {
        return true;
    }
    if (realm == Realm::Maze) return activeMaze().blocksLine(a, b);
    if (realm == Realm::Arena) return false;   // open floor, edge to edge

    int tx = toTileCoord(a.x);
    int ty = toTileCoord(a.y);
    if (tileBlocks(atTile(tx, ty))) return true;

    const int endTx = toTileCoord(b.x);
    const int endTy = toTileCoord(b.y);
    if (tx == endTx && ty == endTy) return false;

    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const int stepX = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
    const int stepY = dy > 0 ? 1 : (dy < 0 ? -1 : 0);

    // Amanatides-Woo: tMax is the ray parameter at the next grid line on each
    // axis, tDelta the parameter cost of a whole tile. An axis with no motion
    // gets an infinite tMax and is simply never chosen.
    const double kInf = std::numeric_limits<double>::infinity();
    double tMaxX = kInf, tMaxY = kInf, tDeltaX = kInf, tDeltaY = kInf;
    if (stepX != 0) {
        const double boundary = (stepX > 0 ? (tx + 1) : tx) * kTileSize;
        tMaxX = (boundary - a.x) / dx;
        tDeltaX = kTileSize / std::fabs(dx);
    }
    if (stepY != 0) {
        const double boundary = (stepY > 0 ? (ty + 1) : ty) * kTileSize;
        tMaxY = (boundary - a.y) / dy;
        tDeltaY = kTileSize / std::fabs(dy);
    }

    for (int step = 0; step < kMaxSegmentSteps; ++step) {
        if (tMaxX < tMaxY) {
            if (tMaxX > 1.0) return false;      // the segment ended first
            tx += stepX;
            tMaxX += tDeltaX;
        } else {
            if (tMaxY > 1.0) return false;
            ty += stepY;
            tMaxY += tDeltaY;
        }
        if (tileBlocks(atTile(tx, ty))) return true;
        if (tx == endTx && ty == endTy) return false;
    }
    // Longer than twice the map: nonsense input, and unseeable is the safe
    // answer for every caller (line of sight, "can I walk straight there").
    return true;
}

bool Terrain::segmentTouchesBlockingTile(Vec2 a, Vec2 b, double eps) const {
    // Nonsense endpoints clip to an empty tile range in the reference, which
    // reports no crossing. Refusing a step on garbage input would be worse
    // than letting it through: the guard exists to stop a body moving where it
    // could not have walked, not to stop it moving at all.
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y)) {
        return false;
    }
    if (!std::isfinite(eps) || eps < 0.0) eps = 0.0;

    // Clamped to the grid, as the reference clamps its scan: tiles outside it
    // are air, so skipping them changes nothing and keeps the loop small.
    const int minTx = std::max(0, toTileCoord(std::min(a.x, b.x) - eps));
    const int maxTx = std::min(kTilesPerAxis - 1, toTileCoord(std::max(a.x, b.x) + eps));
    const int minTy = std::max(0, toTileCoord(std::min(a.y, b.y) - eps));
    const int maxTy = std::min(kTilesPerAxis - 1, toTileCoord(std::max(a.y, b.y) + eps));

    for (int ty = minTy; ty <= maxTy; ++ty) {
        for (int tx = minTx; tx <= maxTx; ++tx) {
            if (!tileBlocks(atTile(tx, ty))) continue;
            if (segmentTouchesRect(a, b, tx * kTileSize - eps, ty * kTileSize - eps,
                                   (tx + 1) * kTileSize + eps, (ty + 1) * kTileSize + eps)) {
                return true;
            }
        }
    }
    return false;
}

bool Terrain::hasLineOfSight(Vec2 a, Vec2 b, Realm realm, int sampleCount) const {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double distance = std::sqrt(dx * dx + dy * dy);

    // Anything this close sees itself, whatever it is standing in. The order
    // matters: the reference answers the short ray before it even asks which
    // world the endpoints are in.
    if (distance < 10.0) return true;

    if (realm == Realm::Maze) return !activeMaze().blocksLine(a, b);
    if (realm == Realm::Arena) return true;

    const int samples = std::max(1, sampleCount);
    for (int i = 0; i <= samples; ++i) {
        const double t = static_cast<double>(i) / samples;
        const int tx = toTileCoord(a.x + dx * t);
        const int ty = toTileCoord(a.y + dy * t);
        // Outside the grid is AIR, not wall -- the reference's grid has no
        // entry out there, so leaving the map does not by itself break sight.
        if (tx < 0 || ty < 0 || tx >= kTilesPerAxis || ty >= kTilesPerAxis) continue;
        if (tileBlocks(atTile(tx, ty))) return false;
    }
    return true;
}

Vec2 Terrain::findOpenSpawn(Rng& rng, Vec2 around, double radius, Realm realm) const {
    if (realm == Realm::Arena) {
        if (!std::isfinite(around.x) || !std::isfinite(around.y)) around = kArenaSpawn;
        radius = std::isfinite(radius) ? clamp(radius, 0.0, kArenaRadius) : 0.0;
        return clampInside(around + rng.insideCircle(radius), kPlayerBaseRadius, realm);
    }
    if (realm == Realm::Maze) {
        const Maze& maze = activeMaze();
        if (!std::isfinite(around.x) || !std::isfinite(around.y)) around = maze.spawn();
        radius = std::isfinite(radius) ? clamp(radius, 0.0, maze.worldSize()) : 0.0;
        for (int attempt = 0; attempt < 24; ++attempt) {
            const Vec2 p = around + rng.insideCircle(radius);
            if (!maze.isFloor(p)) continue;
            if (distanceSq(maze.resolveCircle(p, kPlayerBaseRadius), p) < 1.0) return p;
        }
        // Nothing open nearby: the nearest floor the resolver can reach from
        // the request, or failing that the entrance, which is always floor.
        const Vec2 pushed = maze.resolveCircle(around, kPlayerBaseRadius);
        return maze.isFloor(pushed) ? pushed : maze.spawn();
    }

    if (!std::isfinite(around.x) || !std::isfinite(around.y)) around = spawnPoint();
    radius = std::isfinite(radius) ? clamp(radius, 0.0, kWorldSize) : 0.0;

    for (int attempt = 0; attempt < 24; ++attempt) {
        const Vec2 p = around + rng.insideCircle(radius);
        const Tile t = at(p);
        if (tileBlocks(t) || tileIsWater(t)) continue;
        // Reject pockets a body would immediately be squeezed out of: landing
        // in a one-tile gap between boulders reads as spawning inside a wall.
        if (distanceSq(resolveCircle(p, kPlayerBaseRadius, realm), p) < 1.0) return p;
    }

    int tx = 0, ty = 0;
    if (nearestOpenTile(around, tx, ty)) {
        // Jitter inside the tile so repeated fallbacks do not stack every mob
        // on one point.
        const Vec2 c = tileCenter(tx, ty);
        const double j = kTileSize * 0.25;
        return {c.x + rng.range(-j, j), c.y + rng.range(-j, j)};
    }
    return spawnPoint();
}

// ---------------------------------------------------------------------------
// Maze
// ---------------------------------------------------------------------------

namespace {

/// The authored layouts, one per daily biome, at CORRIDOR resolution: each
/// character becomes a 2x2 block of cells, which is what leaves room for the
/// corner fillets. '#' is void; every other letter is corridor and names the
/// difficulty band it belongs to. 'S' is the entrance, 'B' a boss room.
constexpr int kMazeTemplateDim = 22;
constexpr const char* kMazeTemplates[3][kMazeTemplateDim] = {
    // garden
    {
        "######################",
        "#mmm#mmmm#lll##llllee#",
        "#m#m#m##m#m#l##l##l#e#",
        "#m#m#m##m#m#l##l##l#e#",
        "#m#m#mB#m#m#llll##l#e#",
        "#m#m####m######l##l#e#",
        "#m#mmmmmm######l##l#e#",
        "#m#############m####e#",
        "#m#####cccuuuu#mmB##e#",
        "#l#####c##u##u######e#",
        "#ll##Scc##u##u######e#",
        "##ll######u##rrrrrree#",
        "###l######u##r###e##e#",
        "###ll#rrrrr##r###e##e#",
        "#l##l#r###r##r###e#ee#",
        "#l##l#r###rrrr###e####",
        "#ll#e#r#rrr##r#eeeeee#",
        "#l##e#r######r#e####e#",
        "#l##e#r######r#e##l#e#",
        "#eeeeeeeeeeeer#e##e#e#",
        "###############eeeeee#",
        "######################",
    },
    // desert
    {
        "######################",
        "#Bmmmm#mmmmmBmmm#mll##",
        "#mmmm##m#######mmm#ll#",
        "#mmmm##m############l#",
        "#mmmm##l##S##l######l#",
        "#mmmmmll##c##lll####l#",
        "#mmm###l##c####l#elll#",
        "#mm####l##c####l#e####",
        "#m#####l##c####eeee###",
        "#######l##cc###eeeee##",
        "#######l##c###eeeeeee#",
        "#eeeelll#cc##eeeeeeee#",
        "#e#e###l##c##eeeeeeee#",
        "#e#####ll#c##eeeeeeee#",
        "#e###e##l#u##e#eeee#e#",
        "#eerrre#e#uu#e##ere#e#",
        "#e##r#e#e#u##e###r##e#",
        "#e##r#e#e#u##l##rr##e#",
        "#e##r#eee#u####rr###l#",
        "#e##r#####u###rr###ll#",
        "#e##rrrruuuuurr###lll#",
        "######################",
    },
    // ocean
    {
        "######################",
        "#####mmmmmm######Bmm##",
        "#####m####mmm######m##",
        "###llm######m######m##",
        "###l#m######m#mlllmm##",
        "#lll#m##mmm#m#m#l##m##",
        "#l###mmmm#B#mmm#l##m##",
        "#ll#########m###l##m##",
        "##l#############l##mm#",
        "##le############l#####",
        "###e##cccScccc##l#####",
        "###e##c######c##llll##",
        "###e##c######c###l####",
        "###e##c##u###c###eeee#",
        "#eee##ccccu##cc#####e#",
        "#e#######u####c#u##ee#",
        "#e###r###u####uuuu#e##",
        "#e#rrrr##u######u##ee#",
        "#r#r#r###u#uuu######e#",
        "#r#r#u#uuuuu#u#rrr#rr#",
        "#rrr#uuu#####rrr#rrr##",
        "######################",
    },
};

/// Difficulty band a template character names, or -1 for anything the grid
/// does not understand. An unknown character is treated as void rather than
/// rejected: a bad hand-edit must not take the server down on the day the
/// rotation reaches that biome.
int mazeZoneOfChar(char c) {
    switch (c) {
        case 'c': return 0;
        case 'u': return 1;
        case 'r': return 2;
        case 'e': return 3;
        case 'l': return 4;
        case 'm': return 5;
        case 'S': return 0;   // the entrance room is common
        case 'B': return 5;   // boss rooms are the deepest band
        default: return -1;
    }
}

Vec2 mazeCellCenter(int tx, int ty) {
    return {kMazeOriginX + (tx * 2 + 1) * kMazeCellSize,
            kMazeOriginY + (ty * 2 + 1) * kMazeCellSize};
}

} // namespace

void Maze::setDay(std::int64_t dayNumber) {
    day_ = dayNumber;
    const int pick = static_cast<int>(((dayNumber % 3) + 3) % 3);
    biome_ = static_cast<MazeBiome>(pick);
    const char* const* rows = kMazeTemplates[pick];

    const int d = kMazeTemplateDim;
    templateDim_ = d;
    gridDim_ = d * 2;

    // Pass one: the corridor lattice, its bands, the entrance and the bosses.
    std::vector<std::uint8_t> walkable(static_cast<std::size_t>(d) * d, 0);
    std::vector<std::uint8_t> bands(static_cast<std::size_t>(d) * d, 255);
    int spawnX = -1;
    int spawnY = -1;
    std::vector<int> bossCells;
    for (int y = 0; y < d; ++y) {
        for (int x = 0; x < d; ++x) {
            const char c = rows[y][x];
            const int zone = mazeZoneOfChar(c);
            if (zone < 0) continue;
            walkable[static_cast<std::size_t>(y) * d + x] = 1;
            bands[static_cast<std::size_t>(y) * d + x] = static_cast<std::uint8_t>(zone);
            if (c == 'S' && spawnX < 0) { spawnX = x; spawnY = y; }
            if (c == 'B') bossCells.push_back(y * d + x);
        }
    }
    if (spawnX < 0) {
        // No entrance authored: the first walkable cell, which client and
        // server agree on just as readily as an authored one would.
        for (int i = 0; i < d * d && spawnX < 0; ++i) {
            if (walkable[static_cast<std::size_t>(i)]) { spawnX = i % d; spawnY = i / d; }
        }
    }

    // Pass two: expand each corridor cell to a 2x2 block and code the corners.
    // A floor cell rounds CONVEX where two voids meet it diagonally; a void
    // cell rounds CONCAVE where two corridors do. Both codes name the shared
    // vertex the fillet is centred on, which is what lets one number drive
    // collision and rendering alike.
    const int dim = gridDim_;
    values_.assign(static_cast<std::size_t>(dim) * dim, 0);
    zones_.assign(static_cast<std::size_t>(dim) * dim, 255);
    const auto tileAt = [&](int x, int y, int a, int b) -> int {
        const int nx = x + a;
        const int ny = y + b;
        if (nx < 0 || ny < 0 || nx >= d || ny >= d) return 0;
        return walkable[static_cast<std::size_t>(ny) * d + nx];
    };
    const auto setGrid = [&](int gx, int gy, int v) {
        values_[static_cast<std::size_t>(gy) * dim + gx] = static_cast<std::uint8_t>(v);
    };
    for (int y = 0; y < d; ++y) {
        for (int x = 0; x < d; ++x) {
            const bool walk = walkable[static_cast<std::size_t>(y) * d + x] != 0;
            const std::uint8_t zone = bands[static_cast<std::size_t>(y) * d + x];
            for (int sy = 0; sy < 2; ++sy) {
                for (int sx = 0; sx < 2; ++sx) {
                    zones_[static_cast<std::size_t>(y * 2 + sy) * dim + (x * 2 + sx)] = zone;
                }
            }
            const int top = tileAt(x, y, 0, -1);
            const int bottom = tileAt(x, y, 0, 1);
            const int left = tileAt(x, y, -1, 0);
            const int right = tileAt(x, y, 1, 0);
            if (walk) {
                if (top == 0) {
                    setGrid(x * 2, y * 2, left == 0 ? 7 : 1);
                    setGrid(x * 2 + 1, y * 2, right == 0 ? 5 : 1);
                } else {
                    setGrid(x * 2, y * 2, 1);
                    setGrid(x * 2 + 1, y * 2, 1);
                }
                if (bottom == 0) {
                    setGrid(x * 2, y * 2 + 1, left == 0 ? 6 : 1);
                    setGrid(x * 2 + 1, y * 2 + 1, right == 0 ? 4 : 1);
                } else {
                    setGrid(x * 2, y * 2 + 1, 1);
                    setGrid(x * 2 + 1, y * 2 + 1, 1);
                }
            } else {
                if (top) {
                    setGrid(x * 2, y * 2, (left && tileAt(x, y, -1, -1)) ? 15 : 0);
                    setGrid(x * 2 + 1, y * 2, (right && tileAt(x, y, 1, -1)) ? 13 : 0);
                } else {
                    setGrid(x * 2, y * 2, 0);
                    setGrid(x * 2 + 1, y * 2, 0);
                }
                if (bottom) {
                    setGrid(x * 2, y * 2 + 1, (left && tileAt(x, y, -1, 1)) ? 14 : 0);
                    setGrid(x * 2 + 1, y * 2 + 1, (right && tileAt(x, y, 1, 1)) ? 12 : 0);
                } else {
                    setGrid(x * 2, y * 2 + 1, 0);
                    setGrid(x * 2 + 1, y * 2 + 1, 0);
                }
            }
        }
    }

    spawn_ = spawnX < 0 ? Vec2{kMazeOriginX, kMazeOriginY} : mazeCellCenter(spawnX, spawnY);
    bossSpots_.clear();
    bossSpots_.reserve(bossCells.size());
    for (int idx : bossCells) bossSpots_.push_back(mazeCellCenter(idx % d, idx / d));
}

std::uint8_t Maze::cellValue(int gx, int gy) const {
    if (gx < 0 || gy < 0 || gx >= gridDim_ || gy >= gridDim_) return 0;
    return values_[static_cast<std::size_t>(gy) * gridDim_ + gx];
}

bool Maze::cellBlocksPoint(int gx, int gy, Vec2 world) const {
    const int value = cellValue(gx, gy);
    if (value == 0) return true;
    if (value == 1) return false;
    // The fillet is a circle of one whole cell centred on the vertex the code
    // names. A convex floor corner keeps the point INSIDE that circle; a
    // concave void corner keeps it outside.
    const double cornerX = kMazeOriginX + (gx + ((value >> 1) & 1)) * kMazeCellSize;
    const double cornerY = kMazeOriginY + (gy + (value & 1)) * kMazeCellSize;
    const double dx = world.x - cornerX;
    const double dy = world.y - cornerY;
    const bool withinArc = dx * dx + dy * dy <= kMazeCellSize * kMazeCellSize;
    return value >= 12 ? withinArc : !withinArc;
}

int Maze::zoneOfCell(int gx, int gy) const {
    if (gx < 0 || gy < 0 || gx >= gridDim_ || gy >= gridDim_) return -1;
    const std::uint8_t zone = zones_[static_cast<std::size_t>(gy) * gridDim_ + gx];
    return zone == 255 ? -1 : static_cast<int>(zone);
}

int Maze::floorCellCount() const {
    int count = 0;
    for (const std::uint8_t v : values_) {
        if (v == 1 || (v >= 4 && v <= 7)) ++count;
    }
    return count;
}

int Maze::zoneAt(Vec2 p) const {
    if (!contains(p)) return -1;
    const int gx = static_cast<int>(std::floor((p.x - kMazeOriginX) / kMazeCellSize));
    const int gy = static_cast<int>(std::floor((p.y - kMazeOriginY) / kMazeCellSize));
    if (gx < 0 || gy < 0 || gx >= gridDim_ || gy >= gridDim_) return -1;
    const std::uint8_t zone = zones_[static_cast<std::size_t>(gy) * gridDim_ + gx];
    return zone == 255 ? -1 : static_cast<int>(zone);
}

bool Maze::blocksPoint(Vec2 p) const {
    if (!contains(p)) return false;
    const int gx = static_cast<int>(std::floor((p.x - kMazeOriginX) / kMazeCellSize));
    const int gy = static_cast<int>(std::floor((p.y - kMazeOriginY) / kMazeCellSize));
    return cellBlocksPoint(gx, gy, p);
}

bool Maze::isFloor(Vec2 p) const {
    if (!contains(p)) return false;
    const int gx = static_cast<int>(std::floor((p.x - kMazeOriginX) / kMazeCellSize));
    const int gy = static_cast<int>(std::floor((p.y - kMazeOriginY) / kMazeCellSize));
    const int v = cellValue(gx, gy);
    return v == 1 || (v >= 4 && v <= 7);
}

bool Maze::blocksLine(Vec2 a, Vec2 b) const {
    if (!contains(a) && !contains(b)) return false;
    // A degenerate endpoint would make the step count NaN or astronomical and
    // spin this loop; no legitimate sight line spans anywhere near that far.
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y)) {
        return false;
    }
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double dist = std::sqrt(dx * dx + dy * dy);
    const int steps = static_cast<int>(clamp(std::ceil(dist / (kMazeCellSize / 3.0)), 1.0, 1024.0));
    for (int i = 0; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        if (blocksPoint({a.x + dx * t, a.y + dy * t})) return true;
    }
    return false;
}

bool Maze::resolveOnce(Vec2 position, double radius, Vec2& out) const {
    const double g = kMazeCellSize;
    const double u = position.x - kMazeOriginX;
    const double v = position.y - kMazeOriginY;
    const int cx = static_cast<int>(std::floor(u / g));
    const int cy = static_cast<int>(std::floor(v / g));

    const auto val = [&](int a, int b) { return static_cast<int>(cellValue(cx + a, cy + b)); };

    // Push out around a corner vertex given in maze-local units. A convex
    // floor corner holds the centre within (g - r) of the vertex; a concave
    // void corner holds it beyond (g + r).
    const auto curveCheck = [&](double ox, double oy, int inverse, Vec2& hit) {
        double dx = u - ox;
        double dy = v - oy;
        double d = std::sqrt(dx * dx + dy * dy);
        // A convex corner collides once the centre has strayed OUTSIDE its
        // arc, a concave one once it has strayed inside. Both are written as
        // plain comparisons, so a NaN distance reads as clear and no push is
        // invented for a body that has no position to speak of.
        const double target = inverse == 0 ? g - radius : g + radius;
        const bool overlapping = inverse == 0 ? d > target : d < target;
        if (!overlapping) return false;
        if (d == 0.0) { dx = 1.0; dy = 0.0; d = 1.0; }
        const double s = target / d;
        hit = {kMazeOriginX + ox + dx * s, kMazeOriginY + oy + dy * s};
        return true;
    };

    struct Corner { double ox, oy; int inverse; };
    const auto cornerOf = [&](int tile, int baseX, int baseY) {
        const int left = (tile >> 1) & 1;
        const int top = tile & 1;
        return Corner{(baseX + left) * g, (baseY + top) * g, (tile >> 3) & 1};
    };

    const int tile0 = val(0, 0);
    if (tile0 != 1) {
        if (tile0 == 0) {
            // The centre is inside solid void. Movement never puts it there,
            // but an instantaneous shove -- mob contact, petal knockback --
            // is applied after wall resolution and can. Answering "no
            // collision" would let the body noclip the whole lattice, so it
            // is pushed out through the nearest walkable face and the outer
            // iteration finishes the job.
            const double lx = u - cx * g;
            const double ly = v - cy * g;
            const auto isFloorCell = [&](int a, int b) {
                const int t = val(a, b);
                return t == 1 || (t >= 4 && t <= 7);
            };
            bool found = false;
            double bestDepth = 0.0;
            Vec2 best;
            const auto consider = [&](double depth, Vec2 q) {
                if (!found || depth < bestDepth) { found = true; bestDepth = depth; best = q; }
            };
            if (isFloorCell(-1, 0)) consider(lx, {kMazeOriginX + cx * g - radius, position.y});
            if (isFloorCell(1, 0)) consider(g - lx, {kMazeOriginX + (cx + 1) * g + radius, position.y});
            if (isFloorCell(0, -1)) consider(ly, {position.x, kMazeOriginY + cy * g - radius});
            if (isFloorCell(0, 1)) consider(g - ly, {position.x, kMazeOriginY + (cy + 1) * g + radius});
            // Not found only deep inside the wall mass, which one knock cannot
            // reach.
            if (found) out = best;
            return found;
        }
        const Corner c = cornerOf(tile0, cx, cy);
        if (curveCheck(c.ox, c.oy, c.inverse, out)) return true;
    }
    if (val(-1, 0) != 1 && u - cx * g < radius) {
        const int tile = val(-1, 0);
        if (tile == 0) { out = {kMazeOriginX + cx * g + radius, position.y}; return true; }
        const Corner c = cornerOf(tile, cx - 1, cy);
        if (curveCheck(c.ox, c.oy, c.inverse, out)) return true;
    }
    if (val(0, -1) != 1 && v - cy * g < radius) {
        const int tile = val(0, -1);
        if (tile == 0) { out = {position.x, kMazeOriginY + cy * g + radius}; return true; }
        const Corner c = cornerOf(tile, cx, cy - 1);
        if (curveCheck(c.ox, c.oy, c.inverse, out)) return true;
    }
    if (val(1, 0) != 1 && (cx + 1) * g - u < radius) {
        const int tile = val(1, 0);
        if (tile == 0) { out = {kMazeOriginX + (cx + 1) * g - radius, position.y}; return true; }
        const Corner c = cornerOf(tile, cx + 1, cy);
        if (curveCheck(c.ox, c.oy, c.inverse, out)) return true;
    }
    if (val(0, 1) != 1 && (cy + 1) * g - v < radius) {
        const int tile = val(0, 1);
        if (tile == 0) { out = {position.x, kMazeOriginY + (cy + 1) * g - radius}; return true; }
        const Corner c = cornerOf(tile, cx, cy + 1);
        if (curveCheck(c.ox, c.oy, c.inverse, out)) return true;
    }
    return false;
}

Vec2 Maze::nearestFloor(Vec2 p) const {
    if (gridDim_ <= 0) return spawn_;
    const int cx = clamp(static_cast<int>(std::floor((p.x - kMazeOriginX) / kMazeCellSize)), 0,
                         gridDim_ - 1);
    const int cy = clamp(static_cast<int>(std::floor((p.y - kMazeOriginY) / kMazeCellSize)), 0,
                         gridDim_ - 1);
    for (int ring = 0; ring < gridDim_; ++ring) {
        Vec2 best;
        double bestDistSq = -1.0;
        for (int gy = cy - ring; gy <= cy + ring; ++gy) {
            for (int gx = cx - ring; gx <= cx + ring; ++gx) {
                if (std::abs(gx - cx) != ring && std::abs(gy - cy) != ring) continue;
                if (cellValue(gx, gy) != 1) continue;
                const Vec2 centre{kMazeOriginX + (gx + 0.5) * kMazeCellSize,
                                  kMazeOriginY + (gy + 0.5) * kMazeCellSize};
                const double d = distanceSq(centre, p);
                if (bestDistSq < 0.0 || d < bestDistSq) {
                    bestDistSq = d;
                    best = centre;
                }
            }
        }
        if (bestDistSq >= 0.0) return best;
    }
    return spawn_;
}

Vec2 Maze::resolveCircle(Vec2 position, double radius, bool* collided) const {
    bool hitAny = false;
    for (int pass = 0; pass < 4; ++pass) {
        Vec2 pushed;
        if (!resolveOnce(position, radius, pushed)) break;
        position = pushed;
        hitAny = true;
    }
    if (collided) *collided = hitAny;
    return position;
}

bool Maze::circleWallOverlap(Vec2 position, double radius, Rect& out) const {
    if (!contains(position)) return false;
    const double g = kMazeCellSize;
    const int minGx = static_cast<int>(std::floor((position.x - radius - kMazeOriginX) / g));
    const int maxGx = static_cast<int>(std::floor((position.x + radius - kMazeOriginX) / g));
    const int minGy = static_cast<int>(std::floor((position.y - radius - kMazeOriginY) / g));
    const int maxGy = static_cast<int>(std::floor((position.y + radius - kMazeOriginY) / g));
    for (int gy = minGy; gy <= maxGy; ++gy) {
        for (int gx = minGx; gx <= maxGx; ++gx) {
            const int v = cellValue(gx, gy);
            if (v == 1) continue;               // plain floor never blocks
            const double left = kMazeOriginX + gx * g;
            const double top = kMazeOriginY + gy * g;
            const double nearX = std::max(left, std::min(position.x, left + g));
            const double nearY = std::max(top, std::min(position.y, top + g));
            const double dx = position.x - nearX;
            const double dy = position.y - nearY;
            if (dx * dx + dy * dy > radius * radius) continue;
            // A corner cell only blocks on the black side of its arc, so the
            // nearest point decides -- a projectile grazing the open half of a
            // fillet passes, exactly as the drawn geometry says it should.
            if (!cellBlocksPoint(gx, gy, {nearX, nearY}) && !cellBlocksPoint(gx, gy, position)) {
                continue;
            }
            out = {left, top, g, g};
            return true;
        }
    }
    return false;
}

namespace {

/// The mutable half of the shared maze. Private so that everything outside
/// this file can read today's maze but only setActiveMazeDay() can change it.
Maze& mutableActiveMaze() {
    static Maze maze(currentMazeDay());
    return maze;
}

} // namespace

std::int64_t currentMazeDay() {
    using namespace std::chrono;
    const auto epochMillis =
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    return static_cast<std::int64_t>(epochMillis / 86400000);
}

const Maze& activeMaze() { return mutableActiveMaze(); }

void setActiveMazeDay(std::int64_t dayNumber) {
    if (mutableActiveMaze().day() != dayNumber) mutableActiveMaze().setDay(dayNumber);
}

} // namespace flix
