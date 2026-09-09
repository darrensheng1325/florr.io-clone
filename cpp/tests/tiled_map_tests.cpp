#include "test.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "server_harness.h"
#include "shared/game/map_elements.h"
#include "shared/game/constants.h"
#include "shared/game/terrain.h"
#include "shared/game/tiled_map.h"

using namespace flix;
using flix::testsupport::dataDir;

// The map is authored in Tiled now, and `scripts/encodeMap.js` builds the
// TypeScript bundle out of the same file. Two readers of one map is one more
// than the game can afford to have disagree, so these tests do not check that
// the Tiled reader produces something plausible -- they check that it produces
// exactly what the bundle reader does, tile for tile and rectangle for
// rectangle. A conversion that shifts the map by one tile is invisible in every
// other test in this directory and fatal in play.

namespace {

std::string tiledPath() { return dataDir() + "/world.tmj"; }
std::string bundlePath() { return dataDir() + "/map_bundle.ts"; }

bool staged(const std::string& path) {
    std::ifstream probe(path, std::ios::binary);
    return static_cast<bool>(probe);
}

/// One kind's elements in map order, which is the only order the game can see.
std::vector<const MapElement*> ofKind(const MapData& map, MapElementKind kind) {
    std::vector<const MapElement*> out;
    for (const MapElement& element : map.elements()) {
        if (element.kind == kind) out.push_back(&element);
    }
    return out;
}

bool sameElement(const MapElement& a, const MapElement& b) {
    if (a.kind != b.kind) return false;
    if (a.bounds.x != b.bounds.x || a.bounds.y != b.bounds.y) return false;
    if (a.bounds.w != b.bounds.w || a.bounds.h != b.bounds.h) return false;
    if (a.hasSpawnTier != b.hasSpawnTier || a.spawnTier != b.spawnTier) return false;
    if (a.biomeName != b.biomeName) return false;
    if (a.hasTeleportTo != b.hasTeleportTo) return false;
    if (a.hasTeleportTo && (a.teleportTo.x != b.teleportTo.x || a.teleportTo.y != b.teleportTo.y)) {
        return false;
    }
    if (a.polygon.size() != b.polygon.size()) return false;
    for (std::size_t i = 0; i < a.polygon.size(); ++i) {
        if (a.polygon[i].x != b.polygon[i].x || a.polygon[i].y != b.polygon[i].y) return false;
    }
    if (a.hasSpawnTable != b.hasSpawnTable) return false;
    if (a.spawnTable.size() != b.spawnTable.size()) return false;
    for (std::size_t i = 0; i < a.spawnTable.size(); ++i) {
        if (a.spawnTable[i].tier != b.spawnTable[i].tier) return false;
        if (a.spawnTable[i].weight != b.spawnTable[i].weight) return false;
        if (a.spawnTable[i].mobType != b.spawnTable[i].mobType) return false;
    }
    return true;
}

/// Writes `text` beside the staged data and hands back its path, so a test can
/// hand the loader a map it is meant to refuse.
std::string writeTemp(const std::string& name, const std::string& text) {
    const std::string path = dataDir() + "/" + name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return path;
}

} // namespace

TEST(the_tiled_map_is_staged_beside_the_bundle) {
    // Not a formality: every other test here is vacuous if the map did not get
    // copied into the data directory, and a silently skipped parity check is
    // the one that lets the two formats drift.
    CHECK(staged(tiledPath()));
    CHECK(worldMapPath(dataDir()) == tiledPath());
}

TEST(the_tiled_tile_grid_matches_the_bundles) {
    Terrain fromTiled;
    Terrain fromBundle;
    std::string error;
    CHECK(fromTiled.loadWorldMap(tiledPath(), error));
    CHECK(error.empty());
    CHECK(fromBundle.loadWorldMap(bundlePath(), error));
    CHECK(error.empty());

    CHECK(fromTiled.tileCount() == fromBundle.tileCount());
    int differing = 0;
    for (std::size_t i = 0; i < fromBundle.tileCount(); ++i) {
        if (fromTiled.tiles()[i] != fromBundle.tiles()[i]) ++differing;
    }
    if (differing != 0) {
        std::fprintf(stderr, "[tiled] %d of %zu tiles differ\n", differing, fromBundle.tileCount());
    }
    CHECK(differing == 0);

    // The spawn root falls out of the grid, so an off-by-one in the row order
    // would move it without changing the tile histogram.
    CHECK(fromTiled.spawnPoint().x == fromBundle.spawnPoint().x);
    CHECK(fromTiled.spawnPoint().y == fromBundle.spawnPoint().y);
}

TEST(the_tiled_annotations_match_the_bundles) {
    MapData fromTiled;
    MapData fromBundle;
    std::string error;
    CHECK(fromTiled.loadWorldMap(tiledPath(), error));
    CHECK(fromBundle.loadWorldMap(bundlePath(), error));

    CHECK(fromTiled.elements().size() == fromBundle.elements().size());

    // Per kind, because the Tiled map groups its objects into one layer each
    // and the bundle interleaves them. Only the order within a kind is
    // observable to the game -- every reader filters by kind first -- so that
    // is what is compared.
    for (const MapElementKind kind : {MapElementKind::Spawn, MapElementKind::Biome,
                                      MapElementKind::Teleporter}) {
        const auto a = ofKind(fromTiled, kind);
        const auto b = ofKind(fromBundle, kind);
        CHECK(!a.empty());
        CHECK(a.size() == b.size());
        for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
            if (!sameElement(*a[i], *b[i])) {
                std::fprintf(stderr, "[tiled] element %zu of kind %d differs\n", i,
                             static_cast<int>(kind));
            }
            CHECK(sameElement(*a[i], *b[i]));
        }
    }

    // The two derived lists are what the spawn picker and the title screen are
    // built from, so they are compared as lists, order included.
    CHECK(fromTiled.pickableBiomes() == fromBundle.pickableBiomes());
    CHECK(fromTiled.spawnableBiomes() == fromBundle.spawnableBiomes());
}

TEST(the_tilesets_flags_agree_with_the_engine) {
    TiledMap map;
    std::string error;
    CHECK(map.load(tiledPath(), error));
    // Six tiles: air, wall, water and the three the map adds.
    CHECK(map.palette().size() == 6);
    // The tileset is what an author sees. If it says a tile is walkable and
    // constants.h collides with it, the editor is lying to whoever edits the
    // map, and the map is the thing that needs fixing.
    const std::vector<std::string> mismatched = map.mismatchedFlags();
    for (const std::string& name : mismatched) {
        std::fprintf(stderr, "[tiled] tile \"%s\" disagrees with constants.h\n", name.c_str());
    }
    CHECK(mismatched.empty());
}

TEST(every_spawn_zone_is_a_polygon) {
    // The point of the change: a tier band is an outline, not a box. A zone
    // that came back as a bare rectangle would still work, which is exactly why
    // it needs asserting -- nothing else would notice.
    MapData map;
    std::string error;
    CHECK(map.loadWorldMap(tiledPath(), error));

    int zones = 0;
    for (const MapElement& element : map.elements()) {
        if (element.kind != MapElementKind::Spawn) continue;
        ++zones;
        CHECK(element.polygon.size() >= 3);
        // The bounding box is the outline's, and it is what every broadphase
        // in the spawner still works in.
        for (const Vec2 point : element.polygon) {
            CHECK(point.x >= element.bounds.left() && point.x <= element.bounds.right());
            CHECK(point.y >= element.bounds.top() && point.y <= element.bounds.bottom());
        }
        // Every corner is on the outline, and the boundary is inside.
        for (const Vec2 point : element.polygon) CHECK(element.contains(point));
        CHECK(element.area() > 0.0);
    }
    CHECK(zones > 100);
}

TEST(the_background_layer_reproduces_the_section_grid) {
    // The background layer replaced sectionAt() as the source of what the
    // ground is painted with. Replacing it means the map must still LOOK the
    // same, and this is the exact test of that: the renderer samples the layer
    // once per 400-unit ground tile, at that tile's centre, so those are the
    // only points where the two can be seen to differ.
    //
    // It holds by arithmetic rather than by luck. A ground tile's centre sits
    // 200 units from the nearest section boundary at closest, and the 300-unit
    // background cell containing that centre has its own centre within 150
    // units of it. 150 < 200, so the cell and the ground tile never land on
    // opposite sides of a seam.
    MapData map;
    std::string error;
    CHECK(map.loadWorldMap(tiledPath(), error));
    CHECK(map.hasBackground());

    constexpr double kGroundTileSize = 400.0;
    int differing = 0;
    for (double y = 0; y < kWorldSize; y += kGroundTileSize) {
        for (double x = 0; x < kWorldSize; x += kGroundTileSize) {
            const Vec2 centre{x + kGroundTileSize * 0.5, y + kGroundTileSize * 0.5};
            if (map.groundAt(centre) != sectionAt(centre)) {
                if (differing == 0) {
                    std::fprintf(stderr, "[tiled] ground at (%.0f,%.0f) is %d, section says %d\n",
                                 centre.x, centre.y, map.groundAt(centre), sectionAt(centre));
                }
                ++differing;
            }
        }
    }
    CHECK(differing == 0);

    // Off the map there is no ground, which is not the same as ground type 0 --
    // the renderer draws nothing there rather than painting the garden.
    CHECK(map.groundAt({-1.0, 100.0}) == -1);
    CHECK(map.groundAt({kWorldSize + 1.0, 100.0}) == -1);
}

TEST(the_ground_palette_is_the_nine_artworks) {
    TiledMap map;
    std::string error;
    CHECK(map.load(tiledPath(), error));
    CHECK(map.groundPalette().size() == static_cast<std::size_t>(kSectionCount));
    // Ids are dense and in order, because the renderer indexes its artwork and
    // its fallback colours by them directly.
    for (std::size_t i = 0; i < map.groundPalette().size(); ++i) {
        CHECK(map.groundPalette()[i].id == static_cast<int>(i));
        CHECK(!map.groundPalette()[i].art.empty());
    }
}

TEST(a_map_without_a_background_layer_still_loads) {
    // The TypeScript bundle cannot carry the layer, so "no background" has to
    // stay a legal map: it is what tells the renderer to fall back to the
    // section grid instead of painting the world black.
    MapData map;
    std::string error;
    CHECK(map.loadWorldMap(bundlePath(), error));
    CHECK(!map.elements().empty());
    CHECK(!map.hasBackground());
    CHECK(map.groundAt({100.0, 100.0}) == -1);
}

TEST(a_ground_tile_in_the_terrain_layer_is_refused) {
    // The two tilesets are separate so that this is an error rather than a
    // silent wall: a ground tile carries `groundId`, not `tileId`, so its gid
    // is one the terrain layer's map cannot resolve.
    const std::string path = writeTemp("ground_in_terrain.tmj", R"({
      "type": "map", "orientation": "orthogonal", "infinite": false,
      "width": 2, "height": 1, "tilewidth": 300, "tileheight": 300,
      "tilesets": [{"firstgid": 1, "name": "ground", "type": "tileset",
                    "tilecount": 1, "tilewidth": 300, "tileheight": 300, "columns": 0,
                    "tiles": [{"id": 0, "class": "garden", "image": "ground/land.svg",
                               "properties": [{"name": "groundId", "type": "int", "value": 0}]}]}],
      "layers": [
        {"type": "tilelayer", "name": "terrain", "width": 2, "height": 1, "data": [0, 1]}
      ]})");
    TiledMap map;
    std::string error;
    CHECK(!map.load(path, error));
    CHECK(error.find("gid 1") != std::string::npos);
    std::remove(path.c_str());
}

TEST(a_map_with_the_wrong_tile_size_is_refused) {
    // One Tiled pixel is one world unit. A map saved at a different tile size
    // still loads as valid Tiled JSON and puts every rectangle in the wrong
    // place, so it has to be refused rather than scaled: there is no scale
    // factor that is right for both the grid and the objects.
    const std::string path = writeTemp("bad_tile_size.tmj", R"({
      "type": "map", "orientation": "orthogonal", "infinite": false,
      "width": 2, "height": 1, "tilewidth": 32, "tileheight": 32,
      "tilesets": [], "layers": [
        {"type": "tilelayer", "name": "terrain", "width": 2, "height": 1, "data": [0, 0]}
      ]})");
    TiledMap map;
    std::string error;
    CHECK(!map.load(path, error));
    CHECK(error.find("300") != std::string::npos);
    std::remove(path.c_str());
}

TEST(a_gid_no_tileset_defines_is_refused) {
    // A cell pointing at a tile that does not exist would otherwise become
    // whatever the fallback happened to be -- most likely walkable ground, in
    // the middle of what the author drew as a wall.
    const std::string path = writeTemp("bad_gid.tmj", R"({
      "type": "map", "orientation": "orthogonal", "infinite": false,
      "width": 2, "height": 1, "tilewidth": 300, "tileheight": 300,
      "tilesets": [], "layers": [
        {"type": "tilelayer", "name": "terrain", "width": 2, "height": 1, "data": [0, 9]}
      ]})");
    TiledMap map;
    std::string error;
    CHECK(!map.load(path, error));
    CHECK(error.find("gid 9") != std::string::npos);
    std::remove(path.c_str());
}
