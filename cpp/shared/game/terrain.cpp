#include "shared/game/terrain.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <optional>
#include <string>

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
// Tile collision geometry
// ---------------------------------------------------------------------------
//
// A blocking tile is its plain 300-unit rectangle. What the cell is drawn with
// is the map file's artwork, fitted inside that square, so the rectangle is
// both the hitbox and the silhouette.

constexpr double kWallResolveEpsilon = 0.01;

/// One blocking tile the circle overlaps: its rectangle, and the offset from
/// the circle's centre to the nearest point of it.
struct TileCollision {
    double left = 0.0;
    double right = 0.0;
    double top = 0.0;
    double bottom = 0.0;
    double nearDx = 0.0;
    double nearDy = 0.0;
};

/// The tile a push-out should act on this pass, if any. A flat-face hit (the
/// centre is level with the tile on one axis) wins over a corner hit, so a
/// body sliding along a wall is pushed straight off its face and never off
/// the seam between two tiles of it.
std::optional<TileCollision> findTileCollision(const Terrain& terrain, Vec2 position,
                                               double radius, Realm realm) {
    const double reach = radius + kCollisionScanBuffer;
    const int minX = std::max(0, Terrain::toTileCoord(position.x - reach));
    const int maxX = std::min(terrain.tileCols(realm) - 1, Terrain::toTileCoord(position.x + reach));
    const int minY = std::max(0, Terrain::toTileCoord(position.y - reach));
    const int maxY = std::min(terrain.tileRows(realm) - 1, Terrain::toTileCoord(position.y + reach));
    std::optional<TileCollision> corner;

    for (int tileY = minY; tileY <= maxY; ++tileY) {
        for (int tileX = minX; tileX <= maxX; ++tileX) {
            if (!tileBlocks(terrain.atTile(tileX, tileY, realm))) continue;

            TileCollision hit;
            hit.left = tileX * kTileSize;
            hit.right = hit.left + kTileSize;
            hit.top = tileY * kTileSize;
            hit.bottom = hit.top + kTileSize;

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

/// Pushes the circle out of one tile: through the nearest face when the centre
/// is inside the rectangle (least-penetration ejection), straight off the face
/// when it is level with the tile on one axis, and radially off the corner
/// otherwise.
Vec2 resolveTileCollision(Vec2 position, double radius, const TileCollision& hit) {
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
}

bool Terrain::install(Realm realm, std::vector<std::uint8_t> tiles, int cols, int rows) {
    if (cols <= 0 || rows <= 0 || cols > kMaxTilesPerAxis || rows > kMaxTilesPerAxis) return false;
    if (tiles.size() != static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows)) return false;
    for (const std::uint8_t tile : tiles) {
        if (tile > static_cast<std::uint8_t>(Tile::Block)) return false;
    }
    Grid& g = grid(realm);
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
}

void Terrain::fill(Tile t, Realm realm) {
    Grid& g = grid(realm);
    std::fill(g.tiles.begin(), g.tiles.end(), static_cast<std::uint8_t>(t));
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
    // WHAT THE COLLISION RULE RESOLVED TO, once per map, at load.
    //
    // Collision is a property of the LAYER (shared/game/tiled_map.h): a layer
    // whose `has_collision` is ticked is a wall everywhere it has a tile, and
    // one without the property never blocks. That is one tick box per layer in
    // an editor that does not draw it, so an author who ticks the wrong box --
    // or forgets one -- should read it here rather than discover it by walking
    // through a castle.
    std::string collides;
    std::string scenery;
    for (const TiledLayer& layer : map.layers()) {
        std::string& list = layer.collides ? collides : scenery;
        if (!list.empty()) list += ", ";
        list += layer.name;
    }
    std::fprintf(stderr,
                 "[map] %s: collision from %s; scenery %s; %d wall, %d water, %d ground cells\n",
                 path.c_str(), collides.empty() ? "no layer" : collides.c_str(),
                 scenery.empty() ? "(none)" : scenery.c_str(), map.wallCells(), map.waterCells(),
                 map.groundCells());
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
    // The grid is DERIVED, not painted: tiled_map.h folds every layer of the
    // map down to one Tile per cell, and that is the only thing about a map
    // this class -- or the wire -- ever carries. The artwork stays in the map
    // file, where the client reads it.
    if (!setTiles(map.tiles(), map.width(), map.height(), realm)) {
        errorOut = "could not install the tile grid from " + path;
        return false;
    }
    seed_ = 0;   // an authored map, not a generated one
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
    // inside rock. Take the nearest open tile instead of refusing the map.
    int tx = 0;
    int ty = 0;
    if (realm != Realm::Overworld &&
        nearestOpenTile(tileCenter(g.cols / 2, g.rows / 2), tx, ty, realm)) {
        g.spawnTile = index(g, tx, ty);
        return true;
    }
    return false;
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
    // resolveEntityWallCollisions() in constants.ts, against each blocking
    // tile's plain rectangle: the edge a wall or water tile is drawn with lies
    // inside the tile, so the rectangle is where a body actually stops.
    bool cleared = true;
    for (int pass = 0; pass < kResolvePasses; ++pass) {
        const std::optional<TileCollision> hit = findTileCollision(*this, position, radius, realm);
        if (!hit) {
            cleared = true;
            break;
        }
        position = resolveTileCollision(position, radius, *hit);
        result.collided = true;
        cleared = false;
    }

    // All four passes pushed, so the last push was never re-checked. This one
    // extra check -- reached only on deep multi-tile overlap, never on
    // ordinary wall contact -- is what decides whether the body actually came
    // out clear, and it is the only thing `unresolved` says.
    if (!cleared) cleared = !findTileCollision(*this, position, radius, realm);

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
    // blocking tiles. TypeScript's per-movement caller refuses an unresolved
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
            const std::optional<TileCollision> hit =
                findTileCollision(*this, position, radius, realm);
            if (!hit) break;
            position = resolveTileCollision(position, radius, *hit);
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
    return tileBlocks(at(p, realm));
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

    int tx = toTileCoord(a.x);
    int ty = toTileCoord(a.y);
    if (tileBlocks(atTile(tx, ty, realm))) return true;

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
        if (tileBlocks(atTile(tx, ty, realm))) return true;
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
    // are air, so skipping them changes nothing and keeps the loop small.
    const int minTx = std::max(0, toTileCoord(std::min(a.x, b.x) - eps));
    const int maxTx = std::min(tileCols(realm) - 1, toTileCoord(std::max(a.x, b.x) + eps));
    const int minTy = std::max(0, toTileCoord(std::min(a.y, b.y) - eps));
    const int maxTy = std::min(tileRows(realm) - 1, toTileCoord(std::max(a.y, b.y) + eps));

    for (int ty = minTy; ty <= maxTy; ++ty) {
        for (int tx = minTx; tx <= maxTx; ++tx) {
            if (!tileBlocks(atTile(tx, ty, realm))) continue;
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
        if (tx < 0 || ty < 0 || tx >= tileCols(realm) || ty >= tileRows(realm)) continue;
        if (tileBlocks(atTile(tx, ty, realm))) return false;
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
