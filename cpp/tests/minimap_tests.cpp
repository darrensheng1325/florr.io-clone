#include "test.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "client/minimap.h"

using namespace flix;

// The minimap's fit: the whole of a map, whatever shape it is, inside a 200
// design-unit square. Everything else in that corner is paint, and paint is
// judged by looking at it; this part is arithmetic and can be pinned down.

namespace {
constexpr double kBox = 200.0;
}

TEST(a_square_map_fills_the_minimap_box) {
    // The shipped garden: 128 cells of 300 units. Square, so there is nothing
    // to letterbox and the map uses every pixel of the box.
    const MinimapFit fit = minimapFit({38400.0, 38400.0}, kBox);
    CHECK_NEAR(fit.offsetX, 0.0, 1e-9);
    CHECK_NEAR(fit.offsetY, 0.0, 1e-9);
    CHECK_NEAR(fit.scale * 38400.0, kBox, 1e-9);

    const Vec2 origin = fit.toBox({0.0, 0.0});
    CHECK_NEAR(origin.x, 0.0, 1e-9);
    CHECK_NEAR(origin.y, 0.0, 1e-9);
    // The map's far corner is the box's far corner: nothing is cropped and
    // nothing is left over.
    const Vec2 far = fit.toBox({38400.0, 38400.0});
    CHECK_NEAR(far.x, kBox, 1e-9);
    CHECK_NEAR(far.y, kBox, 1e-9);
    // And the middle is the middle, which is the whole of "no scrolling".
    const Vec2 middle = fit.toBox({19200.0, 19200.0});
    CHECK_NEAR(middle.x, kBox * 0.5, 1e-9);
    CHECK_NEAR(middle.y, kBox * 0.5, 1e-9);
}

TEST(a_wide_map_letterboxes_top_and_bottom) {
    // Twice as wide as it is tall: the width is what runs out first, so the
    // map spans the box across and takes half of it down, centred. Stretching
    // it to fill would put two different scales on a picture people read
    // distance off.
    const MinimapFit fit = minimapFit({40000.0, 20000.0}, kBox);
    CHECK_NEAR(fit.scale * 40000.0, kBox, 1e-9);
    CHECK_NEAR(fit.offsetX, 0.0, 1e-9);
    CHECK_NEAR(fit.offsetY, kBox * 0.25, 1e-9);

    const Vec2 topLeft = fit.toBox({0.0, 0.0});
    CHECK_NEAR(topLeft.x, 0.0, 1e-9);
    CHECK_NEAR(topLeft.y, kBox * 0.25, 1e-9);
    const Vec2 far = fit.toBox({40000.0, 20000.0});
    CHECK_NEAR(far.x, kBox, 1e-9);
    CHECK_NEAR(far.y, kBox * 0.75, 1e-9);
    // One scale, both axes: half the map across is half the map down.
    CHECK_NEAR(fit.toBox({20000.0, 0.0}).x - topLeft.x,
               fit.toBox({0.0, 20000.0}).y - topLeft.y, 1e-9);
}

TEST(a_tall_map_letterboxes_left_and_right) {
    const MinimapFit fit = minimapFit({15000.0, 30000.0}, kBox);
    CHECK_NEAR(fit.scale * 30000.0, kBox, 1e-9);
    CHECK_NEAR(fit.offsetY, 0.0, 1e-9);
    CHECK_NEAR(fit.offsetX, kBox * 0.25, 1e-9);
    const Vec2 far = fit.toBox({15000.0, 30000.0});
    CHECK_NEAR(far.x, kBox * 0.75, 1e-9);
    CHECK_NEAR(far.y, kBox, 1e-9);
}

TEST(a_point_off_the_map_lands_off_the_box) {
    // What the dots are clipped against. A body outside the map -- past its
    // edge, or in the bar beside a letterboxed one -- has to fall outside the
    // box, because the dot test is a plain range check on the result.
    const MinimapFit fit = minimapFit({38400.0, 38400.0}, kBox);
    CHECK(fit.toBox({-1000.0, 19200.0}).x < 0.0);
    CHECK(fit.toBox({19200.0, -1000.0}).y < 0.0);
    CHECK(fit.toBox({39000.0, 19200.0}).x > kBox);
    CHECK(fit.toBox({19200.0, 39000.0}).y > kBox);

    // The letterbox case: a point above a wide map's top edge is in the bar,
    // which is inside the box's range but outside the map's own rectangle --
    // so the offset is what keeps it distinguishable rather than clamped onto
    // the edge.
    const MinimapFit wide = minimapFit({40000.0, 20000.0}, kBox);
    const Vec2 above = wide.toBox({20000.0, -2000.0});
    CHECK(above.y < wide.offsetY);
    CHECK(above.y > 0.0);
}

TEST(a_map_with_no_size_still_gives_a_finite_fit) {
    // Terrain answers with the historical world size for a realm nothing is
    // staged for, so this is not reachable through it -- but an infinite scale
    // paints the whole screen black, and that is not a failure mode worth
    // leaving open to a hand-built Terrain or a future map format.
    const MinimapFit fit = minimapFit({0.0, 0.0}, kBox);
    CHECK(fit.scale > 0.0);
    CHECK_NEAR(fit.scale, 1.0, 1e-9);
    CHECK_NEAR(fit.toBox({kBox, kBox}).x, kBox, 1e-9);
}

// ---------------------------------------------------------------------------
// The solids: WHICH pieces of collision the bake fills, and in WHAT ORDER.
//
// The other half of the minimap that can be checked without painting. Every
// bug this file exists for is a wall on the map that is not on the minimap, or
// one on the minimap that is not on the map, so the questions are: is every
// authored ring emitted, is it emitted exactly once, and does the colour a
// point ends up with agree with the kind the collision reports for it.
// ---------------------------------------------------------------------------

namespace {

std::string fixtureDir() {
    const char* env = std::getenv("TMPDIR");
    std::string base = (env != nullptr && *env != '\0') ? env : "/tmp";
    if (base.back() != '/') base.push_back('/');
    base += "flix_minimap_tests";
    mkdir(base.c_str(), 0755);
    return base;
}

std::string writeFixture(const std::string& name, const std::string& text) {
    const std::string path = fixtureDir() + "/" + name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return path;
}

/// gid = local id + 1: 1 plain (art, no shapes), 2 full (the whole tile),
/// 3 pond (the whole tile, tagged `water`), 4 off (a 200x180 rectangle dragged
/// a WHOLE TILE to the right, which Tiled permits and the loader warns about
/// rather than refusing).
constexpr const char* kTileset = R"({
 "columns": 0, "name": "minimap", "tilecount": 4, "tiledversion": "1.10.1",
 "tilewidth": 256, "tileheight": 256, "tilerendersize": "grid",
 "type": "tileset", "version": "1.10",
 "tiles": [
  { "id": 0, "image": "tiles/plain.svg", "imagewidth": 256, "imageheight": 256 },
  { "id": 1, "image": "tiles/full.svg", "imagewidth": 256, "imageheight": 256,
    "objectgroup": { "type": "objectgroup", "id": 2, "name": "", "draworder": "index",
      "opacity": 1, "visible": true, "x": 0, "y": 0, "objects": [
      { "id": 1, "name": "", "type": "", "rotation": 0, "visible": true,
        "x": 0, "y": 0, "width": 256, "height": 256 } ] } },
  { "id": 2, "image": "tiles/pond.svg", "imagewidth": 256, "imageheight": 256,
    "properties": [ { "name": "water", "type": "bool", "value": true } ],
    "objectgroup": { "type": "objectgroup", "id": 2, "name": "", "draworder": "index",
      "opacity": 1, "visible": true, "x": 0, "y": 0, "objects": [
      { "id": 1, "name": "", "type": "", "rotation": 0, "visible": true,
        "x": 0, "y": 0, "width": 256, "height": 256 } ] } },
  { "id": 3, "image": "tiles/off.svg", "imagewidth": 256, "imageheight": 256,
    "objectgroup": { "type": "objectgroup", "id": 2, "name": "", "draworder": "index",
      "opacity": 1, "visible": true, "x": 0, "y": 0, "objects": [
      { "id": 1, "name": "", "type": "", "rotation": 0, "visible": true,
        "x": 300, "y": 40, "width": 200, "height": 180 } ] } }
 ]
})";

/// A 3x3 map: scenery underneath, then one or two colliding layers whose cells
/// are exactly the gids handed in.
std::string shapeMap(const std::vector<std::uint32_t>& lower,
                     const std::vector<std::uint32_t>& upper = {}) {
    std::string background, low, high;
    for (int i = 0; i < 9; ++i) {
        if (i != 0) { background += ","; low += ","; high += ","; }
        background += "1";
        low += std::to_string(i < static_cast<int>(lower.size()) ? lower[i] : 0u);
        high += std::to_string(i < static_cast<int>(upper.size()) ? upper[i] : 0u);
    }
    const char* collides =
        R"("properties": [ { "name": "has_collision", "type": "bool", "value": true } ],)";
    std::string layers =
        R"({ "type": "tilelayer", "id": 1, "name": "background", "opacity": 1,
             "visible": true, "x": 0, "y": 0, "width": 3, "height": 3, "data": [)" +
        background + R"(] },
           { "type": "tilelayer", "id": 2, "name": "walls", "opacity": 1, "visible": true, )" +
        collides + R"( "x": 0, "y": 0, "width": 3, "height": 3, "data": [)" + low + "] }";
    if (!upper.empty()) {
        layers += R"(, { "type": "tilelayer", "id": 3, "name": "river", "opacity": 1,
                         "visible": true, )" + std::string(collides) +
                  R"( "x": 0, "y": 0, "width": 3, "height": 3, "data": [)" + high + "] }";
    }
    return R"({
 "compressionlevel": -1, "infinite": false, "orientation": "orthogonal",
 "renderorder": "right-down", "tiledversion": "1.10.1", "type": "map", "version": "1.10",
 "tilewidth": 300, "tileheight": 300, "width": 3, "height": 3,
 "tilesets": [ { "firstgid": 1, "source": "minimap.tsj" } ],
 "layers": [)" + layers + "] }";
}

bool loadShapeMap(Terrain& out, const std::string& name,
                  const std::vector<std::uint32_t>& lower,
                  const std::vector<std::uint32_t>& upper = {}) {
    writeFixture("minimap.tsj", kTileset);
    std::string error;
    const bool ok = out.loadTiledMap(writeFixture(name, shapeMap(lower, upper)), error);
    if (!ok) std::printf("  fixture %s did not load: %s\n", name.c_str(), error.c_str());
    return ok;
}

struct Solid {
    std::vector<Vec2> points;   // empty for the whole-cell fallback
    Vec2 origin{0.0, 0.0};
    bool water = false;
};

std::vector<Solid> solidsOf(const Terrain& terrain) {
    std::vector<Solid> out;
    eachMinimapSolid(terrain, Realm::Overworld, [&](const MinimapSolid& solid) {
        Solid copy;
        copy.origin = solid.origin;
        copy.water = solid.water;
        if (solid.ring != nullptr) copy.points = *solid.ring;
        out.push_back(std::move(copy));
    });
    return out;
}

/// Whether a solid covers a world point: the ring by crossing count, the
/// fallback by its cell square. What "the minimap paints this black" means.
bool covers(const Solid& solid, Vec2 p) {
    if (solid.points.empty()) {
        return p.x >= solid.origin.x && p.x <= solid.origin.x + kTileSize &&
               p.y >= solid.origin.y && p.y <= solid.origin.y + kTileSize;
    }
    bool in = false;
    for (std::size_t i = 0, j = solid.points.size() - 1; i < solid.points.size(); j = i++) {
        const Vec2 a = solid.points[i] + solid.origin;
        const Vec2 b = solid.points[j] + solid.origin;
        if ((a.y > p.y) != (b.y > p.y) &&
            p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) {
            in = !in;
        }
    }
    return in;
}

}   // namespace

TEST(every_blocking_point_of_a_map_is_under_some_minimap_solid) {
    // The whole job in one check: paint what the engine collides with. A ring
    // the walk drops is an invisible wall, and a solid over open ground is a
    // wall that is not there, so the test is the two verdicts against each
    // other over the map rather than a count of shapes.
    //
    // `off` is the case that catches the dedupe: its shape is dragged a whole
    // tile clear of the tile it belongs to, so the cell that OWNS it is handed
    // no reference to it at all and every cell that is handed one says the
    // geometry belongs to somebody else. Deduplicating on that flag alone
    // draws it nowhere.
    std::vector<std::uint32_t> walls(9, 0);
    walls[3] = 4u;                   // off  at (0,1): its solid lands in (1,1)
    walls[2] = 2u;                   // full at (2,0)
    walls[7] = 3u;                   // pond at (1,2)
    Terrain t;
    CHECK(loadShapeMap(t, "solids.tmj", walls));

    const std::vector<Solid> solids = solidsOf(t);
    CHECK(!solids.empty());

    int disagreed = 0;
    for (double y = 5.0; y < 3 * kTileSize; y += 11.0) {
        for (double x = 5.0; x < 3 * kTileSize; x += 11.0) {
            const Vec2 p{x, y};
            bool painted = false;
            for (const Solid& solid : solids) painted = painted || covers(solid, p);
            if (painted != t.blocked(p, Realm::Overworld)) ++disagreed;
        }
    }
    CHECK_EQ(disagreed, 0);
}

TEST(a_shape_authored_outside_its_own_tile_is_still_drawn_once) {
    // The same `off` tile on its own. It is emitted -- the regression is that
    // it was not -- and it is emitted ONCE, because a contour laid down twice
    // is a doubled winding whose edge pixels come out darker than the shape
    // the map has.
    std::vector<std::uint32_t> walls(9, 0);
    walls[3] = 4u;                   // off at (0,1)
    Terrain t;
    CHECK(loadShapeMap(t, "offtile.tmj", walls));

    const std::vector<Solid> solids = solidsOf(t);
    int rings = 0;
    const Solid* ring = nullptr;
    for (const Solid& solid : solids) {
        if (solid.points.empty()) continue;
        ++rings;
        ring = &solid;
    }
    CHECK_EQ(rings, 1);
    if (ring == nullptr) return;
    // And it is where the collision is, which is the NEXT cell over.
    CHECK(covers(*ring, {450.0, 450.0}));
    CHECK(t.blocked({450.0, 450.0}, Realm::Overworld));
    CHECK(!covers(*ring, {150.0, 450.0}));

    // The tile's OWN cell holds no reference to the shape -- there is nothing
    // of it in there -- so that cell is a shapeless blocking cell, and both the
    // collision and the minimap treat it as solid all through. The square is
    // not a mistake of the drawing: blocked() says the same thing.
    CHECK_EQ(solids.size(), std::size_t{2});
    CHECK(t.blocked({150.0, 450.0}, Realm::Overworld));
}

TEST(a_shape_that_overhangs_into_its_neighbours_is_drawn_once) {
    // The ordinary overhang: filed in its own cell and in the ones it reaches.
    // Every cell hands it back, and it belongs on the minimap once.
    std::vector<std::uint32_t> walls(9, 0);
    walls[3] = 4u;                   // off at (0,1)
    walls[4] = 2u;                   // full at (1,1), which the off shape crosses
    Terrain t;
    CHECK(loadShapeMap(t, "overlap.tmj", walls));
    const std::vector<Solid> solids = solidsOf(t);
    int rings = 0;
    for (const Solid& solid : solids) {
        if (!solid.points.empty()) ++rings;
    }
    // Cell (1,1) is handed BOTH: its own `full` ring and the `off` one reaching
    // in from next door. Two rings come out, not three, even though the walk
    // meets the overhanging one in two cells.
    CHECK_EQ(rings, 2);
}

TEST(a_blocking_cell_with_no_shapes_falls_back_to_its_square) {
    // A grid with no authored geometry at all -- a generated map, a client with
    // no local map file, a test that called setTile. The collision there IS the
    // cell, so the square is the honest picture of it.
    Terrain plain;
    plain.setTile(2, 1, Tile::Wall, Realm::Overworld);
    plain.setTile(2, 2, Tile::Water, Realm::Overworld);
    const std::vector<Solid> solids = solidsOf(plain);
    CHECK_EQ(solids.size(), std::size_t{2});
    int walls = 0, water = 0;
    for (const Solid& solid : solids) {
        CHECK(solid.points.empty());
        if (solid.water) ++water; else ++walls;
    }
    CHECK_EQ(walls, 1);
    CHECK_EQ(water, 1);
    for (const Solid& solid : solids) {
        CHECK(covers(solid, {solid.origin.x + 150.0, solid.origin.y + 150.0}));
        CHECK(plain.blocked({solid.origin.x + 150.0, solid.origin.y + 150.0}, Realm::Overworld));
    }
}

TEST(the_last_solid_over_a_point_is_the_kind_the_collision_reports) {
    // The colour question. The solids are painted in the order they come out,
    // so the LAST one covering a point decides what that point looks like, and
    // the collision's own answer for the kind is the topmost layer holding it
    // (cellLayerAt). A river layer painted over a dirt layer has to come out
    // blue, the way inWater() reports it -- emitting all the water first and
    // the walls over it renders exactly that case black.
    std::vector<std::uint32_t> lower(9, 0), upper(9, 0);
    lower[4] = 2u;                   // full wall at (1,1)
    upper[4] = 3u;                   // pond over it, on the layer above
    lower[0] = 3u;                   // pond at (0,0), with nothing over it
    Terrain t;
    CHECK(loadShapeMap(t, "kinds.tmj", lower, upper));

    const Vec2 river{450.0, 450.0};
    CHECK(t.blocked(river, Realm::Overworld));
    CHECK(t.inWater(river, Realm::Overworld));

    const std::vector<Solid> solids = solidsOf(t);
    const auto kindAt = [&](Vec2 p, bool& found) {
        bool water = false;
        found = false;
        for (const Solid& solid : solids) {
            if (!covers(solid, p)) continue;
            found = true;
            water = solid.water;
        }
        return water;
    };
    bool found = false;
    CHECK_EQ(kindAt(river, found), t.inWater(river, Realm::Overworld));
    CHECK(found);
    const Vec2 pond{150.0, 150.0};
    CHECK_EQ(kindAt(pond, found), t.inWater(pond, Realm::Overworld));
    CHECK(found);
}
