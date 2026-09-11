#include "test.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "shared/core/json.h"
#include "shared/game/constants.h"
#include "shared/game/map_elements.h"
#include "shared/game/terrain.h"
#include "shared/game/tiled_map.h"

using namespace flix;

// The map reader, against maps it owns.
//
// Every fixture here is written into a temp directory by the test itself, so
// what is checked is the FORMAT rather than whatever the shipped map happens
// to contain today -- a rule the shipped map never exercises (water under a
// castle, a pond on a layer that does not collide, two tilesets fighting over
// a gid) is exactly the rule that breaks silently. The shipped map gets one
// test of its own at the bottom, which is about the map, not about the reader.
//
// The rule every collision test here is about: a cell is BLOCKED when any
// layer whose `has_collision` property is set has a tile there, and it is
// Water rather than Wall when the topmost such tile is tagged `water` in the
// tileset. No tile decides whether anything blocks.

namespace {

std::string tempDir() {
    const char* env = std::getenv("TMPDIR");
    std::string base = (env != nullptr && *env != '\0') ? env : "/tmp";
    if (base.back() != '/') base.push_back('/');
    base += "flix_tiled_tests";
    mkdir(base.c_str(), 0755);   // already there is fine
    return base;
}

/// Writes a fixture file and hands back its path.
std::string write(const std::string& name, const std::string& text) {
    const std::string path = tempDir() + "/" + name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return path;
}

/// The tileset the fixture maps below paint from. Tagged exactly the way
/// maps/tileset.tsj tags its own: a `water` boolean, a `covers_everything`
/// boolean, and nothing else. Nothing here says whether a tile blocks,
/// because no tile does.
///
/// gid = local id + 1, so: 1 grass, 2 castle, 3 water, 4 bridge, 5 dirt.
constexpr const char* kTileset = R"({
 "columns": 0, "name": "fixture", "tilecount": 5, "tiledversion": "1.10.1",
 "tilewidth": 300, "tileheight": 300, "tilerendersize": "grid",
 "type": "tileset", "version": "1.10",
 "tiles": [
  { "id": 0, "image": "tiles/grass.svg",
    "properties": [ { "name": "covers_everything", "type": "bool", "value": true } ] },
  { "id": 1, "image": "tiles/castle.svg",
    "properties": [ { "name": "covers_everything", "type": "bool", "value": true } ] },
  { "id": 2, "image": "tiles/water.svg",
    "properties": [ { "name": "water", "type": "bool", "value": true } ] },
  { "id": 3, "image": "tiles/bridge.svg" },
  { "id": 4, "image": "tiles/dirt.svg",
    "properties": [ { "name": "covers_everything", "type": "bool", "value": true } ] }
 ]
})";

/// A tile layer, as Tiled writes one: `cells` is already a comma-separated
/// list of gids, flip bits and all.
///
/// `collides` writes the `has_collision` property Tiled's layer panel sets,
/// which is the whole of the collision rule. Pass nothing and the layer has no
/// such property at all -- the shape a layer nobody has ticked is saved in,
/// and one that must never block.
std::string layer(const std::string& name, int cols, int rows, const std::string& cells,
                  const char* collides = nullptr) {
    const std::string properties =
        collides == nullptr
            ? std::string()
            : std::string(R"(, "properties": [ { "name": "has_collision", "type": "bool",)"
                          R"( "value": )") + collides + " } ]";
    return std::string(R"({ "type": "tilelayer", "id": 1, "name": ")") + name +
           R"(", "opacity": 1, "visible": true, "x": 0, "y": 0, "width": )" +
           std::to_string(cols) + R"(, "height": )" + std::to_string(rows) + properties +
           R"(, "data": [)" + cells + "] }";
}

/// A whole map around `layers`, which the caller has already assembled.
std::string mapOf(int cols, int rows, const std::string& layers,
                  const std::string& tilesets = R"([ { "firstgid": 1, "source": "fixture.tsj" } ])",
                  int tileSize = static_cast<int>(kTileSize)) {
    return std::string(R"({
 "compressionlevel": -1, "infinite": false, "orientation": "orthogonal",
 "renderorder": "right-down", "tiledversion": "1.10.1", "type": "map",
 "version": "1.10", "nextlayerid": 9, "nextobjectid": 9,
 "tilewidth": )") + std::to_string(tileSize) + R"(, "tileheight": )" + std::to_string(tileSize) +
           R"(, "width": )" + std::to_string(cols) + R"(, "height": )" + std::to_string(rows) +
           R"(, "tilesets": )" + tilesets + R"(, "layers": [)" + layers + "] }";
}

/// The three-layer fixture the collision rules are read off.
///
/// 3x3, and shaped like the shipped map: a background layer with NO
/// `has_collision` property at all, and two layers above it that carry it.
///
///   background (does not collide)  grass, a dirt corner, and a DECORATIVE
///                                  pond at (2,0)
///   water      (collides)          a pond down the middle and one cell at (0,2)
///   castle     (collides)          a castle at (0,0), a plain BRIDGE tile at
///                                  (2,1), a castle over the water at (0,2),
///                                  and a FLIPPED castle at (2,2)
///
/// Which resolves to:
///
///   (0,0) castle                    -> wall
///   (1,0) water, colliding layer    -> water
///   (2,0) water, background layer   -> GROUND   (the layer does not collide)
///   (2,1) bridge, colliding layer   -> WALL     (no tile opts out)
///   (0,2) castle over water         -> wall     (the topmost blocker decides)
///   (2,2) flipped castle            -> wall     (the flips are art only)
std::string threeLayerMap() {
    const std::uint32_t flipped = 2u | 0x80000000u | 0x40000000u | 0x20000000u;
    const std::string background = "1,1,3, 5,1,1, 1,1,1";
    const std::string pond = "0,3,0, 0,3,0, 3,0,0";
    const std::string castle = "2,0,0, 0,0,4, 2,0," + std::to_string(flipped);
    return mapOf(3, 3, layer("background", 3, 3, background) + "," +
                           layer("water", 3, 3, pond, "true") + "," +
                           layer("castle", 3, 3, castle, "true"));
}

Tile tileAt(const TiledMap& map, int x, int y) {
    return static_cast<Tile>(map.tiles()[static_cast<std::size_t>(y * map.width() + x)]);
}

TiledCell cellAt(const TiledMap& map, std::size_t layerIndex, int x, int y) {
    return map.layers()[layerIndex].cells[static_cast<std::size_t>(y * map.width() + x)];
}

/// The shipped map, straight out of the repository rather than out of a staged
/// data directory: this test is about the file the user authors.
std::string shippedMapPath() {
    const std::string here = __FILE__;
    const std::size_t slash = here.find_last_of('/');
    const std::string tests = slash == std::string::npos ? std::string(".") : here.substr(0, slash);
    return tests + "/../../maps/garden.tmj";
}

} // namespace

TEST(collision_is_the_layers_and_the_topmost_blocker_names_the_kind) {
    std::string error;
    write("fixture.tsj", kTileset);
    TiledMap map;
    CHECK(map.load(write("three_layers.tmj", threeLayerMap()), error));
    CHECK(error.empty());
    CHECK_EQ(map.width(), 3);
    CHECK_EQ(map.height(), 3);
    CHECK_EQ(map.layers().size(), std::size_t(3));
    CHECK(map.tiles().size() == std::size_t(9));

    // The property, off the layer. Absent is not ticked.
    CHECK(!map.layers()[0].collides);
    CHECK(map.layers()[1].collides);
    CHECK(map.layers()[2].collides);

    // Blocked wherever a COLLIDING layer has a tile, and nowhere else.
    CHECK(tileAt(map, 0, 0) == Tile::Wall);     // castle
    CHECK(tileAt(map, 1, 0) == Tile::Water);    // pond, on a layer that collides
    CHECK(tileAt(map, 2, 0) == Tile::Ground);   // the same pond tile, on the background
    CHECK(tileAt(map, 0, 1) == Tile::Ground);   // dirt, but only on the background
    CHECK(tileAt(map, 1, 1) == Tile::Water);
    CHECK(tileAt(map, 2, 1) == Tile::Wall);     // a plain bridge tile: no tile opts out
    CHECK(tileAt(map, 0, 2) == Tile::Wall);     // castle OVER water: the topmost blocker wins
    CHECK(tileAt(map, 1, 2) == Tile::Ground);
    CHECK(tileAt(map, 2, 2) == Tile::Wall);

    // Only the three the derivation can produce, ever, and the counts the load
    // report prints agree with the grid.
    int walls = 0;
    int water = 0;
    int ground = 0;
    for (const std::uint8_t tile : map.tiles()) {
        CHECK(tile == static_cast<std::uint8_t>(Tile::Ground) ||
              tile == static_cast<std::uint8_t>(Tile::Wall) ||
              tile == static_cast<std::uint8_t>(Tile::Water));
        if (tile == static_cast<std::uint8_t>(Tile::Wall)) ++walls;
        else if (tile == static_cast<std::uint8_t>(Tile::Water)) ++water;
        else ++ground;
    }
    CHECK_EQ(walls, 4);
    CHECK_EQ(water, 2);
    CHECK_EQ(ground, 3);
    CHECK_EQ(map.wallCells(), walls);
    CHECK_EQ(map.waterCells(), water);
    CHECK_EQ(map.groundCells(), ground);

    // The art: one entry per distinct file, in palette order, by bare name.
    const std::vector<std::string> expected = {"grass.svg", "castle.svg", "water.svg",
                                               "bridge.svg", "dirt.svg"};
    CHECK(map.artFiles() == expected);

    // Layers in FILE order, bottom to top, with their names carried.
    CHECK(map.layers()[0].name == "background");
    CHECK(map.layers()[1].name == "water");
    CHECK(map.layers()[2].name == "castle");
    CHECK_EQ(int(cellAt(map, 0, 0, 0).art), 0);    // grass under the castle
    CHECK_EQ(int(cellAt(map, 0, 0, 1).art), 4);    // the dirt corner
    CHECK_EQ(int(cellAt(map, 0, 2, 0).art), 2);    // the decorative pond
    CHECK_EQ(int(cellAt(map, 1, 1, 1).art), 2);    // the pond that collides
    CHECK_EQ(int(cellAt(map, 2, 0, 0).art), 1);    // the castle
    CHECK_EQ(int(cellAt(map, 2, 2, 1).art), 3);    // the bridge

    // An unpainted cell is empty, which is not the same as a tile with no art.
    CHECK_EQ(int(cellAt(map, 1, 0, 0).art), -1);
    CHECK_EQ(int(cellAt(map, 2, 1, 0).art), -1);

    // covers_everything rides on the cell, so the renderer needs no lookup.
    CHECK((cellAt(map, 2, 0, 0).flags & kTileCoversEverything) != 0);   // castle
    CHECK((cellAt(map, 1, 1, 1).flags & kTileCoversEverything) == 0);   // water
    CHECK((cellAt(map, 2, 2, 1).flags & kTileCoversEverything) == 0);   // bridge
}

TEST(a_layer_that_does_not_collide_never_blocks_whatever_it_holds) {
    std::string error;
    write("fixture.tsj", kTileset);

    // The same castle-and-water painting twice over: once on a layer with
    // `has_collision` false, once on a layer that has no such property at all.
    // Neither is a wall, and neither cell is water.
    const char* const painting = "2,3,3,2";
    for (const char* collides : {"false", static_cast<const char*>(nullptr)}) {
        TiledMap map;
        CHECK(map.load(write("scenery.tmj", mapOf(2, 2, layer("art", 2, 2, painting, collides))),
                       error));
        CHECK(!map.layers()[0].collides);
        for (const std::uint8_t tile : map.tiles()) {
            CHECK(tile == static_cast<std::uint8_t>(Tile::Ground));
        }
        CHECK_EQ(map.wallCells(), 0);
        CHECK_EQ(map.waterCells(), 0);
        CHECK_EQ(map.groundCells(), 4);
        // ...and the art is still there to draw.
        CHECK_EQ(int(cellAt(map, 0, 0, 0).art), 1);
        CHECK_EQ(int(cellAt(map, 0, 1, 0).art), 2);
    }

    // And the same painting on a layer that DOES collide blocks at every cell,
    // as wall or as water depending only on the `water` tag.
    TiledMap walls;
    CHECK(walls.load(write("walls.tmj", mapOf(2, 2, layer("art", 2, 2, painting, "true"))), error));
    CHECK(tileAt(walls, 0, 0) == Tile::Wall);
    CHECK(tileAt(walls, 1, 0) == Tile::Water);
    CHECK(tileAt(walls, 0, 1) == Tile::Water);
    CHECK(tileAt(walls, 1, 1) == Tile::Wall);
    CHECK_EQ(walls.groundCells(), 0);
}

TEST(an_empty_cell_on_a_colliding_layer_is_still_ground) {
    std::string error;
    write("fixture.tsj", kTileset);
    TiledMap map;
    // A colliding layer with a hole in it. gid 0 is Tiled's empty cell, and an
    // empty cell paints nothing and blocks nothing however the layer is ticked.
    CHECK(map.load(write("holes.tmj", mapOf(2, 2, layer("castle", 2, 2, "2,0,0,2", "true"))),
                   error));
    CHECK(tileAt(map, 0, 0) == Tile::Wall);
    CHECK(tileAt(map, 1, 0) == Tile::Ground);
    CHECK(tileAt(map, 0, 1) == Tile::Ground);
    CHECK(tileAt(map, 1, 1) == Tile::Wall);
    CHECK_EQ(int(cellAt(map, 0, 1, 0).art), -1);
}

TEST(flip_bits_are_stripped_for_collision_and_kept_for_the_art) {
    std::string error;
    write("fixture.tsj", kTileset);
    TiledMap map;
    CHECK(map.load(write("three_layers.tmj", threeLayerMap()), error));

    // One edge tile serves all four rotations, so the bits have to survive to
    // the renderer intact...
    const TiledCell rotated = cellAt(map, 2, 2, 2);
    CHECK_EQ(int(rotated.art), 1);
    CHECK((rotated.flags & kTileFlipHorizontal) != 0);
    CHECK((rotated.flags & kTileFlipVertical) != 0);
    CHECK((rotated.flags & kTileFlipDiagonal) != 0);
    // ...and they have to be masked off before the gid is resolved, or the
    // tile would not resolve at all and the cell would not block.
    CHECK(tileAt(map, 2, 2) == Tile::Wall);
    // An unflipped cell of the same tile carries no bits.
    CHECK_EQ(int(cellAt(map, 2, 0, 0).flags & (kTileFlipHorizontal | kTileFlipVertical |
                                               kTileFlipDiagonal)), 0);
}

TEST(a_map_with_no_object_layers_loads) {
    std::string error;
    write("fixture.tsj", kTileset);
    TiledMap map;
    CHECK(map.load(write("art_only.tmj", mapOf(2, 2, layer("base", 2, 2, "0,0,2,0", "true"))),
                   error));
    CHECK(error.empty());
    CHECK(map.elements().size() == 0);
    CHECK(tileAt(map, 0, 1) == Tile::Wall);
    CHECK(tileAt(map, 0, 0) == Tile::Ground);
    // A map with no properties of its own is not a map with no properties:
    // it is an empty object, which every default reads through.
    CHECK(map.properties().isObject());
}

TEST(the_object_layers_become_elements) {
    std::string error;
    write("fixture.tsj", kTileset);
    const std::string doors = R"({ "id": 1, "name": "", "type": "player_spawn",
        "x": 0, "y": 0, "width": 300, "height": 300, "visible": true, "rotation": 0,
        "properties": [ { "name": "label", "type": "string", "value": "The Garden" },
                        { "name": "order", "type": "int", "value": 3 } ] })";
    const std::string pads = R"({ "id": 2, "name": "gate", "type": "teleporter",
        "x": 300, "y": 0, "width": 300, "height": 300, "visible": true, "rotation": 0,
        "properties": [ { "name": "targetMap", "type": "string", "value": "cave" },
                        { "name": "teleportToX", "type": "float", "value": 12 },
                        { "name": "teleportToY", "type": "float", "value": 34 } ] })";
    const std::string layers =
        layer("base", 2, 2, "1,1,1,1") + "," +
        R"({ "type": "objectgroup", "id": 5, "name": "player_spawns", "draworder": "topdown",
             "opacity": 1, "visible": true, "x": 0, "y": 0, "objects": [)" + doors + "] }," +
        R"({ "type": "objectgroup", "id": 6, "name": "teleporters", "draworder": "topdown",
             "opacity": 1, "visible": true, "x": 0, "y": 0, "objects": [)" + pads + "] }";

    TiledMap map;
    CHECK(map.load(write("objects.tmj", mapOf(2, 2, layers)), error));
    CHECK(map.elements().size() == 2);
    const Json& door = map.elements()[0];
    CHECK(door["type"].asString() == "player_spawn");
    CHECK_EQ(door["width"].asDouble(), 300.0);
    CHECK(door["properties"]["label"].asString() == "The Garden");
    CHECK_EQ(door["properties"]["order"].asInt(), 3);
    // An unnamed object carries no spawnId out of here; naming it is MapData's
    // job, off its label.
    CHECK(!door["properties"].contains("spawnId"));

    const Json& pad = map.elements()[1];
    CHECK(pad["type"].asString() == "teleporter");
    CHECK(pad["properties"]["targetMap"].asString() == "cave");
    CHECK_EQ(pad["properties"]["teleportTo"]["x"].asDouble(), 12.0);
    CHECK_EQ(pad["properties"]["teleportTo"]["y"].asDouble(), 34.0);
    // The object's Tiled name is the spawn id when it has one.
    CHECK(pad["properties"]["spawnId"].asString() == "gate");
}

TEST(a_map_the_engine_cannot_read_is_refused_with_a_reason) {
    write("fixture.tsj", kTileset);
    const auto refused = [](const std::string& name, const std::string& text) {
        TiledMap map;
        std::string error;
        const bool ok = map.load(write(name, text), error);
        if (ok) std::printf("  %s loaded when it should not have\n", name.c_str());
        CHECK(!ok);
        CHECK(!error.empty());
        return error;
    };

    // Two tilesets fighting over a gid. Tiled resolves such a gid to whichever
    // tileset it finds first, so the editor and the game would disagree.
    refused("overlap.tmj",
            mapOf(2, 2, layer("base", 2, 2, "1,1,1,1"),
                  R"([ { "firstgid": 1, "source": "fixture.tsj" },
                       { "firstgid": 3, "source": "fixture.tsj" } ])"));

    // A compressed layer. Refused rather than supported: a decompressor in the
    // shared library would land in the wasm build too, and the layer format is
    // a per-map setting the author can change.
    refused("compressed.tmj",
            mapOf(2, 2, R"({ "type": "tilelayer", "id": 1, "name": "base", "opacity": 1,
                             "visible": true, "x": 0, "y": 0, "width": 2, "height": 2,
                             "encoding": "base64", "compression": "zlib",
                             "data": "eJxjYGBgYAAAAAQAAQ==" })"));

    // A tile size that is not the game's. One Tiled pixel is one world unit,
    // so every rectangle on such a map would land somewhere else.
    refused("small_tiles.tmj",
            mapOf(2, 2, layer("base", 2, 2, "1,1,1,1"),
                  R"([ { "firstgid": 1, "source": "fixture.tsj" } ])", 32));

    // A gid no tileset defines: a tile painted from a tileset that was later
    // removed, which would otherwise silently become walkable ground.
    refused("stray_gid.tmj", mapOf(2, 2, layer("base", 2, 2, "1,1,1,99")));

    // A layer that holds the wrong number of cells for the map.
    refused("short_layer.tmj", mapOf(2, 2, layer("base", 2, 2, "1,1,1")));

    // A map with no tile layer at all is art-less and collision-less; it is
    // far likelier to be a mistake than a level.
    refused("no_layers.tmj", mapOf(2, 2, ""));

    // An infinite map has no fixed grid to collide with.
    {
        std::string text = mapOf(2, 2, layer("base", 2, 2, "1,1,1,1"));
        const std::string flag = "\"infinite\": false";
        text.replace(text.find(flag), flag.size(), "\"infinite\": true");
        refused("infinite.tmj", text);
    }
}

TEST(water_art_painted_only_where_it_cannot_block_is_reported) {
    // Since a layer decides blocking, a tile can no longer contradict itself.
    // What it can still do is sit on the wrong layer: a pond painted on the
    // background is a river in the editor and walkable grass in the game.
    std::string error;
    write("fixture.tsj", kTileset);
    TiledMap stranded;
    CHECK(stranded.load(write("stranded.tmj",
                              mapOf(2, 2, layer("background", 2, 2, "3,3,1,1") + "," +
                                              layer("castle", 2, 2, "0,0,0,2", "true"))),
                        error));
    const std::vector<std::string> bad = stranded.strandedWaterTiles();
    CHECK(bad.size() == 1);
    if (!bad.empty()) CHECK(bad[0] == "water.svg");
    // The map still loads; the layer rule wins, and those cells are ground.
    CHECK(tileAt(stranded, 0, 0) == Tile::Ground);

    // The three-layer fixture paints the same water tile on a layer that DOES
    // collide as well, so it is not stranded and nothing is reported.
    TiledMap healthy;
    CHECK(healthy.load(write("three_layers.tmj", threeLayerMap()), error));
    CHECK(healthy.strandedWaterTiles().empty());

    // Nor is a water tile the map simply never paints: a tileset is shared
    // between maps, and a map is not wrong for leaving part of one unused.
    TiledMap unused;
    CHECK(unused.load(write("unused.tmj", mapOf(2, 2, layer("castle", 2, 2, "2,2,2,2", "true"))),
                      error));
    CHECK(unused.strandedWaterTiles().empty());
}

TEST(the_derived_grid_reaches_a_client_through_terrain_and_the_wire) {
    write("fixture.tsj", kTileset);
    const std::string path = write("three_layers.tmj", threeLayerMap());

    Terrain server;
    std::string error;
    const Realm realm = worldRealm(1);
    CHECK(server.loadTiledMap(path, error, realm));
    CHECK(error.empty());
    CHECK_EQ(server.tileCols(realm), 3);
    CHECK_EQ(server.tileRows(realm), 3);
    CHECK(server.atTile(0, 0, realm) == Tile::Wall);
    CHECK(server.atTile(1, 0, realm) == Tile::Water);
    CHECK(server.atTile(2, 0, realm) == Tile::Ground);
    CHECK(server.blocked(Terrain::tileCenter(0, 0), realm));
    CHECK(!server.blocked(Terrain::tileCenter(2, 0), realm));
    CHECK(server.inWater(Terrain::tileCenter(1, 1), realm));

    ByteWriter w;
    writeMapGrid(w, server, realm);
    Terrain client;
    Realm got = Realm::Overworld;
    ByteReader r(w.data(), w.size());
    CHECK(readMapGrid(r, client, got, error));
    CHECK(got == realm);
    for (int ty = 0; ty < 3; ++ty) {
        for (int tx = 0; tx < 3; ++tx) {
            CHECK(client.atTile(tx, ty, realm) == server.atTile(tx, ty, realm));
        }
    }
}

TEST(the_shipped_map_loads) {
    // The reader's tests above are about the format. This one is about
    // maps/garden.tmj: that the file the user authors in Tiled is one this
    // engine reads, with the layers they ticked doing the blocking.
    TiledMap map;
    std::string error;
    if (!map.load(shippedMapPath(), error)) {
        std::printf("  %s\n", error.c_str());
        CHECK(false);
        return;
    }
    CHECK_EQ(map.width(), 64);
    CHECK_EQ(map.height(), 64);
    CHECK(map.tiles().size() == std::size_t(64 * 64));
    CHECK(!map.artFiles().empty());
    CHECK(map.strandedWaterTiles().empty());

    // The layers the author ticked, and the ones they did not. Named rather
    // than counted: which layers are walls is the single most load-bearing
    // decision in the file, and a change to it should have to be made here
    // too rather than quietly rearranging the world.
    std::string collides;
    std::string scenery;
    for (const TiledLayer& l : map.layers()) {
        CHECK(l.cells.size() == std::size_t(64 * 64));
        std::string& list = l.collides ? collides : scenery;
        if (!list.empty()) list += ",";
        list += l.name;
    }
    if (collides != "water,dirt,castle") {
        std::printf("  garden.tmj collides from [%s], scenery [%s]\n", collides.c_str(),
                    scenery.c_str());
    }
    CHECK(collides == "water,dirt,castle");
    // The bottom layer is the backdrop, and it must never block: it paints
    // every cell, walls included, so a reader that took collision off the
    // TILES would make the whole map solid.
    CHECK(map.layers()[0].name == "background");
    CHECK(!map.layers()[0].collides);
    CHECK(!scenery.empty());

    CHECK_EQ(map.wallCells() + map.waterCells() + map.groundCells(), 64 * 64);
    if (map.wallCells() == 0 || map.waterCells() == 0 || map.groundCells() == 0) {
        std::printf("  garden.tmj derived %d wall, %d water, %d ground cells\n", map.wallCells(),
                    map.waterCells(), map.groundCells());
    }
    CHECK(map.wallCells() > 0);
    CHECK(map.waterCells() > 0);
    CHECK(map.groundCells() > 0);

    // THE DOOR HAS TO HAVE SOMEWHERE TO STAND. Roughly half of this map is
    // solid by design, and the one rectangle that must not be is the one the
    // spawn picker drops a player into: a door entirely inside wall is a map
    // nobody can join, and it would otherwise only show up in play.
    MapData data;
    data.setId("garden");
    CHECK(data.loadTiled(shippedMapPath(), error));
    CHECK(data.playerSpawns().size() == std::size_t(1));
    if (!data.playerSpawns().empty()) {
        const MapElement& door = *data.playerSpawns()[0];
        // It has no `spawnId` and no Tiled name, so the id it is picked by
        // comes from its label -- and it has to resolve, or the map has
        // nowhere to join.
        CHECK(!door.spawnId.empty());
        CHECK(data.playerSpawn(door.spawnId) != nullptr);

        const int x0 = static_cast<int>(door.bounds.left() / kTileSize);
        const int y0 = static_cast<int>(door.bounds.top() / kTileSize);
        const int x1 = static_cast<int>((door.bounds.right() - 1.0) / kTileSize);
        const int y1 = static_cast<int>((door.bounds.bottom() - 1.0) / kTileSize);
        int open = 0;
        for (int ty = y0; ty <= y1; ++ty) {
            for (int tx = x0; tx <= x1; ++tx) {
                if (tx < 0 || ty < 0 || tx >= map.width() || ty >= map.height()) continue;
                if (!tileBlocks(static_cast<Tile>(
                        map.tiles()[static_cast<std::size_t>(ty * map.width() + tx)]))) {
                    ++open;
                }
            }
        }
        if (open == 0) {
            std::printf("  garden.tmj: the door \"%s\" at (%.0f,%.0f)-(%.0f,%.0f) is solid "
                        "in every one of its %d cells; nobody can spawn there\n",
                        door.spawnId.c_str(), door.bounds.left(), door.bounds.top(),
                        door.bounds.right(), door.bounds.bottom(),
                        (x1 - x0 + 1) * (y1 - y0 + 1));
        }
        CHECK(open > 0);
    }
    // Both defaults fall out of the map id when the file says nothing.
    CHECK(data.biome() == "garden");
    CHECK(data.defaultMobGroup() == "garden");
}
