#pragma once
// The tile world.
//
// A 200x200 grid of Tile over the 60000-unit map; constants.h owns both
// numbers. Terrain answers three questions and nothing else: what is at a
// point, where does a circle end up once it is out of the walls, and is there
// a clear straight line between two points.
//
// Every accessor is TOTAL: a read outside the grid answers Tile::Wall. That is
// what closes the world -- no system special-cases the map edge, it is simply
// wall all the way out -- and it means resolveCircle keeps a body inside the
// map without a single bounds check of its own.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "shared/core/types.h"
#include "shared/game/constants.h"
#include "shared/game/realm.h"

namespace flix {

/// One point on a tile's jagged edge, in tile-local coordinates: `t` runs
/// along the edge (0..kTileSize) and `offset` is how far the outline bulges
/// outward into the air there.
struct JaggedEdgePoint {
    double t = 0;
    double offset = 0;
};

/// The outline one side of a wall or water tile wears, sorted by `t`, with a
/// zero-offset point pinned at each end so the protrusion closes onto the flat
/// tile edge.
using JaggedEdge = std::array<JaggedEdgePoint, kJaggedSegmentCount + 2>;

/// The outline for one edge of one tile. `edge` is 0 top, 1 bottom, 2 left,
/// 3 right.
///
/// THE single generator, deliberately: collision detection, the wall push-out
/// and the renderer all call this one function. They used to carry two copies
/// of the arithmetic that were required to agree and did not -- the seed
/// additions overflowed in one and not the other -- which meant some tiles
/// were drawn with an outline they did not collide with. Deterministic in the
/// tile coordinates alone, so it is also stable across client and server.
const JaggedEdge& jaggedEdge(int tileX, int tileY, int edge);

class Terrain;

/// Whether that outline is actually worn on that side, i.e. whether the tile
/// shows an edge there. Companion to jaggedEdge() and, for the same reason,
/// THE single answer: collision and the renderer must agree on which sides
/// exist, or the client draws a shore the server does not collide with.
///
/// Outside the grid counts as exposed. A solid tile shows its edge against
/// water; water does NOT show one back, or every shoreline would be drawn
/// twice, once in each colour -- and, crucially, water against water shows
/// nothing at all, or a lake is ruled into tiles.
bool jaggedEdgeExposed(const Terrain& terrain, int tileX, int tileY, int edge);

/// Number of distinct Tile values, i.e. the size of a per-tile-kind table.
inline constexpr int kTileKindCount = 5;

/// The character of one of the nine map sections: what the section is called,
/// and what the renderer paints each tile kind as inside it. The same Tile is
/// a different colour in the Garden and in Hel, which is the whole point of a
/// biome here -- the grid stores geometry, the biome stores mood.
struct Biome {
    const char* name;
    std::array<std::uint32_t, kTileKindCount> tileColors;   ///< 0xRRGGBB, indexed by Tile
};

/// Row-major, matching sectionAt(): top-left is 0, centre is 4.
inline constexpr std::array<Biome, kSectionCount> kBiomes = {{
    // name        ground      wall        water       sand        stone
    {"Garden",   {{0x1EA761u, 0x7C7C7Cu, 0x4AA7F7u, 0xE8DCA6u, 0x9AA0A6u}}},
    {"Desert",   {{0xD9CFA4u, 0xB08A55u, 0x4AA7F7u, 0xEAE4D0u, 0xC0A878u}}},
    {"Hel",      {{0x8F0606u, 0x4E0303u, 0xE2591Bu, 0xB4634Bu, 0x6B2020u}}},
    {"Ocean",    {{0x2F9E62u, 0x6E8FA8u, 0x2E86D8u, 0xE8DCA6u, 0x8FA6B8u}}},
    {"Ant Hell", {{0xA8784Fu, 0x6B4930u, 0x4AA7F7u, 0xC8A375u, 0x8E6140u}}},
    {"Jungle",   {{0x15A12Fu, 0x0B6B1Du, 0x2E8B7Fu, 0xCFC08Au, 0x5E7A4Au}}},
    {"Sewers",   {{0x6B4A18u, 0x3F2200u, 0x5C7A2Eu, 0x8A7040u, 0x633500u}}},
    {"Computer", {{0x0F3D2Au, 0x1B6E4Au, 0x00D885u, 0x1A2A24u, 0x101418u}}},
    {"Unknown",  {{0x1A1730u, 0x2B2740u, 0x3A2E5Cu, 0x2A2440u, 0x231F38u}}},
}};

/// The biome of a section index, or a neutral one for -1 (outside the map),
/// so a renderer that walks past the edge still has something to paint.
inline const Biome& biomeOf(int section) {
    static const Biome kOutside{"Void", {{0x14171Cu, 0x14171Cu, 0x14171Cu, 0x14171Cu, 0x14171Cu}}};
    if (section < 0 || section >= kSectionCount) return kOutside;
    return kBiomes[static_cast<std::size_t>(section)];
}

inline std::uint32_t tileColor(int section, Tile tile) {
    const int kind = static_cast<int>(tile);
    return biomeOf(section).tileColors[static_cast<std::size_t>(kind < kTileKindCount ? kind : 0)];
}

// ---------------------------------------------------------------------------
// Maze
// ---------------------------------------------------------------------------
//
// A second world in its own coordinate space (Realm::Maze), that the daily
// maze mode plays in. Nothing here touches Terrain's grid: the maze is a
// corridor lattice of 1000-unit cells whose every corridor/void junction is
// rounded by a quarter-circle fillet, and its walls are resolved by their own
// circle solver. Every Terrain query takes the realm it is asked about and
// dispatches here for the maze, so no caller has to know the maze exists.
//
// The reference parks the maze at (200000, 200000) inside the one world
// space; here it starts at (0, 0) in a space of its own. See realm.h.
//
// The layouts are authored, not generated. The day number only picks WHICH of
// the three is active, so a client told nothing but the day builds the same
// walls the server did, and no wall data ever goes over the wire.

inline constexpr double kMazeOriginX = 0.0;
inline constexpr double kMazeOriginY = 0.0;

/// World units per grid cell, and therefore the corner fillet radius too.
inline constexpr double kMazeCellSize = 1000.0;

/// Difficulty bands by corridor depth, shallowest first: the zone index a cell
/// carries is an index into the rarity ladder (0 = common .. 5 = mythic).
inline constexpr int kMazeZoneCount = 6;

enum class MazeBiome : std::uint8_t { Garden = 0, Desert = 1, Ocean = 2 };

/// Which map section each maze biome borrows its ground colours from, so the
/// renderer paints a maze the way it paints that biome's overworld.
inline constexpr std::array<int, 3> kMazeBiomeSections = {{0, 1, 3}};

/// One day's maze: the corner-coded cell grid, its difficulty zones, and the
/// two places the mode needs to put things (the entrance, and the boss rooms).
///
/// Cell values carry their own geometry, exactly as the reference's do:
///   0        solid void
///   1        plain floor
///   4..7     floor with a CONVEX rounded corner (bit0 = top, bit1 = left)
///   12..15   void with a CONCAVE rounded corner (bit3 set, bit0/bit1 as above)
/// The same value drives collision and rendering, so what is drawn is what is
/// collided with.
class Maze {
public:
    explicit Maze(std::int64_t dayNumber = 0) { setDay(dayNumber); }

    /// Rebuilds for a UTC day number. Cheap enough to call per join; the day
    /// only selects one of three authored templates.
    void setDay(std::int64_t dayNumber);

    std::int64_t day() const { return day_; }
    MazeBiome biome() const { return biome_; }
    int templateDim() const { return templateDim_; }
    int gridDim() const { return gridDim_; }
    double worldSize() const { return gridDim_ * kMazeCellSize; }

    /// Centre of the entrance room, where a player joining the maze appears.
    Vec2 spawn() const { return spawn_; }

    /// Centres of the deepest rooms, where the mode places its bosses.
    const std::vector<Vec2>& bossSpots() const { return bossSpots_; }

    /// True when a maze-space point lies inside the maze's square at all.
    bool contains(Vec2 p) const {
        const double span = worldSize();
        return p.x >= kMazeOriginX && p.x < kMazeOriginX + span &&
               p.y >= kMazeOriginY && p.y < kMazeOriginY + span;
    }

    /// Cell value at grid coordinates; outside the grid reads as solid void.
    std::uint8_t cellValue(int gx, int gy) const;
    /// Raw corner-coded grid and zone bands, row-major over gridDim() squared.
    /// The renderer and the population target read them whole.
    const std::vector<std::uint8_t>& values() const { return values_; }
    const std::vector<std::uint8_t>& zones() const { return zones_; }
    /// Difficulty band of a cell, or -1 for void.
    int zoneOfCell(int gx, int gy) const;
    /// Plain floor cells, the walkable area the maze's population is sized to.
    int floorCellCount() const;

    /// Difficulty band at a world point, or -1 for void and for outside.
    int zoneAt(Vec2 p) const;

    /// True when the point is inside solid maze wall, fillets included.
    bool blocksPoint(Vec2 p) const;

    /// True when the point stands on walkable floor (plain or convex corner).
    bool isFloor(Vec2 p) const;

    /// Line of sight through the maze: true when the segment crosses wall.
    bool blocksLine(Vec2 a, Vec2 b) const;

    /// Pushes a circle out of the maze walls, sliding along flat faces and
    /// radially around the corner fillets. Iterated, like the tile resolver,
    /// so a corner settles instead of oscillating between its two faces.
    Vec2 resolveCircle(Vec2 position, double radius, bool* collided = nullptr) const;

    /// The centre of the nearest plain floor cell to `p`, searched outward
    /// ring by ring; the entrance when the grid has none. What a body the
    /// resolver cannot free -- one placed deep inside the wall mass -- is
    /// rescued to, the way the tile map rescues to its nearest open tile.
    Vec2 nearestFloor(Vec2 p) const;

    /// Cheap circle-vs-cell overlap for projectiles. Reports the blocking
    /// cell's world rect, which is what a wall-hit effect is placed against.
    bool circleWallOverlap(Vec2 position, double radius, Rect& out) const;

private:
    /// One push-out pass. False when the circle is already clear.
    bool resolveOnce(Vec2 position, double radius, Vec2& out) const;
    bool cellBlocksPoint(int gx, int gy, Vec2 world) const;

    std::int64_t day_ = 0;
    MazeBiome biome_ = MazeBiome::Garden;
    int templateDim_ = 0;
    int gridDim_ = 0;
    std::vector<std::uint8_t> values_;
    std::vector<std::uint8_t> zones_;
    Vec2 spawn_;
    std::vector<Vec2> bossSpots_;
};

/// The one maze the process is playing today.
///
/// A single shared instance rather than a member of anything, because the
/// reference is a module-level singleton and every part of the game -- wall
/// resolution deep inside Terrain, line of sight, spawning -- asks it the same
/// question about the same day. Built for the current UTC day on first use;
/// the server overrides the day at boot and tells clients which one it picked.
const Maze& activeMaze();
void setActiveMazeDay(std::int64_t dayNumber);

/// UTC day number, i.e. whole days since the epoch.
std::int64_t currentMazeDay();

// ---------------------------------------------------------------------------
// Terrain
// ---------------------------------------------------------------------------

class Terrain {
public:
    /// Segments the reference's sight test cuts the ray into. Every call site
    /// there goes through the four-argument form, so it is always this.
    static constexpr int kLineOfSightSamples = 20;

    /// How far every blocking tile is grown for the centre-path test below.
    /// A path that only grazes the shared corner of a diagonal wall seam does
    /// cross it, and the graze can be sub-pixel, so the tiles are inflated
    /// rather than the test loosened.
    static constexpr double kCenterPathInflation = 0.5;

    /// An ungenerated Terrain is all Ground: legal, walkable, and useless as a
    /// map. Systems can run against one, which is what tests want.
    Terrain();

    /// Builds the legacy procedural map for `seed`. Equal seeds give
    /// byte-identical grids; production instead loads map_bundle.ts below.
    ///
    /// Ends by flood-filling from the spawn and carving a corridor to anything
    /// the noise walled off, so the postcondition is always isConnected().
    ///
    /// Reproducible across machines only as far as the floating point is: if
    /// the client and the server are ever built by different toolchains, build
    /// both with -ffp-contract=off, or the odd tile will land on the other
    /// side of a threshold.
    void generate(std::uint64_t seed);

    /// Loads MAP_TILE_RLE from TypeScript's generated map_bundle.ts. The
    /// bundle remains the single map source used by both implementations.
    bool loadMapBundle(const std::string& path, std::string& errorOut);

    /// Replaces the grid with an authoritative network copy.
    bool setTiles(const std::vector<std::uint8_t>& tiles);

    std::uint64_t seed() const { return seed_; }

    // -- reads --------------------------------------------------------------

    Tile atTile(int tx, int ty) const {
        if (tx < 0 || ty < 0 || tx >= kTilesPerAxis || ty >= kTilesPerAxis) return Tile::Wall;
        return static_cast<Tile>(tiles_[static_cast<std::size_t>(ty) * kTilesPerAxis + static_cast<std::size_t>(tx)]);
    }

    Tile at(Vec2 p) const { return atTile(toTileCoord(p.x), toTileCoord(p.y)); }

    /// Whether a point is inside something solid, in the given realm.
    ///
    /// Overworld: a blocking tile. Maze: wall, fillets included -- the
    /// reference's wander probe saw open ground there because its wall grid
    /// simply had no entry, and the maze here answers for itself instead.
    /// Arena: outside the ring, which is the arena's one wall.
    bool blocked(Vec2 p, Realm realm) const;
    /// Water slows; only the overworld has any.
    bool inWater(Vec2 p, Realm realm) const {
        return realm == Realm::Overworld && tileIsWater(at(p));
    }

    // -- realm geometry -------------------------------------------------------

    /// The square a realm's coordinates span, from (0, 0). The broadphase is
    /// sized to it and a body is clamped inside it.
    static double realmSize(Realm realm);

    /// Keeps a body of `radius` inside its realm: the world rectangle, the
    /// arena ring, or the maze square. The reference's PVP clamp
    /// (src/server/playerState.ts:1989) is the arena case.
    static Vec2 clampInside(Vec2 p, double radius, Realm realm);

    /// True when a point has left its realm's playable area altogether. What
    /// the loot system asks about a drop the resolver could not save.
    static bool outside(Vec2 p, Realm realm);

    /// Which of the nine sections a point is in, or -1 outside the map.
    int sectionAt(Vec2 p) const { return flix::sectionAt(p); }
    int sectionOfTile(int tx, int ty) const { return flix::sectionAt(tileCenter(tx, ty)); }
    const Biome& biomeAt(Vec2 p) const { return biomeOf(sectionAt(p)); }

    /// The connectivity root chosen by generate(), and where a fresh player
    /// starts. Guaranteed walkable on a generated map.
    Vec2 spawnPoint() const;

    // -- collision ----------------------------------------------------------

    /// What one wall resolution did, field for field with the reference's
    /// resolveEntityWallCollisions return value.
    struct WallResolution {
        Vec2 position;             ///< the corrected centre
        bool collided = false;     ///< at least one pass had to push
        bool unresolved = false;   ///< four passes ended still overlapping
    };

    /// The reference's resolveEntityWallCollisions, exactly: four push-out
    /// passes and one residual check, and nothing else. A centre the passes
    /// cannot untangle is REPORTED, never relocated.
    ///
    /// This is the entry point a movement step has to use, because `unresolved`
    /// is the signal the reference refuses on: accepting a still-overlapping
    /// result lets per-tile least-penetration ejection flip to a tile's far
    /// face and ratchet the body through the wall over a few ticks.
    /// resolveCircle() below cannot report it -- by the time it returns, it has
    /// already moved the body somewhere the caller did not ask for.
    ///
    /// Inside the maze `unresolved` is always false, as it is in the reference:
    /// the maze resolver's result type has no such field, so a caller that
    /// refuses unresolved output never refuses a maze wall.
    WallResolution resolveWall(Vec2 position, double radius, Realm realm) const;

    /// Pushes a circle out of every solid tile it overlaps and returns the
    /// corrected centre, RESCUING a centre the four passes could not untangle
    /// by ejecting it to the nearest open tile.
    ///
    /// That rescue is not in the reference, and it is why this is the wrong
    /// call for movement -- see resolveWall() above. It exists for the callers
    /// that are PLACING a body rather than moving one (spawners, drops, admin
    /// teleports): they hand over a point that may be deep inside geometry and
    /// need a usable one back, where a movement step needs the truth.
    ///
    /// Robust by construction rather than by contract: absurd radii are
    /// clamped and non-finite input is replaced rather than propagated. Bad
    /// input upstream costs the caller a shove, never the tick.
    Vec2 resolveCircle(Vec2 position, double radius, Realm realm) const;

    /// True when the segment crosses any blocking tile. An exact DDA walk: no
    /// allocation, and bounded even for nonsense endpoints.
    ///
    /// This is the EXACT swept test, and it is not interchangeable with
    /// hasLineOfSight() below -- the reference's sight test samples, and a
    /// sparse sample steps over a wall an exact walk stops at. Use this only
    /// where the question really is "does this segment touch solid".
    bool segmentBlocked(Vec2 a, Vec2 b, Realm realm) const;

    /// True when the straight path between two entity CENTRES touches any
    /// blocking tile, every tile grown by `eps` first.
    ///
    /// Neither a swept body test nor a sight test: this is the containment
    /// guard the reference's movement step runs on the resolver's own output.
    /// A push-out is free to choose a tile's far face, and committing one that
    /// carries the centre across solid is how a body ends up on the other side
    /// of a wall in a single tick; asking whether the centre's path crossed
    /// anything is what catches it.
    ///
    /// Off-grid tiles are air here, exactly as in the reference's scan, so a
    /// path outside the map crosses nothing. A tile question, so it is asked
    /// of the overworld only: the maze and the arena have no tiles.
    bool segmentTouchesBlockingTile(Vec2 a, Vec2 b, double eps = kCenterPathInflation) const;

    /// The reference's sight test, sample for sample: endpoints closer than
    /// ten units always see each other, and otherwise 21 evenly spaced points
    /// are tested and nothing between them is. Sampling is what mob targeting
    /// and the wander probe ask, so its blind spots are part of the behaviour
    /// -- a mob that can shoot across the corner of a wall does so because the
    /// samples fell either side of it, and matching that is the point.
    ///
    /// Outside the grid reads as AIR here, not as wall: the reference's grid
    /// simply has no entry there, so a ray leaving the map is never blocked by
    /// having left it.
    bool hasLineOfSight(Vec2 a, Vec2 b, Realm realm, int sampleCount = kLineOfSightSamples) const;

    /// A walkable point within `radius` of `around`, avoiding water when it
    /// can. Falls back to the nearest open tile, so it always returns
    /// something a body can stand in.
    Vec2 findOpenSpawn(Rng& rng, Vec2 around, double radius, Realm realm) const;

    /// The nearest tile a body can stand in, searched outward from `p`. False
    /// when everything within the search bound is solid.
    bool nearestOpenTile(Vec2 p, int& outTx, int& outTy) const;

    // -- invariants ---------------------------------------------------------

    /// True when every non-blocking tile is reachable from the spawn. generate()
    /// guarantees it; setTile() can break it, which is why it is public.
    bool isConnected() const;

    /// Passable tile count, for tests and map statistics.
    int openTileCount() const;

    // -- authoring ----------------------------------------------------------
    //
    // Direct writes, for tests and for any future map editor. They do not
    // re-verify connectivity: a caller that walls off a region owns the
    // consequences.

    void setTile(int tx, int ty, Tile t);
    void fill(Tile t);

    // -- grid geometry ------------------------------------------------------

    /// Tile index for a world coordinate. Clamped before the cast: a runaway
    /// coordinate (1e30 from a bad teleport) would otherwise be undefined
    /// behaviour here and an unbounded loop in every caller that walks tiles.
    static int toTileCoord(double world) {
        const double t = std::floor(world / kTileSize);
        // Written as a failed > test so NaN takes this branch too.
        if (!(t > -kTileCoordLimit)) return -kTileCoordLimit;
        if (t > kTileCoordLimit) return kTileCoordLimit;
        return static_cast<int>(t);
    }

    static Vec2 tileCenter(int tx, int ty) {
        return {(tx + 0.5) * kTileSize, (ty + 0.5) * kTileSize};
    }

    static Rect tileRect(int tx, int ty) {
        return {tx * kTileSize, ty * kTileSize, kTileSize, kTileSize};
    }

    static constexpr int tilesPerAxis() { return kTilesPerAxis; }

    /// Raw row-major grid, one byte per tile, for the renderer and for saving.
    const std::uint8_t* tiles() const { return tiles_.data(); }
    std::size_t tileCount() const { return tiles_.size(); }

private:
    static constexpr int kTileCoordLimit = 1 << 20;

    /// Beyond four tiles a circle spans more geometry than a push-out can
    /// meaningfully resolve, and the scan cost grows quadratically. Nothing in
    /// the game is this big; the clamp exists so nothing can be.
    static constexpr double kMaxResolveRadius = kTileSize * 4.0;

    /// Overlaps against several tiles fight each other, so the push is
    /// iterated. Four passes settles every concave corner the grid can make;
    /// the cap is what makes a wedge the circle cannot fit into terminate.
    static constexpr int kResolvePasses = 4;

    /// A DDA long enough to cross the map twice. Past that the segment is
    /// nonsense and reporting it blocked is the conservative answer.
    static constexpr int kMaxSegmentSteps = 2 * kTilesPerAxis + 8;

    static constexpr int kNearestOpenSearchTiles = 24;

    int index(int tx, int ty) const { return ty * kTilesPerAxis + tx; }
    bool passableIndex(int i) const { return !tileBlocks(static_cast<Tile>(tiles_[static_cast<std::size_t>(i)])); }

    void generateSections(Rng& rng);
    void carveAntHell(Rng& rng);
    void placeCircuitChips(Rng& rng);
    int chooseGardenSpawn() const;
    void carveDisc(Vec2 center, double radius, Tile t);
    void carveCorridor(int fromTx, int fromTy, int toTx, int toTy, int halfWidth, Tile t);
    /// Flood-fills from the spawn and digs the shortest wall-crossing route to
    /// every region the fill missed.
    void connectAll();

    std::vector<std::uint8_t> tiles_;
    std::uint64_t seed_ = 0;
    int spawnTile_ = 0;
};

} // namespace flix
