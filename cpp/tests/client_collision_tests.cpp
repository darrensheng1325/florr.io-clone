#include "test.h"

#include "client/net_client.h"
#include "server/game_server.h"
#include "server_harness.h"
#include "shared/game/map_elements.h"
#include "shared/game/terrain.h"

#include <cmath>
#include <string>
#include <vector>

using namespace flix;

// Does the CLIENT collide where the server does?
//
// The wire carries one byte per cell, and a cell's collision is not one byte
// any more: it is the shapes the author drew on the tile, which cover a
// triangle, an L or a scatter of rectangles inside their own square cell. So
// the client rebuilds the exact geometry from its own copy of the map and keeps
// the wire's grid as the coarse view -- and the whole point of doing it that
// way is that the two ends then agree exactly, not approximately.
//
// Every test here runs a real server on loopback with a real client on the
// other end of a real socket. The map is a fixture rather than the shipped
// garden, because the assertions are about a KNOWN boundary: a run of diagonal
// wall tiles whose collision shape is the triangle below each one's diagonal,
// which lands the wall exactly on the world line x = y + kTileSize.

namespace {

using flix::testsupport::fixtureDoor;
using flix::testsupport::fixtureMap;
using flix::testsupport::fixturePad;
using flix::testsupport::Harness;
using flix::testsupport::loginNew;
using flix::testsupport::removeDataDir;
using flix::testsupport::stageDataDir;

/// The fixture's diagonal tile blocks the half of its cell below the diagonal
/// running from its top-left corner to its bottom-right one. Painted down a
/// diagonal of the map -- the cells one to the right of the main one -- those
/// halves join into ONE straight 45-degree wall whose face is the world line
/// x = y + kTileSize, because every such cell's local diagonal is a piece of it.
///
/// That is the whole reason the fixture is shaped like this: the expected
/// boundary is an equation rather than a number read out of the engine, so a
/// test can say where a body should stop without asking the code under test.
///
/// One cell to the right rather than through the middle because a map whose
/// CENTRE cell is solid is refused for the overworld (Terrain::setTiles keeps
/// its nearest-open-tile rescue for other realms), and the middle of a map is
/// on its main diagonal.
bool onMapDiagonal(int tx, int ty) { return tx == ty + 1; }

/// Solid border only. The diagonal tiles are painted by `diagonalAt`, and
/// nothing else blocks, so everything either side of the diagonal wall is open
/// ground a body can be driven across.
std::function<bool(int, int)> borderOnly(int cols, int rows) {
    return [cols, rows](int x, int y) {
        return x == 0 || y == 0 || x == cols - 1 || y == rows - 1;
    };
}

/// Signed distance from the wall face (the line x = y + kTileSize) to a point,
/// positive on the OPEN side -- the upper-right half of the map.
double fromWall(Vec2 p) { return (p.x - p.y - kTileSize) / std::sqrt(2.0); }

/// The two-map fixture world: `slope` is the overworld, `cellar` a second realm
/// of a different size, and both are cut in half by the diagonal wall. The pad
/// at (1050, 2550) -- open ground well below the diagonal -- leads to the
/// second map, which is how the realm-change test gets there.
std::string stageSlopeWorld(const std::string& name) {
    const int wide = 16;
    const int small = 12;
    // In CELLS, times the cell size: a door three cells across at cell (1, 9),
    // and a pad in the middle of cell (3, 8). Spelling the pixels out instead
    // put the cellar's door through its own border wall the moment the cell
    // size moved.
    const double cell = kTileSize;
    const std::string slope =
        fixtureMap(wide, wide,
                   fixtureDoor("slope", "Slope", cell, cell * 9, cell * 3, cell * 3, true, 0.0),
                   fixturePad(cell * 3.5, cell * 8.5, "cellar", "cellar_gate"), std::string(),
                   borderOnly(wide, wide), {}, onMapDiagonal);
    const std::string cellar =
        fixtureMap(small, small,
                   fixtureDoor("cellar_gate", "Cellar", cell, cell * 8, cell * 2, cell * 2, false,
                               0.0),
                   std::string(), std::string(), borderOnly(small, small), {}, onMapDiagonal);
    return stageDataDir(name, {{"slope", slope}, {"cellar", cellar}});
}

/// What App does at start-up: the client reads every staged map for its art and
/// its annotations, and for its collision shapes.
struct LocalMaps {
    WorldMaps maps;
    bool load(const std::string& dir) {
        std::string error;
        const bool ok = maps.load(dir, nullptr, error);
        if (!ok) std::printf("  fixture maps did not load: %s\n", error.c_str());
        return ok;
    }
};

Entity onlyPlayer(World& world) {
    Entity found = NULL_ENTITY;
    Query<PlayerTag, Transform> bodies{world};
    bodies.each([&](Entity e, PlayerTag&, Transform&) { found = e; });
    return found;
}

/// Every point of a grid over the diagonal-tiled cells, at a spacing fine
/// enough to fall either side of the shape boundary inside each of them. The
/// cells are `(row + 1, row)`, which is where onMapDiagonal() paints them.
std::vector<Vec2> diagonalProbePoints(int fromRow, int toRow) {
    std::vector<Vec2> points;
    for (int row = fromRow; row <= toRow; ++row) {
        const double left = (row + 1) * kTileSize;
        const double top = row * kTileSize;
        for (double dy = 5.0; dy < kTileSize; dy += 20.0) {
            for (double dx = 5.0; dx < kTileSize; dx += 20.0) {
                points.push_back({left + dx, top + dy});
            }
        }
    }
    return points;
}

}   // namespace

TEST(a_client_collides_against_the_same_shapes_the_server_enforces) {
    const std::string dir = stageSlopeWorld("clientshapes");
    CHECK(!dir.empty());
    if (dir.empty()) return;
    Harness h("client-shapes", {}, dir, 0);
    if (!h.ready) { CHECK(false); removeDataDir(dir); return; }

    LocalMaps local;
    CHECK(local.load(dir));

    NetClient client;
    CHECK(loginNew(h, client, "slider", "password1"));
    client.setWorldMaps(&local.maps);
    client.joinGame(1280, 720, "slope", "slider");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    const Realm realm = client.view().realm();
    const Terrain& server = h.server.terrain();
    const Terrain& mine = client.terrain();

    // The coarse view first: it is what the wire carries and it must still
    // arrive byte for byte, because it is what the minimap paints and what the
    // shape tests fast-reject against.
    CHECK_EQ(mine.tileCols(realm), server.tileCols(realm));
    CHECK_EQ(mine.tileRows(realm), server.tileRows(realm));
    CHECK(mine.tileCount(realm) == server.tileCount(realm));
    CHECK(std::equal(mine.tiles(realm), mine.tiles(realm) + mine.tileCount(realm),
                     server.tiles(realm)));
    // And the diagonal cells read as wall in it, even though they block only
    // half of themselves: the coarse view says "something blocks here".
    CHECK(server.atTile(9, 8, realm) == Tile::Wall);
    CHECK(mine.atTile(9, 8, realm) == Tile::Wall);

    // Now the exact answer, point for point, over the cells the diagonal runs
    // through. Not "close": the client runs the same function over the same
    // file, so any disagreement at all is a wiring bug.
    int disagreements = 0;
    for (Vec2 p : diagonalProbePoints(4, 11)) {
        if (mine.blocked(p, realm) != server.blocked(p, realm)) ++disagreements;
        if (mine.inWater(p, realm) != server.inWater(p, realm)) ++disagreements;
    }
    CHECK_EQ(disagreements, 0);
    // The probe is worth nothing unless it straddles the boundary, so say so:
    // the cells it walks are half blocked and half open.
    int blockedPoints = 0;
    const std::vector<Vec2> probe = diagonalProbePoints(4, 11);
    for (Vec2 p : probe) blockedPoints += server.blocked(p, realm) ? 1 : 0;
    CHECK(blockedPoints > static_cast<int>(probe.size()) / 4);
    CHECK(blockedPoints < static_cast<int>(probe.size()) * 3 / 4);

    // Drive a body into the wall face, perpendicular to it, and see where the
    // server stops it.
    World& world = h.server.world();
    const Entity body = onlyPlayer(world);
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) { removeDataDir(dir); return; }

    const double radius = world.get<Body>(body).radius;
    CHECK(radius > 0.0);
    // Well clear of the wall, on the open side, and on the perpendicular
    // through the MIDDLE of cell (9, 8)'s face, so the body meets the wall in
    // the middle of a cell rather than at a seam between two.
    const Vec2 faceMiddle{9.5 * kTileSize, 8.5 * kTileSize};
    world.get<Transform>(body).position = {faceMiddle.x + 450.0, faceMiddle.y - 450.0};

    net::InputFrame input;
    input.moveAngle = kPi * 0.75;    // down and to the left: straight at the face
    input.moveStrength = 1.0;
    for (int i = 0; i < 80; ++i) {
        input.sequence = static_cast<std::uint32_t>(i + 1);
        client.sendInput(input);
        h.step(1, {&client});
    }

    const Vec2 rest = world.get<Transform>(body).position;
    // It came to rest ON the face, a body radius off it -- which is INSIDE a
    // cell the coarse grid calls wall. A whole-cell resolver would have stopped
    // it at that cell's edge instead, most of a body short of the wall the art
    // draws.
    //
    // A body radius and no more: the resolver ejects to touching (plus its own
    // hundredth of a unit of skin) and the movement step accepts that, so the
    // number is the radius rather than "somewhere near it". It is never LESS
    // than the radius, which would be a body standing in the wall.
    CHECK(fromWall(rest) >= radius - 0.01);
    CHECK_NEAR(fromWall(rest), radius, 1.0);
    const int restTx = Terrain::toTileCoord(rest.x);
    const int restTy = Terrain::toTileCoord(rest.y);
    CHECK_EQ(restTx, 9);
    CHECK_EQ(restTy, 8);
    CHECK(server.atTile(restTx, restTy, realm) == Tile::Wall);

    // The client agrees about that resting place, exactly: it is not blocked,
    // and the client's own push-out leaves it where it is.
    CHECK(!mine.blocked(rest, realm));
    const Terrain::WallResolution clientSays = mine.resolveWall(rest, radius, realm);
    const Terrain::WallResolution serverSays = server.resolveWall(rest, radius, realm);
    CHECK(!clientSays.unresolved);
    CHECK_NEAR(clientSays.position.x, serverSays.position.x, 1e-9);
    CHECK_NEAR(clientSays.position.y, serverSays.position.y, 1e-9);
    CHECK_NEAR(clientSays.position.x, rest.x, 1.0);
    CHECK_NEAR(clientSays.position.y, rest.y, 1.0);

    // And they agree about where the wall starts: a body's width further along
    // the heading is inside it on both ends.
    const Vec2 inside = {rest.x - 3.0 * radius, rest.y + 3.0 * radius};
    CHECK(server.blocked(inside, realm));
    CHECK(mine.blocked(inside, realm));

    removeDataDir(dir);
}

TEST(a_client_with_no_local_map_collides_with_whole_cells) {
    // The conservative fallback. A client whose data directory has no map for
    // the realm it is dropped into still has the wire's grid, so it treats a
    // blocking cell as blocking all of itself: it thinks there is MORE wall
    // than there is, never less, which costs it a little ground at a slope and
    // never lets it draw a body inside a wall.
    const std::string dir = stageSlopeWorld("clientnomap");
    CHECK(!dir.empty());
    if (dir.empty()) return;
    Harness h("client-nomap", {}, dir, 0);
    if (!h.ready) { CHECK(false); removeDataDir(dir); return; }

    NetClient client;                 // no setWorldMaps(): nothing staged here
    CHECK(loginNew(h, client, "blind", "password2"));
    client.joinGame(1280, 720, "slope", "blind");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    const Realm realm = client.view().realm();
    const Terrain& server = h.server.terrain();
    const Terrain& mine = client.terrain();

    // The grid still arrived, so the map is the right shape and the walls are
    // in the right cells.
    CHECK_EQ(mine.tileCols(realm), server.tileCols(realm));
    CHECK(std::equal(mine.tiles(realm), mine.tiles(realm) + mine.tileCount(realm),
                     server.tiles(realm)));

    // Wherever the server blocks, so does this client. That is the direction
    // that matters: never the other way round.
    int walkedThroughWall = 0;
    int conservativeCells = 0;
    for (Vec2 p : diagonalProbePoints(4, 11)) {
        const bool serverBlocks = server.blocked(p, realm);
        const bool clientBlocks = mine.blocked(p, realm);
        if (serverBlocks && !clientBlocks) ++walkedThroughWall;
        if (!serverBlocks && clientBlocks) ++conservativeCells;
    }
    CHECK_EQ(walkedThroughWall, 0);
    // And it really is the coarse view rather than the shapes: the open half
    // of every diagonal cell reads as wall here.
    CHECK(conservativeCells > 0);

    removeDataDir(dir);
}

TEST(a_realm_change_rebuilds_the_clients_collision_shapes) {
    const std::string dir = stageSlopeWorld("clientrealm");
    CHECK(!dir.empty());
    if (dir.empty()) return;
    Harness h("client-realm", {}, dir, 0);
    if (!h.ready) { CHECK(false); removeDataDir(dir); return; }

    LocalMaps local;
    CHECK(local.load(dir));

    NetClient client;
    CHECK(loginNew(h, client, "traveller", "password3"));
    client.setWorldMaps(&local.maps);
    client.joinGame(1280, 720, "slope", "traveller");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    const MapElement* pad = nullptr;
    const MapData* overworld = h.server.worldMaps().forRealm(Realm::Overworld);
    CHECK(overworld != nullptr);
    if (overworld != nullptr) {
        for (const MapElement& element : overworld->elements()) {
            if (element.kind == MapElementKind::Teleporter) pad = &element;
        }
    }
    CHECK(pad != nullptr);
    if (pad == nullptr) { removeDataDir(dir); return; }

    World& world = h.server.world();
    const Entity body = onlyPlayer(world);
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) { removeDataDir(dir); return; }

    world.get<Transform>(body).position = pad->centre();
    CHECK(h.stepUntil({&client}, [&] {
        return world.get<Transform>(body).realm != Realm::Overworld;
    }, 200));
    const Realm cellar = world.get<Transform>(body).realm;
    CHECK(h.stepUntil({&client}, [&] { return client.view().realm() == cellar; }));

    const Terrain& server = h.server.terrain();
    const Terrain& mine = client.terrain();
    // A different map of a different size, and the client is holding its grid.
    CHECK_EQ(mine.tileCols(cellar), server.tileCols(cellar));
    CHECK(server.tileCols(cellar) != server.tileCols(Realm::Overworld));

    // The shapes came with it: the second map's diagonal is exact on the
    // client too, which is only true if the realm change rebuilt them.
    int disagreements = 0;
    int openInsideWallCells = 0;
    for (Vec2 p : diagonalProbePoints(4, 9)) {
        if (mine.blocked(p, cellar) != server.blocked(p, cellar)) ++disagreements;
        const int tx = Terrain::toTileCoord(p.x);
        const int ty = Terrain::toTileCoord(p.y);
        if (mine.atTile(tx, ty, cellar) == Tile::Wall && !mine.blocked(p, cellar)) {
            ++openInsideWallCells;
        }
    }
    CHECK_EQ(disagreements, 0);
    CHECK(openInsideWallCells > 0);

    // And the realm it LEFT is still exact, rather than having been replaced by
    // the arrival's shapes: a pad leads back.
    int overworldDisagreements = 0;
    for (Vec2 p : diagonalProbePoints(4, 11)) {
        if (mine.blocked(p, Realm::Overworld) != server.blocked(p, Realm::Overworld)) {
            ++overworldDisagreements;
        }
    }
    CHECK_EQ(overworldDisagreements, 0);

    removeDataDir(dir);
}
