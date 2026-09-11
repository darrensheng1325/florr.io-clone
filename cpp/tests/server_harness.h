#pragma once
// A real server on loopback, with real clients speaking the real protocol.
//
// Extracted from integration_tests.cpp once a second file needed it. Nothing
// here is a stub: below the socket it is the shipping code path, which is the
// whole point -- a test that mocks the server tests the mock.

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "client/net_client.h"
#include "server/game_server.h"
#include "shared/game/config.h"

namespace flix::testsupport {

inline std::string tempPath(const char* name) {
    return std::string("/tmp/florr-itest-") + name + "-" + std::to_string(::getpid()) + ".json";
}

/// The content directory, found relative to THIS source file rather than to
/// the working directory: the test binary is run from the build tree, from the
/// project root, and from ctest, and all three must work.
inline const std::string& dataDir() {
    static const std::string resolved = [] {
#ifdef FLIX_TEST_DATA_DIR
        {
            // The staged directory is the one the shipping server reads, so it
            // is preferred whenever it exists. Probed on maps.json, which is
            // what says the directory holds maps at all.
            const std::string staged = FLIX_TEST_DATA_DIR;
            std::ifstream mapProbe(staged + "/maps.json", std::ios::binary);
            if (mapProbe) return staged;
        }
#endif
        const std::string here = __FILE__;
        const std::size_t slash = here.find_last_of('/');
        const std::string tests = slash == std::string::npos ? std::string(".") : here.substr(0, slash);
        const std::string candidates[] = {
            tests + "/../build/data",   // staged beside the binaries
            tests + "/../data",         // the checked-in mob_xp.json plus copies
            "data",
        };
        for (const std::string& candidate : candidates) {
            std::ifstream probe(candidate + "/mobs.json", std::ios::binary);
            if (probe) return candidate;
        }
        return std::string("data");
    }();
    return resolved;
}

/// A server on a free port with an empty database, plus the plumbing to step
/// it and its clients forward together.
struct Harness {
    GameServer server;
    std::string dbPath;
    std::uint16_t port = 0;
    bool ready = false;
    double clock = 0;
    /// For tests that need to ask the terrain for a point. Separate from the
    /// server's own stream so a probe cannot shift what the simulation rolls.
    Rng probeRng{0xA11CE};

    /// `seed` runs against the database file BEFORE the server opens it, for
    /// tests that need an account to already own something.
    ///
    /// `contentDir` is the data directory to boot on, defaulting to the staged
    /// one. A test that needs a map the shipped data does not have -- a second
    /// realm to teleport into, a door that is not pickable -- stages one of its
    /// own with mapFixture() below and passes it here. The alternative is
    /// bending the shipped map into a test rig, which makes the game's own data
    /// a test fixture and stops an author from changing it.
    /// `bots` is the bot population to run with, or -1 for the server's usual
    /// one. The world is ONE map now and its only door is small, so the bots
    /// stand exactly where a joining player is put down -- which is right for
    /// a live server and fatal to any test that says "these are the only
    /// flowers in sight" or "this mob lived long enough to act". Those pass 0;
    /// bot_tests.cpp leaves it alone.
    explicit Harness(const char* dbName,
                     const std::function<void(const std::string&)>& seed = {},
                     const std::string& contentDir = dataDir(), int bots = -1) {
        dbPath = tempPath(dbName);
        std::remove(dbPath.c_str());
        if (seed) seed(dbPath);

        ServerConfig config;
        config.dataDir = contentDir;
        config.databasePath = dbPath;
        config.worldSeed = 12345;
        config.botCount = bots;

        std::string error;
        for (std::uint16_t candidate = 47100; candidate < 47160; ++candidate) {
            config.port = candidate;
            if (server.start(config, error)) { port = candidate; ready = true; break; }
        }
        if (!ready) std::printf("  harness could not start a server: %s\n", error.c_str());
    }

    ~Harness() { std::remove(dbPath.c_str()); }

    /// Advances the simulation by `ticks`, servicing both ends each step.
    ///
    /// The server's socket and its simulation are stepped separately, exactly
    /// as run() interleaves them, so a test sees the same ordering production
    /// does rather than a convenient fiction.
    void step(int ticks, std::vector<NetClient*> clients) {
        for (int i = 0; i < ticks; ++i) {
            for (NetClient* c : clients) c->poll(1);
            server.serviceNetwork(1);
            clock += net::kTickMillis;
            server.tick(clock);
            server.serviceNetwork(0);
            for (NetClient* c : clients) c->poll(1);
        }
    }

    /// Steps until `done` holds or the budget runs out; returns whether it did.
    template <class F>
    bool stepUntil(std::vector<NetClient*> clients, F done, int maxTicks = 400) {
        for (int i = 0; i < maxTicks; ++i) {
            step(1, clients);
            if (done()) return true;
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// Synthetic map fixtures
// ---------------------------------------------------------------------------
//
// A data directory the server can boot on, built out of the staged content's
// mobs and petals plus maps written here. The shipped world is ONE map now, so
// anything about several realms -- a teleporter that lands somewhere, a door
// an admin may name but the picker may not offer -- needs a world of its own,
// and a fixture the test can read is a better statement of the invariant than
// a rectangle hidden in the game's art.

/// Writes `text` to `path`, creating nothing: the directory must exist.
inline bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

inline bool copyFile(const std::string& from, const std::string& to) {
    std::ifstream in(from, std::ios::binary);
    if (!in) return false;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return writeFile(to, text);
}

/// The tileset every fixture map paints from: ground, wall, water, and one
/// DIAGONAL wall whose collision is half its cell.
///
/// Nothing is tagged solid, because there is no such tag: a cell blocks where
/// a layer with `has_collision` paints a tile that CARRIES COLLISION SHAPES,
/// which is Tiled's own semantic (shared/game/tiled_map.h). So each blocking
/// tile here declares an `objectgroup`, exactly as maps/tileset.tsj does --
/// the three whole-cell ones a rectangle over the entire tile, and
/// `castle_tri` a triangle over the half below its diagonal. A tile with no
/// objectgroup (the grass) contributes no collision wherever it is painted,
/// which is what makes the background layer scenery twice over.
///
/// The shapes are in the TILE'S OWN IMAGE SPACE, which is what Tiled's Tile
/// Collision Editor draws in and what the loader reads them in -- 300 here, so
/// the load scales them by one. (This is an image-collection tileset, so every
/// tile declares its image size and the tileset-level tilewidth/tileheight is
/// only the display grid; the two agree here.) The shipped tileset draws at 256
/// into 300-unit cells and is the case that exercises the scale; a fixture that
/// also rescaled would make every expected number in a test an arithmetic
/// puzzle.
///
/// `water` is the one tag left on a tile and it says only what KIND of blocker
/// a colliding cell is -- exactly as maps/tileset.tsj uses it.
inline std::string fixtureTileset() {
    return R"({
 "columns": 0,
 "grid": { "height": 300, "orientation": "orthogonal", "width": 300 },
 "name": "fixture",
 "tilecount": 4,
 "tiledversion": "1.10.1",
 "tileheight": 300,
 "tilerendersize": "grid",
 "tilewidth": 300,
 "type": "tileset",
 "version": "1.10",
 "tiles": [
  { "id": 0, "image": "tiles/grass_c_0.svg", "imageheight": 300, "imagewidth": 300,
    "properties": [ { "name": "covers_everything", "type": "bool", "value": true } ] },
  { "id": 1, "image": "tiles/castle_c_0.svg", "imageheight": 300, "imagewidth": 300,
    "properties": [ { "name": "covers_everything", "type": "bool", "value": true } ],
    "objectgroup": { "draworder": "index", "id": 2, "name": "", "opacity": 1,
      "type": "objectgroup", "visible": true, "x": 0, "y": 0,
      "objects": [ { "id": 1, "name": "", "type": "", "rotation": 0, "visible": true,
                     "x": 0, "y": 0, "width": 300, "height": 300 } ] } },
  { "id": 2, "image": "tiles/water_c_0.svg", "imageheight": 300, "imagewidth": 300,
    "properties": [ { "name": "water", "type": "bool", "value": true },
                    { "name": "covers_everything", "type": "bool", "value": true } ],
    "objectgroup": { "draworder": "index", "id": 2, "name": "", "opacity": 1,
      "type": "objectgroup", "visible": true, "x": 0, "y": 0,
      "objects": [ { "id": 1, "name": "", "type": "", "rotation": 0, "visible": true,
                     "x": 0, "y": 0, "width": 300, "height": 300 } ] } },
  { "id": 3, "image": "tiles/castle_tri_0.svg", "imageheight": 300, "imagewidth": 300,
    "properties": [ { "name": "covers_everything", "type": "bool", "value": true } ],
    "objectgroup": { "draworder": "index", "id": 2, "name": "", "opacity": 1,
      "type": "objectgroup", "visible": true, "x": 0, "y": 0,
      "objects": [ { "id": 1, "name": "", "type": "", "rotation": 0, "visible": true,
                     "x": 0, "y": 0, "width": 0, "height": 0,
                     "polygon": [ { "x": 0, "y": 0 }, { "x": 300, "y": 300 },
                                  { "x": 0, "y": 300 } ] } ] } }
 ]
})";
}

/// One `player_spawns` object: a door rectangle with its authored properties.
///
/// `spawnId` empty exercises the id fallback the shipped map relies on -- no
/// Tiled name, no `spawnId`, just a label.
inline std::string fixtureDoor(const std::string& spawnId, const std::string& label,
                               double x, double y, double w, double h, bool pickable = true,
                               double order = 0.0) {
    std::string out = "{ \"id\": " + std::to_string(static_cast<int>(x + y) + 1) +
                      ", \"name\": \"\", \"type\": \"player_spawn\", \"visible\": true," +
                      " \"rotation\": 0, \"x\": " + std::to_string(x) +
                      ", \"y\": " + std::to_string(y) + ", \"width\": " + std::to_string(w) +
                      ", \"height\": " + std::to_string(h) + ", \"properties\": [";
    if (!spawnId.empty()) {
        out += "{ \"name\": \"spawnId\", \"type\": \"string\", \"value\": \"" + spawnId + "\" },";
    }
    out += "{ \"name\": \"label\", \"type\": \"string\", \"value\": \"" + label + "\" },";
    out += "{ \"name\": \"order\", \"type\": \"float\", \"value\": " + std::to_string(order) + " },";
    out += std::string("{ \"name\": \"pickable\", \"type\": \"bool\", \"value\": ") +
           (pickable ? "true" : "false") + " }]}";
    return out;
}

/// One `spawns` object with no difficulty: a mob REGION, which only says what
/// lives on this ground.
///
/// A fixture map needs one of these or a `defaultMobGroup`, or the spawner has
/// nothing to put on it -- the map's id is its default group, and `hollow` is
/// not a group mobs.json defines.
inline std::string fixtureRegion(double x, double y, double w, double h,
                                 const std::string& mobs) {
    return "{ \"id\": 80, \"name\": \"\", \"type\": \"spawn\", \"visible\": true,"
           " \"rotation\": 0, \"x\": " + std::to_string(x) +
           ", \"y\": " + std::to_string(y) + ", \"width\": " + std::to_string(w) +
           ", \"height\": " + std::to_string(h) + ", \"properties\": ["
           "{ \"name\": \"mobs\", \"type\": \"string\", \"value\": \"" + mobs + "\" }]}";
}

/// One `spawns` object WITH a difficulty: a spawn BAND, which owns a
/// population of its own at the tier that difficulty buys.
///
/// A number, not a rarity name -- zero is fully common and three hundred is
/// unique with a little apex in it; see the difficulty curve in
/// shared/game/difficulty.h. Written as a float property because that is what
/// Tiled writes for any number an author types into one.
inline std::string fixtureBand(double x, double y, double w, double h, const std::string& mobs,
                               double difficulty) {
    return "{ \"id\": 81, \"name\": \"\", \"type\": \"spawn\", \"visible\": true,"
           " \"rotation\": 0, \"x\": " + std::to_string(x) +
           ", \"y\": " + std::to_string(y) + ", \"width\": " + std::to_string(w) +
           ", \"height\": " + std::to_string(h) + ", \"properties\": ["
           "{ \"name\": \"difficulty\", \"type\": \"float\", \"value\": " +
           std::to_string(difficulty) + " },"
           "{ \"name\": \"mobs\", \"type\": \"string\", \"value\": \"" + mobs + "\" }]}";
}

/// One `teleporters` object: a POINT, which is how every pad is authored.
inline std::string fixturePad(double x, double y, const std::string& targetMap,
                              const std::string& targetSpawn) {
    return "{ \"id\": 90, \"name\": \"\", \"type\": \"teleporter\", \"visible\": true,"
           " \"rotation\": 0, \"width\": 0, \"height\": 0, \"x\": " + std::to_string(x) +
           ", \"y\": " + std::to_string(y) + ", \"properties\": ["
           "{ \"name\": \"targetMap\", \"type\": \"string\", \"value\": \"" + targetMap + "\" },"
           "{ \"name\": \"targetSpawn\", \"type\": \"string\", \"value\": \"" + targetSpawn +
           "\" }]}";
}

/// A Tiled map `cols` x `rows`, plus whatever objects the caller names on each
/// of the three object layers the game reads.
///
/// Deliberately the same shape the real reader sees: one external tileset,
/// plain-array layer data, and THREE tile layers whose `has_collision`
/// properties are the only thing that decides collision --
///
///     background   has_collision false   painted everywhere, never blocks
///     water        has_collision true    the water tiles
///     walls        has_collision true    the solid tiles
///
/// -- exactly as maps/garden.tmj is built. `solidAt` says which cells the
/// walls layer paints; the default is a one-cell border, which is what most
/// callers want. `waterAt` is the same for the water layer, and a cell it
/// claims is a blocker of a different KIND: it still stops a body, and it is
/// what tileIsWater() is true of. `diagonalAt` paints the DIAGONAL wall tile,
/// whose authored shape is the triangle below its diagonal -- a cell that
/// blocks half of itself, which is what the whole map is made of now and what
/// a whole-cell collision reader cannot tell from a full wall.
///
/// Note the background layer: it paints every cell, INCLUDING the ones under
/// the walls, and it never blocks a thing -- twice over, since its tile
/// carries no collision shapes either.
inline std::string fixtureMap(int cols, int rows, const std::string& doors,
                              const std::string& pads, const std::string& spawns = {},
                              const std::function<bool(int, int)>& solidAt = {},
                              const std::function<bool(int, int)>& waterAt = {},
                              const std::function<bool(int, int)>& diagonalAt = {}) {
    std::string background, wall, water;
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) {
            const bool border = x == 0 || y == 0 || x == cols - 1 || y == rows - 1;
            const bool diagonal = diagonalAt && diagonalAt(x, y);
            const bool solid = !diagonal && (solidAt ? solidAt(x, y) : border);
            const bool wet = waterAt && waterAt(x, y) && !solid && !diagonal;
            if (!background.empty()) { background += ","; wall += ","; water += ","; }
            background += "1";
            wall += diagonal ? "4" : (solid ? "2" : "0");
            water += wet ? "3" : "0";
        }
    }
    const std::string size = std::to_string(cols);
    const std::string tall = std::to_string(rows);
    // Tiled writes a layer's custom properties exactly like this, and the
    // property is absent rather than false on a layer nobody ticked -- which
    // is the case the reader has to read as "scenery".
    const char* collides = R"("properties": [ { "name": "has_collision", "type": "bool", "value": true } ],)";
    std::string out = R"({
 "compressionlevel": -1, "infinite": false, "orientation": "orthogonal",
 "renderorder": "right-down", "tiledversion": "1.10.1", "type": "map",
 "version": "1.10", "nextlayerid": 9, "nextobjectid": 9,
 "tilewidth": 300, "tileheight": 300,
 "width": )" + size + R"(, "height": )" + tall + R"(,
 "tilesets": [ { "firstgid": 1, "source": "fixture.tsj" } ],
 "layers": [
  { "type": "tilelayer", "id": 1, "name": "background", "opacity": 1, "visible": true,
    "x": 0, "y": 0, "width": )" + size + R"(, "height": )" + tall + R"(,
    "data": [)" + background + R"(] },
  { "type": "tilelayer", "id": 2, "name": "water", "opacity": 1, "visible": true,
    )" + collides + R"(
    "x": 0, "y": 0, "width": )" + size + R"(, "height": )" + tall + R"(,
    "data": [)" + water + R"(] },
  { "type": "tilelayer", "id": 3, "name": "walls", "opacity": 1, "visible": true,
    )" + collides + R"(
    "x": 0, "y": 0, "width": )" + size + R"(, "height": )" + tall + R"(,
    "data": [)" + wall + R"(] },
  { "type": "objectgroup", "id": 4, "name": "player_spawns", "draworder": "topdown",
    "opacity": 1, "visible": true, "x": 0, "y": 0, "objects": [)" + doors + R"(] },
  { "type": "objectgroup", "id": 5, "name": "teleporters", "draworder": "topdown",
    "opacity": 1, "visible": true, "x": 0, "y": 0, "objects": [)" + pads + R"(] },
  { "type": "objectgroup", "id": 6, "name": "spawns", "draworder": "topdown",
    "opacity": 1, "visible": true, "x": 0, "y": 0, "objects": [)" + spawns + R"(] }
 ]
})";
    return out;
}

/// The wall pattern for a map as SOLID as the shipped one.
///
/// garden.tmj is about three fifths solid -- water, dirt and castle all
/// collide -- and every placement path on the server has to survive that. So
/// does this: a solid border, one clear room at the top left for the door, and
/// everywhere else walls except a grid of corridors, one row in four and one
/// column in five. That is 60% solid and, unlike a scatter of open cells, it
/// is CONNECTED: a body can walk from any open cell to any other, which is
/// what makes it a map rather than a set of pockets.
inline std::function<bool(int, int)> denseWalls(int cols, int rows, int roomCols, int roomRows) {
    return [cols, rows, roomCols, roomRows](int x, int y) {
        if (x == 0 || y == 0 || x == cols - 1 || y == rows - 1) return true;
        if (x >= 1 && y >= 1 && x <= roomCols && y <= roomRows) return false;   // the door's room
        return !(y % 4 == 1 || x % 5 == 1);
    };
}

/// A data directory with the staged content and the caller's maps in it.
///
/// `maps` is (file stem, .tmj text). The manifest is written in that order, so
/// maps[0] is the overworld and maps[1] the next realm -- the same rule the
/// shipped maps.json follows. Returns the directory, or "" when it could not
/// be staged.
inline std::string stageDataDir(const std::string& name,
                                const std::vector<std::pair<std::string, std::string>>& maps) {
    const std::string dir =
        "/tmp/florr-fixture-" + name + "-" + std::to_string(::getpid());
    ::mkdir(dir.c_str(), 0755);
    ::mkdir((dir + "/tiles").c_str(), 0755);
    for (const char* file : {"mobs.json", "petals.json", "mob_xp.json", "mob_drops.json"}) {
        // mob_drops is optional in some staged trees; the rest are not.
        if (!copyFile(dataDir() + "/" + file, dir + "/" + file) &&
            std::string(file) != "mob_drops.json") {
            return std::string();
        }
    }
    if (!writeFile(dir + "/fixture.tsj", fixtureTileset())) return std::string();
    // The art itself is optional: a missing tile file is one warning in the
    // client's sprite cache, never a failure, and no test here renders.
    copyFile(dataDir() + "/grass_c_0.svg", dir + "/grass_c_0.svg");
    copyFile(dataDir() + "/castle_c_0.svg", dir + "/castle_c_0.svg");
    copyFile(dataDir() + "/water_c_0.svg", dir + "/water_c_0.svg");
    copyFile(dataDir() + "/castle_tri_0.svg", dir + "/castle_tri_0.svg");

    std::string manifest = "{\n  \"maps\": [";
    for (std::size_t i = 0; i < maps.size(); ++i) {
        if (!writeFile(dir + "/" + maps[i].first + ".tmj", maps[i].second)) return std::string();
        if (i != 0) manifest += ",";
        manifest += "\n    { \"file\": \"" + maps[i].first + ".tmj\" }";
    }
    manifest += "\n  ]\n}\n";
    if (!writeFile(dir + "/maps.json", manifest)) return std::string();
    return dir;
}

/// The standard two-map fixture world.
///
/// `meadow` is the overworld: two pickable doors, `meadow` first in button
/// order, and a pad at (3600, 900) into `warren`. `warren` is a second,
/// differently-sized realm whose only door, `warren_gate`, is NOT pickable --
/// the arrangement the picker's two lists exist for, which the shipped data no
/// longer contains.
inline std::string twoMapDataDir(const std::string& name) {
    const std::string meadow =
        fixtureMap(24, 24,
                   fixtureDoor("meadow", "Meadow", 600, 600, 1200, 1200, true, 0.0) + "," +
                       fixtureDoor("dunes", "Dunes", 4800, 4800, 1200, 1200, true, 1.0),
                   fixturePad(3600, 900, "warren", "warren_gate"));
    const std::string warren =
        fixtureMap(16, 16, fixtureDoor("warren_gate", "Warren", 900, 900, 900, 900, false, 0.0),
                   std::string());
    return stageDataDir(name, {{"meadow", meadow}, {"warren", warren}});
}

/// A one-map world that is MOSTLY WALL, as the shipped map is.
///
/// 32x32 cells (9600 units square), three fifths of it solid, with two doors:
///
///   `hollow` -- pickable, order 0, in the one clear room. This is where a
///               join with no choice lands.
///   `cellar` -- NOT pickable, and drawn over a single cell that is SOLID all
///               the way across. A door an author placed badly, which is the
///               case every "give up and use the rectangle's centre" fallback
///               used to hand back a point inside a wall for.
///
/// Every placement path the server has -- the join, the respawn, the bots, the
/// density fill, a drop -- runs on this, and a path that quietly gives up on a
/// dense map shows up here rather than in the game.
inline std::string denseMapDataDir(const std::string& name) {
    const int side = 32;
    const std::string world =
        fixtureMap(side, side,
                   fixtureDoor("hollow", "Hollow", 600, 600, 1200, 1200, true, 0.0) + "," +
                       fixtureDoor("cellar", "Cellar", 3600, 4200, 300, 300, false, 1.0),
                   std::string(),
                   // A region over the whole map, so the spawner has something
                   // to put on it: the map's id is its default mob group and
                   // `hollow` is not a group mobs.json defines.
                   fixtureRegion(0, 0, side * kTileSize, side * kTileSize, "garden 100%"),
                   denseWalls(side, side, 6, 6));
    return stageDataDir(name, {{"hollow", world}});
}

/// Deletes a directory stageDataDir() made, files and all.
inline void removeDataDir(const std::string& dir) {
    if (dir.empty()) return;
    // No <filesystem>: this tree targets a toolchain without it in the wasm
    // build, and the fixture's shape is known -- flat files plus `tiles/`.
    const std::string command = "rm -rf '" + dir + "'";
    if (std::system(command.c_str()) != 0) {
        std::printf("  could not remove fixture dir %s\n", dir.c_str());
    }
}

inline bool connectClient(Harness& h, NetClient& client) {
    client.contentHash = content().contentHash();
    if (!client.connect("127.0.0.1", h.port)) return false;
    return h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Ready; });
}

/// Registers `name` and waits for the account state to arrive.
inline bool loginNew(Harness& h, NetClient& client, const char* name, const char* password) {
    if (!connectClient(h, client)) return false;
    client.requestRegister(name, password);
    return h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; });
}

} // namespace flix::testsupport
