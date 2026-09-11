#include "test.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "server_harness.h"
#include "shared/core/json.h"
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
    if (a.mobDistribution.size() != b.mobDistribution.size()) return false;
    for (std::size_t i = 0; i < a.mobDistribution.size(); ++i) {
        if (a.mobDistribution[i].name != b.mobDistribution[i].name) return false;
        if (a.mobDistribution[i].weight != b.mobDistribution[i].weight) return false;
    }
    if (a.targetMap != b.targetMap || a.targetSpawn != b.targetSpawn) return false;
    if (a.hasTeleportTo != b.hasTeleportTo) return false;
    if (a.hasTeleportTo && (a.teleportTo.x != b.teleportTo.x || a.teleportTo.y != b.teleportTo.y)) {
        return false;
    }
    if (a.polygon.size() != b.polygon.size()) return false;
    for (std::size_t i = 0; i < a.polygon.size(); ++i) {
        if (a.polygon[i].x != b.polygon[i].x || a.polygon[i].y != b.polygon[i].y) return false;
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

    // The bundle carries no player spawn rectangles -- encodeMap.js leaves
    // them out, because the TypeScript server it feeds has no picker -- so the
    // two differ by exactly that kind and agree on everything else.
    const std::size_t doors = ofKind(fromTiled, MapElementKind::PlayerSpawn).size();
    CHECK(doors > 0);
    CHECK(ofKind(fromBundle, MapElementKind::PlayerSpawn).empty());
    CHECK(fromTiled.elements().size() == fromBundle.elements().size() + doors);

    // Per kind, because the Tiled map groups its objects into one layer each
    // and the bundle interleaves them. Only the order within a kind is
    // observable to the game -- every reader filters by kind first -- so that
    // is what is compared.
    for (const MapElementKind kind : {MapElementKind::Spawn, MapElementKind::Teleporter}) {
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

    // What the Tiled map says about itself never reaches the bundle either.
    CHECK_EQ(fromTiled.defaultMobGroup(), std::string("garden"));
    CHECK(fromBundle.defaultMobGroup().empty());
}

TEST(the_tilesets_flags_agree_with_the_engine) {
    TiledMap map;
    std::string error;
    CHECK(map.load(tiledPath(), error));
    // The default family: six plain tiles -- air, wall, water and the three
    // the map adds -- plus an edge variant per non-empty side combination
    // for wall and for water. The biome skins come after (see the tile-skin
    // section below) and never change this first block.
    int defaultFamily = 0;
    int plain = 0;
    for (const TiledTileType& entry : map.palette()) {
        if (entry.skin != 0) continue;
        ++defaultFamily;
        if (entry.edgeMask == 0) ++plain;
    }
    CHECK_EQ(defaultFamily, 6 + 15 + 15);
    CHECK_EQ(plain, 6);
    CHECK(map.palette().size() >= 36);
    // The tileset is what an author sees. If it says a tile is walkable and
    // constants.h collides with it, the editor is lying to whoever edits the
    // map, and the map is the thing that needs fixing.
    const std::vector<std::string> mismatched = map.mismatchedFlags();
    for (const std::string& name : mismatched) {
        std::fprintf(stderr, "[tiled] tile \"%s\" disagrees with constants.h\n", name.c_str());
    }
    CHECK(mismatched.empty());
}

TEST(every_polygon_zone_keeps_its_outline) {
    // A tier band drawn as an outline has to come back as one: a zone that
    // came back as its bounding box would still work, which is exactly why it
    // needs asserting -- nothing else would notice. Rectangles are legal too
    // -- the converted biome rooms and the mob regions are rectangles -- so
    // what is checked is that the shipped map's polygon bands survived, not
    // that every band is one.
    MapData map;
    std::string error;
    CHECK(map.loadWorldMap(tiledPath(), error));

    int zones = 0;
    int outlined = 0;
    for (const MapElement& element : map.elements()) {
        if (element.kind != MapElementKind::Spawn) continue;
        ++zones;
        if (element.polygon.empty()) continue;
        ++outlined;
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
    // The shipped world's coastline bands are polygons; a loader that
    // flattened every band to its bounding box would pass everything above.
    CHECK(outlined > 0);
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

// ---------------------------------------------------------------------------
// Edge variants
// ---------------------------------------------------------------------------
//
// The outline a wall or water tile wears is a TILE now: scripts/edgeTiles.js
// looks at each cell's neighbours and writes the tileset variant for the sides
// it exposes. The loader's part is to hand back the base tile id -- so the
// game collides with exactly what it did before -- and the mask beside it.

namespace {

/// The gid of a terrain.tsj tile: the plain tiles are local ids 0..5, the
/// wall variants 6..20 and the water variants 21..35, each in mask order.
int wallGid(std::uint8_t mask) { return mask == 0 ? 2 : 6 + mask; }
int waterGid(std::uint8_t mask) { return mask == 0 ? 3 : 21 + mask; }

/// The contract's exposure rule, over a TiledMap's resolved grid: a wall or
/// water side is exposed when the neighbour is off the map or air, and a
/// WALL side also when the neighbour is water. Everything else shows nothing.
std::uint8_t expectedMask(const TiledMap& map, int tx, int ty) {
    const auto tileAt = [&](int x, int y) -> int {
        if (x < 0 || y < 0 || x >= map.width() || y >= map.height()) return -1;
        return map.tiles()[static_cast<std::size_t>(y * map.width() + x)];
    };
    const int self = tileAt(tx, ty);
    if (self != static_cast<int>(Tile::Wall) && self != static_cast<int>(Tile::Water)) return 0;
    const auto exposed = [&](int x, int y) {
        const int other = tileAt(x, y);
        if (other < 0 || other == static_cast<int>(Tile::Ground)) return true;
        return self == static_cast<int>(Tile::Wall) && other == static_cast<int>(Tile::Water);
    };
    std::uint8_t mask = 0;
    if (exposed(tx, ty - 1)) mask |= kEdgeNorth;
    if (exposed(tx + 1, ty)) mask |= kEdgeEast;
    if (exposed(tx, ty + 1)) mask |= kEdgeSouth;
    if (exposed(tx - 1, ty)) mask |= kEdgeWest;
    return mask;
}

std::string smallMap(const std::vector<int>& gids) {
    std::string data;
    for (std::size_t i = 0; i < gids.size(); ++i) {
        if (i) data += ", ";
        data += std::to_string(gids[i]);
    }
    return R"({
      "type": "map", "orientation": "orthogonal", "infinite": false,
      "width": 3, "height": 2, "tilewidth": 300, "tileheight": 300,
      "tilesets": [{"firstgid": 1, "source": "terrain.tsj"}],
      "layers": [
        {"type": "tilelayer", "name": "terrain", "width": 3, "height": 2, "data": [)" +
           data + R"(]}
      ]})";
}

} // namespace

TEST(the_edges_property_parses_in_every_spelling) {
    CHECK_EQ(int(parseEdgeMask("n")), int(kEdgeNorth));
    CHECK_EQ(int(parseEdgeMask("ne")), int(kEdgeNorth | kEdgeEast));
    CHECK_EQ(int(parseEdgeMask("n,e")), int(kEdgeNorth | kEdgeEast));
    CHECK_EQ(int(parseEdgeMask("n e s")), int(kEdgeNorth | kEdgeEast | kEdgeSouth));
    CHECK_EQ(int(parseEdgeMask("S, W")), int(kEdgeSouth | kEdgeWest));
    CHECK_EQ(int(parseEdgeMask("nesw")), int(kEdgeMaskMax));
    CHECK_EQ(int(parseEdgeMask("")), 0);
    CHECK_EQ(int(parseEdgeMask("x")), 0);
    CHECK(edgeMaskSuffix(kEdgeNorth | kEdgeWest) == "nw");
    CHECK(edgeMaskSuffix(kEdgeMaskMax) == "nesw");
    CHECK(edgeMaskSuffix(0).empty());
}

TEST(the_tilesets_edge_tiles_resolve_to_their_base_tile_and_mask) {
    TiledMap map;
    std::string error;
    CHECK(map.load(tiledPath(), error));
    // Every mask 1..15 once for wall and once for water, each named for its
    // sides and carrying its base tile's id and flags -- per skin, with the
    // skin's name spliced into the class (`wall_sewers_edge_ne`).
    int wallMasks[kTileSkinCount] = {};
    int waterMasks[kTileSkinCount] = {};
    for (const TiledTileType& entry : map.palette()) {
        if (entry.edgeMask == 0) continue;
        CHECK(entry.edgeMask <= kEdgeMaskMax);
        CHECK(entry.skin < kTileSkinCount);
        const std::string suffix = edgeMaskSuffix(entry.edgeMask);
        const std::string skin = entry.skin == 0 ? "" : std::string("_") + kTileSkinNames[entry.skin];
        if (entry.id == static_cast<int>(Tile::Wall)) {
            CHECK(entry.name == "wall" + skin + "_edge_" + suffix);
            CHECK(entry.solid && !entry.water);
            wallMasks[entry.skin] |= 1 << entry.edgeMask;
        } else if (entry.id == static_cast<int>(Tile::Water)) {
            CHECK(entry.name == "water" + skin + "_edge_" + suffix);
            CHECK(!entry.solid && entry.water);
            waterMasks[entry.skin] |= 1 << entry.edgeMask;
        } else {
            std::fprintf(stderr, "[tiled] edge tile \"%s\" has tile id %d\n", entry.name.c_str(),
                         entry.id);
            CHECK(false);
        }
    }
    CHECK_EQ(wallMasks[0], 0xFFFE);
    CHECK_EQ(waterMasks[0], 0xFFFE);
    // A biome skin is all or nothing: either the tileset has not grown it
    // yet, or it carries the complete set for both families.
    for (int skin = 1; skin < kTileSkinCount; ++skin) {
        CHECK(wallMasks[skin] == 0 || wallMasks[skin] == 0xFFFE);
        CHECK(waterMasks[skin] == wallMasks[skin]);
    }
    // The variants disagree with the engine about nothing: they are their
    // base tile, flags included.
    CHECK(map.mismatchedFlags().empty());
}

TEST(a_map_with_edge_variants_loads_to_the_same_tile_grid) {
    // A 3x2 map: a wall row over a pond and air. Once as plain tiles, once as
    // the variants the authoring script would write for it.
    //
    //   W W W        nw  ns  nes
    //   W A G        esw es  -
    const std::vector<int> plain = {wallGid(0), wallGid(0), wallGid(0),
                                    wallGid(0), waterGid(0), 0};
    const std::vector<int> edged = {wallGid(kEdgeNorth | kEdgeWest),
                                    wallGid(kEdgeNorth | kEdgeSouth),
                                    wallGid(kEdgeNorth | kEdgeEast | kEdgeSouth),
                                    wallGid(kEdgeEast | kEdgeSouth | kEdgeWest),
                                    waterGid(kEdgeEast | kEdgeSouth), 0};
    const std::string plainPath = writeTemp("edge_plain.tmj", smallMap(plain));
    const std::string edgedPath = writeTemp("edge_variants.tmj", smallMap(edged));

    TiledMap a;
    TiledMap b;
    std::string error;
    CHECK(a.load(plainPath, error));
    CHECK(b.load(edgedPath, error));
    CHECK(a.tiles() == b.tiles());
    CHECK(a.styles().size() == a.tiles().size());
    CHECK(b.styles().size() == b.tiles().size());
    for (const std::uint8_t style : a.styles()) CHECK_EQ(int(style), 0);
    // The default family's variants are skin 0, so the style IS the mask.
    const std::vector<std::uint8_t> expected = {
        kEdgeNorth | kEdgeWest, kEdgeNorth | kEdgeSouth, kEdgeNorth | kEdgeEast | kEdgeSouth,
        kEdgeEast | kEdgeSouth | kEdgeWest, kEdgeEast | kEdgeSouth, 0};
    CHECK(b.styles() == expected);
    // And those are exactly the masks the exposure rule gives, so the sample
    // agrees with what the script would have written.
    for (int ty = 0; ty < b.height(); ++ty) {
        for (int tx = 0; tx < b.width(); ++tx) {
            const std::uint8_t style = b.styles()[static_cast<std::size_t>(ty * b.width() + tx)];
            CHECK_EQ(int(styleSkin(style)), 0);
            CHECK_EQ(int(styleEdgeMask(style)), int(expectedMask(b, tx, ty)));
        }
    }

    // Through Terrain, into a realm of its own, the masks come with the tiles.
    Terrain terrain;
    const Realm realm = worldRealm(3);
    CHECK(terrain.loadTiledMap(edgedPath, error, realm));
    CHECK_EQ(terrain.tileCols(realm), 3);
    CHECK_EQ(terrain.tileRows(realm), 2);
    CHECK(terrain.atTile(1, 1, realm) == Tile::Water);
    CHECK_EQ(int(terrain.edgeMaskAt(1, 1, realm)), int(kEdgeEast | kEdgeSouth));
    CHECK_EQ(int(terrain.edgeMaskAt(2, 0, realm)), int(kEdgeNorth | kEdgeEast | kEdgeSouth));
    CHECK_EQ(int(terrain.edgeMaskAt(2, 1, realm)), 0);
    CHECK_EQ(int(terrain.skinAt(1, 1, realm)), 0);
    Terrain fromPlain;
    CHECK(fromPlain.loadTiledMap(plainPath, error, realm));
    CHECK(fromPlain.styles(realm).empty() ||
          std::count(fromPlain.styles(realm).begin(), fromPlain.styles(realm).end(), 0) ==
              static_cast<long>(fromPlain.styles(realm).size()));

    std::remove(plainPath.c_str());
    std::remove(edgedPath.c_str());
}

TEST(the_shipped_maps_edge_variants_match_their_exposure) {
    // The authoring script and the engine implement one exposure rule. A map
    // that has been run through the script must carry, on every wall and
    // water cell, exactly the mask that rule gives -- whatever skin the cell
    // wears -- and a map that has not carries no masks at all, never a stale
    // half-set. Every map the manifest names, so a generated biome map that
    // skipped the script is caught here too.
    std::vector<std::string> names = {"world.tmj", "sewers.tmj"};
    {
        Json manifest;
        std::string text;
        std::ifstream input(dataDir() + "/maps.json", std::ios::binary);
        text.assign((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        std::string parseError;
        if (!text.empty() && Json::parse(text, manifest, parseError)) {
            for (const Json& entry : manifest["maps"].items()) {
                const std::string file = entry["file"].asString();
                if (!file.empty() && std::find(names.begin(), names.end(), file) == names.end()) {
                    names.push_back(file);
                }
            }
        }
    }
    for (const std::string& name : names) {
        const std::string path = dataDir() + "/" + name;
        if (!staged(path)) continue;
        TiledMap map;
        std::string error;
        if (!map.load(path, error)) {
            std::fprintf(stderr, "[tiled] %s\n", error.c_str());
            CHECK(false);
            continue;
        }
        CHECK(map.styles().size() == map.tiles().size());
        bool any = false;
        for (std::size_t i = 0; i < map.styles().size(); ++i) {
            const std::uint8_t tile = map.tiles()[i];
            if (tile != static_cast<std::uint8_t>(Tile::Wall) &&
                tile != static_cast<std::uint8_t>(Tile::Water)) {
                continue;
            }
            any = any || styleEdgeMask(map.styles()[i]) != 0;
        }
        int wrong = 0;
        for (int ty = 0; ty < map.height(); ++ty) {
            for (int tx = 0; tx < map.width(); ++tx) {
                const std::size_t i = static_cast<std::size_t>(ty * map.width() + tx);
                const std::uint8_t tile = map.tiles()[i];
                if (tile != static_cast<std::uint8_t>(Tile::Wall) &&
                    tile != static_cast<std::uint8_t>(Tile::Water)) {
                    continue;   // an air cell's low nibble is a floor variant
                }
                const std::uint8_t have = styleEdgeMask(map.styles()[i]);
                const std::uint8_t want = any ? expectedMask(map, tx, ty) : 0;
                if (have != want && wrong++ < 5) {
                    std::fprintf(stderr, "[tiled] %s: cell (%d, %d) has edges %d, expected %d\n",
                                 name.c_str(), tx, ty, have, want);
                }
            }
        }
        CHECK_EQ(wrong, 0);
    }
}

// ---------------------------------------------------------------------------
// Tile skins
// ---------------------------------------------------------------------------
//
// A cell's style byte is its skin (high nibble) and its edge mask or, for air,
// its floor variant (low nibble). The tileset carries both as properties, and
// the loader resolves a gid to (tileId, style). These tests use a tileset of
// their own, written on the spot, so they hold whether or not the shipped
// terrain.tsj has grown its biome families yet.

namespace {

/// A tileset in the shape scripts/lib/tileArt.js writes, with one of each
/// kind of tile the loader has to tell apart, plus two it has to complain
/// about. Local ids:
///   0 air  1 wall  2 water  3 wall_edge_n
///   4 wall_sewers  5 wall_sewers_edge_ne  6 water_unknown  7 water_unknown_edge_s
///   8 floor_sewers_2  9 wall_mars (a skin the engine does not know)
const char* kSkinTileset = R"({
  "type": "tileset", "name": "skins", "tilecount": 10, "tilewidth": 300, "tileheight": 300,
  "columns": 0, "tiles": [
    {"id": 0, "class": "air", "image": "tiles/air.svg", "properties": [
      {"name": "tileId", "type": "int", "value": 0}, {"name": "solid", "type": "bool", "value": false},
      {"name": "water", "type": "bool", "value": false}]},
    {"id": 1, "class": "wall", "image": "tiles/wall.svg", "properties": [
      {"name": "tileId", "type": "int", "value": 1}, {"name": "solid", "type": "bool", "value": true},
      {"name": "water", "type": "bool", "value": false}]},
    {"id": 2, "class": "water", "image": "tiles/water.svg", "properties": [
      {"name": "tileId", "type": "int", "value": 2}, {"name": "solid", "type": "bool", "value": false},
      {"name": "water", "type": "bool", "value": true}]},
    {"id": 3, "class": "wall_edge_n", "image": "tiles/wall_edge_n.svg", "properties": [
      {"name": "tileId", "type": "int", "value": 1}, {"name": "solid", "type": "bool", "value": true},
      {"name": "water", "type": "bool", "value": false}, {"name": "edges", "type": "string", "value": "n"}]},
    {"id": 4, "class": "wall_sewers", "image": "tiles/wall_sewers.svg", "properties": [
      {"name": "tileId", "type": "int", "value": 1}, {"name": "solid", "type": "bool", "value": true},
      {"name": "water", "type": "bool", "value": false}, {"name": "skin", "type": "string", "value": "sewers"}]},
    {"id": 5, "class": "wall_sewers_edge_ne", "image": "tiles/wall_sewers_edge_ne.svg", "properties": [
      {"name": "tileId", "type": "int", "value": 1}, {"name": "solid", "type": "bool", "value": true},
      {"name": "water", "type": "bool", "value": false}, {"name": "skin", "type": "string", "value": "sewers"},
      {"name": "edges", "type": "string", "value": "ne"}]},
    {"id": 6, "class": "water_unknown", "image": "tiles/water_unknown.svg", "properties": [
      {"name": "tileId", "type": "int", "value": 2}, {"name": "solid", "type": "bool", "value": false},
      {"name": "water", "type": "bool", "value": true}, {"name": "skin", "type": "string", "value": "unknown"}]},
    {"id": 7, "class": "water_unknown_edge_s", "image": "tiles/water_unknown_edge_s.svg", "properties": [
      {"name": "tileId", "type": "int", "value": 2}, {"name": "solid", "type": "bool", "value": false},
      {"name": "water", "type": "bool", "value": true}, {"name": "skin", "type": "string", "value": "unknown"},
      {"name": "edges", "type": "string", "value": "s"}]},
    {"id": 8, "class": "floor_sewers_2", "image": "tiles/floor_sewers_2.svg", "properties": [
      {"name": "tileId", "type": "int", "value": 0}, {"name": "solid", "type": "bool", "value": false},
      {"name": "water", "type": "bool", "value": false}, {"name": "skin", "type": "string", "value": "sewers"},
      {"name": "variant", "type": "int", "value": 2}]},
    {"id": 9, "class": "wall_mars", "image": "tiles/wall_mars.svg", "properties": [
      {"name": "tileId", "type": "int", "value": 1}, {"name": "solid", "type": "bool", "value": true},
      {"name": "water", "type": "bool", "value": false}, {"name": "skin", "type": "string", "value": "mars"}]}
  ]})";

/// A 5x2 map over kSkinTileset (firstgid 1, so gid = local id + 1) with an
/// embedded one-tile ground tileset at `groundFirstGid`, and a background
/// layer painting every cell with it.
std::string skinMap(int groundFirstGid, const std::vector<int>& gids) {
    std::string data;
    for (std::size_t i = 0; i < gids.size(); ++i) {
        if (i) data += ", ";
        data += std::to_string(gids[i]);
    }
    std::string ground;
    for (std::size_t i = 0; i < gids.size(); ++i) {
        if (i) ground += ", ";
        ground += std::to_string(groundFirstGid);
    }
    return R"({
      "type": "map", "orientation": "orthogonal", "infinite": false,
      "width": 5, "height": 2, "tilewidth": 300, "tileheight": 300,
      "tilesets": [
        {"firstgid": 1, "source": "skins_test.tsj"},
        {"firstgid": )" + std::to_string(groundFirstGid) + R"(, "name": "ground", "type": "tileset",
         "tilecount": 1, "tilewidth": 300, "tileheight": 300, "columns": 0,
         "tiles": [{"id": 0, "class": "garden", "image": "ground/land.svg",
                    "properties": [{"name": "groundId", "type": "int", "value": 0}]}]}
      ],
      "layers": [
        {"type": "tilelayer", "name": "terrain", "width": 5, "height": 2, "data": [)" + data + R"(]},
        {"type": "tilelayer", "name": "background", "width": 5, "height": 2, "data": [)" + ground + R"(]}
      ]})";
}

} // namespace

TEST(skinned_tiles_resolve_to_their_base_tile_and_style) {
    const std::string tilesetPath = writeTemp("skins_test.tsj", kSkinTileset);
    //   wall  wall_edge_n  wall_sewers  wall_sewers_edge_ne  water
    //   water_unknown  water_unknown_edge_s  floor_sewers_2  air(gid 1)  empty(gid 0)
    const std::string mapPath = writeTemp("skins_test.tmj",
                                          skinMap(11, {2, 4, 5, 6, 3, 7, 8, 9, 1, 0}));
    TiledMap map;
    std::string error;
    CHECK(map.load(mapPath, error));
    if (!error.empty()) std::fprintf(stderr, "[tiled] %s\n", error.c_str());
    CHECK(map.warnings().size() == 1);   // wall_mars, below

    // The tile ids are the base family's: the game collides with exactly what
    // an unskinned map would give it.
    const std::vector<std::uint8_t> tiles = {1, 1, 1, 1, 2, 2, 2, 0, 0, 0};
    CHECK(map.tiles() == tiles);
    const std::vector<std::uint8_t> styles = {
        0,
        makeStyle(0, kEdgeNorth),
        makeStyle(1, 0),
        makeStyle(1, kEdgeNorth | kEdgeEast),
        0,
        makeStyle(3, 0),
        makeStyle(3, kEdgeSouth),
        makeStyle(1, 2),
        0,
        0,
    };
    CHECK(map.styles() == styles);
    // Spelled as bytes: a default wall is 0x00, a sewers-skinned wall 0x10 --
    // the skin in the high nibble, nothing in the low one (mask aside).
    CHECK_EQ(int(map.styles()[0]), 0x00);
    CHECK_EQ(int(map.styles()[2]), 0x10);
    CHECK_EQ(int(styleSkin(map.styles()[2])), int(tileSkinIndex("sewers")));
    CHECK_EQ(int(map.styles()[5]), 0x30);   // water_unknown: skin 3
    for (std::size_t i = 0; i < styles.size() && i < map.styles().size(); ++i) {
        if (map.styles()[i] != styles[i]) {
            std::fprintf(stderr, "[tiled] cell %zu has style %d, expected %d\n", i,
                         map.styles()[i], styles[i]);
        }
    }

    // The palette says the same thing per tile.
    for (const TiledTileType& entry : map.palette()) {
        if (entry.name == "wall_sewers_edge_ne") {
            CHECK_EQ(int(entry.skin), 1);
            CHECK_EQ(int(entry.edgeMask), int(kEdgeNorth | kEdgeEast));
            CHECK_EQ(int(entry.variant), 0);
            CHECK_EQ(int(entry.style()), int(makeStyle(1, kEdgeNorth | kEdgeEast)));
        } else if (entry.name == "floor_sewers_2") {
            CHECK_EQ(entry.id, 0);
            CHECK_EQ(int(entry.skin), 1);
            CHECK_EQ(int(entry.variant), 2);
            CHECK_EQ(int(entry.edgeMask), 0);
            CHECK(!entry.solid && !entry.water);
        } else if (entry.name == "water_unknown") {
            CHECK_EQ(int(entry.skin), int(tileSkinIndex("unknown")));
            CHECK(entry.water);
        }
    }
    CHECK(map.mismatchedFlags().empty());

    // Through Terrain, the per-cell reads split the byte by tile kind.
    Terrain terrain;
    const Realm realm = worldRealm(4);
    CHECK(terrain.loadTiledMap(mapPath, error, realm));
    CHECK(terrain.atTile(2, 0, realm) == Tile::Wall);
    CHECK_EQ(int(terrain.skinAt(2, 0, realm)), 1);
    CHECK_EQ(int(terrain.edgeMaskAt(2, 0, realm)), 0);
    CHECK_EQ(int(terrain.skinAt(3, 0, realm)), 1);
    CHECK_EQ(int(terrain.edgeMaskAt(3, 0, realm)), int(kEdgeNorth | kEdgeEast));
    CHECK(terrain.atTile(1, 1, realm) == Tile::Water);
    CHECK_EQ(int(terrain.skinAt(1, 1, realm)), 3);
    CHECK_EQ(int(terrain.edgeMaskAt(1, 1, realm)), int(kEdgeSouth));
    CHECK(terrain.atTile(2, 1, realm) == Tile::Ground);
    CHECK_EQ(int(terrain.skinAt(2, 1, realm)), 1);
    CHECK_EQ(int(terrain.floorVariantAt(2, 1, realm)), 2);
    CHECK_EQ(int(terrain.edgeMaskAt(2, 1, realm)), 0);   // a floor has no edges
    CHECK_EQ(int(terrain.floorVariantAt(1, 1, realm)), 0);   // and water no floor
    // A floor decoration is ordinary walkable ground for everything else.
    CHECK(!terrain.blocked(Terrain::tileCenter(2, 1), realm));

    std::remove(mapPath.c_str());
    std::remove(tilesetPath.c_str());
}

TEST(an_unknown_skin_is_reported_and_drawn_as_the_default) {
    const std::string tilesetPath = writeTemp("skins_test.tsj", kSkinTileset);
    const std::string mapPath = writeTemp("skins_unknown.tmj",
                                          skinMap(11, {10, 10, 2, 0, 0, 0, 0, 0, 0, 0}));
    TiledMap map;
    std::string error;
    // Not refused: an author's new family should not stop the server. But
    // said out loud, because the wall will draw as the plain family.
    CHECK(map.load(mapPath, error));
    CHECK(map.warnings().size() == 1);
    if (!map.warnings().empty()) {
        CHECK(map.warnings()[0].find("wall_mars") != std::string::npos);
        CHECK(map.warnings()[0].find("mars") != std::string::npos);
    }
    CHECK_EQ(int(map.tiles()[0]), int(Tile::Wall));
    CHECK_EQ(int(map.styles()[0]), 0);
    CHECK_EQ(int(map.styles()[1]), 0);
    for (const TiledTileType& entry : map.palette()) {
        if (entry.name == "wall_mars") CHECK_EQ(int(entry.skin), 0);
    }
    CHECK(tileSkinIndex("mars") < 0);
    CHECK_EQ(tileSkinIndex(""), 0);
    CHECK_EQ(tileSkinIndex("sewers"), 1);
    CHECK_EQ(tileSkinIndex("computer"), 2);
    CHECK_EQ(tileSkinIndex("unknown"), 3);
    std::remove(mapPath.c_str());
    std::remove(tilesetPath.c_str());
}

TEST(a_removed_skin_family_is_reported_and_drawn_as_the_default) {
    // The six biomes that used to have tile families of their own (garden,
    // desert, hel, ocean, ant_hell, jungle) are plain default tiles now. A
    // tileset that still names one of them -- a map from before the revert,
    // say -- is not refused, but the engine says so and the cell draws as the
    // default family rather than borrowing whichever skin now sits at the
    // old index.
    for (const char* removed : {"garden", "desert", "hel", "ocean", "ant_hell", "jungle"}) {
        CHECK(tileSkinIndex(removed) < 0);
    }
    const std::string tilesetPath = writeTemp("skins_removed.tsj", R"({
      "type": "tileset", "name": "removed", "tilecount": 2, "tilewidth": 300, "tileheight": 300,
      "columns": 0, "tiles": [
        {"id": 0, "class": "wall_garden", "image": "tiles/wall_garden.svg", "properties": [
          {"name": "tileId", "type": "int", "value": 1}, {"name": "solid", "type": "bool", "value": true},
          {"name": "water", "type": "bool", "value": false}, {"name": "skin", "type": "string", "value": "garden"},
          {"name": "edges", "type": "string", "value": "ne"}]},
        {"id": 1, "class": "wall_sewers", "image": "tiles/wall_sewers.svg", "properties": [
          {"name": "tileId", "type": "int", "value": 1}, {"name": "solid", "type": "bool", "value": true},
          {"name": "water", "type": "bool", "value": false}, {"name": "skin", "type": "string", "value": "sewers"}]}
      ]})");
    const std::string mapPath = writeTemp("skins_removed.tmj", R"({
      "type": "map", "orientation": "orthogonal", "infinite": false,
      "width": 2, "height": 1, "tilewidth": 300, "tileheight": 300,
      "tilesets": [{"firstgid": 1, "source": "skins_removed.tsj"}],
      "layers": [{"type": "tilelayer", "name": "terrain", "width": 2, "height": 1, "data": [1, 2]}]})");
    TiledMap map;
    std::string error;
    CHECK(map.load(mapPath, error));
    CHECK(error.empty());
    CHECK(map.warnings().size() == 1);
    if (!map.warnings().empty()) {
        CHECK(map.warnings()[0].find("wall_garden") != std::string::npos);
        CHECK(map.warnings()[0].find("\"garden\"") != std::string::npos);
    }
    CHECK_EQ(int(map.tiles()[0]), int(Tile::Wall));
    CHECK_EQ(int(map.tiles()[1]), int(Tile::Wall));
    // Skin 0 with its edge mask kept; the sewers wall beside it is untouched.
    CHECK_EQ(int(map.styles()[0]), int(makeStyle(0, kEdgeNorth | kEdgeEast)));
    CHECK_EQ(int(map.styles()[1]), 0x10);
    for (const TiledTileType& entry : map.palette()) {
        if (entry.name == "wall_garden") CHECK_EQ(int(entry.skin), 0);
        if (entry.name == "wall_sewers") CHECK_EQ(int(entry.skin), 1);
    }
    std::remove(mapPath.c_str());
    std::remove(tilesetPath.c_str());
}

TEST(the_ground_fallback_skin_is_a_table_not_ground_plus_one) {
    // The renderer dresses a plain wall in skinForGround() of the ground
    // under it. Only sewers, computer and unknown have families; every other
    // ground -- and anything off the table -- is the default brown wall.
    CHECK_EQ(kSectionCount, 9);
    const int expected[kSectionCount] = {0, 0, 0, 0, 0, 0, 1, 2, 3};
    for (int ground = 0; ground < kSectionCount; ++ground) {
        CHECK_EQ(int(skinForGround(ground)), expected[ground]);
        CHECK(skinForGround(ground) < kTileSkinCount);
    }
    CHECK_EQ(int(skinForGround(6)), tileSkinIndex("sewers"));
    CHECK_EQ(int(skinForGround(7)), tileSkinIndex("computer"));
    CHECK_EQ(int(skinForGround(8)), tileSkinIndex("unknown"));
    // Void (-1) and anything past the palette: the default family.
    CHECK_EQ(int(skinForGround(-1)), 0);
    CHECK_EQ(int(skinForGround(kSectionCount)), 0);
    CHECK_EQ(int(skinForGround(255)), 0);
}

TEST(a_floor_variant_out_of_range_is_refused) {
    const std::string tilesetPath = writeTemp("skins_bad_variant.tsj", R"({
      "type": "tileset", "name": "bad", "tilecount": 1, "tilewidth": 300, "tileheight": 300,
      "columns": 0, "tiles": [
        {"id": 0, "class": "floor_sewers_16", "image": "tiles/floor_sewers_16.svg", "properties": [
          {"name": "tileId", "type": "int", "value": 0}, {"name": "skin", "type": "string", "value": "sewers"},
          {"name": "variant", "type": "int", "value": 16}]}
      ]})");
    const std::string mapPath = writeTemp("skins_bad_variant.tmj", R"({
      "type": "map", "orientation": "orthogonal", "infinite": false,
      "width": 1, "height": 1, "tilewidth": 300, "tileheight": 300,
      "tilesets": [{"firstgid": 1, "source": "skins_bad_variant.tsj"}],
      "layers": [{"type": "tilelayer", "name": "terrain", "width": 1, "height": 1, "data": [1]}]})");
    TiledMap map;
    std::string error;
    // A variant is a nibble; 16 would silently become skin bits.
    CHECK(!map.load(mapPath, error));
    CHECK(error.find("16") != std::string::npos);
    std::remove(mapPath.c_str());
    std::remove(tilesetPath.c_str());
}

TEST(tilesets_with_overlapping_gid_ranges_are_refused) {
    const std::string tilesetPath = writeTemp("skins_test.tsj", kSkinTileset);
    // The skins tileset owns gids 1..10 (an EXTERNAL tileset, so its count is
    // read out of its own file). A ground tileset starting at 10 shares one
    // gid with it; at 11 it does not.
    const std::string overlapping = writeTemp("skins_overlap.tmj",
                                              skinMap(10, {2, 2, 2, 2, 2, 2, 2, 2, 2, 2}));
    TiledMap map;
    std::string error;
    CHECK(!map.load(overlapping, error));
    CHECK(error.find("overlap") != std::string::npos);
    // Named after both tilesets and saying what would fix it.
    CHECK(error.find("skins_test.tsj") != std::string::npos);
    CHECK(error.find("ground") != std::string::npos);
    CHECK(error.find("11") != std::string::npos);
    CHECK(map.tiles().empty());

    const std::string adjacent = writeTemp("skins_adjacent.tmj",
                                           skinMap(11, {2, 2, 2, 2, 2, 2, 2, 2, 2, 2}));
    CHECK(map.load(adjacent, error));
    CHECK(map.background().size() == 10);

    // The shipped shape of the bug: the ground tileset at the firstgid the
    // six-tile palette left it at, under a terrain tileset that has grown.
    const std::string shipped = writeTemp("skins_shipped_overlap.tmj",
                                          skinMap(7, {2, 2, 2, 2, 2, 2, 2, 2, 2, 2}));
    CHECK(!map.load(shipped, error));
    CHECK(error.find("overlap") != std::string::npos);

    std::remove(overlapping.c_str());
    std::remove(adjacent.c_str());
    std::remove(shipped.c_str());
    std::remove(tilesetPath.c_str());
}

TEST(the_shipped_tileset_follows_the_skin_contract) {
    // terrain.tsj carries, per skin in kTileSkinNames (sewers, computer,
    // unknown), the exact 35-entry block scripts/lib/tileArt.js is contracted
    // to write after the 36 default tiles -- 141 in all -- or exactly the
    // default family and nothing else. Anything in between is a half-written
    // tileset (a family the engine no longer knows still in the file, say),
    // and a map painted with it draws holes.
    TiledMap map;
    std::string error;
    CHECK(map.load(tiledPath(), error));
    const std::size_t perSkin = 1 + 15 + 1 + 15 + kFloorVariantsPerSkin;
    const std::size_t grown = 36 + (kTileSkinCount - 1) * perSkin;
    CHECK_EQ(grown, std::size_t(141));
    CHECK(map.palette().size() == 36 || map.palette().size() == grown);
    if (map.palette().size() != 36 && map.palette().size() != grown) {
        std::fprintf(stderr, "[tiled] terrain.tsj has %zu tiles, expected 36 or %zu\n",
                     map.palette().size(), grown);
    }
    if (map.palette().size() != grown) return;

    // The first 36 are untouched: the six plain tiles, then the default
    // wall and water variants.
    CHECK(map.palette()[0].name == "air");
    CHECK(map.palette()[1].name == "wall");
    CHECK(map.palette()[2].name == "water");
    CHECK(map.palette()[6].name == "wall_edge_n");
    CHECK(map.palette()[35].name == "water_edge_nesw");
    for (std::size_t i = 0; i < 36; ++i) CHECK_EQ(int(map.palette()[i].skin), 0);

    // Then each skin's block, in kTileSkinNames order.
    for (int skin = 1; skin < kTileSkinCount; ++skin) {
        const std::string name = kTileSkinNames[skin];
        std::size_t at = 36 + static_cast<std::size_t>(skin - 1) * perSkin;
        const auto expect = [&](const std::string& className, int tileId, std::uint8_t mask,
                                std::uint8_t variant) {
            const TiledTileType& entry = map.palette()[at++];
            if (entry.name != className) {
                std::fprintf(stderr, "[tiled] tileset entry %zu is \"%s\", expected \"%s\"\n",
                             at - 1, entry.name.c_str(), className.c_str());
            }
            CHECK(entry.name == className);
            CHECK_EQ(entry.id, tileId);
            CHECK_EQ(int(entry.skin), skin);
            CHECK_EQ(int(entry.edgeMask), int(mask));
            CHECK_EQ(int(entry.variant), int(variant));
        };
        expect("wall_" + name, 1, 0, 0);
        for (std::uint8_t mask = 1; mask <= kEdgeMaskMax; ++mask) {
            expect("wall_" + name + "_edge_" + edgeMaskSuffix(mask), 1, mask, 0);
        }
        expect("water_" + name, 2, 0, 0);
        for (std::uint8_t mask = 1; mask <= kEdgeMaskMax; ++mask) {
            expect("water_" + name + "_edge_" + edgeMaskSuffix(mask), 2, mask, 0);
        }
        for (int variant = 0; variant < kFloorVariantsPerSkin; ++variant) {
            expect("floor_" + name + "_" + std::to_string(variant), 0, 0,
                   static_cast<std::uint8_t>(variant));
        }
    }
    CHECK(map.mismatchedFlags().empty());
    CHECK(map.warnings().empty());
}
