#include "shared/game/terrain.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>

#include "shared/game/tiled_map.h"

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
// Collision geometry
// ---------------------------------------------------------------------------
//
// A cell blocks where its tile's authored SHAPES are, and a cell with no shapes
// blocks over its whole 300-unit square if its coarse Tile says so. Both are
// the same code here: the fallback is a four-point ring, so there is one set of
// polygon routines and no second path to keep honest.
//
// Everything below works in ONE CELL'S LOCAL SPACE -- (0,0) at the cell's
// top-left corner -- because that is how a shape is stored. A query subtracts
// the cell origin from its point once and adds it back to the answer, instead of
// translating fourteen polygon vertices.

constexpr double kWallResolveEpsilon = 0.01;

/// The four corners of a whole cell, wound positive, for the fallback.
std::array<Vec2, 4> wholeCellRing() {
    return {Vec2{0.0, 0.0}, Vec2{kTileSize, 0.0}, Vec2{kTileSize, kTileSize},
            Vec2{0.0, kTileSize}};
}

/// A ring, as every routine here takes one: a pointer and a count, so a stored
/// CollisionShape and a stack-built cell square are the same thing to them.
struct Ring {
    const Vec2* points = nullptr;
    std::size_t count = 0;
    const Vec2& operator[](std::size_t i) const { return points[i]; }
};

/// Is the point inside the ring? Crossing (even-odd) test.
///
/// A point exactly on an edge is either answer, and deliberately not special-
/// cased: every caller that cares is already asking about a circle of some
/// radius, and the one that is not (a sample of hasLineOfSight) is allowed a
/// pixel of slop by construction.
bool pointInRing(const Ring& ring, Vec2 p) {
    bool inside = false;
    for (std::size_t i = 0, j = ring.count - 1; i < ring.count; j = i++) {
        const Vec2& a = ring[i];
        const Vec2& b = ring[j];
        if ((a.y > p.y) == (b.y > p.y)) continue;
        const double t = (p.y - a.y) / (b.y - a.y);
        if (p.x < a.x + t * (b.x - a.x)) inside = !inside;
    }
    return inside;
}

/// The closest point of a ring's BOUNDARY to `p`.
struct NearestOnRing {
    Vec2 at;
    double distSq = 0.0;
    std::size_t edge = 0;
    /// The closest point fell strictly inside the edge, not on a vertex. A
    /// face hit, in other words, which is what the push-out prefers.
    bool interior = false;
};

NearestOnRing nearestOnRing(const Ring& ring, Vec2 p) {
    NearestOnRing best;
    best.distSq = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < ring.count; ++i) {
        const Vec2& a = ring[i];
        const Vec2& b = ring[(i + 1) % ring.count];
        const Vec2 d = b - a;
        const double lengthSq = d.lengthSq();
        double t = 0.0;
        if (lengthSq > 0.0) t = clamp(((p - a).x * d.x + (p - a).y * d.y) / lengthSq, 0.0, 1.0);
        const Vec2 at = a + d * t;
        const double d2 = distanceSq(at, p);
        const bool interior = t > 0.0 && t < 1.0;
        // A tie between a face and a vertex goes to the face, so a circle
        // resting exactly on a corner of one edge is still pushed off the face.
        if (d2 < best.distSq || (d2 == best.distSq && interior && !best.interior)) {
            best.at = at;
            best.distSq = d2;
            best.edge = i;
            best.interior = interior;
        }
    }
    return best;
}

/// The unit outward normal of one edge. Only needed for a centre sitting
/// exactly ON the boundary, where there is no direction to push along; every
/// other case has one. Rings are wound so their signed area is positive, which
/// is what makes (dy, -dx) point out.
Vec2 outwardNormal(const Ring& ring, std::size_t edge) {
    const Vec2& a = ring[edge];
    const Vec2& b = ring[(edge + 1) % ring.count];
    const Vec2 d = b - a;
    const double length = d.length();
    if (!(length > 0.0)) return {1.0, 0.0};
    return {d.y / length, -d.x / length};
}

/// Where one ring pushes a circle to, and whether the push came off a face.
/// Empty when the circle is clear of the ring.
///
/// THE CONTRACT, which is the same one the whole-cell rectangle always had:
/// the result stands exactly radius + kWallResolveEpsilon clear of the closest
/// point of the ring. A centre inside the ring leaves along the shortest way
/// out; a centre outside it is pushed back the way it came. Against a rectangle
/// this is arithmetically identical to the least-penetration ejection this file
/// used to write out by hand, face by face.
struct RingPush {
    Vec2 position;
    bool flat = false;
};

std::optional<RingPush> pushOutOfRing(const Ring& ring, Vec2 p, double radius) {
    if (ring.count < 3) return std::nullopt;
    const NearestOnRing near = nearestOnRing(ring, p);
    const bool inside = pointInRing(ring, p);
    // Touching is not overlapping: the same strict test the rectangle used, so
    // a body resting against a wall is not pushed again every tick.
    if (!inside && near.distSq >= radius * radius) return std::nullopt;

    const double reach = radius + kWallResolveEpsilon;
    const double distance = std::sqrt(near.distSq);
    Vec2 direction;
    if (distance > 1e-12) {
        const Vec2 away = inside ? near.at - p : p - near.at;
        direction = away / distance;
    } else {
        // Dead on the boundary. Nothing in the geometry says which way is out,
        // so the edge's own normal does.
        direction = outwardNormal(ring, near.edge);
    }
    RingPush push;
    push.position = near.at + direction * reach;
    push.flat = inside || near.interior;
    return push;
}

/// Do two segments touch at all? Orientation signs, with the collinear and
/// endpoint-touching cases counted as touching.
bool segmentsIntersect(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
    const auto cross = [](Vec2 o, Vec2 u, Vec2 v) {
        return (u.x - o.x) * (v.y - o.y) - (u.y - o.y) * (v.x - o.x);
    };
    const auto onSegment = [](Vec2 u, Vec2 v, Vec2 q) {
        return std::min(u.x, v.x) <= q.x && q.x <= std::max(u.x, v.x) &&
               std::min(u.y, v.y) <= q.y && q.y <= std::max(u.y, v.y);
    };
    const double d1 = cross(a, b, c);
    const double d2 = cross(a, b, d);
    const double d3 = cross(c, d, a);
    const double d4 = cross(c, d, b);
    if (((d1 > 0.0) != (d2 > 0.0)) && ((d3 > 0.0) != (d4 > 0.0)) && d1 != 0.0 && d2 != 0.0 &&
        d3 != 0.0 && d4 != 0.0) {
        return true;
    }
    if (d1 == 0.0 && onSegment(a, b, c)) return true;
    if (d2 == 0.0 && onSegment(a, b, d)) return true;
    if (d3 == 0.0 && onSegment(c, d, a)) return true;
    if (d4 == 0.0 && onSegment(c, d, b)) return true;
    return false;
}

double pointSegmentDistSq(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 d = b - a;
    const double lengthSq = d.lengthSq();
    double t = 0.0;
    if (lengthSq > 0.0) t = clamp(((p - a).x * d.x + (p - a).y * d.y) / lengthSq, 0.0, 1.0);
    return distanceSq(a + d * t, p);
}

/// The squared distance between two segments that do NOT cross -- which is the
/// only case it is called in, so the four endpoint-to-segment distances are the
/// whole answer.
double segmentDistSq(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
    return std::min(std::min(pointSegmentDistSq(a, c, d), pointSegmentDistSq(b, c, d)),
                    std::min(pointSegmentDistSq(c, a, b), pointSegmentDistSq(d, a, b)));
}

/// Does the segment come within `eps` of the ring, inside included?
///
/// `eps` grows the shape by a DISC of that radius: the region tested is every
/// point within `eps` of the ring, which is the Minkowski sum with a circle.
/// The whole-cell version this replaced grew an axis-aligned rectangle by `eps`
/// on each side, so at a CORNER it reached eps*sqrt(2) where this reaches eps
/// -- i.e. the new test is very slightly TIGHTER diagonally off a corner, not
/// looser. Deliberate, and the only honest choice now that a shape may be a
/// turned polygon, where "grow the rectangle" is not defined: distance to the
/// shape is the same question whichever way the shape is turned, and a body
/// that clears a corner by more than the inflation has not touched it. eps is
/// kCenterPathInflation, a half unit against a 300-unit cell.
bool ringTouchesSegment(const Ring& ring, Vec2 a, Vec2 b, double eps) {
    if (ring.count < 3) return false;
    if (pointInRing(ring, a) || pointInRing(ring, b)) return true;
    const double epsSq = eps * eps;
    for (std::size_t i = 0; i < ring.count; ++i) {
        const Vec2& e0 = ring[i];
        const Vec2& e1 = ring[(i + 1) % ring.count];
        if (segmentsIntersect(a, b, e0, e1)) return true;
        if (eps > 0.0 && segmentDistSq(a, b, e0, e1) <= epsSq) return true;
    }
    return false;
}

/// The box a ring's own points span, for the reject.
Rect ringBounds(const std::vector<Vec2>& points) {
    double minX = points.empty() ? 0.0 : points[0].x;
    double minY = points.empty() ? 0.0 : points[0].y;
    double maxX = minX;
    double maxY = minY;
    for (const Vec2& point : points) {
        minX = std::min(minX, point.x);
        maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y);
        maxY = std::max(maxY, point.y);
    }
    return {minX, minY, maxX - minX, maxY - minY};
}

/// Is this ring exactly its own bounding box? Four points, each on a corner of
/// it, and no two on the same one. See CollisionShape::rectangle.
bool ringIsItsBox(const std::vector<Vec2>& points, const Rect& bounds) {
    if (points.size() != 4) return false;
    if (!(bounds.w > 0.0) || !(bounds.h > 0.0)) return false;
    int seen = 0;
    for (const Vec2& p : points) {
        const bool left = p.x == bounds.left();
        const bool right = p.x == bounds.right();
        const bool top = p.y == bounds.top();
        const bool bottom = p.y == bounds.bottom();
        if (!(left || right) || !(top || bottom)) return false;
        seen |= 1 << ((right ? 1 : 0) | (bottom ? 2 : 0));
    }
    return seen == 0b1111;
}

Rect unionRect(const Rect& a, const Rect& b) {
    const double left = std::min(a.left(), b.left());
    const double top = std::min(a.top(), b.top());
    return {left, top, std::max(a.right(), b.right()) - left,
            std::max(a.bottom(), b.bottom()) - top};
}

bool rectOverlaps(const Rect& r, double left, double top, double right, double bottom) {
    return r.left() <= right && r.right() >= left && r.top() <= bottom && r.bottom() >= top;
}

/// Rect::contains is half-open, which would reject a point sitting exactly on a
/// shape's right or bottom edge. Every box here is a REJECT, so it has to be
/// inclusive on all four sides: a point the box drops is a point the ring is
/// never asked about.
bool rectHolds(const Rect& r, Vec2 p) {
    return p.x >= r.left() && p.x <= r.right() && p.y >= r.top() && p.y <= r.bottom();
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
// Construction and generation
// ---------------------------------------------------------------------------

Terrain::Terrain() {
    // Only the overworld starts with a grid. Every other realm is empty until
    // a map is staged into it, and an empty grid reads as solid everywhere --
    // which is exactly what a realm nobody authored should be.
    Grid& world = grid(Realm::Overworld);
    world.cols = kAxis;
    world.rows = kAxis;
    world.tiles.assign(static_cast<std::size_t>(kTotalTiles), static_cast<std::uint8_t>(Tile::Ground));
    world.spawnTile = index(world, kAxis / 2, kAxis / 2);
}

bool Terrain::hasMap(Realm realm) const { return !grid(realm).tiles.empty(); }

void Terrain::clearRealm(Realm realm) {
    Grid& g = grid(realm);
    g.cols = 0;
    g.rows = 0;
    g.tiles.clear();
    g.spawnTile = 0;
    clearCollisionShapes(realm);
}

bool Terrain::install(Realm realm, std::vector<std::uint8_t> tiles, int cols, int rows) {
    if (cols <= 0 || rows <= 0 || cols > kMaxTilesPerAxis || rows > kMaxTilesPerAxis) return false;
    if (tiles.size() != static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows)) return false;
    for (const std::uint8_t tile : tiles) {
        if (tile > static_cast<std::uint8_t>(Tile::Block)) return false;
    }
    Grid& g = grid(realm);
    // Shapes are indexed by the grid's width, so a grid of a different shape
    // describes a different map and the store cannot survive it. The same
    // dimensions keep it, which is what lets a client install the wire grid and
    // its own shapes in either order.
    if (g.cols != cols || g.rows != rows) clearCollisionShapes(realm);
    g.cols = cols;
    g.rows = rows;
    g.tiles = std::move(tiles);
    g.spawnTile = clamp(g.spawnTile, 0, static_cast<int>(g.tiles.size()) - 1);
    return true;
}

Vec2 Terrain::spawnPoint(Realm realm) const {
    const Grid& g = grid(realm);
    if (g.cols <= 0 || g.rows <= 0) return {0.0, 0.0};
    return tileCenter(g.spawnTile % g.cols, g.spawnTile / g.cols);
}

void Terrain::setTile(int tx, int ty, Tile t, Realm realm) {
    Grid& g = grid(realm);
    if (tx < 0 || ty < 0 || tx >= g.cols || ty >= g.rows) return;
    g.tiles[static_cast<std::size_t>(index(g, tx, ty))] = static_cast<std::uint8_t>(t);
    // A direct write says what the CELL is, and there is no shape for it to
    // say it in: the authored shapes come from a tile in a tileset, and this
    // caller has none. So a write onto a realm with authored shapes drops them
    // and the realm falls back to whole-cell collision -- which is exactly what
    // "this cell is now wall" can be made to mean, and is conservative. Only a
    // test or a future editor ever writes into a loaded map.
    clearCollisionShapes(realm);
}

void Terrain::fill(Tile t, Realm realm) {
    Grid& g = grid(realm);
    std::fill(g.tiles.begin(), g.tiles.end(), static_cast<std::uint8_t>(t));
    clearCollisionShapes(realm);   // see setTile
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
    // starter mobs live (the `garden` group in mobs.json), so that is where a
    // new flower belongs.
    grid(Realm::Overworld).spawnTile = chooseGardenSpawn();
    connectAll();
    assert(isConnected());
}

std::vector<std::uint8_t> encodeTileRle(const std::vector<std::uint8_t>& tiles) {
    std::vector<std::uint8_t> out;
    std::size_t at = 0;
    while (at < tiles.size()) {
        const std::uint8_t tile = tiles[at];
        std::size_t run = 1;
        while (at + run < tiles.size() && tiles[at + run] == tile) ++run;
        at += run;
        // Long runs are split at the widest a single header can carry, which
        // is 127 + 65535. A grid of one tile encodes in five bytes per chunk.
        while (run > 0) {
            const std::size_t chunk = std::min<std::size_t>(run, 127 + 0xFFFF);
            run -= chunk;
            if (chunk <= 127) {
                out.push_back(static_cast<std::uint8_t>(chunk << 1));
            } else {
                const std::size_t extra = chunk - 127;
                out.push_back(static_cast<std::uint8_t>((127u << 1) | 1u));
                out.push_back(static_cast<std::uint8_t>((extra >> 8) & 0xFFu));
                out.push_back(static_cast<std::uint8_t>(extra & 0xFFu));
            }
            out.push_back(tile);
        }
    }
    return out;
}

bool decodeTileRle(const std::uint8_t* data, std::size_t size, std::size_t expected,
                   std::vector<std::uint8_t>& out, std::string& errorOut) {
    constexpr std::uint8_t maxValue = static_cast<std::uint8_t>(Tile::Block);
    out.clear();
    out.reserve(expected);
    std::size_t at = 0;
    while (at < size) {
        const std::uint8_t header = data[at++];
        std::size_t count = header >> 1;
        if (header & 1u) {
            if (at + 2 > size) {
                errorOut = "the tile stream has a truncated extended run";
                return false;
            }
            count += (static_cast<std::size_t>(data[at]) << 8) | static_cast<std::size_t>(data[at + 1]);
            at += 2;
        }
        if (at >= size || count == 0 || out.size() + count > expected) {
            errorOut = "the tile stream contains an invalid run";
            return false;
        }
        const std::uint8_t tile = data[at++];
        if (tile > maxValue) {
            errorOut = "the tile stream contains a value past " + std::to_string(maxValue);
            return false;
        }
        out.insert(out.end(), count, tile);
    }
    if (out.size() != expected) {
        errorOut = "the tile stream decoded to " + std::to_string(out.size()) + " tiles; expected " +
                   std::to_string(expected);
        return false;
    }
    return true;
}

void writeMapGrid(ByteWriter& out, const Terrain& terrain, Realm realm) {
    // A generated realm -- the arena, the maze -- has no grid to send, and
    // says so with an empty one rather than by being a different message.
    const std::vector<std::uint8_t> tiles(terrain.tiles(realm),
                                          terrain.tiles(realm) + terrain.tileCount(realm));
    const std::vector<std::uint8_t> packed = encodeTileRle(tiles);
    out.u8(static_cast<std::uint8_t>(realm));
    out.u16(static_cast<std::uint16_t>(terrain.tileCols(realm)));
    out.u16(static_cast<std::uint16_t>(terrain.tileRows(realm)));
    out.u32(static_cast<std::uint32_t>(packed.size()));
    out.raw(packed.data(), packed.size());
}

bool readMapGrid(ByteReader& in, Terrain& terrain, Realm& realmOut, std::string& errorOut) {
    const Realm realm = realmFromByte(in.u8());
    const int cols = in.u16();
    const int rows = in.u16();
    const std::uint32_t byteCount = in.u32();
    if (!in.ok()) {
        errorOut = "the map grid header is truncated";
        return false;
    }
    // The arena and the maze have no grid: they are generated, and the client
    // builds them from the same constants and day number the server did. The
    // payload for one is an empty grid, and there is nothing to install.
    if (!isWorldRealm(realm)) {
        if (cols != 0 || rows != 0 || byteCount != 0) {
            errorOut = "a generated realm arrived with a tile grid";
            return false;
        }
        realmOut = realm;
        return true;
    }
    // Bounded before anything is allocated: these numbers came off a socket,
    // and a corrupt header must cost a refused message rather than a gigabyte.
    if (cols <= 0 || rows <= 0 || cols > kMaxTilesPerAxis || rows > kMaxTilesPerAxis) {
        errorOut = "the map grid is " + std::to_string(cols) + "x" + std::to_string(rows) +
                   ", which is not a size a map can be";
        return false;
    }
    const std::size_t expected = static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows);

    // Bounded before the read, for the same reason the dimensions are: the
    // byte count came off the same socket.
    if (byteCount > expected * 4 + 16) {
        errorOut = "the map grid claims more encoded bytes than a grid that size can hold";
        return false;
    }
    std::vector<std::uint8_t> packed;
    packed.reserve(byteCount);
    for (std::uint32_t i = 0; i < byteCount; ++i) packed.push_back(in.u8());
    if (!in.ok()) {
        errorOut = "the map grid's tile stream is truncated";
        return false;
    }
    std::vector<std::uint8_t> tiles;
    if (!decodeTileRle(packed.data(), packed.size(), expected, tiles, errorOut)) return false;

    if (!terrain.setTiles(tiles, cols, rows, realm)) {
        errorOut = "the map grid could not be installed";
        return false;
    }
    realmOut = realm;
    return true;
}

bool Terrain::loadWorldMap(const std::string& path, std::string& errorOut, Realm realm) {
    return loadTiledMap(path, errorOut, realm);
}

bool Terrain::loadTiledMap(const std::string& path, std::string& errorOut, Realm realm) {
    TiledMap map;
    if (!map.load(path, errorOut)) return false;
    // Dimensions come from the FILE. What is still refused is a map bigger
    // than the engine will hold -- see kMaxTilesPerAxis -- because past that
    // the grid stops fitting on the wire and the DDA's step bound stops being
    // long enough to cross it.
    if (map.width() <= 0 || map.height() <= 0 ||
        map.width() > kMaxTilesPerAxis || map.height() > kMaxTilesPerAxis) {
        errorOut = path + " is " + std::to_string(map.width()) + "x" + std::to_string(map.height()) +
                   " tiles; a map must be between 1 and " + std::to_string(kMaxTilesPerAxis) +
                   " tiles on each axis";
        return false;
    }
    // The grid is DERIVED, not painted, and the SHAPES beside it are what a
    // body actually collides with: tiled_map.h folds every colliding layer of
    // the map down to one Tile per cell for the coarse view, and this class
    // keeps the authored shapes per realm for the exact one. Only the grid ever
    // goes on the wire; the artwork and the shapes stay in the map file, which
    // the client reads for itself.
    if (!setTiles(map.tiles(), map.width(), map.height(), realm)) {
        errorOut = "could not install the tile grid from " + path;
        return false;
    }
    // The dimensions just came from this same map, so this cannot fail; the
    // check is here because a silent fallback to whole-cell collision is
    // exactly the sort of thing that would be found months later on a slope.
    if (!setCollisionShapes(map, realm)) {
        errorOut = "could not install the collision shapes from " + path;
        return false;
    }

    // WHAT THE COLLISION RULE RESOLVED TO, once per map, at load.
    //
    // A LAYER decides which cells can collide and the TILE's shapes decide
    // where inside them (shared/game/tiled_map.h). Both halves are invisible in
    // the editor -- a tick box in the layer panel, and a shape drawn on a tile
    // in a different window -- so an author who ticks the wrong box, or paints a
    // tile they never drew a shape on, should read it here rather than discover
    // it by walking through a castle.
    std::string collides;
    std::string scenery;
    for (const TiledLayer& layer : map.layers()) {
        std::string& list = layer.collides ? collides : scenery;
        if (!list.empty()) list += ", ";
        list += layer.name;
    }
    std::fprintf(stderr,
                 "[map] %s: collision from %s; scenery %s; %d wall, %d water, %d ground cells; "
                 "%d shape sets over %d shaped cells\n",
                 path.c_str(), collides.empty() ? "no layer" : collides.c_str(),
                 scenery.empty() ? "(none)" : scenery.c_str(), map.wallCells(), map.waterCells(),
                 map.groundCells(), collisionShapeSetCount(realm),
                 collisionShapeCellCount(realm));
    // A shape that leaves its tile still collides -- it is filed in every cell
    // it reaches into -- but it is nearly always a slip of the mouse in Tiled's
    // Tile Collision Editor rather than a decision, and nothing in the editor
    // shows it. Reported, with the distance, so it can be put back.
    if (collisionOverhangUnits(realm) > 0.0) {
        std::fprintf(stderr,
                     "[map] %s: a collision shape reaches %.3f world units outside its own tile; "
                     "it still blocks, in every cell it reaches, but check it was meant\n",
                     path.c_str(), collisionOverhangUnits(realm));
    }
    // A cell on a colliding layer whose tile carries no shape blocks NOTHING.
    // Legal -- a walkable footpath painted onto the dirt layer is exactly this
    // -- and the one way a map can look solid in the editor and be walkable in
    // the game, so it is a warning with the tiles named.
    if (map.unshapedBlockingCells() > 0) {
        std::string names;
        for (const std::string& name : map.unshapedBlockingTiles()) {
            if (!names.empty()) names += ", ";
            names += name;
        }
        std::fprintf(stderr,
                     "[map] WARNING %s: %d cells on a colliding layer use a tile with no "
                     "collision shape and so block nothing (%s); draw shapes on them in Tiled's "
                     "Tile Collision Editor, or move them to a layer that does not collide\n",
                     path.c_str(), map.unshapedBlockingCells(), names.c_str());
    }
    // A map nobody ticked a box on is walkable everywhere, boundary wall
    // included. Legal, and almost certainly not meant.
    if (collides.empty()) {
        std::fprintf(stderr, "[map] %s: no layer has \"%s\" set, so nothing on it blocks\n",
                     path.c_str(), kLayerCollisionProperty);
    }
    // Water art painted only where it cannot block: the editor shows a river
    // and the game gives you grass. Reported, not corrected -- the layer rule
    // wins, and the map needs the tiles moved onto a colliding layer.
    for (const std::string& name : map.strandedWaterTiles()) {
        std::fprintf(stderr, "[map] %s: tile \"%s\" is tagged water but is painted only on "
                             "layers that do not collide; those cells are plain ground\n",
                     path.c_str(), name.c_str());
    }
    seed_ = 0;   // an authored map, not a generated one
    return true;
}

// ---------------------------------------------------------------------------
// The authored collision shapes
// ---------------------------------------------------------------------------

bool Terrain::hasCollisionShapes(Realm realm) const { return !shapeGrid(realm).empty(); }

int Terrain::collisionShapeSetCount(Realm realm) const {
    return static_cast<int>(shapeGrid(realm).sets.size());
}

int Terrain::collisionShapeCellCount(Realm realm) const {
    return shapeGrid(realm).cellsWithShapes;
}

double Terrain::collisionOverhangUnits(Realm realm) const {
    return shapeGrid(realm).overhangUnits;
}

void Terrain::clearCollisionShapes(Realm realm) {
    ShapeGrid& store = shapeGrid(realm);
    if (store.cols == 0 && store.rows == 0 && store.refs.empty() && store.sets.empty()) return;
    store = ShapeGrid();
}

bool Terrain::setCollisionShapes(const TiledMap& map, Realm realm) {
    if (!isWorldRealm(realm)) return false;
    const Grid& g = grid(realm);
    // The store is indexed by the grid's dimensions, so it has to BE the same
    // map. A mismatch leaves whole-cell collision in place rather than
    // installing geometry for somewhere else.
    if (map.width() <= 0 || map.height() <= 0) return false;
    if (g.cols != map.width() || g.rows != map.height()) return false;

    const std::size_t cellCount =
        static_cast<std::size_t>(map.width()) * static_cast<std::size_t>(map.height());
    ShapeGrid store;
    store.cols = map.width();
    store.rows = map.height();

    // One set per (tile, orientation) the map actually paints on a colliding
    // layer, built the first time that pair is seen: at most eight per tile in
    // the tileset, and 86 for the shipped garden. Every cell painted with that
    // pair then shares one already-transformed ring.
    std::unordered_map<std::uint64_t, std::uint32_t> setOfPair;
    // The cells each set reaches, in step with store.sets. Almost always the
    // owning cell alone; see ShapeReach.
    std::vector<ShapeReach> reachOfSet;
    const auto setFor = [&](const TiledCell& cell, const TiledTileType& type) -> std::uint32_t {
        const std::uint8_t flags = cell.flags & 7u;
        const std::uint64_t key =
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cell.type)) << 3) | flags;
        const auto found = setOfPair.find(key);
        if (found != setOfPair.end()) return found->second;
        CollisionShapeSet built;
        bool first = true;
        for (const TiledShape& turned : orientTileShapes(type.shapes, flags)) {
            CollisionShape shape;
            shape.points = turned.points;
            shape.bounds = ringBounds(shape.points);
            shape.rectangle = ringIsItsBox(shape.points, shape.bounds);
            built.bounds = first ? shape.bounds : unionRect(built.bounds, shape.bounds);
            first = false;
            built.shapes.push_back(std::move(shape));
        }
        const std::uint32_t at = static_cast<std::uint32_t>(store.sets.size());
        reachOfSet.push_back(shapeReach(built.bounds));
        store.sets.push_back(std::move(built));
        setOfPair.emplace(key, at);
        return at;
    };

    // Two passes over the layers: count the refs per cell, then fill them. The
    // second pass runs bottom layer first, so a cell's refs come out in LAYER
    // ORDER and the last one to contain a point is the topmost -- which is what
    // decides whether that point is water or wall. Two cells of ONE layer can
    // both reach a third through an overhang; they carry the same layer number,
    // so which of them names the kind is unspecified, exactly as it is for two
    // shapes of one tile.
    //
    // `visit` is handed the cell the ref goes IN and the offset back to the
    // cell that owns the geometry, which is (0, 0) for every shape drawn inside
    // its tile.
    std::vector<std::uint32_t> counts(cellCount + 1, 0);
    const auto eachContributingCell = [&](const auto& visit) {
        std::size_t layerIndex = 0;
        for (const TiledLayer& layer : map.layers()) {
            // A ref's layer is one byte, and it is only ever compared: a map
            // with more than 255 layers has its deepest ones share a number
            // rather than wrapping round to the bottom of the stack.
            const std::uint8_t thisLayer =
                static_cast<std::uint8_t>(std::min<std::size_t>(layerIndex++, 255));
            if (!layer.collides) continue;
            for (std::size_t i = 0; i < cellCount && i < layer.cells.size(); ++i) {
                const TiledCell& cell = layer.cells[i];
                if (cell.type < 0) continue;
                const std::size_t typeIndex = static_cast<std::size_t>(cell.type);
                if (typeIndex >= map.palette().size()) continue;
                const TiledTileType& type = map.palette()[typeIndex];
                if (type.shapes.empty()) continue;   // contributes nothing; see tiled_map.h
                const std::uint32_t set = setFor(cell, type);
                const ShapeReach& reach = reachOfSet[set];
                if (reach.ownCellOnly()) {
                    visit(i, set, type, thisLayer, 0, 0);
                    continue;
                }
                const int tx = static_cast<int>(i % static_cast<std::size_t>(store.cols));
                const int ty = static_cast<int>(i / static_cast<std::size_t>(store.cols));
                for (int dy = reach.dyMin; dy <= reach.dyMax; ++dy) {
                    for (int dx = reach.dxMin; dx <= reach.dxMax; ++dx) {
                        const int nx = tx + dx;
                        const int ny = ty + dy;
                        if (nx < 0 || ny < 0 || nx >= store.cols || ny >= store.rows) continue;
                        visit(static_cast<std::size_t>(ny) * static_cast<std::size_t>(store.cols) +
                                  static_cast<std::size_t>(nx),
                              set, type, thisLayer, -dx, -dy);
                    }
                }
            }
        }
    };
    eachContributingCell([&](std::size_t i, std::uint32_t, const TiledTileType&, std::uint8_t, int,
                             int) { ++counts[i]; });
    store.firstRef.assign(cellCount + 1, 0);
    std::uint32_t total = 0;
    for (std::size_t i = 0; i < cellCount; ++i) {
        store.firstRef[i] = total;
        if (counts[i] != 0) ++store.cellsWithShapes;
        total += counts[i];
    }
    store.firstRef[cellCount] = total;
    store.refs.assign(total, ShapeGrid::Ref());
    std::vector<std::uint32_t> cursor(store.firstRef.begin(), store.firstRef.end() - 1);
    eachContributingCell([&](std::size_t i, std::uint32_t set, const TiledTileType& type,
                             std::uint8_t layerIndex, int dx, int dy) {
        ShapeGrid::Ref ref;
        ref.set = set;
        ref.dx = static_cast<std::int16_t>(dx);
        ref.dy = static_cast<std::int16_t>(dy);
        ref.layer = layerIndex;
        ref.water = type.water;
        store.refs[cursor[i]++] = ref;
    });

    // How far any shape reaches outside its own cell, in world units. Zero for
    // every shape drawn inside its tile. Nothing in a query uses it -- the refs
    // above already reach every cell a shape touches -- but an author who
    // nudged a vertex a fraction of a unit past a tile edge cannot see that in
    // Tiled, so the load report says it.
    double overhang = 0.0;
    for (const CollisionShapeSet& set : store.sets) {
        if (set.shapes.empty()) continue;
        overhang = std::max(overhang, -set.bounds.left());
        overhang = std::max(overhang, -set.bounds.top());
        overhang = std::max(overhang, set.bounds.right() - kTileSize);
        overhang = std::max(overhang, set.bounds.bottom() - kTileSize);
    }
    store.overhangUnits = std::max(0.0, overhang);

    shapeGrid(realm) = std::move(store);
    return true;
}

bool Terrain::loadCollisionShapes(const std::string& path, std::string& errorOut, Realm realm) {
    TiledMap map;
    if (!map.load(path, errorOut)) return false;
    if (!setCollisionShapes(map, realm)) {
        errorOut = path + " is " + std::to_string(map.width()) + "x" +
                   std::to_string(map.height()) + " cells, but realm " +
                   std::to_string(realmIndex(realm)) + " holds a " +
                   std::to_string(tileCols(realm)) + "x" + std::to_string(tileRows(realm)) +
                   " grid; its collision stays whole-cell";
        return false;
    }
    return true;
}

bool Terrain::setTiles(const std::vector<std::uint8_t>& tiles, int cols, int rows, Realm realm) {
    if (!install(realm, tiles, cols, rows)) return false;
    Grid& g = grid(realm);
    // The connectivity root is only meaningful for the overworld, which is the
    // one grid generate() and chooseGardenSpawn() know how to reason about. On
    // any other map the nearest open tile to the middle is the honest answer,
    // and it is only ever a fallback for a caller with nowhere better to go.
    g.spawnTile = realm == Realm::Overworld && cols == kAxis && rows == kAxis
                      ? chooseGardenSpawn()
                      : index(g, cols / 2, rows / 2);
    if (!tileBlocks(atTile(g.spawnTile % g.cols, g.spawnTile / g.cols, realm))) return true;
    // A map whose middle is solid is perfectly legal -- a cave level starts
    // inside rock, and now that a cell blocks where its SHAPES are, an authored
    // map's middle cell is solid far more often than it used to be. Take the
    // nearest open tile instead of refusing the map. For any realm: this
    // fallback used to be reserved for the non-overworld realms, which refused
    // every small overworld map whose centre cell happened to be painted.
    int tx = 0;
    int ty = 0;
    if (nearestOpenTile(tileCenter(g.cols / 2, g.rows / 2), tx, ty, realm)) {
        g.spawnTile = index(g, tx, ty);
        return true;
    }
    // No open CELL anywhere, which no longer means there is nowhere to stand:
    // a cell counts as blocking when it holds any shape at all, and a map whose
    // every cell holds one -- a small map floored entirely with dirt edges -- is
    // mostly walkable. So the map loads, with the middle as the last-resort
    // spawn; the doors on its object layers are what actually place a player.
    std::fprintf(stderr,
                 "[map] every cell of this %dx%d grid holds a blocking shape; spawnPoint() "
                 "falls back to the middle of it\n", cols, rows);
    return true;
}

void Terrain::generateSections(Rng& rng) {
    // The procedural map is the OVERWORLD's, and only ever was: it is built
    // out of the nine sections, which are a property of the default world's
    // dimensions. Every other realm is authored.
    Grid& g = grid(Realm::Overworld);
    const NoiseSet noise(rng);
    for (int ty = 0; ty < g.rows; ++ty) {
        for (int tx = 0; tx < g.cols; ++tx) {
            const int section = flix::sectionAt(tileCenter(tx, ty));
            g.tiles[static_cast<std::size_t>(index(g, tx, ty))] =
                static_cast<std::uint8_t>(classifyTile(section, tx, ty, noise));
        }
    }
}

/// An open tile near the middle of the Garden section, searched outward so the
/// result is the closest walkable spot to the section's centre rather than the
/// first one in scan order.
int Terrain::chooseGardenSpawn() const {
    const Grid& g = grid(Realm::Overworld);
    const int perSection = g.cols / kSectionsPerAxis;
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
                if (atTile(tx, ty) == Tile::Ground) return index(g, tx, ty);
            }
        }
    }
    // The Garden is noise-generated and always has ground, but if it somehow
    // did not, the centre is still a defined tile and connectAll() will open it.
    return index(g, centreTx, centreTy);
}

void Terrain::carveDisc(Vec2 center, double radius, Tile t) {
    const Grid& g = grid(Realm::Overworld);
    const int x0 = clamp(toTileCoord(center.x - radius), 0, g.cols - 1);
    const int x1 = clamp(toTileCoord(center.x + radius), 0, g.cols - 1);
    const int y0 = clamp(toTileCoord(center.y - radius), 0, g.rows - 1);
    const int y1 = clamp(toTileCoord(center.y + radius), 0, g.rows - 1);
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
    const Grid& g = grid(Realm::Overworld);
    for (int cellY = 0; cellY + 9 <= g.rows; cellY += 9) {
        for (int cellX = 0; cellX + 9 <= g.cols; cellX += 9) {
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

int Terrain::openTileCount(Realm realm) const {
    const Grid& g = grid(realm);
    int n = 0;
    for (std::size_t i = 0; i < g.tiles.size(); ++i) {
        if (passableIndex(g, static_cast<int>(i))) ++n;
    }
    return n;
}

bool Terrain::isConnected(Realm realm) const {
    const Grid& g = grid(realm);
    if (g.tiles.empty()) return true;
    const int total = static_cast<int>(g.tiles.size());
    const int open = openTileCount(realm);
    if (!passableIndex(g, g.spawnTile)) return open == 0;

    std::vector<std::uint8_t> seen(static_cast<std::size_t>(total), 0);
    std::vector<int> stack;
    stack.reserve(256);
    stack.push_back(g.spawnTile);
    seen[static_cast<std::size_t>(g.spawnTile)] = 1;
    int reached = 0;
    while (!stack.empty()) {
        const int cur = stack.back();
        stack.pop_back();
        ++reached;
        const int cx = cur % g.cols;
        const int cy = cur / g.cols;
        for (int d = 0; d < 4; ++d) {
            const int nx = cx + kNeighborDx[d];
            const int ny = cy + kNeighborDy[d];
            if (nx < 0 || ny < 0 || nx >= g.cols || ny >= g.rows) continue;
            const int ni = index(g, nx, ny);
            if (seen[static_cast<std::size_t>(ni)] || !passableIndex(g, ni)) continue;
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
    //
    // The generated map is the overworld's, so this repairs that grid alone.
    Grid& g = grid(Realm::Overworld);
    const std::int32_t total = static_cast<std::int32_t>(g.tiles.size());
    constexpr std::int32_t kUnreached = std::numeric_limits<std::int32_t>::max();
    std::vector<std::int32_t> dist(static_cast<std::size_t>(total), kUnreached);
    std::vector<std::int32_t> parent(static_cast<std::size_t>(total), -1);
    std::vector<std::uint8_t> settled(static_cast<std::size_t>(total), 0);

    std::deque<std::int32_t> queue;
    dist[static_cast<std::size_t>(g.spawnTile)] = 0;
    queue.push_back(g.spawnTile);
    while (!queue.empty()) {
        const std::int32_t cur = queue.front();
        queue.pop_front();
        if (settled[static_cast<std::size_t>(cur)]) continue;
        settled[static_cast<std::size_t>(cur)] = 1;
        const int cx = cur % g.cols;
        const int cy = cur / g.cols;
        for (int d = 0; d < 4; ++d) {
            const int nx = cx + kNeighborDx[d];
            const int ny = cy + kNeighborDy[d];
            if (nx < 0 || ny < 0 || nx >= g.cols || ny >= g.rows) continue;
            const std::int32_t ni = index(g, nx, ny);
            if (settled[static_cast<std::size_t>(ni)]) continue;
            const std::int32_t cost = passableIndex(g, ni) ? 0 : 1;
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
    for (std::int32_t t = 0; t < total; ++t) {
        if (!passableIndex(g, t) || dist[static_cast<std::size_t>(t)] == 0) continue;

        for (std::int32_t cur = t; cur >= 0 && dist[static_cast<std::size_t>(cur)] != 0;
             cur = parent[static_cast<std::size_t>(cur)]) {
            if (!passableIndex(g, cur)) {
                g.tiles[static_cast<std::size_t>(cur)] = static_cast<std::uint8_t>(Tile::Ground);
            }
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
            const int cx = cur % g.cols;
            const int cy = cur / g.cols;
            for (int d = 0; d < 4; ++d) {
                const int nx = cx + kNeighborDx[d];
                const int ny = cy + kNeighborDy[d];
                if (nx < 0 || ny < 0 || nx >= g.cols || ny >= g.rows) continue;
                const std::int32_t ni = index(g, nx, ny);
                if (dist[static_cast<std::size_t>(ni)] == 0 || !passableIndex(g, ni)) continue;
                dist[static_cast<std::size_t>(ni)] = 0;
                stack.push_back(ni);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Collision
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The exact tests, one cell at a time
// ---------------------------------------------------------------------------
//
// Each of the three below is asked about ONE IN-GRID CELL and answers from that
// cell's authored shapes, or -- when the cell has none -- from its coarse Tile
// over the whole 300-unit square. Which of the two is in play is a property of
// the cell, not of the map: a map may perfectly well have shapes on some cells
// and none on others, and mixing them is only ever conservative.

int Terrain::cellLayerAt(int tx, int ty, Vec2 p, Realm realm, bool& water) const {
    const Grid& g = grid(realm);
    const ShapeGrid& store = shapeGrid(realm);
    const Vec2 base{p.x - tx * kTileSize, p.y - ty * kTileSize};
    int best = -1;
    if (store.cols == g.cols && store.rows == g.rows && !store.firstRef.empty()) {
        const std::size_t cell = static_cast<std::size_t>(index(g, tx, ty));
        const std::uint32_t from = store.firstRef[cell];
        const std::uint32_t to = store.firstRef[cell + 1];
        for (std::uint32_t i = from; i < to; ++i) {
            const ShapeGrid::Ref& ref = store.refs[i];
            const CollisionShapeSet& set = store.sets[ref.set];
            // A ref's geometry is in ITS OWN cell's coordinates, which is this
            // cell for all but an overhanging shape. See ShapeGrid::Ref.
            const Vec2 local = ref.dx == 0 && ref.dy == 0
                                   ? base
                                   : Vec2{base.x - ref.dx * kTileSize, base.y - ref.dy * kTileSize};
            if (!rectHolds(set.bounds, local)) continue;
            for (const CollisionShape& shape : set.shapes) {
                if (!rectHolds(shape.bounds, local)) continue;
                // An axis-aligned rectangle IS its box, so the reject just
                // above was the containment test.
                if (!shape.rectangle &&
                    !pointInRing(Ring{shape.points.data(), shape.points.size()}, local)) {
                    continue;
                }
                // Refs are in layer order, so this is a running maximum; the
                // topmost shape containing the point names the KIND.
                if (static_cast<int>(ref.layer) >= best) {
                    best = static_cast<int>(ref.layer);
                    water = ref.water;
                }
                break;
            }
        }
        // The cell HAS authored shapes: they are the whole answer, including
        // when the point is in none of them.
        if (from != to) return best;
    }
    // The whole-cell fallback answers for THIS CELL'S 300-unit square, and only
    // for points in it. Its two siblings below get that for free -- they test a
    // segment or a circle against the square itself, which IS the containment
    // test -- and this one has to say so. No caller violates it today, but one
    // did: a neighbourhood scan that handed each cell around a point the point
    // in the middle turned one shape-less blocking cell (a client's wire grid, a
    // test's setTile) into a solid 3x3 of open ground.
    if (base.x < 0.0 || base.y < 0.0 || base.x > kTileSize || base.y > kTileSize) return -1;
    const Tile tile = atTile(tx, ty, realm);
    if (!tileBlocks(tile)) return -1;
    water = tileIsWater(tile);
    return 0;
}

bool Terrain::cellTouchesSegment(int tx, int ty, Vec2 a, Vec2 b, double eps, Realm realm) const {
    const Grid& g = grid(realm);
    const ShapeGrid& store = shapeGrid(realm);
    const Vec2 origin{tx * kTileSize, ty * kTileSize};
    const Vec2 localA = a - origin;
    const Vec2 localB = b - origin;
    const double left = std::min(localA.x, localB.x) - eps;
    const double right = std::max(localA.x, localB.x) + eps;
    const double top = std::min(localA.y, localB.y) - eps;
    const double bottom = std::max(localA.y, localB.y) + eps;
    if (store.cols == g.cols && store.rows == g.rows && !store.firstRef.empty()) {
        const std::size_t cell = static_cast<std::size_t>(index(g, tx, ty));
        const std::uint32_t from = store.firstRef[cell];
        const std::uint32_t to = store.firstRef[cell + 1];
        for (std::uint32_t i = from; i < to; ++i) {
            const ShapeGrid::Ref& ref = store.refs[i];
            const CollisionShapeSet& set = store.sets[ref.set];
            // Into the owning cell's coordinates; see ShapeGrid::Ref.
            const Vec2 shift{ref.dx * kTileSize, ref.dy * kTileSize};
            const Vec2 refA{localA.x - shift.x, localA.y - shift.y};
            const Vec2 refB{localB.x - shift.x, localB.y - shift.y};
            if (!rectOverlaps(set.bounds, left - shift.x, top - shift.y, right - shift.x,
                              bottom - shift.y)) {
                continue;
            }
            for (const CollisionShape& shape : set.shapes) {
                if (!rectOverlaps(shape.bounds, left - shift.x, top - shift.y, right - shift.x,
                                  bottom - shift.y)) {
                    continue;
                }
                if (ringTouchesSegment(Ring{shape.points.data(), shape.points.size()}, refA, refB,
                                       eps)) {
                    return true;
                }
            }
        }
        if (from != to) return false;
    }
    if (!tileBlocks(atTile(tx, ty, realm))) return false;
    const std::array<Vec2, 4> square = wholeCellRing();
    return ringTouchesSegment(Ring{square.data(), square.size()}, localA, localB, eps);
}

bool Terrain::cellPushCircle(int tx, int ty, Vec2 p, double radius, Realm realm, Vec2& pushed,
                             bool& flat) const {
    const Grid& g = grid(realm);
    const ShapeGrid& store = shapeGrid(realm);
    const Vec2 origin{tx * kTileSize, ty * kTileSize};
    const Vec2 local = p - origin;
    const double reach = radius;
    std::optional<RingPush> corner;
    if (store.cols == g.cols && store.rows == g.rows && !store.firstRef.empty()) {
        const std::size_t cell = static_cast<std::size_t>(index(g, tx, ty));
        const std::uint32_t from = store.firstRef[cell];
        const std::uint32_t to = store.firstRef[cell + 1];
        for (std::uint32_t i = from; i < to; ++i) {
            const ShapeGrid::Ref& ref = store.refs[i];
            const CollisionShapeSet& set = store.sets[ref.set];
            // Into the owning cell's coordinates, and back out again for the
            // answer; see ShapeGrid::Ref.
            const Vec2 shift{ref.dx * kTileSize, ref.dy * kTileSize};
            const Vec2 refLocal{local.x - shift.x, local.y - shift.y};
            if (!rectOverlaps(set.bounds, refLocal.x - reach, refLocal.y - reach,
                              refLocal.x + reach, refLocal.y + reach)) {
                continue;
            }
            for (const CollisionShape& shape : set.shapes) {
                if (!rectOverlaps(shape.bounds, refLocal.x - reach, refLocal.y - reach,
                                  refLocal.x + reach, refLocal.y + reach)) {
                    continue;
                }
                const std::optional<RingPush> push =
                    pushOutOfRing(Ring{shape.points.data(), shape.points.size()}, refLocal, radius);
                if (!push) continue;
                // A face hit ends the search; a corner hit is only taken if no
                // face hit turns up. See findCollision.
                if (push->flat) {
                    pushed = push->position + origin + shift;
                    flat = true;
                    return true;
                }
                if (!corner) corner = RingPush{push->position + shift, push->flat};
            }
        }
        if (from != to) {
            if (!corner) return false;
            pushed = corner->position + origin;
            flat = false;
            return true;
        }
    }
    if (!tileBlocks(atTile(tx, ty, realm))) return false;
    const std::array<Vec2, 4> square = wholeCellRing();
    const std::optional<RingPush> push =
        pushOutOfRing(Ring{square.data(), square.size()}, local, radius);
    if (!push) return false;
    pushed = push->position + origin;
    flat = push->flat;
    return true;
}

int Terrain::blockingLayerAt(Vec2 p, Realm realm, bool& water, bool outsideBlocks) const {
    water = false;
    const Grid& g = grid(realm);
    const int tx = toTileCoord(p.x);
    const int ty = toTileCoord(p.y);
    if (tx < 0 || ty < 0 || tx >= g.cols || ty >= g.rows) {
        // Off the grid. Wall everywhere for gameplay -- which is what closes the
        // world without a single caller bounds-checking -- and air for the
        // sight test, whose reference simply has no grid entry out there.
        return outsideBlocks ? 0 : -1;
    }
    // ONE cell. A shape that leaves its tile is filed in every cell it touches
    // (ShapeGrid), so the cell a point is in is the only cell that can hold
    // geometry containing it.
    return cellLayerAt(tx, ty, p, realm, water);
}

std::optional<Terrain::ShapeCollision> Terrain::findCollision(Vec2 position, double radius,
                                                              Realm realm) const {
    const double reach = radius + kCollisionScanBuffer;
    const int minX = std::max(0, toTileCoord(position.x - reach));
    const int maxX = std::min(tileCols(realm) - 1, toTileCoord(position.x + reach));
    const int minY = std::max(0, toTileCoord(position.y - reach));
    const int maxY = std::min(tileRows(realm) - 1, toTileCoord(position.y + reach));
    std::optional<ShapeCollision> corner;
    for (int tileY = minY; tileY <= maxY; ++tileY) {
        for (int tileX = minX; tileX <= maxX; ++tileX) {
            Vec2 pushed;
            bool flat = false;
            if (!cellPushCircle(tileX, tileY, position, radius, realm, pushed, flat)) continue;
            // Prefer a face hit over the seam between two cells of one wall, so
            // a body sliding along it is pushed straight off the face.
            if (flat) return ShapeCollision{pushed, true};
            if (!corner) corner = ShapeCollision{pushed, false};
        }
    }
    return corner;
}

bool Terrain::nearestOpenTile(Vec2 p, int& outTx, int& outTy, Realm realm) const {
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
                if (tileBlocks(atTile(tx, ty, realm))) continue;
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
        position = realm == Realm::Arena ? kArenaSpawn
                 : realm == Realm::Maze  ? activeMaze().spawn()
                                         : spawnPoint(realm);
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
    // the tile loop below run for the rest of the universe. The bound is this
    // realm's own rectangle, so a small map's scan stays small.
    const Vec2 extent = realmExtent(realm);
    position.x = clamp(position.x, -kTileSize, extent.x + kTileSize);
    position.y = clamp(position.y, -kTileSize, extent.y + kTileSize);

    // This is the same four-pass collision solver used by
    // resolveEntityWallCollisions() in constants.ts, now against the SHAPES the
    // author drew on each blocking cell's tile rather than against the cell's
    // square: a body stops where the art stops, which on a diagonal edge is
    // most of a cell away from where the square would have stopped it.
    bool cleared = true;
    for (int pass = 0; pass < kResolvePasses; ++pass) {
        const std::optional<ShapeCollision> hit = findCollision(position, radius, realm);
        if (!hit) {
            cleared = true;
            break;
        }
        position = hit->position;
        result.collided = true;
        cleared = false;
    }

    // All four passes pushed, so the last push was never re-checked. This one
    // extra check -- reached only on deep multi-tile overlap, never on
    // ordinary wall contact -- is what decides whether the body actually came
    // out clear, and it is the only thing `unresolved` says.
    if (!cleared) cleared = !findCollision(position, radius, realm);

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
    if (!isWorldRealm(realm)) return position;

    // Spawners and admin teleports can place a centre deep inside several
    // blocking shapes. TypeScript's per-movement caller refuses an unresolved
    // four-pass result, but these non-movement callers need a usable point.
    // Fall back only when the exact solver is still embedded; ordinary contact
    // and sliding keep the TypeScript result above.
    if (wall.unresolved) {
        int tx = 0;
        int ty = 0;
        if (!nearestOpenTile(position, tx, ty, realm)) position = spawnPoint(realm);
        else {
            const Rect open = tileRect(tx, ty);
            const double inset = std::min(radius + kWallResolveEpsilon, kTileSize * 0.49);
            position.x = clamp(position.x, open.left() + inset, open.right() - inset);
            position.y = clamp(position.y, open.top() + inset, open.bottom() - inset);
        }
        for (int pass = 0; pass < kResolvePasses; ++pass) {
            const std::optional<ShapeCollision> hit = findCollision(position, radius, realm);
            if (!hit) break;
            position = hit->position;
        }
    }

    // Last-resort closure. The out-of-bounds-is-wall rule already keeps a body
    // inside; this makes it true even when the push-out could not converge.
    const Vec2 span = realmExtent(realm);
    position.x = clamp(position.x, std::min(radius, span.x * 0.25), span.x - std::min(radius, span.x * 0.25));
    position.y = clamp(position.y, std::min(radius, span.y * 0.25), span.y - std::min(radius, span.y * 0.25));
    return position;
}

bool Terrain::blocked(Vec2 p, Realm realm) const {
    if (realm == Realm::Maze) return activeMaze().blocksPoint(p);
    if (realm == Realm::Arena) return !insideArena(p);
    bool water = false;
    return blockingLayerAt(p, realm, water) >= 0;
}

bool Terrain::inWater(Vec2 p, Realm realm) const {
    if (!isWorldRealm(realm)) return false;
    bool water = false;
    // The TOPMOST shape containing the point names the kind, so a bridge drawn
    // over a pond is a bridge. Off the grid is wall, and wall is not water.
    return blockingLayerAt(p, realm, water) >= 0 && water;
}

Vec2 Terrain::realmExtent(Realm realm) const {
    if (realm == Realm::Maze) {
        const double side = activeMaze().worldSize();
        return {side, side};
    }
    if (realm == Realm::Arena) return {kArenaWorldSize, kArenaWorldSize};
    const Grid& g = grid(realm);
    // A realm with no map staged still has to answer with something finite --
    // a clamp against zero would collapse every coordinate onto the origin.
    if (g.cols <= 0 || g.rows <= 0) return {kWorldSize, kWorldSize};
    return {g.cols * kTileSize, g.rows * kTileSize};
}

double Terrain::realmSize(Realm realm) const {
    const Vec2 extent = realmExtent(realm);
    return std::max(extent.x, extent.y);
}

Vec2 Terrain::clampInside(Vec2 p, double radius, Realm realm) const {
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
    // Per axis, because a map need not be square: clamping both axes against
    // the longer one would let a body walk off the short end of a corridor
    // level and stand in the void beside it.
    const Vec2 extent = realmExtent(realm);
    const double marginX = std::min(radius, extent.x * 0.25);
    const double marginY = std::min(radius, extent.y * 0.25);
    return {clamp(p.x, marginX, extent.x - marginX), clamp(p.y, marginY, extent.y - marginY)};
}

bool Terrain::outside(Vec2 p, Realm realm) const {
    if (realm == Realm::Arena) return !insideArena(p);
    const Vec2 extent = realmExtent(realm);
    return p.x < 0.0 || p.x >= extent.x || p.y < 0.0 || p.y >= extent.y;
}

bool Terrain::segmentBlocked(Vec2 a, Vec2 b, Realm realm) const {
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y)) {
        return true;
    }
    if (realm == Realm::Maze) return activeMaze().blocksLine(a, b);
    if (realm == Realm::Arena) return false;   // open floor, edge to edge

    // The DDA visits every cell the segment passes through and asks each for
    // the shapes filed in it. A cell it does not visit cannot hold geometry the
    // segment touches: a shape that leaves its tile is filed in every cell it
    // reaches into (ShapeGrid), so "filed in" is not "painted in".
    const auto crosses = [&](int cx, int cy) {
        if (cx < 0 || cy < 0 || cx >= tileCols(realm) || cy >= tileRows(realm)) return true;
        return cellTouchesSegment(cx, cy, a, b, 0.0, realm);
    };

    int tx = toTileCoord(a.x);
    int ty = toTileCoord(a.y);
    if (crosses(tx, ty)) return true;

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
        if (crosses(tx, ty)) return true;
        if (tx == endTx && ty == endTy) return false;
    }
    // Longer than twice the map: nonsense input, and unseeable is the safe
    // answer for every caller (line of sight, "can I walk straight there").
    return true;
}

bool Terrain::segmentTouchesBlockingTile(Vec2 a, Vec2 b, double eps, Realm realm) const {
    // Nonsense endpoints clip to an empty tile range in the reference, which
    // reports no crossing. Refusing a step on garbage input would be worse
    // than letting it through: the guard exists to stop a body moving where it
    // could not have walked, not to stop it moving at all.
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y)) {
        return false;
    }
    if (!std::isfinite(eps) || eps < 0.0) eps = 0.0;

    // Clamped to the grid, as the reference clamps its scan: tiles outside it
    // are air, so skipping them changes nothing and keeps the loop small. No
    // widening for an overhanging shape -- it is filed in every cell it reaches
    // into, so the box the segment covers is the box to ask.
    const int minTx = std::max(0, toTileCoord(std::min(a.x, b.x) - eps));
    const int maxTx = std::min(tileCols(realm) - 1, toTileCoord(std::max(a.x, b.x) + eps));
    const int minTy = std::max(0, toTileCoord(std::min(a.y, b.y) - eps));
    const int maxTy = std::min(tileRows(realm) - 1, toTileCoord(std::max(a.y, b.y) + eps));

    for (int ty = minTy; ty <= maxTy; ++ty) {
        for (int tx = minTx; tx <= maxTx; ++tx) {
            if (cellTouchesSegment(tx, ty, a, b, eps, realm)) return true;
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
        const Vec2 sample{a.x + dx * t, a.y + dy * t};
        // Outside the grid is AIR, not wall -- the reference's grid has no
        // entry out there, so leaving the map does not by itself break sight.
        // Each sample is a POINT against the shapes: a sample inside the notch
        // of a concave edge sees through it, which is what the art shows.
        bool water = false;
        if (blockingLayerAt(sample, realm, water, /*outsideBlocks=*/false) >= 0) return false;
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

    if (!std::isfinite(around.x) || !std::isfinite(around.y)) around = spawnPoint(realm);
    radius = std::isfinite(radius) ? clamp(radius, 0.0, realmSize(realm)) : 0.0;

    for (int attempt = 0; attempt < 24; ++attempt) {
        const Vec2 p = around + rng.insideCircle(radius);
        const Tile t = at(p, realm);
        if (tileBlocks(t) || tileIsWater(t)) continue;
        // Reject pockets a body would immediately be squeezed out of: landing
        // in a one-tile gap between boulders reads as spawning inside a wall.
        if (distanceSq(resolveCircle(p, kPlayerBaseRadius, realm), p) < 1.0) return p;
    }

    int tx = 0, ty = 0;
    if (nearestOpenTile(around, tx, ty, realm)) {
        // Jitter inside the tile so repeated fallbacks do not stack every mob
        // on one point.
        const Vec2 c = tileCenter(tx, ty);
        const double j = kTileSize * 0.25;
        return {c.x + rng.range(-j, j), c.y + rng.range(-j, j)};
    }
    return spawnPoint(realm);
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
