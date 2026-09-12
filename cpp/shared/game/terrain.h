#pragma once
// The tile world.
//
// One grid of Tile per world realm, 300 units a tile; constants.h owns the
// tile size, the map file owns the dimensions. Terrain answers three questions
// and nothing else: what is at a point, where does a circle end up once it is
// out of the walls, and is there a clear straight line between two points.
//
// Every accessor is TOTAL: a read outside the grid answers Tile::Wall. That is
// what closes the world -- no system special-cases the map edge, it is simply
// wall all the way out -- and it means resolveCircle keeps a body inside the
// map without a single bounds check of its own.
//
// A CELL IS THE SHAPES ITS TILE CARRIES, not its square. The author draws
// collision in Tiled's Tile Collision Editor, per tile, and a cell contributes
// those shapes turned by its flip bits and scaled onto the cell -- so a body
// walks up to the edge the art draws and stops there. Terrain keeps two views
// of that, and the difference between them matters at every call site:
//
//   the COARSE grid   one Tile per cell: does this cell hold ANY blocking
//                     shape, and of what kind. What the minimap paints, what
//                     the bots' flow field walks, what spawn placement rejects
//                     conservatively, and the only thing that travels over the
//                     wire.
//   the SHAPES        the exact geometry, per realm, beside the grid. What
//                     blocked(), resolveWall(), resolveCircle(), the segment
//                     tests, hasLineOfSight() and inWater() answer from.
//
// A realm with no shape store -- a generated map, a grid a test wrote with
// setTile(), a client whose data directory has no map for the realm it is in --
// falls back to the whole 300-unit square of every blocking cell, which is the
// old behaviour and is conservative: it blocks a little more than the art does,
// never less.
//
// What a cell LOOKS like is not here at all: the artwork is the map file's
// layers (tiled_map.h), which the client reads for itself.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "shared/core/types.h"
#include "shared/net/bytebuffer.h"
#include "shared/game/constants.h"
#include "shared/game/realm.h"

namespace flix {

class TiledMap;

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

/// Which MOB GROUP each maze biome is stocked from: a garden maze is full of
/// garden mobs. Names rather than section indices, because what lives where is
/// mobs.json's business now -- see MobGroup in config.h. A group the content
/// does not define leaves that maze empty, which is visible, rather than
/// quietly full of whatever happened to be nearby.
inline constexpr std::array<const char*, 3> kMazeBiomeGroups = {{"garden", "desert", "ocean"}};

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
// Authored collision shapes
// ---------------------------------------------------------------------------

/// One collision polygon, in CELL-LOCAL world units: (0,0) is its cell's
/// top-left corner, the cell is kTileSize across, y is down.
///
/// Closed, wound so its signed area is positive, and already turned by its
/// cell's flip bits and scaled from the tileset's tile size onto the cell --
/// see TiledShape. CONCAVE RINGS ARE NORMAL: the authored dirt edges are, and
/// every test here works on them.
///
/// The bounding box is carried rather than recomputed because it is the reject
/// that keeps a query down to a couple of edge tests: a scan of nine cells with
/// two 14-sided rings each is 250 edges if you test them all and a handful if
/// you look at the boxes first.
struct CollisionShape {
    std::vector<Vec2> points;
    Rect bounds;
    /// True when the ring IS its bounding box -- an axis-aligned rectangle,
    /// which 29 of the shipped tileset's 40 shapes are, the whole-cell fallback
    /// included. Containment is then the box test the reject has already done,
    /// so the ring is never walked; it is the reason a point test against a
    /// map floored with plain dirt costs about what the old array read cost.
    /// Every one of the eight orientations takes an axis-aligned rectangle to
    /// another one, so this survives the transform.
    bool rectangle = false;
};

/// The shapes ONE (tile, orientation) pair contributes to a cell.
///
/// Built once per pair the map actually paints on a colliding layer -- at most
/// eight per tile in the tileset, and 86 for the shipped garden -- and shared
/// by every cell that paints it. That is the whole performance story: a
/// polygon is transformed once at load, and a query adds a cell origin to a
/// point rather than rebuilding a ring.
struct CollisionShapeSet {
    std::vector<CollisionShape> shapes;
    Rect bounds;   ///< the union of the shapes' boxes, cell-local
};

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
    ///
    /// The overworld grid starts at the historical size (kTilesPerAxis square)
    /// so a harness that never loads a map gets the world it always did. Every
    /// other world realm starts EMPTY, which reads as solid everywhere: a
    /// realm nobody staged a map for is not somewhere a body can be.
    Terrain();

    /// Builds the legacy procedural map for `seed`. Equal seeds give
    /// byte-identical grids; production instead loads an authored map below.
    ///
    /// Ends by flood-filling from the spawn and carving a corridor to anything
    /// the noise walled off, so the postcondition is always isConnected().
    ///
    /// Reproducible across machines only as far as the floating point is: if
    /// the client and the server are ever built by different toolchains, build
    /// both with -ffp-contract=off, or the odd tile will land on the other
    /// side of a threshold.
    void generate(std::uint64_t seed);

    /// Loads a map's collision: the authored SHAPES, and the coarse grid over
    /// them. A map is a Tiled `.tmj` and nothing else; both are DERIVED from
    /// the layers the author painted, by the rule in shared/game/tiled_map.h --
    /// a tile layer whose `has_collision` property is set contributes the
    /// collision shapes of every tile it paints, no other layer contributes
    /// anything, and a tile carrying no shapes contributes nothing anywhere.
    ///
    /// Prints what that rule resolved to -- which layers collide, the coarse
    /// cell counts, how many distinct shape sets it built, and a WARNING for
    /// any cell on a colliding layer whose tile has no shapes -- once per map,
    /// because a tick box in the layer panel and a shape nobody drew are both
    /// invisible until somebody walks through a wall.
    ///
    /// `realm` says WHICH world this map is. Every world realm carries its own
    /// grid with its own dimensions, so a second map need not be the size of
    /// the first -- a corridor level is a hundred tiles across and would
    /// otherwise have to be drawn inside a 200-square sheet of wall.
    ///
    /// loadTiledMap() is the same function under the name that says what the
    /// file is; loadWorldMap() is the name the rest of the engine asks by.
    bool loadWorldMap(const std::string& path, std::string& errorOut,
                      Realm realm = Realm::Overworld);
    bool loadTiledMap(const std::string& path, std::string& errorOut,
                      Realm realm = Realm::Overworld);

    /// Installs ONE REALM'S AUTHORED SHAPES from a map already read, leaving
    /// the coarse grid exactly as it is.
    ///
    /// THIS IS WHAT THE CLIENT CALLS. A client is handed the coarse grid over
    /// the wire (readMapGrid) and stages the map file itself, so it can build
    /// the same shapes the server did and predict against the same geometry --
    /// without which prediction disagrees with the server on every slope. The
    /// handshake's content hash covers the map's bytes AND its tileset's --
    /// the shapes are in the tileset -- so the two are known to be the same
    /// geometry, not merely the same map.
    ///
    /// Refuses a map whose dimensions do not match the grid already installed
    /// for the realm, because shapes indexed at the wrong width are a world
    /// sheared diagonally. A refusal leaves the realm on whole-cell collision
    /// from the grid, which is conservative and still playable.
    bool setCollisionShapes(const TiledMap& map, Realm realm = Realm::Overworld);

    /// setCollisionShapes() straight off a `.tmj` path, for a caller that has
    /// not already read the file.
    bool loadCollisionShapes(const std::string& path, std::string& errorOut,
                             Realm realm = Realm::Overworld);

    /// True when a realm has authored shapes, i.e. when its collision is exact
    /// rather than whole-cell.
    bool hasCollisionShapes(Realm realm = Realm::Overworld) const;

    /// How many distinct (tile, orientation) shape sets a realm's store holds,
    /// and how many cells reference at least one. What the load report prints,
    /// and what a test asserts the sharing on.
    int collisionShapeSetCount(Realm realm = Realm::Overworld) const;
    int collisionShapeCellCount(Realm realm = Realm::Overworld) const;

    /// One ring of a cell's authored collision, as a query sees it.
    ///
    /// `points` is the ring in the CELL-LOCAL units of the cell that owns the
    /// geometry, and `origin` is where those units are measured from in world
    /// space: a vertex is points[i] + origin whatever cell the ring was filed
    /// under. `ownCell` is false in the one case where those two cells differ
    /// -- a shape drawn past its tile's edge is filed in every cell it reaches
    /// (see ShapeGrid) -- so a caller that walks the whole grid can draw each
    /// ring once instead of once per cell it touches.
    ///
    /// The pointer is into the realm's shape store: it is valid until that
    /// realm's shapes are replaced or dropped, which is the same lifetime the
    /// collision queries themselves run inside.
    struct CellCollisionRing {
        const std::vector<Vec2>* points = nullptr;
        Vec2 origin{0.0, 0.0};
        std::uint8_t layer = 0;
        bool water = false;
        bool ownCell = true;
    };

    /// The authored rings filed under one cell, appended to `out` (cleared
    /// first) in LAYER ORDER, bottom first -- the same refs, in the same
    /// order, that blocked() and resolveWall() walk for that cell. There is no
    /// second copy of the geometry anywhere: this hands back the store.
    ///
    /// EMPTY for a cell with no authored shapes, including one whose coarse
    /// Tile blocks. That is not an omission: a shape-less blocking cell is
    /// collided with as its whole 300-unit square (see the cell tests below),
    /// and a caller that draws geometry has to draw that square itself rather
    /// than be handed a ring the store does not hold.
    ///
    /// For DRAWING and for inspection. Nothing in the collision path calls it:
    /// the queries walk the refs in place, and an out-parameter vector has no
    /// business in a per-tick test.
    void collisionRingsAt(int tx, int ty, Realm realm,
                          std::vector<CellCollisionRing>& out) const;

    /// The furthest any of a realm's shapes reaches outside the cell it was
    /// painted in, in world units. Zero for a tileset whose shapes were all
    /// drawn inside their tiles; the load report says it when it is not,
    /// because a vertex nudged past a tile edge is invisible in Tiled.
    double collisionOverhangUnits(Realm realm = Realm::Overworld) const;

    /// Drops a realm's shapes, putting it back on whole-cell collision.
    void clearCollisionShapes(Realm realm);

    /// Replaces one realm's grid with an authoritative network copy.
    ///
    /// The dimensions travel WITH the tiles: a client is told the shape of the
    /// map it is being dropped into, because a grid interpreted at the wrong
    /// width is a world sheared diagonally.
    ///
    /// Keeps the realm's authored shapes when the dimensions match and DROPS
    /// them when they do not: a store indexed at the old width describes a
    /// different map. So a client may install the wire grid and its own shapes
    /// in either order.
    bool setTiles(const std::vector<std::uint8_t>& tiles, int cols, int rows,
                  Realm realm = Realm::Overworld);

    /// Drops a realm's grid, so the realm reads as solid everywhere again.
    void clearRealm(Realm realm);

    /// True when a realm has a grid installed. The arena and the maze never
    /// do -- they answer for themselves -- so this asks only about maps.
    bool hasMap(Realm realm) const;

    std::uint64_t seed() const { return seed_; }

    // -- reads --------------------------------------------------------------

    /// Off the grid -- and every tile of a realm with no map -- reads as Wall,
    /// which is what keeps a body inside a map whatever its dimensions are.
    Tile atTile(int tx, int ty, Realm realm = Realm::Overworld) const {
        const Grid& g = grid(realm);
        if (tx < 0 || ty < 0 || tx >= g.cols || ty >= g.rows) return Tile::Wall;
        return static_cast<Tile>(
            g.tiles[static_cast<std::size_t>(ty) * static_cast<std::size_t>(g.cols) +
                    static_cast<std::size_t>(tx)]);
    }

    Tile at(Vec2 p, Realm realm = Realm::Overworld) const {
        return atTile(toTileCoord(p.x), toTileCoord(p.y), realm);
    }

    /// A realm's grid dimensions, in tiles. Zero for the arena, the maze and
    /// any realm no map was staged for.
    int tileCols(Realm realm = Realm::Overworld) const { return grid(realm).cols; }
    int tileRows(Realm realm = Realm::Overworld) const { return grid(realm).rows; }

    /// Whether a point is inside something solid, in the given realm.
    ///
    /// Overworld: a blocking tile. Maze: wall, fillets included -- the
    /// reference's wander probe saw open ground there because its wall grid
    /// simply had no entry, and the maze here answers for itself instead.
    /// Arena: outside the ring, which is the arena's one wall.
    /// EXACT against the authored shapes: a point is blocked when it is inside
    /// one of them, not when its cell holds one. Falls back to the cell's
    /// coarse Tile where a realm has no shapes.
    bool blocked(Vec2 p, Realm realm) const;

    /// Water slows; only a tile map has any. Exact too: the point must be
    /// inside a water-tagged SHAPE, and the topmost shape containing it decides,
    /// so a bridge tile drawn over a pond is not water.
    bool inWater(Vec2 p, Realm realm) const;

    // -- realm geometry -------------------------------------------------------

    /// The rectangle a realm's coordinates span, from (0, 0).
    ///
    /// A member rather than a static now: the arena and the maze are fixed
    /// shapes the class can answer for on its own, but a world map's extent is
    /// whatever the file said, so the answer belongs to the loaded Terrain.
    Vec2 realmExtent(Realm realm) const;

    /// The longer axis of realmExtent(). What the broadphase sizes a square
    /// layer to.
    double realmSize(Realm realm) const;

    /// Keeps a body of `radius` inside its realm: the map rectangle, the
    /// arena ring, or the maze square. The reference's PVP clamp
    /// (src/server/playerState.ts:1989) is the arena case.
    Vec2 clampInside(Vec2 p, double radius, Realm realm) const;

    /// True when a point has left its realm's playable area altogether. What
    /// the loot system asks about a drop the resolver could not save.
    bool outside(Vec2 p, Realm realm) const;

    /// Which of the nine sections a point is in, or -1 outside the map.
    ///
    /// The section grid is a RENDERING question now -- which palette a tile is
    /// painted in, and which ground the maze borrows. What lives where is a
    /// mob group's business (content.h), not a corner of the map's.
    int sectionAt(Vec2 p) const { return flix::sectionAt(p); }
    int sectionOfTile(int tx, int ty) const { return flix::sectionAt(tileCenter(tx, ty)); }
    const Biome& biomeAt(Vec2 p) const { return biomeOf(sectionAt(p)); }

    /// The connectivity root chosen by generate(), and where a fresh player
    /// starts. Guaranteed walkable on a generated map.
    Vec2 spawnPoint(Realm realm = Realm::Overworld) const;

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
    /// The push is against the authored SHAPES. For each one the circle
    /// overlaps: the closest point on the ring is found; a centre inside the
    /// ring is ejected along the shortest way out, a centre outside it but
    /// within the radius is pushed back along the outward normal, and either
    /// way it ends up exactly radius + epsilon clear of that closest point.
    /// Against a full-cell rectangle that is arithmetically the same
    /// least-penetration ejection this function has always done.
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

    /// True when the segment crosses any blocking SHAPE. A DDA walk over the
    /// cells, testing the shapes of each: no allocation, and bounded even for
    /// nonsense endpoints.
    ///
    /// This is the EXACT swept test, and it is not interchangeable with
    /// hasLineOfSight() below -- the reference's sight test samples, and a
    /// sparse sample steps over a wall an exact walk stops at. Use this only
    /// where the question really is "does this segment touch solid".
    bool segmentBlocked(Vec2 a, Vec2 b, Realm realm) const;

    /// True when the straight path between two entity CENTRES touches any
    /// blocking shape, every shape grown by `eps` first.
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
    bool segmentTouchesBlockingTile(Vec2 a, Vec2 b, double eps = kCenterPathInflation,
                                    Realm realm = Realm::Overworld) const;

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
    bool nearestOpenTile(Vec2 p, int& outTx, int& outTy, Realm realm = Realm::Overworld) const;

    // -- invariants ---------------------------------------------------------

    /// True when every non-blocking tile is reachable from the spawn. generate()
    /// guarantees it; setTile() can break it, which is why it is public.
    bool isConnected(Realm realm = Realm::Overworld) const;

    /// Passable tile count, for tests and map statistics.
    int openTileCount(Realm realm = Realm::Overworld) const;

    // -- authoring ----------------------------------------------------------
    //
    // Direct writes, for tests and for any future map editor. They do not
    // re-verify connectivity: a caller that walls off a region owns the
    // consequences.

    void setTile(int tx, int ty, Tile t, Realm realm = Realm::Overworld);
    void fill(Tile t, Realm realm = Realm::Overworld);

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

    /// Raw row-major grid, one byte per tile, for the renderer and for the
    /// wire. `tileCols`/`tileRows` above say how to read it.
    const std::uint8_t* tiles(Realm realm = Realm::Overworld) const {
        return grid(realm).tiles.data();
    }
    std::size_t tileCount(Realm realm = Realm::Overworld) const {
        return grid(realm).tiles.size();
    }

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

    /// A DDA long enough to cross the widest map twice. Past that the segment
    /// is nonsense and reporting it blocked is the conservative answer.
    ///
    /// Sized off the LARGEST grid a map may have rather than off the one being
    /// walked, so the bound is a constant and the loop needs no per-call
    /// arithmetic; a small map simply reaches its far edge long before it.
    static constexpr int kMaxSegmentSteps = 2 * kMaxTilesPerAxis + 8;

    static constexpr int kNearestOpenSearchTiles = 24;

    /// One realm's authored collision shapes, beside its coarse grid.
    ///
    /// Cell -> shapes goes through `firstRef`, a prefix table of cols*rows + 1
    /// offsets into `refs`: the refs of cell i are refs[firstRef[i]] up to
    /// firstRef[i + 1], in LAYER ORDER, bottom first. A cell with no blocking
    /// shape has no refs, which is one subtraction to find out -- the same cost
    /// as the Tile read it replaces.
    ///
    /// A shape is listed in EVERY cell it touches, not only the one its tile
    /// was painted in (shapeReach). Tiled lets an author drag a collision shape
    /// past the tile's edge, and a turn can sweep one clean into the next cell;
    /// fanning the ref out at build time is what lets every query ask ONE cell
    /// and be right. The alternative -- scanning a ring of neighbours in case
    /// some shape overhangs -- cost a measured 2-4x on blocked(), resolveWall()
    /// and the sight test for a shipped map whose worst overhang is 0.39 units,
    /// because a whole 300-unit ring is the smallest ring there is.
    struct ShapeGrid {
        /// One layer's contribution to one cell: which shape set, which layer
        /// it came from (bottom is 0, so the largest wins a kind dispute),
        /// whether that tile is water-tagged, and WHERE THE SHAPE'S OWN CELL IS
        /// relative to the cell this ref is filed under.
        ///
        /// `dx`/`dy` are zero for all but a shape that leaves its tile, and
        /// they are what the shape's points are measured from: a ref's geometry
        /// lives in the coordinates of cell (tx + dx, ty + dy).
        struct Ref {
            std::uint32_t set = 0;
            std::int16_t dx = 0;
            std::int16_t dy = 0;
            std::uint8_t layer = 0;
            bool water = false;
        };
        int cols = 0;
        int rows = 0;
        std::vector<CollisionShapeSet> sets;
        std::vector<Ref> refs;
        std::vector<std::uint32_t> firstRef;
        int cellsWithShapes = 0;
        /// The furthest any shape reaches outside its own cell, in world units.
        /// Reported at load so an author who nudged a vertex past a tile edge
        /// can see it; nothing in a query reads it, because the fan-out above
        /// already put the shape in every cell it touches.
        double overhangUnits = 0.0;
        bool empty() const { return cols <= 0 || rows <= 0 || refs.empty(); }
    };

    /// One world realm's tile grid.
    ///
    /// `cols`/`rows` are the map's own dimensions, so two realms may be
    /// different shapes. An EMPTY grid (both zero) is a realm nothing was
    /// staged for: every read off it is Wall, so no body can be there and no
    /// query has to special-case it.
    struct Grid {
        int cols = 0;
        int rows = 0;
        std::vector<std::uint8_t> tiles;
        /// The connectivity root generate() chose, as a tile index.
        int spawnTile = 0;
    };

    Grid& grid(Realm realm) { return grids_[realmIndex(realm)]; }
    const Grid& grid(Realm realm) const { return grids_[realmIndex(realm)]; }

    ShapeGrid& shapeGrid(Realm realm) { return shapes_[realmIndex(realm)]; }
    const ShapeGrid& shapeGrid(Realm realm) const { return shapes_[realmIndex(realm)]; }

    // -- the exact tests, one cell at a time --------------------------------
    //
    // Each takes the cell it is asked about and answers from that cell's
    // authored shapes, falling back to the whole 300-unit square when the cell
    // has none but its coarse Tile blocks. That fallback is what keeps a
    // generated map, a grid a test wrote with setTile(), and a client with no
    // map file working unchanged.
    //
    // The callers pass IN-GRID cells only; what a point off the grid is (wall
    // for gameplay, air for the sight test) is the caller's rule, not a cell's.

    /// The topmost colliding layer whose shape contains `p`, or -1 when nothing
    /// there does; `water` is that layer's kind.
    int cellLayerAt(int tx, int ty, Vec2 p, Realm realm, bool& water) const;

    /// True when the segment touches any of the cell's shapes, each grown by
    /// `eps`.
    bool cellTouchesSegment(int tx, int ty, Vec2 a, Vec2 b, double eps, Realm realm) const;

    /// Where this cell's shapes push a circle to, and whether the push came off
    /// a face rather than a corner. False when the circle is clear of the cell.
    bool cellPushCircle(int tx, int ty, Vec2 p, double radius, Realm realm, Vec2& pushed,
                        bool& flat) const;

    /// The topmost layer whose shape contains `p`, over every cell that could
    /// reach it, or -1. `outsideBlocks` is what a point off the grid means:
    /// Wall for every gameplay query, which is what closes the world, and AIR
    /// for hasLineOfSight(), whose reference has no grid entry out there.
    int blockingLayerAt(Vec2 p, Realm realm, bool& water, bool outsideBlocks = true) const;

    /// The cell whose geometry the next push-out pass should act on, and where
    /// it puts the centre. A face hit wins over a corner hit, so a body sliding
    /// along a wall is pushed straight off the face it is touching and never
    /// off the seam between two cells of it.
    struct ShapeCollision {
        Vec2 position;
        bool flat = false;
    };
    std::optional<ShapeCollision> findCollision(Vec2 position, double radius, Realm realm) const;

    int index(const Grid& g, int tx, int ty) const { return ty * g.cols + tx; }
    bool passableIndex(const Grid& g, int i) const {
        return !tileBlocks(static_cast<Tile>(g.tiles[static_cast<std::size_t>(i)]));
    }

    /// Installs a grid of `cols` x `rows`, rejecting a shape the engine cannot
    /// hold or a tile it has no Tile for. Shared by every loader and by the
    /// wire.
    bool install(Realm realm, std::vector<std::uint8_t> tiles, int cols, int rows);

    void generateSections(Rng& rng);
    void carveAntHell(Rng& rng);
    void placeCircuitChips(Rng& rng);
    int chooseGardenSpawn() const;
    void carveDisc(Vec2 center, double radius, Tile t);
    void carveCorridor(int fromTx, int fromTy, int toTx, int toTy, int halfWidth, Tile t);
    /// Flood-fills from the spawn and digs the shortest wall-crossing route to
    /// every region the fill missed.
    void connectAll();

    /// One grid per realm. The arena's and the maze's slots stay empty: those
    /// two realms are geometry, not tiles, and every query dispatches to them
    /// before it ever looks in here.
    std::array<Grid, kMaxRealms> grids_;
    /// One shape store per realm, empty until a map is loaded into it. The
    /// maze and the arena never have one -- they are geometry of their own.
    std::array<ShapeGrid, kMaxRealms> shapes_;
    std::uint64_t seed_ = 0;
};

// ---------------------------------------------------------------------------
// The tile run-length encoding
// ---------------------------------------------------------------------------
//
// The format a tile grid travels over the wire in.
//
// A run is a header byte, an optional two-byte extension, then the value:
//
//   header = (count << 1) | extended     count 0..127
//   if extended: count += (hi << 8) | lo
//   value
//
// A raw grid is a byte per tile, which for a 512-square map is a quarter of a
// megabyte -- far past the socket's backpressure ceiling. Maps are
// overwhelmingly long runs of the same tile, so this costs a few hundred bytes
// instead.

/// Encodes a row-major grid. Never fails: every byte value encodes.
std::vector<std::uint8_t> encodeTileRle(const std::vector<std::uint8_t>& tiles);

/// Decodes one, refusing a stream that does not yield exactly `expected`
/// values or that carries a value the engine has no Tile for.
bool decodeTileRle(const std::uint8_t* data, std::size_t size, std::size_t expected,
                   std::vector<std::uint8_t>& out, std::string& errorOut);

/// Writes one realm's grid in the wire's MapGrid shape (net/protocol.h).
void writeMapGrid(ByteWriter& out, const Terrain& terrain, Realm realm);

/// Reads one back and installs it. `realmOut` is the realm it named; false
/// means the payload was malformed and nothing was installed.
bool readMapGrid(ByteReader& in, Terrain& terrain, Realm& realmOut, std::string& errorOut);

} // namespace flix
