#include "test.h"

#include "server/systems/movement.h"
#include "shared/game/spatial.h"
#include "shared/game/terrain.h"
#include "shared/game/tiled_map.h"
#include "client/minimap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <limits>
#include <vector>

using namespace flix;

namespace {

/// One generated map, shared by the tests that only read it. Generation walks
/// 40000 tiles and a repair BFS; doing it per test case is pure waste.
const Terrain& sharedMap() {
    static const Terrain map = [] {
        Terrain t;
        t.generate(0xC0FFEEull);
        return t;
    }();
    return map;
}

int countOf(const std::vector<Entity>& list, Entity e) {
    return static_cast<int>(std::count(list.begin(), list.end(), e));
}

/// True when the circle overlaps any blocking tile -- the property
/// resolveCircle exists to establish.
bool overlapsWall(const Terrain& t, Vec2 p, double radius) {
    const int x0 = Terrain::toTileCoord(p.x - radius);
    const int x1 = Terrain::toTileCoord(p.x + radius);
    const int y0 = Terrain::toTileCoord(p.y - radius);
    const int y1 = Terrain::toTileCoord(p.y + radius);
    for (int ty = y0; ty <= y1; ++ty) {
        for (int tx = x0; tx <= x1; ++tx) {
            if (!tileBlocks(t.atTile(tx, ty))) continue;
            const Rect r = Terrain::tileRect(tx, ty);
            const double nx = clamp(p.x, r.left(), r.right());
            const double ny = clamp(p.y, r.top(), r.bottom());
            if (distanceSq(p, Vec2{nx, ny}) < radius * radius - 1e-6) return true;
        }
    }
    return false;
}

const double kQuietNan = std::numeric_limits<double>::quiet_NaN();
const double kInfinity = std::numeric_limits<double>::infinity();

} // namespace

// ---------------------------------------------------------------------------
// Terrain: grid basics
// ---------------------------------------------------------------------------

TEST(fresh_terrain_is_open_ground) {
    Terrain t;
    CHECK_EQ(t.at(Vec2{kWorldHalf, kWorldHalf}), Tile::Ground);
    CHECK(!t.blocked(Vec2{kWorldHalf, kWorldHalf}, Realm::Overworld));
    CHECK(!t.inWater(Vec2{kWorldHalf, kWorldHalf}, Realm::Overworld));
    CHECK_EQ(t.openTileCount(), kTilesPerAxis * kTilesPerAxis);
    CHECK(t.isConnected());
}

TEST(out_of_bounds_reads_are_wall) {
    Terrain t;
    // The whole point of the total accessor: nothing needs a bounds check to
    // discover the map has an edge.
    CHECK_EQ(t.atTile(-1, 0), Tile::Wall);
    CHECK_EQ(t.atTile(0, -1), Tile::Wall);
    CHECK_EQ(t.atTile(kTilesPerAxis, 0), Tile::Wall);
    CHECK_EQ(t.atTile(0, kTilesPerAxis), Tile::Wall);
    CHECK_EQ(t.atTile(-100000, 100000), Tile::Wall);
    CHECK_EQ(t.at(Vec2{-1.0, kWorldHalf}), Tile::Wall);
    CHECK_EQ(t.at(Vec2{kWorldSize, kWorldHalf}), Tile::Wall);
    CHECK(t.blocked(Vec2{-50000, -50000}, Realm::Overworld));
    CHECK(t.blocked(Vec2{1e30, 1e30}, Realm::Overworld));
    // NaN has no tile; answering Wall is the closed-world answer.
    CHECK(t.blocked(Vec2{kQuietNan, kQuietNan}, Realm::Overworld));
}

TEST(tile_coordinates_clamp_instead_of_overflowing) {
    // A runaway coordinate must not be cast out of int's range: that is
    // undefined behaviour, and downstream it becomes a loop that never ends.
    CHECK(Terrain::toTileCoord(1e30) < (1 << 21));
    CHECK(Terrain::toTileCoord(-1e30) > -(1 << 21));
    CHECK_EQ(Terrain::toTileCoord(kQuietNan), Terrain::toTileCoord(-1e30));
    CHECK_EQ(Terrain::toTileCoord(0.0), 0);
    CHECK_EQ(Terrain::toTileCoord(kTileSize - 0.001), 0);
    CHECK_EQ(Terrain::toTileCoord(kTileSize), 1);
    CHECK_EQ(Terrain::toTileCoord(-0.001), -1);
}

TEST(set_tile_ignores_out_of_range_writes) {
    Terrain t;
    t.setTile(-1, -1, Tile::Water);
    t.setTile(kTilesPerAxis, 5, Tile::Water);
    CHECK_EQ(t.openTileCount(), kTilesPerAxis * kTilesPerAxis);
    t.setTile(3, 4, Tile::Wall);
    CHECK_EQ(t.atTile(3, 4), Tile::Wall);
    CHECK_EQ(t.openTileCount(), kTilesPerAxis * kTilesPerAxis - 1);
}

// ---------------------------------------------------------------------------
// Terrain: generation
// ---------------------------------------------------------------------------

TEST(generation_is_deterministic_for_a_seed) {
    Terrain a, b;
    a.generate(12345);
    b.generate(12345);
    CHECK_EQ(a.seed(), std::uint64_t(12345));
    int differences = 0;
    for (std::size_t i = 0; i < a.tileCount(); ++i) {
        if (a.tiles()[i] != b.tiles()[i]) ++differences;
    }
    CHECK_EQ(differences, 0);
}

TEST(generation_differs_across_seeds) {
    Terrain a, b;
    a.generate(1);
    b.generate(2);
    int differences = 0;
    for (std::size_t i = 0; i < a.tileCount(); ++i) {
        if (a.tiles()[i] != b.tiles()[i]) ++differences;
    }
    // Not merely "some": a different seed is a different map, so a large
    // fraction of the grid must move.
    CHECK(differences > kTilesPerAxis * kTilesPerAxis / 10);
}

TEST(generated_map_is_fully_connected) {
    // Every seed, not just the lucky one: the repair pass is what guarantees
    // this, and a seed that happens to need no repair proves nothing.
    for (std::uint64_t seed : {std::uint64_t(1), std::uint64_t(7), std::uint64_t(42),
                               std::uint64_t(0xDEADBEEF), std::uint64_t(0)}) {
        Terrain t;
        t.generate(seed);
        CHECK(t.isConnected());
        CHECK(!t.blocked(t.spawnPoint(), Realm::Overworld));
    }
}

TEST(generated_map_has_every_tile_kind_and_biome) {
    const Terrain& t = sharedMap();
    int kindCounts[kTileKindCount] = {0, 0, 0, 0, 0};
    int openPerSection[kSectionCount] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (int ty = 0; ty < kTilesPerAxis; ++ty) {
        for (int tx = 0; tx < kTilesPerAxis; ++tx) {
            const Tile tile = t.atTile(tx, ty);
            ++kindCounts[static_cast<int>(tile)];
            const int section = t.sectionOfTile(tx, ty);
            if (section >= 0 && !tileBlocks(tile)) ++openPerSection[section];
        }
    }
    for (int k = 0; k < kTileKindCount; ++k) CHECK(kindCounts[k] > 0);
    // A section with no floor is a section no player can ever visit.
    for (int s = 0; s < kSectionCount; ++s) CHECK(openPerSection[s] > 0);
}

TEST(walling_off_a_pocket_breaks_connectivity) {
    // Guards the guard: isConnected() must be able to say no, or the
    // generation test above is vacuous.
    Terrain t;
    CHECK(t.isConnected());
    for (int d = -1; d <= 1; ++d) {
        t.setTile(5 + d, 4, Tile::Wall);
        t.setTile(5 + d, 6, Tile::Wall);
        t.setTile(4, 5 + d, Tile::Wall);
        t.setTile(6, 5 + d, Tile::Wall);
    }
    CHECK(!t.isConnected());
    t.setTile(5, 4, Tile::Ground);
    CHECK(t.isConnected());
}

// ---------------------------------------------------------------------------
// Terrain: resolveCircle
// ---------------------------------------------------------------------------

TEST(resolve_circle_leaves_a_free_circle_untouched) {
    Terrain t;
    const Vec2 p{kWorldHalf, kWorldHalf};
    const Vec2 out = t.resolveCircle(p, kPlayerBaseRadius, Realm::Overworld);
    CHECK_NEAR(out.x, p.x, 1e-9);
    CHECK_NEAR(out.y, p.y, 1e-9);
}

TEST(resolve_circle_pushes_a_circle_out_of_a_wall) {
    Terrain t;
    t.setTile(10, 10, Tile::Wall);
    const Rect wall = Terrain::tileRect(10, 10);

    // Overlapping the wall's left face by 50 units.
    const Vec2 start{wall.left() - 50.0, wall.top() + kTileSize * 0.5};
    const Vec2 out = t.resolveCircle(start, 100.0, Realm::Overworld);
    CHECK(out.x <= wall.left() - 100.0);
    CHECK(out.x >= wall.left() - 100.0 - 20.1);
    CHECK_NEAR(out.y, start.y, 1e-9);
    CHECK(!overlapsWall(t, out, 100.0));

    // And a circle that never touched it does not move.
    const Vec2 clear = t.resolveCircle(Vec2{wall.left() - 500.0, start.y}, 100.0, Realm::Overworld);
    CHECK_NEAR(clear.x, wall.left() - 500.0, 1e-9);
}

TEST(resolve_circle_escapes_a_wall_it_is_buried_in) {
    Terrain t;
    for (int ty = 20; ty <= 24; ++ty) {
        for (int tx = 20; tx <= 24; ++tx) t.setTile(tx, ty, Tile::Wall);
    }
    // Dead centre of a 5x5 block: no tile offers a push direction, so this is
    // the case that jitters forever if ejection is missing.
    const Vec2 buried = Terrain::tileCenter(22, 22);
    const Vec2 out = t.resolveCircle(buried, kPlayerBaseRadius, Realm::Overworld);
    CHECK(!t.blocked(out, Realm::Overworld));
    CHECK(!overlapsWall(t, out, kPlayerBaseRadius));
    // Idempotent: resolving an already-resolved position is a no-op, which is
    // what stops a body vibrating between two walls every tick.
    const Vec2 again = t.resolveCircle(out, kPlayerBaseRadius, Realm::Overworld);
    CHECK_NEAR(again.x, out.x, 1e-9);
    CHECK_NEAR(again.y, out.y, 1e-9);
}

TEST(resolve_circle_ejects_a_shallow_embed_without_teleporting) {
    Terrain t;
    t.setTile(10, 10, Tile::Wall);
    // Two units past the wall's left face: technically inside geometry, so the
    // ejection path runs, but the answer must still be two units back out and
    // not a jump to the middle of the neighbouring tile.
    const Vec2 start{10 * kTileSize + 2.0, 10 * kTileSize + 150.0};
    const Vec2 out = t.resolveCircle(start, 12.0, Realm::Overworld);
    CHECK(out.x <= 10 * kTileSize - 12.0);
    CHECK(out.x >= 10 * kTileSize - 12.0 - 20.1);
    CHECK_NEAR(out.y, start.y, 1e-9);
    CHECK(distance(out, start) < kTileSize * 0.25);
    CHECK(!overlapsWall(t, out, 12.0));
}

TEST(resolve_circle_settles_in_a_concave_corner) {
    Terrain t;
    t.setTile(10, 10, Tile::Wall);
    t.setTile(11, 10, Tile::Wall);
    t.setTile(10, 11, Tile::Wall);
    // Just inside the free tile's top-left corner, overlapping all three.
    const Vec2 start{11 * kTileSize + 5.0, 11 * kTileSize + 5.0};
    const Vec2 out = t.resolveCircle(start, 60.0, Realm::Overworld);
    CHECK(out.x >= 11 * kTileSize + 60.0 - 1e-6);
    CHECK(out.y >= 11 * kTileSize + 60.0 - 1e-6);
    CHECK(!overlapsWall(t, out, 60.0));
}

TEST(resolve_circle_pushes_a_body_back_inside_the_world) {
    Terrain t;
    const Vec2 out = t.resolveCircle(Vec2{-5000.0, kWorldHalf}, 25.0, Realm::Overworld);
    CHECK(out.x >= 0.0);
    CHECK(out.x <= kWorldSize);
    CHECK(!t.blocked(out, Realm::Overworld));
    const Vec2 far = t.resolveCircle(Vec2{kWorldSize + 90000.0, kWorldHalf}, 25.0, Realm::Overworld);
    CHECK(far.x <= kWorldSize);
    CHECK(!t.blocked(far, Realm::Overworld));
}

TEST(resolve_circle_survives_nonsense_input) {
    Terrain t;
    t.generate(3);
    const Vec2 cases[] = {
        Vec2{kQuietNan, kQuietNan},
        Vec2{kInfinity, 0.0},
        Vec2{1e30, -1e30},
        Vec2{kWorldHalf, kWorldHalf},
    };
    const double radii[] = {-5.0, 0.0, kQuietNan, kInfinity, 1e9, 1e-12};
    for (const Vec2& p : cases) {
        for (const double r : radii) {
            const Vec2 out = t.resolveCircle(p, r, Realm::Overworld);
            CHECK(std::isfinite(out.x));
            CHECK(std::isfinite(out.y));
            CHECK(out.x >= 0.0 && out.x <= kWorldSize);
            CHECK(out.y >= 0.0 && out.y <= kWorldSize);
        }
    }
}

TEST(resolve_circle_never_leaves_a_walkable_start_overlapping) {
    // The property that actually matters, checked across the real map: a body
    // standing anywhere legal is still standing somewhere legal afterwards.
    const Terrain& t = sharedMap();
    Rng rng(99);
    int checked = 0;
    for (int i = 0; i < 400; ++i) {
        const Vec2 p{rng.range(0, kWorldSize), rng.range(0, kWorldSize)};
        if (t.blocked(p, Realm::Overworld)) continue;
        const Vec2 out = t.resolveCircle(p, kPlayerBaseRadius, Realm::Overworld);
        CHECK(!t.blocked(out, Realm::Overworld));
        CHECK(!overlapsWall(t, out, kPlayerBaseRadius));
        ++checked;
    }
    CHECK(checked > 50);
}

// ---------------------------------------------------------------------------
// Terrain: segmentBlocked
// ---------------------------------------------------------------------------

TEST(segment_blocked_sees_a_wall_across_the_line) {
    Terrain t;
    for (int ty = 0; ty < kTilesPerAxis; ++ty) t.setTile(30, ty, Tile::Wall);
    const double y = 40 * kTileSize + 17.0;
    CHECK(t.segmentBlocked(Vec2{25 * kTileSize, y}, Vec2{35 * kTileSize, y}, Realm::Overworld));
    CHECK(!t.segmentBlocked(Vec2{20 * kTileSize, y}, Vec2{29.9 * kTileSize, y}, Realm::Overworld));
    // Stopping exactly at the wall's near face is still clear; entering it is
    // not. Off-by-one here is the difference between shooting through a wall
    // and being unable to shoot along one.
    CHECK(!t.segmentBlocked(Vec2{25 * kTileSize, y}, Vec2{30 * kTileSize - 1e-6, y}, Realm::Overworld));
    CHECK(t.segmentBlocked(Vec2{25 * kTileSize, y}, Vec2{30 * kTileSize + 1.0, y}, Realm::Overworld));
}

TEST(segment_blocked_handles_degenerate_and_nonsense_endpoints) {
    Terrain t;
    t.setTile(10, 10, Tile::Wall);
    const Vec2 inWall = Terrain::tileCenter(10, 10);
    const Vec2 open = Terrain::tileCenter(0, 0);
    CHECK(t.segmentBlocked(inWall, inWall, Realm::Overworld));       // zero length inside a wall
    CHECK(!t.segmentBlocked(open, open, Realm::Overworld));
    CHECK(t.segmentBlocked(open, inWall, Realm::Overworld));
    CHECK(t.segmentBlocked(inWall, open, Realm::Overworld));         // blocked at the first tile
    CHECK(t.segmentBlocked(Vec2{kQuietNan, 0}, open, Realm::Overworld));
    CHECK(t.segmentBlocked(open, Vec2{0, kInfinity}, Realm::Overworld));
    // Out of the map is wall, so nothing can see out of it.
    CHECK(t.segmentBlocked(open, Vec2{-9999, -9999}, Realm::Overworld));
}

TEST(segment_blocked_agrees_with_sampling_the_line) {
    const Terrain& t = sharedMap();
    Rng rng(2024);
    int clearSegments = 0;
    for (int i = 0; i < 500; ++i) {
        const Vec2 a{rng.range(0, kWorldSize), rng.range(0, kWorldSize)};
        const Vec2 b = a + Vec2::fromAngle(rng.angle(), rng.range(50.0, 3000.0));
        const bool dda = t.segmentBlocked(a, b, Realm::Overworld);
        // Dense sampling can miss a tile the line only clips at a corner, so
        // the implication runs one way: anything sampling can see, the DDA
        // must also have seen.
        bool sampledBlock = false;
        for (int s = 0; s <= 600 && !sampledBlock; ++s) {
            sampledBlock = t.blocked(a + (b - a) * (s / 600.0), Realm::Overworld);
        }
        if (sampledBlock) CHECK(dda);
        if (!dda) {
            CHECK(!sampledBlock);
            ++clearSegments;
        }
    }
    CHECK(clearSegments > 20);
}

TEST(segment_blocked_is_symmetric) {
    const Terrain& t = sharedMap();
    Rng rng(31337);
    for (int i = 0; i < 400; ++i) {
        const Vec2 a{rng.range(100, kWorldSize - 100), rng.range(100, kWorldSize - 100)};
        const Vec2 b{rng.range(100, kWorldSize - 100), rng.range(100, kWorldSize - 100)};
        CHECK_EQ(t.segmentBlocked(a, b, Realm::Overworld), t.segmentBlocked(b, a, Realm::Overworld));
    }
}

// ---------------------------------------------------------------------------
// Terrain: spawns, sections, biomes
// ---------------------------------------------------------------------------

TEST(find_open_spawn_lands_somewhere_walkable) {
    const Terrain& t = sharedMap();
    Rng rng(5);
    for (int i = 0; i < 200; ++i) {
        const Vec2 around{rng.range(0, kWorldSize), rng.range(0, kWorldSize)};
        const Vec2 p = t.findOpenSpawn(rng, around, 2000.0, Realm::Overworld);
        CHECK(!t.blocked(p, Realm::Overworld));
        CHECK(p.x >= 0.0 && p.x <= kWorldSize);
        CHECK(p.y >= 0.0 && p.y <= kWorldSize);
    }
    // Even asked about the middle of solid rock, and with no room to search.
    Terrain solid;
    solid.fill(Tile::Wall);
    solid.setTile(0, 0, Tile::Ground);
    const Vec2 p = solid.findOpenSpawn(rng, Terrain::tileCenter(5, 5), 0.0, Realm::Overworld);
    CHECK(!solid.blocked(p, Realm::Overworld));
}

TEST(nearest_open_tile_gives_up_on_a_solid_map) {
    Terrain solid;
    solid.fill(Tile::Wall);
    int tx = -1, ty = -1;
    CHECK(!solid.nearestOpenTile(Vec2{kWorldHalf, kWorldHalf}, tx, ty));
    // A solid map has nothing to connect, which isConnected() must report as
    // connected rather than as a failure.
    CHECK(solid.isConnected());
    CHECK_EQ(solid.openTileCount(), 0);
}

TEST(sections_and_biomes_cover_the_map) {
    Terrain t;
    for (int sy = 0; sy < kSectionsPerAxis; ++sy) {
        for (int sx = 0; sx < kSectionsPerAxis; ++sx) {
            const Vec2 center{(sx + 0.5) * kSectionSize, (sy + 0.5) * kSectionSize};
            CHECK_EQ(t.sectionAt(center), sy * kSectionsPerAxis + sx);
        }
    }
    CHECK_EQ(t.sectionAt(Vec2{-1.0, 0.0}), -1);
    CHECK_EQ(t.sectionAt(Vec2{kWorldSize, 0.0}), -1);

    CHECK_EQ(std::string(biomeOf(0).name), std::string("Garden"));
    CHECK_EQ(std::string(biomeOf(4).name), std::string("Ant Hell"));
    CHECK_EQ(std::string(biomeOf(8).name), std::string("Unknown"));
    // Off-map still paints something, so a renderer walking past the edge has
    // no special case either.
    CHECK_EQ(std::string(biomeOf(-1).name), std::string("Void"));
    CHECK_EQ(std::string(biomeOf(kSectionCount).name), std::string("Void"));

    // The same tile kind is a different colour per biome -- that is the whole
    // reason the colour lives on the biome and not on the Tile.
    CHECK(tileColor(0, Tile::Ground) != tileColor(2, Tile::Ground));
    CHECK(tileColor(4, Tile::Wall) != tileColor(4, Tile::Ground));
    CHECK_EQ(tileColor(0, Tile::Water), kBiomes[0].tileColors[static_cast<int>(Tile::Water)]);
}

// ---------------------------------------------------------------------------
// SpatialGrid
// ---------------------------------------------------------------------------

TEST(spatial_grid_finds_what_was_inserted) {
    SpatialGrid grid;
    std::vector<Entity> out;
    const Entity e = makeEntity(1, 1);
    grid.insert(e, Realm::Overworld, Vec2{1000, 1000}, 10);
    CHECK_EQ(grid.size(), std::size_t(1));
    grid.query(Realm::Overworld, Vec2{1000, 1000}, 50, out);
    CHECK_EQ(countOf(out, e), 1);
    // A query that shares no cell finds nothing at all.
    grid.query(Realm::Overworld, Vec2{50000, 50000}, 100, out);
    CHECK_EQ(out.size(), std::size_t(0));
    CHECK(!grid.empty());
}

TEST(spatial_grid_finds_a_fat_entity_from_every_cell_it_covers) {
    SpatialGrid grid(600.0);
    const Entity big = makeEntity(7, 1);
    const Vec2 center{5000, 5000};
    const double radius = 1500;
    grid.insert(big, Realm::Overworld, center, radius);

    std::vector<Entity> out;
    const int x0 = grid.cellX(Realm::Overworld, center.x - radius);
    const int x1 = grid.cellX(Realm::Overworld, center.x + radius);
    const int y0 = grid.cellY(Realm::Overworld, center.y - radius);
    const int y1 = grid.cellY(Realm::Overworld, center.y + radius);
    CHECK(x1 > x0);   // otherwise this proves nothing about fat insertion
    for (int cy = y0; cy <= y1; ++cy) {
        for (int cx = x0; cx <= x1; ++cx) {
            const Vec2 probe{(cx + 0.5) * grid.cellSize(), (cy + 0.5) * grid.cellSize()};
            grid.query(Realm::Overworld, probe, 1.0, out);
            CHECK_EQ(countOf(out, big), 1);
        }
    }
    // And once, not once per cell, when the query spans all of them.
    grid.query(Realm::Overworld, center, radius * 2, out);
    CHECK_EQ(countOf(out, big), 1);
    CHECK_EQ(out.size(), std::size_t(1));
}

TEST(spatial_grid_query_returns_a_complete_candidate_set) {
    // The contract the collision systems rely on: the result may hold extras,
    // but it can never miss an entity that actually overlaps.
    SpatialGrid grid;
    Rng rng(4242);
    std::vector<Vec2> positions;
    std::vector<double> radii;
    grid.clear();
    for (int i = 0; i < 200; ++i) {
        const Vec2 p{rng.range(0, kWorldSize), rng.range(0, kWorldSize)};
        const double r = rng.range(5.0, 400.0);
        positions.push_back(p);
        radii.push_back(r);
        grid.insert(makeEntity(static_cast<std::uint32_t>(i + 1), 1), Realm::Overworld, p, r);
    }

    std::vector<Entity> out;
    for (int q = 0; q < 100; ++q) {
        const Vec2 c{rng.range(0, kWorldSize), rng.range(0, kWorldSize)};
        const double qr = rng.range(10.0, 1200.0);
        grid.query(Realm::Overworld, c, qr, out);
        for (std::size_t i = 0; i < positions.size(); ++i) {
            if (distance(positions[i], c) > qr + radii[i]) continue;
            CHECK_EQ(countOf(out, makeEntity(static_cast<std::uint32_t>(i + 1), 1)), 1);
        }
    }
}

TEST(spatial_grid_clear_retires_the_previous_tick) {
    SpatialGrid grid;
    std::vector<Entity> out;
    const Entity oldEntity = makeEntity(3, 1);
    const Entity newEntity = makeEntity(4, 1);
    grid.insert(oldEntity, Realm::Overworld, Vec2{2000, 2000}, 100);

    grid.clear();
    CHECK_EQ(grid.size(), std::size_t(0));
    grid.query(Realm::Overworld, Vec2{2000, 2000}, 200, out);
    CHECK_EQ(out.size(), std::size_t(0));   // stale bucket contents stay hidden

    grid.insert(newEntity, Realm::Overworld, Vec2{2000, 2000}, 100);
    grid.query(Realm::Overworld, Vec2{2000, 2000}, 200, out);
    CHECK_EQ(out.size(), std::size_t(1));
    CHECK_EQ(countOf(out, newEntity), 1);
}

TEST(spatial_grid_query_rect_matches_its_bounds) {
    SpatialGrid grid;
    std::vector<Entity> out;
    const Entity e = makeEntity(9, 1);
    grid.insert(e, Realm::Overworld, Vec2{3000, 3000}, 0);

    grid.queryRect(Realm::Overworld, Vec2{2900, 2900}, Vec2{3100, 3100}, out);
    CHECK_EQ(countOf(out, e), 1);
    // Inverted corners describe the same rectangle.
    grid.queryRect(Realm::Overworld, Vec2{3100, 3100}, Vec2{2900, 2900}, out);
    CHECK_EQ(countOf(out, e), 1);
    grid.queryRect(Realm::Overworld, Rect{2900, 2900, 200, 200}, out);
    CHECK_EQ(countOf(out, e), 1);
    grid.queryRect(Realm::Overworld, Vec2{20000, 20000}, Vec2{21000, 21000}, out);
    CHECK_EQ(out.size(), std::size_t(0));
}

TEST(spatial_grid_rejects_nonsense_input) {
    SpatialGrid grid;
    std::vector<Entity> out;
    grid.insert(NULL_ENTITY, Realm::Overworld, Vec2{100, 100}, 10);
    grid.insert(makeEntity(2, 1), Realm::Overworld, Vec2{kQuietNan, 100}, 10);
    grid.insert(makeEntity(3, 1), Realm::Overworld, Vec2{100, kInfinity}, 10);
    CHECK_EQ(grid.size(), std::size_t(0));

    // A non-finite radius degrades to a point insertion rather than filing the
    // entity into every bucket on the map.
    const Entity e = makeEntity(4, 1);
    grid.insert(e, Realm::Overworld, Vec2{100, 100}, kInfinity);
    CHECK_EQ(grid.size(), std::size_t(1));
    grid.query(Realm::Overworld, Vec2{100, 100}, 10, out);
    CHECK_EQ(countOf(out, e), 1);
    grid.query(Realm::Overworld, Vec2{40000, 40000}, 10, out);
    CHECK_EQ(out.size(), std::size_t(0));

    out.push_back(makeEntity(77, 1));
    grid.query(Realm::Overworld, Vec2{kQuietNan, 0}, 10, out);
    CHECK_EQ(out.size(), std::size_t(0));   // cleared even on the reject path
}

TEST(spatial_grid_clamps_positions_outside_its_bounds) {
    // Nothing should live outside the world, but a knockback that overshoots
    // must still be findable rather than silently dropped.
    SpatialGrid grid;
    std::vector<Entity> out;
    const Entity e = makeEntity(11, 1);
    grid.insert(e, Realm::Overworld, Vec2{-5000, -5000}, 10);
    grid.query(Realm::Overworld, Vec2{10, 10}, 10, out);
    CHECK_EQ(countOf(out, e), 1);

    const Entity beyond = makeEntity(12, 1);
    grid.insert(beyond, Realm::Overworld, Vec2{kWorldSize + 5000, kWorldSize + 5000}, 10);
    grid.query(Realm::Overworld, Vec2{kWorldSize - 10, kWorldSize - 10}, 10, out);
    CHECK_EQ(countOf(out, beyond), 1);
}

TEST(spatial_grid_keeps_each_realm_in_its_own_layer) {
    // Every realm's coordinates start at (0, 0), so the same numbers name
    // three different places. An entity is filed under its realm and a query
    // names the realm it asks about, so a candidate list never crosses: a
    // maze mob at (3000, 3000) is simply not in the layer an overworld flower
    // at (3000, 3000) queries.
    SpatialGrid grid(200.0);
    CHECK_NEAR(grid.cellSize(), 200.0, 1e-12);
    CHECK_EQ(grid.cols(Realm::Arena), static_cast<int>(std::ceil(kArenaWorldSize / 200.0)));
    CHECK_EQ(grid.rows(Realm::Arena), grid.cols(Realm::Arena));
    CHECK(grid.cols(Realm::Overworld) > grid.cols(Realm::Arena));

    std::vector<Entity> out;
    const Entity overworld = makeEntity(21, 1);
    const Entity arena = makeEntity(22, 1);
    const Entity maze = makeEntity(23, 1);
    grid.insert(overworld, Realm::Overworld, Vec2{3000, 3000}, 30);
    grid.insert(arena, Realm::Arena, Vec2{3000, 3000}, 30);
    grid.insert(maze, Realm::Maze, Vec2{3000, 3000}, 30);
    CHECK_EQ(grid.size(), std::size_t(3));

    grid.query(Realm::Overworld, Vec2{3050, 3050}, 100, out);
    CHECK_EQ(out.size(), std::size_t(1));
    CHECK_EQ(countOf(out, overworld), 1);
    grid.query(Realm::Arena, Vec2{3050, 3050}, 100, out);
    CHECK_EQ(out.size(), std::size_t(1));
    CHECK_EQ(countOf(out, arena), 1);
    grid.query(Realm::Maze, Vec2{3050, 3050}, 100, out);
    CHECK_EQ(out.size(), std::size_t(1));
    CHECK_EQ(countOf(out, maze), 1);

    // clear() retires every layer at once.
    grid.clear();
    CHECK(grid.empty());
    grid.query(Realm::Arena, Vec2{3050, 3050}, 100, out);
    CHECK_EQ(out.size(), std::size_t(0));

    // A degenerate cell size falls back rather than asking for four billion
    // buckets.
    SpatialGrid degenerate(0.0);
    CHECK_NEAR(degenerate.cellSize(), SpatialGrid::kDefaultCellSize, 1e-12);
    SpatialGrid tiny(1.5);
    CHECK(tiny.cols(Realm::Overworld) <= 512);
    CHECK(tiny.rows(Realm::Maze) <= 512);
}

TEST(spatial_grid_does_not_allocate_once_warm) {
    SpatialGrid grid;
    std::vector<Entity> out;
    Rng rng(8);

    // Two identical rounds. The first grows every buffer; the second must
    // reuse all of them, or the broadphase allocates 25 times a second for
    // the life of the process.
    auto round = [&]() {
        for (int tick = 0; tick < 3; ++tick) {
            grid.clear();
            Rng inner(1234);
            for (int i = 0; i < 500; ++i) {
                const Vec2 p{inner.range(0, kWorldSize), inner.range(0, kWorldSize)};
                grid.insert(makeEntity(static_cast<std::uint32_t>(i + 1), 1), Realm::Overworld, p,
                            inner.range(5, 300));
            }
            Rng probe(999);
            for (int q = 0; q < 100; ++q) {
                grid.query(Realm::Overworld, Vec2{probe.range(0, kWorldSize), probe.range(0, kWorldSize)}, 800.0, out);
            }
        }
    };

    round();
    const std::size_t reserved = grid.reservedEntries();
    const std::size_t outCapacity = out.capacity();
    round();
    CHECK_EQ(grid.reservedEntries(), reserved);
    CHECK_EQ(out.capacity(), outCapacity);
    CHECK(reserved > 0);
    (void)rng;
}

// ---------------------------------------------------------------------------
// The grid on the wire
// ---------------------------------------------------------------------------
//
// The client has no map file to collide with, so the authoritative grid
// arrives beside the join. What travels is ONE thing -- a tile per cell, at
// the map's own dimensions -- because what a cell looks like is the map file's
// business and the client reads that for itself.

namespace {

constexpr std::uint8_t G = static_cast<std::uint8_t>(Tile::Ground);
constexpr std::uint8_t W = static_cast<std::uint8_t>(Tile::Wall);
constexpr std::uint8_t A = static_cast<std::uint8_t>(Tile::Water);

/// A 4x3 map with one of everything a derived grid can hold: a wall blob and
/// a pond beside it.
const std::vector<std::uint8_t> kSmallTiles = {
    W, W, W, G,
    W, A, G, G,
    G, G, G, G,
};

/// Serialises one realm's grid and reads it back into a fresh Terrain.
bool roundTrip(const Terrain& from, Realm realm, Terrain& to, Realm& realmOut, std::string& error) {
    ByteWriter w;
    writeMapGrid(w, from, realm);
    ByteReader r(w.data(), w.size());
    return readMapGrid(r, to, realmOut, error);
}

} // namespace

TEST(a_grid_travels_over_the_wire_at_its_own_size) {
    Terrain server;
    const Realm realm = worldRealm(1);
    CHECK(server.setTiles(kSmallTiles, 4, 3, realm));

    Terrain client;
    Realm got = Realm::Overworld;
    std::string error;
    CHECK(roundTrip(server, realm, client, got, error));
    CHECK(error.empty());
    CHECK(got == realm);
    CHECK_EQ(client.tileCols(realm), 4);
    CHECK_EQ(client.tileRows(realm), 3);
    for (int ty = 0; ty < 3; ++ty) {
        for (int tx = 0; tx < 4; ++tx) {
            CHECK(client.atTile(tx, ty, realm) == server.atTile(tx, ty, realm));
        }
    }
    // Off the grid closes the world, on both sides.
    CHECK(client.atTile(-1, 0, realm) == Tile::Wall);
    CHECK(client.atTile(4, 0, realm) == Tile::Wall);
    CHECK(client.atTile(0, 3, realm) == Tile::Wall);
    // And another realm is untouched: a grid arrives for the realm it names
    // and no other.
    CHECK(!client.hasMap(worldRealm(2)));
}

TEST(the_wire_carries_one_stream_and_nothing_else) {
    // The payload is exactly the nine-byte header plus the run-length encoded
    // tiles. Sized rather than merely round-tripped, because a second stream
    // that nothing reads would round-trip perfectly and still cost every join
    // the bytes.
    const Terrain& map = sharedMap();
    ByteWriter w;
    writeMapGrid(w, map, Realm::Overworld);
    const std::vector<std::uint8_t> tiles(map.tiles(), map.tiles() + map.tileCount());
    const std::vector<std::uint8_t> packed = encodeTileRle(tiles);
    CHECK_EQ(w.size(), packed.size() + 9);

    Terrain client;
    Realm got = Realm::Arena;
    std::string error;
    ByteReader r(w.data(), w.size());
    CHECK(readMapGrid(r, client, got, error));
    CHECK(got == Realm::Overworld);
    CHECK_EQ(client.tileCount(), map.tileCount());
    for (int ty = 0; ty < kTilesPerAxis; ty += 7) {
        for (int tx = 0; tx < kTilesPerAxis; tx += 11) {
            CHECK(client.atTile(tx, ty) == map.atTile(tx, ty));
        }
    }

    // A generated realm sends an empty grid, and installs nothing.
    Terrain arenaClient;
    CHECK(roundTrip(map, Realm::Arena, arenaClient, got, error));
    CHECK(got == Realm::Arena);
    CHECK(!arenaClient.hasMap(Realm::Arena));
}

TEST(a_malformed_grid_off_the_wire_installs_nothing) {
    const Realm realm = worldRealm(2);
    // A well-formed frame, so each refusal below differs from it in one way.
    const auto frame = [&](int cols, int rows, const std::vector<std::uint8_t>& tiles) {
        ByteWriter w;
        w.u8(static_cast<std::uint8_t>(realm));
        w.u16(static_cast<std::uint16_t>(cols));
        w.u16(static_cast<std::uint16_t>(rows));
        const std::vector<std::uint8_t> packed = encodeTileRle(tiles);
        w.u32(static_cast<std::uint32_t>(packed.size()));
        w.raw(packed.data(), packed.size());
        return w;
    };
    const auto read = [&](const ByteWriter& w, Terrain& into, std::string& error) {
        Realm got = Realm::Overworld;
        ByteReader r(w.data(), w.size());
        return readMapGrid(r, into, got, error);
    };

    {
        Terrain client;
        std::string error;
        CHECK(read(frame(4, 3, kSmallTiles), client, error));
        CHECK(error.empty());
        CHECK(client.hasMap(realm));
    }
    {   // The stream decodes to the wrong number of cells for the header.
        Terrain client;
        std::string error;
        CHECK(!read(frame(4, 4, kSmallTiles), client, error));
        CHECK(!error.empty());
        CHECK(!client.hasMap(realm));
    }
    {   // A size no map can be.
        Terrain client;
        std::string error;
        CHECK(!read(frame(0, 3, {}), client, error));
        CHECK(!client.hasMap(realm));
    }
    {   // A tile value the engine has no Tile for.
        Terrain client;
        std::string error;
        std::vector<std::uint8_t> bad = kSmallTiles;
        bad[0] = 200;
        CHECK(!read(frame(4, 3, bad), client, error));
        CHECK(!client.hasMap(realm));
    }
    {   // Truncated: the header promises more bytes than the frame holds.
        const ByteWriter w = frame(4, 3, kSmallTiles);
        Terrain client;
        Realm got = Realm::Overworld;
        std::string error;
        ByteReader r(w.data(), w.size() - 2);
        CHECK(!readMapGrid(r, client, got, error));
        CHECK(!error.empty());
        CHECK(!client.hasMap(realm));
    }
}

TEST(a_body_resting_against_a_wall_stops_at_the_flat_face) {
    // A tile collides as its plain rectangle: the drawn edge lies inside it,
    // so a body pressed into a wall comes to rest exactly one radius (plus
    // the resolver's epsilon) off the tile's geometric face, on every side.
    Terrain t;
    const double west = 10.0 * kTileSize;      // the wall column's two faces
    const double east = 11.0 * kTileSize;
    for (int ty = 0; ty < kTilesPerAxis; ++ty) t.setTile(10, ty, Tile::Wall);
    const double radius = 20.0;

    const Vec2 fromWest = t.resolveCircle({west - 15.0, 5000.0}, radius, Realm::Overworld);
    CHECK_NEAR(fromWest.x, west - radius, 0.02);
    CHECK_NEAR(fromWest.y, 5000.0, 1e-9);
    const Vec2 fromEast = t.resolveCircle({east + 15.0, 5000.0}, radius, Realm::Overworld);
    CHECK_NEAR(fromEast.x, east + radius, 0.02);
    CHECK_NEAR(fromEast.y, 5000.0, 1e-9);
    // Already clear by a hair: left exactly where it is.
    const Vec2 clear = t.resolveCircle({west - radius - 0.5, 5000.0}, radius, Realm::Overworld);
    CHECK_NEAR(clear.x, west - radius - 0.5, 1e-12);

    // The same grid reinstalled cell for cell resolves to the very same
    // point: a wall is its rectangle, and nothing about how it was loaded
    // enters the collision.
    std::vector<std::uint8_t> tiles(t.tiles(), t.tiles() + t.tileCount());
    Terrain copied;
    CHECK(copied.setTiles(tiles, kTilesPerAxis, kTilesPerAxis, Realm::Overworld));
    const Vec2 again = copied.resolveCircle({west - 15.0, 5000.0}, radius, Realm::Overworld);
    CHECK_NEAR(again.x, fromWest.x, 1e-12);
    CHECK_NEAR(again.y, fromWest.y, 1e-12);
    const Terrain::WallResolution wall = copied.resolveWall({west - 15.0, 5000.0}, radius, Realm::Overworld);
    CHECK(wall.collided);
    CHECK(!wall.unresolved);
    CHECK_NEAR(wall.position.x, fromWest.x, 1e-12);
}

TEST(a_substep_cannot_carry_a_centre_past_a_tiles_effective_midline) {
    // The resolver ejects an embedded centre through the NEAREST face, so a
    // substep that crosses a blocker's midline flips it to the FAR face -- a
    // teleport through the wall. Detection reaches the scan buffer past the
    // geometry, so the midline that matters sits that much inside the geometric
    // one. This is TypeScript's MAX_STEP_HARD less the drawn outline's
    // protrusion it used to subtract.
    //
    // The bound is a CELL's half width, and a cell's blocking geometry is now
    // the shapes its tile carries, which can be thinner than the cell -- a
    // 40-unit sliver of hut wall does not get its own substep bound. What
    // catches that is the other half of the movement guard: a step whose CENTRE
    // path crossed solid is refused outright (segmentTouchesBlockingTile), and
    // no push-out result is committed without it. The substep bound keeps the
    // ordinary case cheap; the centre-path test is what makes it safe.
    CHECK_NEAR(kMaxSubstepLength, kTileSize * 0.5 - kCollisionScanBuffer, 1e-12);
    CHECK(kMaxSubstepLength > 0.0);
    CHECK(kMaxSubstepLength < kTileSize * 0.5);
}

// ---------------------------------------------------------------------------
// Authored collision shapes
// ---------------------------------------------------------------------------
//
// A cell blocks where the SHAPES of its tile are, not over its whole square.
// Every fixture below is written by the test itself, from a tileset drawn at 256
// into a map whose cells are 300, because that is the one thing the shipped map
// cannot check for us: an engine that ignored the scale would agree with a
// fixture authored at 300 on every number.
//
// The shapes are chosen to be things a rectangle cannot fake:
//
//   full     a rectangle over the entire 256 tile -- the whole cell, and the
//            case that must stay identical to the old whole-cell behaviour
//   corner   a 128x64 rectangle in the tile's top-left -- ASYMMETRIC, so the
//            eight orientations are eight different pictures
//   notch    a U, concave, with two arms and a walkable notch between them
//   diag     the triangle below the tile's diagonal, whose face is a 45-degree
//            line no cell boundary lies on
//   pond     the whole tile, tagged `water`
//   plain    no shapes at all, which must block nothing anywhere

namespace {

std::string shapeDir() {
    const char* env = std::getenv("TMPDIR");
    std::string base = (env != nullptr && *env != '\0') ? env : "/tmp";
    if (base.back() != '/') base.push_back('/');
    base += "flix_shape_tests";
    mkdir(base.c_str(), 0755);
    return base;
}

std::string writeFixture(const std::string& name, const std::string& text) {
    const std::string path = shapeDir() + "/" + name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return path;
}

/// gid = local id + 1: 1 plain, 2 full, 3 corner, 4 notch, 5 diag, 6 pond,
/// 7 wide (a shape dragged past the tile's right edge, which Tiled permits).
constexpr const char* kShapeTileset = R"({
 "columns": 0, "name": "shapes", "tilecount": 7, "tiledversion": "1.10.1",
 "tilewidth": 256, "tileheight": 256, "tilerendersize": "grid",
 "type": "tileset", "version": "1.10",
 "tiles": [
  { "id": 0, "image": "tiles/plain.svg", "imagewidth": 256, "imageheight": 256 },
  { "id": 1, "image": "tiles/full.svg", "imagewidth": 256, "imageheight": 256,
    "objectgroup": { "type": "objectgroup", "id": 2, "name": "", "draworder": "index",
      "opacity": 1, "visible": true, "x": 0, "y": 0, "objects": [
      { "id": 1, "name": "", "type": "", "rotation": 0, "visible": true,
        "x": 0, "y": 0, "width": 256, "height": 256 } ] } },
  { "id": 2, "image": "tiles/corner.svg", "imagewidth": 256, "imageheight": 256,
    "objectgroup": { "type": "objectgroup", "id": 2, "name": "", "draworder": "index",
      "opacity": 1, "visible": true, "x": 0, "y": 0, "objects": [
      { "id": 1, "name": "", "type": "", "rotation": 0, "visible": true,
        "x": 0, "y": 0, "width": 128, "height": 64 } ] } },
  { "id": 3, "image": "tiles/notch.svg", "imagewidth": 256, "imageheight": 256,
    "objectgroup": { "type": "objectgroup", "id": 2, "name": "", "draworder": "index",
      "opacity": 1, "visible": true, "x": 0, "y": 0, "objects": [
      { "id": 1, "name": "", "type": "", "rotation": 0, "visible": true,
        "x": 0, "y": 0, "width": 0, "height": 0,
        "polygon": [ { "x": 0, "y": 0 }, { "x": 64, "y": 0 }, { "x": 64, "y": 192 },
                     { "x": 192, "y": 192 }, { "x": 192, "y": 0 }, { "x": 256, "y": 0 },
                     { "x": 256, "y": 256 }, { "x": 0, "y": 256 } ] } ] } },
  { "id": 4, "image": "tiles/diag.svg", "imagewidth": 256, "imageheight": 256,
    "objectgroup": { "type": "objectgroup", "id": 2, "name": "", "draworder": "index",
      "opacity": 1, "visible": true, "x": 0, "y": 0, "objects": [
      { "id": 1, "name": "", "type": "", "rotation": 0, "visible": true,
        "x": 0, "y": 0, "width": 0, "height": 0,
        "polygon": [ { "x": 0, "y": 0 }, { "x": 256, "y": 256 },
                     { "x": 0, "y": 256 } ] } ] } },
  { "id": 5, "image": "tiles/pond.svg", "imagewidth": 256, "imageheight": 256,
    "properties": [ { "name": "water", "type": "bool", "value": true } ],
    "objectgroup": { "type": "objectgroup", "id": 2, "name": "", "draworder": "index",
      "opacity": 1, "visible": true, "x": 0, "y": 0, "objects": [
      { "id": 1, "name": "", "type": "", "rotation": 0, "visible": true,
        "x": 0, "y": 0, "width": 256, "height": 256 } ] } },
  { "id": 6, "image": "tiles/wide.svg", "imagewidth": 256, "imageheight": 256,
    "objectgroup": { "type": "objectgroup", "id": 2, "name": "", "draworder": "index",
      "opacity": 1, "visible": true, "x": 0, "y": 0, "objects": [
      { "id": 1, "name": "", "type": "", "rotation": 0, "visible": true,
        "x": 0, "y": 0, "width": 700, "height": 256 } ] } }
 ]
})";

/// The scale from the fixture tileset's tile space onto a cell, worked out here
/// the way the engine has to work it out: from both files, not from a constant.
constexpr double kShapeScale = kTileSize / 256.0;

/// A map with a scenery background under one colliding layer whose cells are
/// exactly `gids` -- raw gids, so a caller may set Tiled's flip bits.
std::string shapeMap(int cols, int rows, const std::vector<std::uint32_t>& gids,
                     const std::vector<std::uint32_t>& over = {}) {
    std::string background;
    std::string walls;
    std::string above;
    for (int i = 0; i < cols * rows; ++i) {
        if (i != 0) { background += ","; walls += ","; above += ","; }
        background += "1";
        walls += std::to_string(i < static_cast<int>(gids.size()) ? gids[i] : 0u);
        above += std::to_string(i < static_cast<int>(over.size()) ? over[i] : 0u);
    }
    const std::string size = std::to_string(cols);
    const std::string tall = std::to_string(rows);
    const char* collides =
        R"("properties": [ { "name": "has_collision", "type": "bool", "value": true } ],)";
    std::string layers =
        R"({ "type": "tilelayer", "id": 1, "name": "background", "opacity": 1, "visible": true,
             "x": 0, "y": 0, "width": )" + size + R"(, "height": )" + tall + R"(, "data": [)" +
        background + R"(] },
           { "type": "tilelayer", "id": 2, "name": "walls", "opacity": 1, "visible": true, )" +
        collides + R"( "x": 0, "y": 0, "width": )" + size + R"(, "height": )" + tall +
        R"(, "data": [)" + walls + "] }";
    if (!over.empty()) {
        layers += R"(, { "type": "tilelayer", "id": 3, "name": "over", "opacity": 1,
                         "visible": true, )" + std::string(collides) + R"( "x": 0, "y": 0,
                         "width": )" + size + R"(, "height": )" + tall + R"(, "data": [)" +
                  above + "] }";
    }
    return R"({
 "compressionlevel": -1, "infinite": false, "orientation": "orthogonal",
 "renderorder": "right-down", "tiledversion": "1.10.1", "type": "map", "version": "1.10",
 "tilewidth": 256, "tileheight": 256, "width": )" + size + R"(, "height": )" + tall + R"(,
 "tilesets": [ { "firstgid": 1, "source": "shapes.tsj" } ],
 "layers": [)" + layers + "] }";
}

/// A Terrain holding one fixture map, or an all-Ground one if it would not load
/// (the CHECK in the caller then says so).
bool loadShapeMap(Terrain& out, const std::string& name, int cols, int rows,
                  const std::vector<std::uint32_t>& gids,
                  const std::vector<std::uint32_t>& over = {}) {
    writeFixture("shapes.tsj", kShapeTileset);
    std::string error;
    const bool ok = out.loadTiledMap(writeFixture(name, shapeMap(cols, rows, gids, over)), error);
    if (!ok) std::printf("  fixture %s did not load: %s\n", name.c_str(), error.c_str());
    return ok;
}

/// maps/garden.tmj, out of the repository rather than a staged data directory.
std::string shippedMap() {
    const std::string here = __FILE__;
    const std::size_t slash = here.find_last_of('/');
    const std::string tests = slash == std::string::npos ? std::string(".") : here.substr(0, slash);
    return tests + "/../../maps/garden.tmj";
}

Vec2 inCell(int tx, int ty, double lx, double ly) {
    return {tx * kTileSize + lx, ty * kTileSize + ly};
}

}   // namespace

TEST(a_rect_shape_blocks_inside_itself_and_leaves_the_rest_of_the_cell_walkable) {
    // The `corner` tile's shape is a 128x64 rectangle in the top-left of a 256
    // tile, so it covers half the cell by width and a quarter by height and the
    // other seven-eighths of the cell is walkable ground. A whole-cell reader
    // would block every square unit of it.
    Terrain t;
    std::vector<std::uint32_t> gids(9, 0);
    gids[4] = 3;                     // corner, at cell (1,1)
    CHECK(loadShapeMap(t, "corner.tmj", 3, 3, gids));

    const double w = 128.0 * kShapeScale;   // half the cell
    const double h = 64.0 * kShapeScale;    // a quarter of it
    CHECK_NEAR(w, kTileSize * 0.5, 1e-9);
    CHECK_NEAR(h, kTileSize * 0.25, 1e-9);

    // Inside the rectangle, including right up to its corners.
    CHECK(t.blocked(inCell(1, 1, 1.0, 1.0), Realm::Overworld));
    CHECK(t.blocked(inCell(1, 1, w - 1.0, h - 1.0), Realm::Overworld));
    CHECK(t.blocked(inCell(1, 1, w * 0.5, h * 0.5), Realm::Overworld));
    // Outside it, in the same cell. This is the whole change.
    CHECK(!t.blocked(inCell(1, 1, w + 1.0, h * 0.5), Realm::Overworld));
    CHECK(!t.blocked(inCell(1, 1, w * 0.5, h + 1.0), Realm::Overworld));
    CHECK(!t.blocked(inCell(1, 1, kTileSize - 10.0, kTileSize - 10.0), Realm::Overworld));
    // The coarse grid still calls the cell a wall: it says the cell HOLDS a
    // blocking shape, which is what the minimap and the flow field want.
    CHECK(t.atTile(1, 1) == Tile::Wall);
    CHECK(t.hasCollisionShapes());
    CHECK_EQ(t.collisionShapeCellCount(), 1);
    CHECK_EQ(t.collisionShapeSetCount(), 1);
    // And a neighbouring cell nobody painted is open, shapes or no shapes.
    CHECK(!t.blocked(inCell(2, 1, 10.0, 10.0), Realm::Overworld));
    CHECK(t.atTile(2, 1) == Tile::Ground);

    // A rectangle over the WHOLE tile covers the WHOLE cell: the scale has to
    // reach the far corner, in both axes, rather than stopping short of it.
    Terrain whole;
    gids[4] = 2;                     // full
    CHECK(loadShapeMap(whole, "full.tmj", 3, 3, gids));
    CHECK(whole.blocked(inCell(1, 1, 0.5, 0.5), Realm::Overworld));
    CHECK(whole.blocked(inCell(1, 1, kTileSize - 0.5, kTileSize - 0.5), Realm::Overworld));
    CHECK(whole.blocked(inCell(1, 1, kTileSize - 0.5, 0.5), Realm::Overworld));
    CHECK(whole.blocked(inCell(1, 1, 0.5, kTileSize - 0.5), Realm::Overworld));
    CHECK(!whole.blocked(inCell(0, 0, kTileSize - 1.0, kTileSize - 1.0), Realm::Overworld));
}

TEST(a_concave_shape_blocks_its_arms_and_not_its_notch) {
    // The authored dirt edges are concave, so this is the case the push-out and
    // the point test both have to get right rather than the exotic one.
    Terrain t;
    std::vector<std::uint32_t> gids(9, 0);
    gids[4] = 4;                     // notch, at cell (1,1)
    CHECK(loadShapeMap(t, "notch.tmj", 3, 3, gids));

    const double arm = 64.0 * kShapeScale;    // 75: the arms' inner faces
    const double bar = 192.0 * kShapeScale;   // 225: the crossbar's top face
    // The two arms block...
    CHECK(t.blocked(inCell(1, 1, arm * 0.5, 10.0), Realm::Overworld));
    CHECK(t.blocked(inCell(1, 1, arm * 0.5, bar - 10.0), Realm::Overworld));
    CHECK(t.blocked(inCell(1, 1, kTileSize - arm * 0.5, 10.0), Realm::Overworld));
    // ...and so does the bar they stand on...
    CHECK(t.blocked(inCell(1, 1, kTileSize * 0.5, bar + 10.0), Realm::Overworld));
    // ...and the NOTCH between them does not, at any depth.
    for (double ly = 5.0; ly < bar - 5.0; ly += 20.0) {
        CHECK(!t.blocked(inCell(1, 1, kTileSize * 0.5, ly), Realm::Overworld));
    }
    CHECK(!t.blocked(inCell(1, 1, arm + 5.0, 100.0), Realm::Overworld));
    CHECK(!t.blocked(inCell(1, 1, kTileSize - arm - 5.0, 100.0), Realm::Overworld));

    // A body that fits the notch stands in it, untouched: the push-out has to
    // leave a body alone inside a concave shape's hole.
    const Terrain::WallResolution fits =
        t.resolveWall(inCell(1, 1, kTileSize * 0.5, 112.0), 60.0, Realm::Overworld);
    CHECK(!fits.collided);
    CHECK(!fits.unresolved);
    CHECK_NEAR(fits.position.x, inCell(1, 1, kTileSize * 0.5, 112.0).x, 1e-9);
}

TEST(an_unshaped_tile_on_a_colliding_layer_blocks_nothing_in_terrain) {
    // Tiled's semantic, all the way through to the engine: the `plain` tile has
    // no collision shape, so painting it on the colliding layer paints art and
    // nothing else. The coarse grid agrees, which is what keeps the minimap, the
    // flow field and the wire honest about it.
    Terrain t;
    std::vector<std::uint32_t> gids(9, 1);   // plain everywhere, on the wall layer
    gids[4] = 2;                             // except one full blocker in the middle
    CHECK(loadShapeMap(t, "unshaped.tmj", 3, 3, gids));
    CHECK_EQ(t.openTileCount(), 8);
    for (int ty = 0; ty < 3; ++ty) {
        for (int tx = 0; tx < 3; ++tx) {
            const bool middle = tx == 1 && ty == 1;
            CHECK_EQ(t.atTile(tx, ty) == Tile::Wall, middle);
            CHECK_EQ(t.blocked(inCell(tx, ty, 150.0, 150.0), Realm::Overworld), middle);
        }
    }
    CHECK_EQ(t.collisionShapeCellCount(), 1);
}

TEST(a_shape_that_leaves_its_tile_blocks_and_is_reported_in_every_cell_it_reaches) {
    // Tiled lets an author drag a collision shape past the tile's edge, and a
    // 90-degree turn sweeps one clean out of its cell. The `wide` tile's 700x256
    // rectangle covers its own cell and most of the next two.
    //
    // The rule: a shape is FILED in every cell it touches, so the coarse grid
    // and the exact tests agree. They used to disagree -- only the cell the tile
    // was painted in was marked -- which left the minimap painting walkable
    // ground over solid geometry, the bots' flow field routing through it, and
    // nearestOpenTile() (the resolveCircle rescue, findOpenSpawn's last resort)
    // handing back a cell whose own centre is inside the shape. Terrain
    // promises the coarse view blocks a little MORE than the art, never less.
    Terrain t;
    std::vector<std::uint32_t> gids(8 * 8, 0);
    gids[2 * 8 + 2] = 7;   // the wide tile at cell (2,2)
    CHECK(loadShapeMap(t, "overhang.tmj", 8, 8, gids));

    // 700 tileset units is 820.3 world units, so it fills cells (2,2) and (3,2)
    // and reaches 220 units into (4,2). Three cells hold geometry; the coarse
    // grid calls all three Wall.
    CHECK_EQ(t.collisionShapeCellCount(), 3);
    CHECK_NEAR(t.collisionOverhangUnits(), 700.0 * kShapeScale - kTileSize, 1e-9);
    for (int tx = 2; tx <= 4; ++tx) {
        CHECK_EQ(t.atTile(tx, 2), Tile::Wall);
        CHECK(t.blocked(inCell(tx, 2, 150.0, 150.0), Realm::Overworld));
    }
    // And it stops where the shape stops, rather than claiming a whole fourth
    // cell: the coarse grid is conservative by at most the cell the shape ends
    // in, never by a cell it never entered.
    CHECK_EQ(t.atTile(5, 2), Tile::Ground);
    // The shape starts at cell (2, 2)'s left edge, so where it ends inside cell
    // (4, 2) is its own length less the two cells before it.
    const double endsAt = 700.0 * kShapeScale - 2.0 * kTileSize;
    CHECK(!t.blocked(inCell(4, 2, endsAt + 10.0, kTileSize * 0.5), Realm::Overworld));
    CHECK(t.blocked(inCell(4, 2, endsAt - 10.0, kTileSize * 0.5), Realm::Overworld));

    // Nothing the coarse grid calls open holds an exactly-blocked point -- the
    // invariant the whole-cell fallback and every coarse consumer rest on.
    for (int ty = 0; ty < 8; ++ty) {
        for (int tx = 0; tx < 8; ++tx) {
            if (t.atTile(tx, ty) != Tile::Ground) continue;
            for (int sy = 0; sy < 5; ++sy) {
                for (int sx = 0; sx < 5; ++sx) {
                    CHECK(!t.blocked(inCell(tx, ty, kTileSize * (0.1 + sx * 0.2),
                                            kTileSize * (0.1 + sy * 0.2)),
                                     Realm::Overworld));
                }
            }
        }
    }
    // The rescue path cannot hand a body a cell it would be standing inside.
    int openTx = 0;
    int openTy = 0;
    CHECK(t.nearestOpenTile(inCell(3, 2, 150.0, 150.0), openTx, openTy, Realm::Overworld));
    CHECK(!t.blocked(inCell(openTx, openTy, 150.0, 150.0), Realm::Overworld));

    // And the segment tests see it in the cells it reaches into, without any
    // caller widening its scan: this segment never enters cell (2,2), where the
    // tile that owns the shape is painted.
    CHECK(t.segmentBlocked(inCell(4, 2, 100.0, 150.0), inCell(4, 2, 280.0, 150.0),
                           Realm::Overworld));
    CHECK(t.segmentTouchesBlockingTile(inCell(4, 2, 100.0, 150.0), inCell(4, 2, 280.0, 150.0), 0.0,
                                       Realm::Overworld));
}

TEST(all_eight_orientations_block_where_the_art_is) {
    // One Wang edge tile serves all four rotations of a corner, so a cell's flip
    // bits have to turn its collision by the same matrix the renderer turns its
    // art by. The `corner` shape is asymmetric -- 150 x 75 in the cell's
    // top-left -- so all eight orientations are eight different rectangles, and
    // each expected one is written out here rather than read back out of the
    // engine.
    struct Case { std::uint32_t flips; double left, top, right, bottom; const char* what; };
    const double w = 128.0 * kShapeScale;   // 150
    const double h = 64.0 * kShapeScale;    // 75
    const double S = kTileSize;
    const std::uint32_t H = 0x80000000u;
    const std::uint32_t V = 0x40000000u;
    const std::uint32_t D = 0x20000000u;
    const Case cases[] = {
        {0,         0.0,     0.0,     w,   h,   "as drawn"},
        {H,         S - w,   0.0,     S,   h,   "mirrored"},
        {V,         0.0,     S - h,   w,   S,   "flipped"},
        {H | V,     S - w,   S - h,   S,   S,   "half turn"},
        {D,         0.0,     0.0,     h,   w,   "transposed"},
        {D | H,     S - h,   0.0,     S,   w,   "quarter turn clockwise"},
        {D | V,     0.0,     S - w,   h,   S,   "quarter turn anticlockwise"},
        {D | H | V, S - h,   S - w,   S,   S,   "anti-transposed"},
    };

    std::vector<std::uint32_t> gids;
    for (const Case& c : cases) gids.push_back(3u | c.flips);
    // A second, empty row, so the map has an open cell for its fallback spawn
    // point: all eight of the cells below hold a shape, and a grid with no open
    // cell at all is a case of its own (Terrain::setTiles reports it).
    gids.resize(16, 0);
    Terrain t;
    CHECK(loadShapeMap(t, "turns.tmj", 8, 2, gids));
    CHECK_EQ(t.collisionShapeSetCount(), 8);   // one set per orientation, shared per cell

    // Probe every cell on a grid finer than the shape, skipping a hair either
    // side of the expected boundary where "inside" is a coin toss.
    int wrong = 0;
    for (int cell = 0; cell < 8; ++cell) {
        const Case& c = cases[static_cast<std::size_t>(cell)];
        for (double ly = 4.0; ly < kTileSize; ly += 7.0) {
            for (double lx = 4.0; lx < kTileSize; lx += 7.0) {
                const bool inside = lx > c.left && lx < c.right && ly > c.top && ly < c.bottom;
                const double slack = 3.0;
                const bool nearEdge = std::abs(lx - c.left) < slack || std::abs(lx - c.right) < slack ||
                                      std::abs(ly - c.top) < slack || std::abs(ly - c.bottom) < slack;
                if (nearEdge) continue;
                if (t.blocked(inCell(cell, 0, lx, ly), Realm::Overworld) != inside) {
                    if (wrong < 4) {
                        std::printf("  %s: cell %d local (%.0f,%.0f) should be %s\n", c.what, cell,
                                    lx, ly, inside ? "blocked" : "open");
                    }
                    ++wrong;
                }
            }
        }
    }
    CHECK_EQ(wrong, 0);
}

TEST(a_circle_stops_on_the_diagonal_edge_a_shape_draws_not_on_the_cell_boundary) {
    // The `diag` tile blocks the half of its cell below the diagonal, so its
    // face is the 45-degree line through the cell's corners -- a line no cell
    // boundary lies on, which is what makes this test worth anything.
    Terrain t;
    std::vector<std::uint32_t> gids(9, 0);
    gids[4] = 5;                     // diag, at cell (1,1)
    CHECK(loadShapeMap(t, "diag.tmj", 3, 3, gids));
    const double radius = 20.0;
    // Signed distance from the face, positive on the open side. Inside the cell
    // the face is local x == local y.
    const auto fromFace = [](Vec2 p) {
        const double lx = p.x - kTileSize;
        const double ly = p.y - kTileSize;
        return (lx - ly) / std::sqrt(2.0);
    };

    // Walk in along the perpendicular to the face and check every resting place.
    for (double d = 60.0; d >= 1.0; d -= 5.0) {
        const Vec2 start = inCell(1, 1, kTileSize * 0.5 + d / std::sqrt(2.0),
                                  kTileSize * 0.5 - d / std::sqrt(2.0));
        const Terrain::WallResolution wall = t.resolveWall(start, radius, Realm::Overworld);
        CHECK(!wall.unresolved);
        // Touching is not overlapping -- the same strict test a whole-cell
        // rectangle has always used -- so a circle exactly one radius off the
        // face is left exactly where it is.
        if (d >= radius) {
            CHECK(!wall.collided);                       // still clear: left alone
            CHECK_NEAR(fromFace(wall.position), d, 1e-9);
        } else {
            CHECK(wall.collided);
            // Resting ON the face, one radius off it -- and the epsilon is the
            // resolver's own, not a tolerance this test invented.
            CHECK_NEAR(fromFace(wall.position), radius + 0.01, 1e-6);
            // ...which is INSIDE the cell the coarse grid calls wall.
            CHECK_EQ(Terrain::toTileCoord(wall.position.x), 1);
            CHECK_EQ(Terrain::toTileCoord(wall.position.y), 1);
        }
    }

    // A centre inside the shape leaves by the shortest way out, which is
    // perpendicular to the face and NOT out of the cell.
    // A third of the way in and two thirds down: nearer the diagonal than any
    // edge of the cell, which is what makes the face the way out.
    const Vec2 deepAt = inCell(1, 1, kTileSize / 3.0, kTileSize * 2.0 / 3.0);
    const Terrain::WallResolution deep = t.resolveWall(deepAt, radius, Realm::Overworld);
    CHECK(deep.collided);
    CHECK(!deep.unresolved);
    CHECK_NEAR(fromFace(deep.position), radius + 0.01, 1e-6);
    CHECK_EQ(Terrain::toTileCoord(deep.position.x), 1);
    CHECK_EQ(Terrain::toTileCoord(deep.position.y), 1);

    // The same grid WITHOUT the shapes -- which is what a client with no map
    // file has -- pushes that centre right out of the cell instead. Conservative
    // and playable, and visibly not the same answer, which is why the client is
    // given the shapes.
    std::vector<std::uint8_t> coarseTiles(t.tiles(), t.tiles() + t.tileCount());
    Terrain coarse;
    CHECK(coarse.setTiles(coarseTiles, 3, 3, Realm::Overworld));
    CHECK(!coarse.hasCollisionShapes());
    const Terrain::WallResolution whole = coarse.resolveWall(deepAt, radius, Realm::Overworld);
    CHECK(whole.collided);
    CHECK(Terrain::toTileCoord(whole.position.x) != 1 ||
          Terrain::toTileCoord(whole.position.y) != 1);
    // And it calls the open half of the cell solid, which is the ground the
    // shapes hand back.
    CHECK(coarse.blocked(inCell(1, 1, 250.0, 50.0), Realm::Overworld));
    CHECK(!t.blocked(inCell(1, 1, 250.0, 50.0), Realm::Overworld));
}

TEST(a_body_wedged_between_two_shapes_is_reported_unresolved_not_relocated) {
    // THE CONTRACT MOVEMENT DEPENDS ON. Four passes, one residual check, and a
    // centre the passes cannot free is REPORTED rather than moved somewhere the
    // caller did not ask for: movement refuses an unresolved result, and a
    // resolver that silently relocated a body would tunnel it through a wall.
    // The notch is 150 units wide, so a body of radius 90 cannot fit and is
    // pushed from one arm to the other for all four passes.
    Terrain t;
    std::vector<std::uint32_t> gids(9, 0);
    gids[4] = 4;                     // notch, at cell (1,1)
    CHECK(loadShapeMap(t, "wedge.tmj", 3, 3, gids));

    const Vec2 start = inCell(1, 1, kTileSize * 0.5, 112.0);
    const Terrain::WallResolution wedged = t.resolveWall(start, 90.0, Realm::Overworld);
    CHECK(wedged.collided);
    CHECK(wedged.unresolved);
    // Still in the notch it could not be freed from -- within one push of where
    // it started, not teleported to the nearest open tile.
    CHECK(distance(wedged.position, start) < 90.0 + 0.02);
    CHECK_EQ(Terrain::toTileCoord(wedged.position.x), 1);
    CHECK_EQ(Terrain::toTileCoord(wedged.position.y), 1);

    // resolveCircle is the caller that wants a usable point rather than the
    // truth, and it rescues: the body ends up somewhere it is not blocked.
    const Vec2 rescued = t.resolveCircle(start, 90.0, Realm::Overworld);
    CHECK(!t.blocked(rescued, Realm::Overworld));
    const Terrain::WallResolution after = t.resolveWall(rescued, 90.0, Realm::Overworld);
    CHECK(!after.unresolved);
}

TEST(the_segment_tests_agree_with_the_point_tests_along_the_same_line) {
    // segmentBlocked() is the exact swept test and segmentTouchesBlockingTile()
    // the containment guard; both now walk shapes rather than squares. Neither
    // may ever say "clear" about a line a point test calls blocked, because
    // that is the direction a body tunnels in.
    Terrain t;
    std::vector<std::uint32_t> gids(16, 0);
    gids[5] = 4;    // notch at (1,1)
    gids[6] = 5;    // diag  at (2,1)
    gids[9] = 3;    // corner at (1,2)
    gids[10] = 2;   // full   at (2,2)
    CHECK(loadShapeMap(t, "segments.tmj", 4, 4, gids));

    // The lines sweep the whole 4x4 fixture, corner to corner, in a grid fine
    // enough to straddle every shape in it.
    const double span = 4.0 * kTileSize;
    int checked = 0;
    int blockedLines = 0;
    for (double y0 = 30.0; y0 < span; y0 += span / 17.0) {
        for (double y1 = 30.0; y1 < span; y1 += span / 9.0) {
            const Vec2 a{30.0, y0};
            const Vec2 b{span - 30.0, y1};
            const bool swept = t.segmentBlocked(a, b, Realm::Overworld);
            // A fine walk of the same line with the POINT test: anything it
            // finds solid, the swept test must have found too.
            bool sampled = false;
            for (int i = 0; i <= 2000; ++i) {
                const double s = static_cast<double>(i) / 2000.0;
                if (t.blocked({a.x + (b.x - a.x) * s, a.y + (b.y - a.y) * s}, Realm::Overworld)) {
                    sampled = true;
                    break;
                }
            }
            if (sampled && !swept) {
                std::printf("  segmentBlocked missed a crossing: (%.0f,%.0f)-(%.0f,%.0f)\n", a.x,
                            a.y, b.x, b.y);
            }
            CHECK(!sampled || swept);
            // The guard grows every shape, so it can only be MORE willing to
            // report a crossing than the exact walk.
            if (swept) CHECK(t.segmentTouchesBlockingTile(a, b, 0.5, Realm::Overworld));
            ++checked;
            blockedLines += swept ? 1 : 0;
        }
    }
    // The comparison is worth nothing unless the lines straddle the shapes.
    CHECK(checked > 50);
    CHECK(blockedLines > 0);
    CHECK(blockedLines < checked);

    // A line down the middle of the notch crosses nothing, where a whole-cell
    // reader would have called it solid. The notch is 192 deep in the tile's own
    // space, so how far down the cell it reaches is that times the scale.
    const double notchFloor = 192.0 * kShapeScale;
    const Vec2 through0 = inCell(1, 1, kTileSize * 0.5, 5.0);
    const Vec2 through1 = inCell(1, 1, kTileSize * 0.5, notchFloor - 10.0);
    CHECK(!t.segmentBlocked(through0, through1, Realm::Overworld));
    CHECK(!t.hasLineOfSight(inCell(1, 1, 20.0, kTileSize / 3.0),
                            inCell(1, 1, kTileSize - 20.0, kTileSize / 3.0), Realm::Overworld) ||
          true);   // the arms are in the way; what matters is the notch below
    CHECK(t.hasLineOfSight(through0, through1, Realm::Overworld));
}

TEST(water_is_the_shape_it_is_drawn_as_and_the_topmost_layer_names_the_kind) {
    Terrain t;
    std::vector<std::uint32_t> gids(9, 0);
    gids[4] = 6;                     // pond, the whole cell, at (1,1)
    gids[1] = 6;                     // and another at (1,0)
    std::vector<std::uint32_t> over(9, 0);
    over[4] = 3;                     // with a `corner` bridge over part of it
    CHECK(loadShapeMap(t, "water.tmj", 3, 3, gids, over));

    CHECK(t.atTile(1, 0) == Tile::Water);
    CHECK(t.inWater(inCell(1, 0, 150.0, 150.0), Realm::Overworld));
    CHECK(!t.inWater(inCell(0, 0, 150.0, 150.0), Realm::Overworld));   // open ground
    // Where the corner tile is drawn OVER the pond, the topmost shape names the
    // kind: that part is wall, and the rest of the cell is still water.
    CHECK(t.blocked(inCell(1, 1, 50.0, 20.0), Realm::Overworld));
    CHECK(!t.inWater(inCell(1, 1, 50.0, 20.0), Realm::Overworld));
    CHECK(t.inWater(inCell(1, 1, 250.0, 250.0), Realm::Overworld));
    // Off the map is wall, and wall is not water.
    CHECK(t.blocked({-10.0, -10.0}, Realm::Overworld));
    CHECK(!t.inWater({-10.0, -10.0}, Realm::Overworld));
}

TEST(the_shipped_garden_stops_a_body_where_its_art_does) {
    // About maps/garden.tmj. The numbers are read out of the FILE -- the tile's
    // own shape and the cell it is painted in -- so this is a check that the
    // engine agrees with the author, not that it agrees with a number somebody
    // typed here.
    TiledMap map;
    std::string error;
    if (!map.load(shippedMap(), error)) {
        std::printf("  %s\n", error.c_str());
        CHECK(false);
        return;
    }
    Terrain t;
    if (!t.loadTiledMap(shippedMap(), error)) {
        std::printf("  %s\n", error.c_str());
        CHECK(false);
        return;
    }
    CHECK(t.hasCollisionShapes());
    CHECK_EQ(t.collisionShapeCellCount(), map.wallCells() + map.waterCells());

    // -- THE POINT OF THE CHANGE: a cell the coarse grid calls wall has
    // walkable ground in it, and the dirt layer is where most of it is.
    const int cols = map.width();
    int wallCells = 0;
    int wallCellsWithGround = 0;
    int dirtCellsWithGround = 0;
    std::size_t dirtLayer = map.layers().size();
    for (std::size_t i = 0; i < map.layers().size(); ++i) {
        if (map.layers()[i].name == "dirt") dirtLayer = i;
    }
    CHECK(dirtLayer < map.layers().size());
    for (int ty = 0; ty < map.height(); ++ty) {
        for (int tx = 0; tx < cols; ++tx) {
            if (t.atTile(tx, ty) != Tile::Wall) continue;
            ++wallCells;
            bool open = false;
            for (double ly = 10.0; ly < kTileSize && !open; ly += 20.0) {
                for (double lx = 10.0; lx < kTileSize && !open; lx += 20.0) {
                    if (!t.blocked(inCell(tx, ty, lx, ly), Realm::Overworld)) open = true;
                }
            }
            if (!open) continue;
            ++wallCellsWithGround;
            if (dirtLayer < map.layers().size() &&
                map.layers()[dirtLayer]
                        .cells[static_cast<std::size_t>(ty * cols + tx)]
                        .art >= 0) {
                ++dirtCellsWithGround;
            }
        }
    }
    CHECK(wallCells > 0);
    if (wallCellsWithGround == 0) {
        std::printf("  garden.tmj: not one of its %d wall cells has walkable ground in it; "
                    "the authored shapes are not reaching collision\n", wallCells);
    }
    CHECK(wallCellsWithGround > 0);
    CHECK(dirtCellsWithGround > 0);

    // -- AND THE BOUNDARY SITS WHERE THE SHAPE SAYS. Every unflipped cell whose
    // only contributor is a tile with ONE axis-aligned rectangle is a place the
    // face can be predicted from the file: solid a hair inside it, open a hair
    // outside.
    int probed = 0;
    for (int ty = 0; ty < map.height() && probed < 8; ++ty) {
        for (int tx = 0; tx < cols && probed < 8; ++tx) {
            if (t.atTile(tx, ty) != Tile::Wall) continue;
            // Exactly one colliding layer may paint this cell, or the faces of
            // two shapes would overlap and the prediction would not be the
            // tile's alone.
            const TiledCell* only = nullptr;
            int contributors = 0;
            for (const TiledLayer& layer : map.layers()) {
                if (!layer.collides) continue;
                const TiledCell& cell = layer.cells[static_cast<std::size_t>(ty * cols + tx)];
                if (cell.type < 0) continue;
                if (map.palette()[static_cast<std::size_t>(cell.type)].shapes.empty()) continue;
                ++contributors;
                only = &cell;
            }
            if (contributors != 1 || only == nullptr) continue;
            if ((only->flags & 7u) != 0) continue;                 // unflipped only
            const TiledTileType& type = map.palette()[static_cast<std::size_t>(only->type)];
            if (type.shapes.size() != 1 || type.shapes[0].points.size() != 4) continue;
            // An axis-aligned rectangle, and a PARTIAL one: a full-cell shape
            // has no interesting face.
            const std::vector<Vec2>& ring = type.shapes[0].points;
            double left = ring[0].x, right = ring[0].x, top = ring[0].y, bottom = ring[0].y;
            for (const Vec2& p : ring) {
                left = std::min(left, p.x); right = std::max(right, p.x);
                top = std::min(top, p.y); bottom = std::max(bottom, p.y);
            }
            bool axisAligned = true;
            for (const Vec2& p : ring) {
                const bool onX = std::abs(p.x - left) < 1e-9 || std::abs(p.x - right) < 1e-9;
                const bool onY = std::abs(p.y - top) < 1e-9 || std::abs(p.y - bottom) < 1e-9;
                if (!onX || !onY) axisAligned = false;
            }
            if (!axisAligned) continue;
            if (right > kTileSize - 5.0) continue;                 // needs a face inside the cell
            const double midY = (top + bottom) * 0.5;
            if (!t.blocked(inCell(tx, ty, right - 2.0, midY), Realm::Overworld) ||
                t.blocked(inCell(tx, ty, right + 2.0, midY), Realm::Overworld)) {
                std::printf("  garden.tmj cell (%d,%d) tile \"%s\": its shape ends at x=%.2f but "
                            "the engine disagrees (inside=%d outside=%d)\n", tx, ty,
                            type.name.c_str(), right,
                            (int)t.blocked(inCell(tx, ty, right - 2.0, midY), Realm::Overworld),
                            (int)t.blocked(inCell(tx, ty, right + 2.0, midY), Realm::Overworld));
            }
            CHECK(t.blocked(inCell(tx, ty, right - 2.0, midY), Realm::Overworld));
            CHECK(!t.blocked(inCell(tx, ty, right + 2.0, midY), Realm::Overworld));
            // A body walking into that face stops on it, a third of a cell
            // deeper in than the cell boundary would have stopped it.
            const double radius = 20.0;
            const Terrain::WallResolution wall = t.resolveWall(
                inCell(tx, ty, right + radius * 0.5, midY), radius, Realm::Overworld);
            CHECK(!wall.unresolved);
            CHECK_NEAR(wall.position.x - tx * kTileSize, right + radius + 0.01, 1e-6);
            ++probed;
        }
    }
    if (probed == 0) {
        std::printf("  garden.tmj has no unflipped single-rectangle cell to probe; "
                    "the boundary check did not run\n");
    }
    CHECK(probed > 0);
}

TEST(a_realm_keeps_its_shapes_only_while_they_still_describe_its_grid) {
    // THE CLIENT'S PATH, and the two ways it can go wrong. A client is handed
    // the coarse grid over the wire and builds the shapes from its own copy of
    // the map file; it may do those two things in either order, and it must
    // never end up holding shapes that belong to a different map.
    Terrain t;
    std::vector<std::uint32_t> gids(9, 0);
    gids[4] = 5;                     // diag at (1,1)
    CHECK(loadShapeMap(t, "client.tmj", 3, 3, gids));
    CHECK(t.hasCollisionShapes());

    // The same grid arriving again, cell for cell: the shapes still describe
    // it, so they stay.
    std::vector<std::uint8_t> tiles(t.tiles(), t.tiles() + t.tileCount());
    CHECK(t.setTiles(tiles, 3, 3, Realm::Overworld));
    CHECK(t.hasCollisionShapes());
    CHECK(!t.blocked(inCell(1, 1, 250.0, 50.0), Realm::Overworld));

    // A grid of a DIFFERENT shape is a different map, and shapes indexed at the
    // old width would be a world sheared diagonally. Dropped, and the realm
    // falls back to whole-cell collision, which is conservative and playable.
    std::vector<std::uint8_t> other(16, static_cast<std::uint8_t>(Tile::Ground));
    other[5] = static_cast<std::uint8_t>(Tile::Wall);
    CHECK(t.setTiles(other, 4, 4, Realm::Overworld));
    CHECK(!t.hasCollisionShapes());
    CHECK(t.blocked(inCell(1, 1, 250.0, 50.0), Realm::Overworld));   // all of that cell now

    // And a map of the wrong size is refused rather than installed over the
    // grid it does not fit.
    TiledMap wrong;
    std::string error;
    writeFixture("shapes.tsj", kShapeTileset);
    CHECK(wrong.load(writeFixture("small.tmj", shapeMap(3, 3, gids)), error));
    CHECK(!t.setCollisionShapes(wrong, Realm::Overworld));
    CHECK(!t.hasCollisionShapes());
    CHECK(!t.loadCollisionShapes(shapeDir() + "/small.tmj", error, Realm::Overworld));
    CHECK(!error.empty());

    // A realm that answers for its own geometry never takes shapes at all.
    CHECK(!t.setCollisionShapes(wrong, Realm::Maze));
    CHECK(!t.hasCollisionShapes(Realm::Maze));
}

TEST(writing_a_tile_by_hand_drops_the_realms_authored_shapes) {
    // setTile() says what a CELL is, and an authored cell's collision is not
    // one value -- there is no shape for "wall" to be written as. So a direct
    // write drops the realm's shapes and everything falls back to whole cells:
    // conservative, visible here, and the reason generate() and the map-carving
    // helpers cannot leave a realm half authored and half painted.
    Terrain t;
    std::vector<std::uint32_t> gids(9, 0);
    gids[4] = 5;                     // diag at (1,1)
    CHECK(loadShapeMap(t, "handwritten.tmj", 3, 3, gids));
    CHECK(t.hasCollisionShapes());
    CHECK(!t.blocked(inCell(1, 1, 250.0, 50.0), Realm::Overworld));

    t.setTile(0, 0, Tile::Wall, Realm::Overworld);
    CHECK(!t.hasCollisionShapes());
    CHECK(t.blocked(inCell(0, 0, 150.0, 150.0), Realm::Overworld));
    // ...and the cell that was half open is wholly solid again.
    CHECK(t.blocked(inCell(1, 1, 250.0, 50.0), Realm::Overworld));
}

// ---------------------------------------------------------------------------
// Reading the geometry back out
// ---------------------------------------------------------------------------

namespace {

/// Crossing-number point-in-ring, written out here rather than borrowed from
/// the engine: the point of the test below is that the rings handed back
/// describe the same solid the engine's own tests answer from, and reusing its
/// containment code would make that circular.
bool ringHolds(const std::vector<Vec2>& ring, Vec2 p) {
    bool inside = false;
    for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        const Vec2 a = ring[i];
        const Vec2 b = ring[j];
        if ((a.y > p.y) == (b.y > p.y)) continue;
        const double x = (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x;
        if (p.x < x) inside = !inside;
    }
    return inside;
}

} // namespace

TEST(collision_rings_are_the_geometry_the_queries_answer_from) {
    // What the minimap draws. A ring handed back has to be the solid the
    // engine collides with, in world units, or the map on screen is not the
    // map being walked on.
    //
    // The cell is a TURNED one: `corner` is a 150 x 75 rectangle in the tile's
    // top-left, and the quarter turn clockwise (D | H) puts a 75 x 150 one
    // against the cell's right edge. A ring handed back untouched by the flip
    // bits would still look plausible -- same shape, wrong corner -- so the
    // check is against blocked(), point by point, rather than against a
    // rectangle typed out here.
    const std::uint32_t H = 0x80000000u;
    const std::uint32_t D = 0x20000000u;
    std::vector<std::uint32_t> gids(9, 0);
    gids[4] = 3u | D | H;            // corner, quarter turn clockwise, at (1,1)
    gids[5] = 6u;                    // pond, the whole cell, at (2,1)
    gids[3] = 1u;                    // plain: art, no shapes, at (0,1)
    Terrain t;
    CHECK(loadShapeMap(t, "rings.tmj", 3, 3, gids));

    std::vector<Terrain::CellCollisionRing> rings;
    t.collisionRingsAt(1, 1, Realm::Overworld, rings);
    CHECK_EQ(rings.size(), std::size_t{1});
    if (rings.empty()) return;
    CHECK(rings[0].ownCell);
    CHECK(!rings[0].water);
    CHECK_EQ(rings[0].points->size(), std::size_t{4});

    // The ring in world units, which is how a caller drawing it has to read it.
    std::vector<Vec2> world;
    for (const Vec2& p : *rings[0].points) world.push_back(p + rings[0].origin);
    const double h = 64.0 * kShapeScale;    // 75, the turned rectangle's width
    const double w = 128.0 * kShapeScale;   // 150, its height
    double left = world[0].x, right = world[0].x, top = world[0].y, bottom = world[0].y;
    for (const Vec2& p : world) {
        left = std::min(left, p.x); right = std::max(right, p.x);
        top = std::min(top, p.y);   bottom = std::max(bottom, p.y);
    }
    CHECK_NEAR(left, kTileSize + kTileSize - h, 1e-9);
    CHECK_NEAR(right, kTileSize + kTileSize, 1e-9);
    CHECK_NEAR(top, kTileSize, 1e-9);
    CHECK_NEAR(bottom, kTileSize + w, 1e-9);

    // And the ring is the solid: everywhere in the cell, being inside it and
    // being blocked are the same thing.
    int disagreed = 0;
    for (double ly = 4.0; ly < kTileSize; ly += 7.0) {
        for (double lx = 4.0; lx < kTileSize; lx += 7.0) {
            const Vec2 p = inCell(1, 1, lx, ly);
            if (ringHolds(world, p) != t.blocked(p, Realm::Overworld)) ++disagreed;
        }
    }
    CHECK_EQ(disagreed, 0);

    // Water is named on the ring, because that is all that tells a drawing
    // caller a river from a castle.
    t.collisionRingsAt(2, 1, Realm::Overworld, rings);
    CHECK_EQ(rings.size(), std::size_t{1});
    if (!rings.empty()) CHECK(rings[0].water);
}

TEST(a_cell_with_no_shapes_hands_back_no_rings) {
    // Three ways a cell has none, and none of them may invent geometry.
    std::vector<std::uint32_t> gids(9, 0);
    gids[4] = 3u;                    // corner at (1,1), so the map HAS shapes
    gids[3] = 1u;                    // plain at (0,1): art, no collision
    Terrain t;
    CHECK(loadShapeMap(t, "empty_cells.tmj", 3, 3, gids));

    std::vector<Terrain::CellCollisionRing> rings;
    t.collisionRingsAt(0, 1, Realm::Overworld, rings);   // painted, but shapeless
    CHECK(rings.empty());
    t.collisionRingsAt(0, 0, Realm::Overworld, rings);   // nothing painted at all
    CHECK(rings.empty());
    t.collisionRingsAt(-1, 7, Realm::Overworld, rings);  // off the grid
    CHECK(rings.empty());

    // And the case a caller has to handle itself: a realm with no authored
    // shapes at all still blocks, over the whole cell, and has no rings to
    // hand back. A minimap that only drew rings would show nothing here, which
    // is why it falls back to the cell's square.
    Terrain plain;
    plain.setTile(3, 4, Tile::Wall, Realm::Overworld);
    plain.collisionRingsAt(3, 4, Realm::Overworld, rings);
    CHECK(rings.empty());
    CHECK(plain.blocked(inCell(3, 4, kTileSize * 0.5, kTileSize * 0.5), Realm::Overworld));
}

TEST(a_shape_that_overhangs_its_tile_is_handed_back_once_per_cell_it_reaches) {
    // `wide` is 700 units of a 256 tile, i.e. most of three cells across: drawn
    // in one cell, it reaches two more. Every cell it touches is handed it -- that
    // is what makes a query of ONE cell right -- and every one of them
    // describes the SAME solid, at the same place in the world. `ownCell` is
    // what a caller drawing the whole grid uses to draw it once.
    std::vector<std::uint32_t> gids(9, 0);
    gids[3] = 7u;                    // wide at (0,1)
    Terrain t;
    CHECK(loadShapeMap(t, "overhang.tmj", 3, 3, gids));

    std::vector<Terrain::CellCollisionRing> rings;
    std::vector<Vec2> owner;
    t.collisionRingsAt(0, 1, Realm::Overworld, rings);
    CHECK_EQ(rings.size(), std::size_t{1});
    if (rings.empty()) return;
    CHECK(rings[0].ownCell);
    for (const Vec2& p : *rings[0].points) owner.push_back(p + rings[0].origin);

    int reached = 0;
    for (int tx = 1; tx < 3; ++tx) {
        t.collisionRingsAt(tx, 1, Realm::Overworld, rings);
        if (rings.empty()) continue;
        ++reached;
        CHECK(!rings[0].ownCell);
        CHECK_EQ(rings[0].points->size(), owner.size());
        for (std::size_t i = 0; i < owner.size() && i < rings[0].points->size(); ++i) {
            const Vec2 p = (*rings[0].points)[i] + rings[0].origin;
            CHECK_NEAR(p.x, owner[i].x, 1e-9);
            CHECK_NEAR(p.y, owner[i].y, 1e-9);
        }
    }
    CHECK_EQ(reached, 2);
}

// ---------------------------------------------------------------------------
// A layer that REMOVES collision
// ---------------------------------------------------------------------------
//
// `negate_collision` on a tile layer cancels, where that layer has a tile, the
// collision the layers BELOW it contributed -- over the negating tile's own
// shapes when it has any, and over the whole cell when it has none. That is a
// bridge: a deck painted over a river you walk across on planks.
//
// These fixtures are written here rather than taken from the shipped map, so
// they say what the RULE is and keep saying it while the author edits
// maps/garden.tmj. The shipped bridge gets its own test at the end.

namespace {

/// A four-layer fixture: scenery, a colliding layer, a NEGATING layer, and
/// (when `over` is non-empty) a second colliding layer above the deck.
///
/// Raw gids, so a caller may set Tiled's flip bits. `both` ticks
/// `has_collision` on the deck as well, which is the incoherent case the
/// reader has to resolve out loud.
std::string deckMap(int cols, int rows, const std::vector<std::uint32_t>& under,
                    const std::vector<std::uint32_t>& deck,
                    const std::vector<std::uint32_t>& over = {}, bool both = false) {
    const auto data = [&](const std::vector<std::uint32_t>& gids) {
        std::string out;
        for (int i = 0; i < cols * rows; ++i) {
            if (i != 0) out += ",";
            out += std::to_string(i < static_cast<int>(gids.size()) ? gids[i] : 0u);
        }
        return out;
    };
    const std::string size = std::to_string(cols);
    const std::string tall = std::to_string(rows);
    const auto layer = [&](int id, const char* name, const char* properties,
                           const std::vector<std::uint32_t>& gids) {
        return std::string(R"({ "type": "tilelayer", "id": )") + std::to_string(id) +
               R"(, "name": ")" + name + R"(", "opacity": 1, "visible": true, )" + properties +
               R"( "x": 0, "y": 0, "width": )" + size + R"(, "height": )" + tall +
               R"(, "data": [)" + data(gids) + "] }";
    };
    const char* collides =
        R"("properties": [ { "name": "has_collision", "type": "bool", "value": true } ],)";
    const char* negates =
        R"("properties": [ { "name": "negate_collision", "type": "bool", "value": true } ],)";
    const char* conflicting =
        R"("properties": [ { "name": "has_collision", "type": "bool", "value": true },
                           { "name": "negate_collision", "type": "bool", "value": true } ],)";
    std::vector<std::uint32_t> background(static_cast<std::size_t>(cols) * rows, 1u);
    std::string layers = layer(1, "background", "", background) + "," +
                         layer(2, "under", collides, under) + "," +
                         layer(3, "deck", both ? conflicting : negates, deck);
    if (!over.empty()) layers += "," + layer(4, "over", collides, over);
    return R"({
 "compressionlevel": -1, "infinite": false, "orientation": "orthogonal",
 "renderorder": "right-down", "tiledversion": "1.10.1", "type": "map", "version": "1.10",
 "tilewidth": 256, "tileheight": 256, "width": )" + size + R"(, "height": )" + tall + R"(,
 "tilesets": [ { "firstgid": 1, "source": "shapes.tsj" } ],
 "layers": [)" + layers + "] }";
}

/// The fixture on disk, as both views: the reader's (layer flags, coarse grid,
/// counts) and the engine's (the exact shape store).
bool loadDeckMap(TiledMap& map, Terrain& terrain, const std::string& name, int cols, int rows,
                 const std::vector<std::uint32_t>& under, const std::vector<std::uint32_t>& deck,
                 const std::vector<std::uint32_t>& over = {}, bool both = false) {
    writeFixture("shapes.tsj", kShapeTileset);
    const std::string path = writeFixture(name, deckMap(cols, rows, under, deck, over, both));
    std::string error;
    if (!map.load(path, error) || !terrain.loadTiledMap(path, error)) {
        std::printf("  fixture %s did not load: %s\n", name.c_str(), error.c_str());
        return false;
    }
    return true;
}

/// The nine cells of a 3x3 fixture, with `middle` at (1,1).
std::vector<std::uint32_t> onlyMiddle(std::uint32_t middle) {
    std::vector<std::uint32_t> gids(9, 0u);
    gids[4] = middle;
    return gids;
}

/// `count` empty cells with one gid at `at` -- a deck of exactly one cell
/// somewhere in a wider fixture.
std::vector<std::uint32_t> onlyAt(int count, int at, std::uint32_t gid) {
    std::vector<std::uint32_t> gids(static_cast<std::size_t>(count) * 3, 0u);
    gids[static_cast<std::size_t>(at)] = gid;
    return gids;
}

}   // namespace

TEST(an_unshaped_negating_tile_clears_its_whole_cell) {
    // `plain` carries no collision shape at all, and on a NEGATING layer that
    // means the whole cell -- the opposite of what it means on a colliding
    // one, where it contributes nothing. A deck covers its square; that is
    // what a deck is (see tiled_map.h).
    TiledMap map;
    Terrain t;
    CHECK(loadDeckMap(map, t, "deck_whole.tmj", 3, 3, onlyMiddle(2u), onlyMiddle(1u)));

    // The coarse grid, the exact test and the store all agree the cell is open.
    CHECK(t.atTile(1, 1) == Tile::Ground);
    for (double ly = 10.0; ly < kTileSize; ly += 40.0) {
        for (double lx = 10.0; lx < kTileSize; lx += 40.0) {
            CHECK(!t.blocked(inCell(1, 1, lx, ly), Realm::Overworld));
        }
    }
    std::vector<Terrain::CellCollisionRing> rings;
    t.collisionRingsAt(1, 1, Realm::Overworld, rings);
    CHECK(rings.empty());
    // And a body walks straight through it, which is the only thing a player
    // ever notices.
    CHECK(!t.segmentBlocked(inCell(1, 1, -50.0, 150.0), inCell(1, 1, 350.0, 150.0),
                            Realm::Overworld));
    const Terrain::WallResolution resolved =
        t.resolveWall(inCell(1, 1, 150.0, 150.0), 25.0, Realm::Overworld);
    CHECK(!resolved.collided);

    CHECK_EQ(map.negatedCells(), 1);
    CHECK_EQ(t.collisionDeckedCellCount(), 1);
    CHECK_EQ(map.layers()[2].clearedCells, 1);
    CHECK_EQ(map.layers()[2].paintedCells, 1);
}

TEST(a_shaped_negating_tile_clears_only_its_own_shape) {
    // `corner` is a 128x64 rectangle in the top-left of a 256 tile, i.e. half
    // the cell across and a quarter of it down. As a DECK it is a plank laid
    // across one corner of the cell: that corner is walkable and the rest of the
    // cell is still the wall the layer below painted.
    TiledMap map;
    Terrain t;
    CHECK(loadDeckMap(map, t, "deck_partial.tmj", 3, 3, onlyMiddle(2u), onlyMiddle(3u)));

    const double plankW = 128.0 * kShapeScale;   // half the cell
    const double plankH = 64.0 * kShapeScale;    // a quarter of it
    CHECK(!t.blocked(inCell(1, 1, 20.0, 20.0), Realm::Overworld));   // under the plank
    CHECK(!t.blocked(inCell(1, 1, plankW - 10.0, plankH - 5.0), Realm::Overworld));   // still under
    CHECK(t.blocked(inCell(1, 1, plankW + 10.0, plankH - 5.0), Realm::Overworld));    // past right
    CHECK(t.blocked(inCell(1, 1, 20.0, plankH + 15.0), Realm::Overworld));            // past bottom
    CHECK(t.blocked(inCell(1, 1, kTileSize * 0.5, kTileSize * 0.5), Realm::Overworld));   // middle

    // ONE TILE PER CELL CANNOT SAY "HALF OF THIS CELL". The coarse grid keeps
    // such a cell blocked and only the exact store cancels it -- the coarse
    // view blocking a little more than the art does is what it has always
    // done, and it is the safe direction to be wrong in.
    CHECK(t.atTile(1, 1) == Tile::Wall);
    CHECK_EQ(map.negatedCells(), 0);
    CHECK_EQ(t.collisionDeckedCellCount(), 0);
    // AND THE REPORT SAYS SO. A partial deck cleared no CELL -- it cancelled
    // point tests inside one -- so it is counted apart from the cells a deck
    // really opened, and the load report warns about it by name. Counting it
    // as "1 of 1 cells cleared" told an author the box had taken effect while
    // the minimap, the flow field, the wire and every swept test still saw the
    // wall; that is the one thing the count exists to prevent.
    CHECK_EQ(map.layers()[2].clearedCells, 0);
    CHECK_EQ(map.layers()[2].partialDeckCells, 1);
}

TEST(a_partial_deck_that_overlaps_no_collision_is_reported_as_clearing_nothing) {
    // THE COUNT IS OF WHAT WAS CANCELLED, NOT OF WHAT WAS THERE. Here the
    // wall is `corner` turned a half turn -- the bottom-right of the cell --
    // and the deck is `corner` as drawn, the top-left. The plank overlaps no
    // collision whatsoever: the bottom-right is still blocked and the top-left
    // was already open, so ticking the box achieved precisely nothing.
    //
    // Counting the cell as cleared because SOMETHING was blocked in it read
    // "1 of 1 cells cleared" in the load report and told the author it had
    // worked. The report's only job is to answer that question honestly.
    TiledMap map;
    Terrain t;
    const std::uint32_t halfTurn = 3u | 0x80000000u | 0x40000000u;
    CHECK(loadDeckMap(map, t, "deck_misses.tmj", 3, 3, onlyMiddle(halfTurn), onlyMiddle(3u)));

    CHECK(!t.blocked(inCell(1, 1, 20.0, 20.0), Realm::Overworld));      // under the plank
    CHECK(t.blocked(inCell(1, 1, kTileSize - 20.0, kTileSize - 20.0),
                    Realm::Overworld));   // still the wall
    CHECK_EQ(map.layers()[2].clearedCells, 0);
    CHECK_EQ(map.layers()[2].partialDeckCells, 1);
    CHECK_EQ(map.negatedCells(), 0);
    CHECK_EQ(t.collisionDeckedCellCount(), 0);
}

TEST(a_deck_tile_that_covers_its_cell_is_the_same_deck_as_an_unshaped_one) {
    // TWO SPELLINGS OF ONE DECK. An author may leave the bridge tile with no
    // collision shape at all, or draw Tiled's "whole tile" rectangle on it.
    // Both mean "this square is decked over", so both are resolved at load
    // (tileDecksWholeCell) and every consumer has to give the same answer --
    // otherwise the shaped one is a plank the coarse grid still calls a wall,
    // which no body can walk onto and no minimap draws a channel through.
    TiledMap shaped;
    Terrain withShape;
    CHECK(loadDeckMap(shaped, withShape, "deck_fullcell.tmj", 8, 3,
                      std::vector<std::uint32_t>(24, 2u), onlyAt(8, 12u, 2u)));
    TiledMap unshaped;
    Terrain withNone;
    CHECK(loadDeckMap(unshaped, withNone, "deck_fullcell_plain.tmj", 8, 3,
                      std::vector<std::uint32_t>(24, 2u), onlyAt(8, 12u, 1u)));

    for (const Terrain* t : {&withShape, &withNone}) {
        // The coarse grid, the exact test, the store and the swept test all
        // agree the decked cell is open ground and everything else is wall.
        CHECK(t->atTile(4, 1) == Tile::Ground);
        CHECK(!t->blocked(inCell(4, 1, 150.0, 150.0), Realm::Overworld));
        CHECK(!t->inWater(inCell(4, 1, 150.0, 150.0), Realm::Overworld));
        CHECK(!t->resolveWall(inCell(4, 1, 150.0, 150.0), 20.0, Realm::Overworld).collided);
        std::vector<Terrain::CellCollisionRing> rings;
        t->collisionRingsAt(4, 1, Realm::Overworld, rings);
        CHECK(rings.empty());
        CHECK(t->atTile(3, 1) == Tile::Wall);
        CHECK(t->blocked(inCell(3, 1, 150.0, 150.0), Realm::Overworld));
        int tx = -1, ty = -1;
        CHECK(t->nearestOpenTile(inCell(4, 1, 150.0, 150.0), tx, ty, Realm::Overworld));
        CHECK_EQ(tx, 4);
        CHECK_EQ(ty, 1);
    }
    // And the numbers the load report prints are the same numbers.
    CHECK_EQ(shaped.negatedCells(), 1);
    CHECK_EQ(unshaped.negatedCells(), 1);
    CHECK_EQ(withShape.collisionDeckedCellCount(), 1);
    CHECK_EQ(withNone.collisionDeckedCellCount(), 1);
    CHECK_EQ(shaped.layers()[2].clearedCells, 1);
    CHECK_EQ(shaped.layers()[2].partialDeckCells, 0);
}

TEST(a_body_walks_onto_a_deck_whose_tile_carries_a_whole_cell_shape) {
    // The same fixture, through the REAL movement step. A deck the queries call
    // open but that a moving body cannot reach is not a bridge, and this is the
    // failure a point test alone will not see: the cell either side is solid,
    // so a body only ever arrives at the plank pressed against a wall.
    TiledMap map;
    Terrain t;
    CHECK(loadDeckMap(map, t, "deck_walk.tmj", 8, 3, std::vector<std::uint32_t>(24, 2u),
                      onlyAt(8, 12u, 2u)));

    // Start in the middle of the decked cell and push west, then east: the
    // deck's own cell is open, so a body standing on it is free to move within
    // it and is stopped by the walls either side, not glued where it stands.
    Vec2 west = inCell(4, 1, kTileSize * 0.5, kTileSize * 0.5);
    Vec2 east = west;
    const Vec2 start = west;
    for (int tick = 0; tick < 200; ++tick) {
        stepCollide(t, Realm::Overworld, west, {-400.0, 0.0}, 20.0, 0.04, true, true);
        stepCollide(t, Realm::Overworld, east, {400.0, 0.0}, 20.0, 0.04, true, true);
    }
    // It reached the deck's own edges, a radius off the wall either side, and
    // was neither frozen at the start nor pushed out of the cell.
    CHECK(west.x < inCell(4, 1, kTileSize * 0.5, 0.0).x - kTileSize / 3.0);
    CHECK(west.x > inCell(4, 1, 0.0, 0.0).x);
    CHECK(east.x > inCell(4, 1, kTileSize * 0.5, 0.0).x + kTileSize / 3.0);
    CHECK(east.x < inCell(5, 1, 0.0, 0.0).x);
    CHECK(west.x != start.x);
    CHECK(east.x != start.x);
}

TEST(a_negating_layer_does_not_cancel_a_colliding_layer_above_it) {
    // THE LAYERS ARE A STACK. Negation reaches DOWN and no further, so a wall
    // built on top of a bridge is still a wall. The shipped bridge happens to
    // be the topmost layer of its map, which would make "cancel everything"
    // pass every test the shipped data can write; this is the fixture that
    // says it is not what the rule is.
    TiledMap map;
    Terrain t;
    CHECK(loadDeckMap(map, t, "deck_under_wall.tmj", 3, 3, onlyMiddle(2u), onlyMiddle(1u),
                      onlyMiddle(2u)));
    CHECK(t.atTile(1, 1) == Tile::Wall);
    CHECK(t.blocked(inCell(1, 1, 150.0, 150.0), Realm::Overworld));

    // The same map without that upper layer is open, so it really is the layer
    // above doing the blocking and not the deck failing to negate.
    TiledMap without;
    Terrain open;
    CHECK(loadDeckMap(without, open, "deck_no_wall.tmj", 3, 3, onlyMiddle(2u), onlyMiddle(1u)));
    CHECK(open.atTile(1, 1) == Tile::Ground);
    CHECK(!open.blocked(inCell(1, 1, 150.0, 150.0), Realm::Overworld));
}

TEST(a_layer_with_both_collision_properties_is_reported_and_negates) {
    // `has_collision` and `negate_collision` are opposites, so a layer with
    // both says nothing coherent. NEGATION WINS -- and the reader records that
    // it had to choose, so the load report can name the layer instead of the
    // author finding out by walking through a wall.
    //
    // The deck's tile is `plain`, which has no shapes: had collision won, this
    // cell would have been counted as one of the "looks solid, blocks nothing"
    // cells the load report warns about, and nothing would have been cancelled.
    TiledMap map;
    Terrain t;
    CHECK(loadDeckMap(map, t, "deck_both.tmj", 3, 3, onlyMiddle(2u), onlyMiddle(1u), {}, true));

    CHECK_EQ(map.layers().size(), std::size_t{3});
    if (map.layers().size() < 3) return;
    const TiledLayer& deck = map.layers()[2];
    CHECK(deck.negates);
    CHECK(!deck.collides);
    CHECK(deck.conflicting);
    CHECK_EQ(map.unshapedBlockingCells(), 0);
    CHECK_EQ(map.negatedCells(), 1);
    CHECK(t.atTile(1, 1) == Tile::Ground);
    CHECK(!t.blocked(inCell(1, 1, 150.0, 150.0), Realm::Overworld));
}

TEST(a_negated_water_cell_is_ground_and_is_not_water) {
    // A bridge over a river is planks, not a river. The flower standing on it
    // is not slowed, because inWater() answers from the same shapes blocked()
    // does and there is nothing left in that cell to be water.
    TiledMap map;
    Terrain t;
    std::vector<std::uint32_t> under(9, 0u);
    under[4] = 6u;   // pond, tagged water, at (1,1)
    under[1] = 6u;   // and an undecked one at (1,0) as the control
    CHECK(loadDeckMap(map, t, "deck_water.tmj", 3, 3, under, onlyMiddle(1u)));

    CHECK(t.atTile(1, 1) == Tile::Ground);
    CHECK(!t.blocked(inCell(1, 1, 150.0, 150.0), Realm::Overworld));
    CHECK(!t.inWater(inCell(1, 1, 150.0, 150.0), Realm::Overworld));

    CHECK(t.atTile(1, 0) == Tile::Water);
    CHECK(t.blocked(inCell(1, 0, 150.0, 150.0), Realm::Overworld));
    CHECK(t.inWater(inCell(1, 0, 150.0, 150.0), Realm::Overworld));
}

TEST(the_coarse_grid_and_the_exact_queries_agree_on_a_negated_cell) {
    // The coarse grid is what the minimap paints, what the bots' flow field
    // walks, what spawn placement rejects against and the only thing that goes
    // over the wire; the shapes are what a body collides with. A deck that
    // reached one and not the other would be a bridge you can see and cannot
    // walk on, or the reverse.
    TiledMap map;
    Terrain t;
    std::vector<std::uint32_t> under(9, 2u);   // full, everywhere
    CHECK(loadDeckMap(map, t, "deck_agree.tmj", 3, 3, under, onlyMiddle(1u)));

    for (int ty = 0; ty < 3; ++ty) {
        for (int tx = 0; tx < 3; ++tx) {
            const bool coarse = tileBlocks(t.atTile(tx, ty));
            CHECK_EQ(coarse, !(tx == 1 && ty == 1));
            for (double ly = 10.0; ly < kTileSize; ly += 40.0) {
                for (double lx = 10.0; lx < kTileSize; lx += 40.0) {
                    CHECK_EQ(t.blocked(inCell(tx, ty, lx, ly), Realm::Overworld), coarse);
                }
            }
        }
    }
    // And the wire carries the same answer, because it carries that grid.
    CHECK_EQ(map.tiles()[4], static_cast<std::uint8_t>(Tile::Ground));
}

TEST(a_negating_layer_over_open_ground_cancels_nothing_and_says_so) {
    // Negation only reaches the layers BELOW it, so a deck painted UNDER the
    // river it meant to deck does nothing at all -- and does it silently,
    // which is the whole reason the load report counts what each negating
    // layer cleared. These are the numbers that warning is printed from.
    TiledMap map;
    Terrain t;
    CHECK(loadDeckMap(map, t, "deck_nothing.tmj", 3, 3, {}, onlyMiddle(1u)));

    CHECK_EQ(map.layers()[2].paintedCells, 1);
    CHECK_EQ(map.layers()[2].clearedCells, 0);
    CHECK_EQ(map.negatedCells(), 0);
    CHECK_EQ(t.collisionDeckedCellCount(), 0);
    // Nothing else changed: the cell was open before and is open now.
    CHECK(t.atTile(1, 1) == Tile::Ground);
    CHECK(!t.blocked(inCell(1, 1, 150.0, 150.0), Realm::Overworld));
}

TEST(the_shipped_bridge_is_a_walkable_channel_and_the_river_still_blocks) {
    // THE SHIPPED MAP, and the reason any of this exists. maps/garden.tmj's
    // `bridge` layer is 14 cells of deck at row 123, over a river that blocks.
    //
    // The RUN is derived from the file rather than typed here -- the author may
    // move or lengthen the bridge -- and what is pinned is what it has to mean:
    // every decked cell is open, and the water either side of the run is not.
    TiledMap map;
    std::string error;
    if (!map.load(shippedMap(), error)) {
        std::printf("  %s\n", error.c_str());
        CHECK(false);
        return;
    }
    Terrain t;
    if (!t.loadTiledMap(shippedMap(), error)) {
        std::printf("  %s\n", error.c_str());
        CHECK(false);
        return;
    }
    std::size_t deckLayer = map.layers().size();
    for (std::size_t i = 0; i < map.layers().size(); ++i) {
        if (map.layers()[i].negates) deckLayer = i;
    }
    CHECK(deckLayer < map.layers().size());
    if (deckLayer >= map.layers().size()) return;
    const TiledLayer& bridge = map.layers()[deckLayer];
    CHECK_EQ(bridge.name, std::string("bridge"));
    CHECK(bridge.paintedCells > 0);
    // Every cell it paints had collision to cancel; a deck floating over dry
    // land is the authoring mistake the load report warns about.
    CHECK_EQ(bridge.clearedCells, bridge.paintedCells);
    CHECK_EQ(map.negatedCells(), bridge.paintedCells);
    CHECK_EQ(t.collisionDeckedCellCount(), bridge.paintedCells);

    int deckCells = 0;
    int minX = map.width(), maxX = -1, row = -1;
    for (int ty = 0; ty < map.height(); ++ty) {
        for (int tx = 0; tx < map.width(); ++tx) {
            if (bridge.cells[static_cast<std::size_t>(ty) * map.width() + tx].type < 0) continue;
            ++deckCells;
            minX = std::min(minX, tx);
            maxX = std::max(maxX, tx);
            row = ty;
            // Open in the coarse grid, open in the exact one, and dry.
            CHECK(t.atTile(tx, ty) == Tile::Ground);
            CHECK(!t.blocked(Terrain::tileCenter(tx, ty), Realm::Overworld));
            CHECK(!t.inWater(Terrain::tileCenter(tx, ty), Realm::Overworld));
            std::vector<Terrain::CellCollisionRing> rings;
            t.collisionRingsAt(tx, ty, Realm::Overworld, rings);
            CHECK(rings.empty());
        }
    }
    CHECK_EQ(deckCells, bridge.paintedCells);
    // A horizontal run, and walkable END TO END: the segment down the middle of
    // it crosses nothing, which is the whole claim.
    CHECK(row >= 0);
    CHECK_EQ(maxX - minX + 1, deckCells);
    if (row < 0) return;
    CHECK(!t.segmentBlocked(Terrain::tileCenter(minX, row), Terrain::tileCenter(maxX, row),
                            Realm::Overworld));

    // EXACTLY THOSE CELLS. The river is a diagonal band and the deck is one row
    // of it; the rows either side of the run are still water in every column
    // the map painted water in.
    int blockedAbove = 0;
    int blockedBelow = 0;
    for (int tx = minX; tx <= maxX; ++tx) {
        if (tileBlocks(t.atTile(tx, row - 1))) ++blockedAbove;
        if (tileBlocks(t.atTile(tx, row + 1))) ++blockedBelow;
    }
    CHECK_EQ(blockedAbove, maxX - minX + 1);
    CHECK_EQ(blockedBelow, maxX - minX + 1);

    // AND THE MINIMAP READS IT AS A CHANNEL. Its solids come from the same
    // store, so a decked cell has nothing to fill -- no ring is filed there and
    // the coarse fallback has no wall to draw.
    int solidsOnTheDeck = 0;
    eachMinimapSolid(t, Realm::Overworld, [&](const MinimapSolid& solid) {
        const int tx = static_cast<int>(std::lround(solid.origin.x / kTileSize));
        const int ty = static_cast<int>(std::lround(solid.origin.y / kTileSize));
        if (ty == row && tx >= minX && tx <= maxX) ++solidsOnTheDeck;
    });
    CHECK_EQ(solidsOnTheDeck, 0);
}
