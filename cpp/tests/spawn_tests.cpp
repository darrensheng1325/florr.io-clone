#include "test.h"

#include "server/db.h"
#include "server_harness.h"
#include "shared/core/json.h"
#include "shared/game/map_elements.h"

#include <sys/stat.h>

#include <fstream>
#include <iterator>

using namespace flix;
using flix::testsupport::connectClient;
using flix::testsupport::dataDir;
using flix::testsupport::Harness;
using flix::testsupport::loginNew;

// Where a player appears.
//
// The map says where a player may be put down: a `player_spawns` rectangle is
// a door, and the picker's row is the list of doors. The middle of the world
// is the legendary and mythic band, which a level-1 flower cannot survive and
// cannot walk out of. These tests exist because spawning there was not an
// obviously wrong line of code -- it was a plausible-looking "start at the
// centre".

namespace {

/// True when `at` is inside the player spawn rectangle called `spawnId`.
bool inSpawnPoint(const MapData& map, const std::string& spawnId, Vec2 at) {
    const MapElement* point = map.playerSpawn(spawnId);
    return point != nullptr && point->contains(at);
}

/// True when a band over `at` is DANGEROUS ground -- difficulty
/// kDangerousGroundDifficulty or above, which is where the curve's blend first
/// CAN produce a rare (difficulty 16.61, the top of pure uncommon). A spawn
/// that lands in one of these is a spawn into mobs a fresh flower cannot
/// fight.
///
/// The invariant this expresses is the same one the old `spawnType` version
/// did -- "no door under rare-or-better ground" -- said on the scale the map
/// now uses: a band carries a difficulty, and the difficulty curve
/// (shared/game/difficulty.h) says which tiers that difficulty grows.
bool onDangerousGround(const MapData& map, Vec2 at) {
    for (const MapElement& element : map.elements()) {
        if (!element.isSpawnBand()) continue;
        if (element.difficulty < kDangerousGroundDifficulty) continue;
        if (element.contains(at)) return true;
    }
    return false;
}

/// The difficulty of the ground at `at`, read off the MAP -- the first band
/// covering it, and otherwise the map's own `defaultDifficulty`.
///
/// The same rule SpawnSystem::difficultyAt() applies, stated here so a test can
/// ask it of a map without reaching inside the running server for its spawner.
/// If the two ever disagree this is the copy that is wrong.
double difficultyOnMap(const MapData& map, Vec2 at) {
    for (const MapElement& element : map.elements()) {
        if (!element.isSpawnBand()) continue;
        if (element.contains(at)) return element.difficulty;
    }
    return map.defaultDifficulty();
}

Entity onlyPlayer(World& world) {
    Entity found = NULL_ENTITY;
    Query<PlayerTag, Transform> bodies{world};
    bodies.each([&](Entity e, PlayerTag&, Transform&) { found = e; });
    return found;
}

const MapData& overworld(const Harness& h) {
    static const MapData kEmpty;
    const MapData* map = h.server.worldMaps().forRealm(Realm::Overworld);
    return map != nullptr ? *map : kEmpty;
}

} // namespace

TEST(the_shipped_map_loads_and_resolves_its_defaults) {
    // The new truth: maps/maps.json names ONE map, garden.tmj, and that map is
    // the overworld. It declares no map properties at all, so this is also the
    // test of the defaults that make an unedited Tiled file work -- `biome`
    // falls back to the map id and `defaultMobGroup` falls back to `biome`.
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dataDir(), nullptr, error));
    CHECK(error.empty());
    // Nothing about the shipped data may be broken: a pad that leads nowhere,
    // a door a teleporter cannot reach. Empty is the only passing answer.
    for (const std::string& warning : maps.warnings()) {
        ::testing::reportFailure(__FILE__, __LINE__, "map warning: " + warning);
    }
    CHECK(maps.warnings().empty());

    CHECK_EQ(maps.count(), 1);
    CHECK(maps.forRealm(Realm::Overworld) != nullptr);
    CHECK(maps.forRealm(worldRealm(1)) == nullptr);
    CHECK(maps.forRealm(Realm::Arena) == nullptr);
    CHECK(maps.forRealm(Realm::Maze) == nullptr);
    bool found = false;
    CHECK(maps.realmOfId("garden", found) == Realm::Overworld);
    CHECK(found);

    const MapData& world = *maps.forRealm(Realm::Overworld);
    CHECK_EQ(world.id(), std::string("garden"));
    // None of these is written anywhere in garden.tmj.
    CHECK_EQ(world.biome(), std::string("garden"));
    CHECK_EQ(world.defaultMobGroup(), std::string("garden"));
    // The map declares no `defaultDifficulty`, so every square no band covers
    // is difficulty zero -- fully common, which is what a flower walking out of
    // the one door has to meet.
    CHECK_EQ(world.defaultDifficulty(), 0.0);
    CHECK(tierMixForDifficulty(world.defaultDifficulty()).lower == Rarity::Common);
    CHECK_EQ(tierMixForDifficulty(world.defaultDifficulty()).upperChance, 0.0);

    // 64 x 64 cells at kTileSize, which is what the file says and what the art
    // has to be indexed against.
    CHECK_EQ(world.width(), 64);
    CHECK_EQ(world.height(), 64);
    CHECK(!world.artFiles().empty());

    // The layer count is NOT pinned: the author adds and removes layers as the
    // map is drawn, and a test that counted them would fail every time they
    // did. What is pinned is the shape the engine needs -- every layer is a
    // full grid, and the map has both kinds of layer, because a map with no
    // colliding layer is one a player walks straight off and a map with no
    // scenery layer means the collision rule has collapsed into "everything
    // painted is a wall".
    CHECK(world.layers().size() >= 2);
    int collidingLayers = 0, sceneryLayers = 0;
    for (const TiledLayer& layer : world.layers()) {
        CHECK_EQ(layer.cells.size(), std::size_t{64} * 64);
        (layer.collides ? collidingLayers : sceneryLayers) += 1;
    }
    CHECK(collidingLayers > 0);
    CHECK(sceneryLayers > 0);
    // Every non-empty cell names an artwork the map actually carries.
    for (const TiledLayer& layer : world.layers()) {
        for (const TiledCell& cell : layer.cells) {
            CHECK(cell.art < static_cast<int>(world.artFiles().size()));
        }
    }
    // Off the map is an empty cell rather than a read past the end.
    CHECK_EQ(world.cellAt(0, -1, 0).art, -1);
    CHECK_EQ(world.cellAt(0, 64, 0).art, -1);
    CHECK_EQ(world.cellAt(world.layers().size(), 0, 0).art, -1);

    // One door and no pads. The BAND COUNT IS NOT PINNED: the author draws
    // difficulty onto the map as it is balanced, and a test that counted bands
    // would fail on every edit. What is pinned is that each band the file
    // carries is one the engine can act on -- a difficulty at or above zero (a
    // negative one would clamp and read as a mistake nobody was told about),
    // an area that can contain a point, and either its own mob group that the
    // content actually defines or none at all, which falls back to the map's.
    int bands = 0, regions = 0, doors = 0, teleporters = 0;
    for (const MapElement& element : world.elements()) {
        if (element.isSpawnBand()) {
            ++bands;
            CHECK(element.difficulty >= 0.0);
            CHECK(element.bounds.w > 0.0);
            CHECK(element.bounds.h > 0.0);
            for (const ZoneMobEntry& row : element.mobDistribution) {
                CHECK(content().mobGroupIndex(row.name) != kInvalidIndex ||
                      content().mobIndex(row.name) != kInvalidIndex);
            }
        }
        if (element.isMobRegion()) {
            ++regions;
            for (const ZoneMobEntry& row : element.mobDistribution) {
                CHECK(content().mobGroupIndex(row.name) != kInvalidIndex ||
                      content().mobIndex(row.name) != kInvalidIndex);
            }
        }
        if (element.kind == MapElementKind::PlayerSpawn) ++doors;
        if (element.kind == MapElementKind::Teleporter) ++teleporters;
    }
    CHECK_EQ(doors, 1);
    CHECK_EQ(teleporters, 0);
    // Bands and regions are two readings of one `spawn` object, and no object
    // is ever both: a `spawn` with a difficulty is a band, one without is a
    // region.
    int spawnObjects = 0;
    for (const MapElement& element : world.elements()) {
        if (element.kind == MapElementKind::Spawn) ++spawnObjects;
        CHECK(!(element.isSpawnBand() && element.isMobRegion()));
    }
    CHECK_EQ(bands + regions, spawnObjects);
    // A pad, if the map ever grows one, still has to say where it leads.
    for (const MapElement& element : world.elements()) {
        if (element.kind != MapElementKind::Teleporter) continue;
        CHECK(!element.targetMap.empty());
    }
}

TEST(the_shipped_door_is_named_by_its_label_and_is_pickable) {
    // garden.tmj's one door has an EMPTY Tiled name and no `spawnId` property.
    // All it says is `label: "Garden"`, and that is enough: the id falls back
    // to a slug of the label, so the door is `garden` and the picker offers
    // it. Before that fallback this object was dropped at load and the map had
    // no doors at all.
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dataDir(), nullptr, error));
    const MapData* world = maps.forRealm(Realm::Overworld);
    CHECK(world != nullptr);
    if (world == nullptr) return;

    CHECK_EQ(world->playerSpawns().size(), std::size_t{1});
    if (world->playerSpawns().empty()) return;
    const MapElement& door = *world->playerSpawns().front();
    CHECK_EQ(door.spawnId, std::string("garden"));
    CHECK_EQ(door.label, std::string("Garden"));
    CHECK(door.pickable);

    // And it is offered, under its map's biome tab, with its label on it.
    CHECK_EQ(maps.spawnChoices().size(), std::size_t{1});
    CHECK_EQ(maps.doors().size(), std::size_t{1});
    const SpawnChoice* choice = maps.choice("garden");
    CHECK(choice != nullptr);
    if (choice == nullptr) return;
    CHECK_EQ(choice->label, std::string("Garden"));
    CHECK_EQ(choice->biome, std::string("garden"));
    CHECK(choice->pickable);
    CHECK(choice->realm == Realm::Overworld);
    CHECK(maps.door("garden") != nullptr);
}

TEST(the_shipped_door_stands_on_open_ground) {
    // THE test of the shipped data under the layer collision rule. Collision
    // is now a property of the LAYER -- water, dirt and castle all have
    // `has_collision` ticked, so around three fifths of garden.tmj is solid
    // (the background and sand layers are painted over all of it and block
    // nothing, which is the rule working) -- and the
    // one thing that has to survive that is the door: a player must be able to
    // be put down inside it.
    //
    // The door is NOT wall-free to its last millimetre, and that is fine. Its
    // rectangle is 1900 x 1833 at (1166.67, 16800), which is not tile-aligned:
    // it overhangs the wall in column 3 by 33 units on the left and the wall in
    // row 62 by 33 units at the bottom. What matters is that the room inside it
    // is open -- 42 of the 56 cells it touches are ground -- and that every
    // real placement lands clear, which is what findOpenPoint's body test
    // guarantees. So this pins the two facts a player depends on: the great
    // majority of the rectangle is standable, and a placement never is not.
    Terrain terrain;
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dataDir(), &terrain, error));
    const MapData* world = maps.forRealm(Realm::Overworld);
    CHECK(world != nullptr);
    if (world == nullptr || world->playerSpawns().empty()) { CHECK(false); return; }
    const MapElement& door = *world->playerSpawns().front();

    CHECK(!terrain.blocked(door.centre(), Realm::Overworld));

    // The door's own cells, counted exactly rather than sampled: a wall
    // growing across the room shows up here as a number, not as a flaky
    // sample.
    int openCells = 0, walledCells = 0;
    const int minTx = Terrain::toTileCoord(door.bounds.x);
    const int maxTx = Terrain::toTileCoord(door.bounds.right() - 1e-6);
    const int minTy = Terrain::toTileCoord(door.bounds.y);
    const int maxTy = Terrain::toTileCoord(door.bounds.bottom() - 1e-6);
    for (int ty = minTy; ty <= maxTy; ++ty) {
        for (int tx = minTx; tx <= maxTx; ++tx) {
            if (tileBlocks(terrain.atTile(tx, ty, Realm::Overworld))) ++walledCells;
            else ++openCells;
        }
    }
    // Loudly: a door with no room in it is a map bug, and this is the number
    // that says so.
    if (openCells * 2 < openCells + walledCells) {
        ::testing::reportFailure(__FILE__, __LINE__,
                                 "the shipped door is mostly wall: " +
                                     std::to_string(openCells) + " open cells, " +
                                     std::to_string(walledCells) + " walled");
    }
    CHECK(openCells >= 24);

    // And two hundred real placements, every one of them clear -- not the
    // centre, not a nudge, the actual answer the join path takes.
    Rng rng(0xD00D);
    for (int i = 0; i < 200; ++i) {
        Vec2 placed{};
        CHECK(world->spawnAt("garden", rng, terrain, placed));
        CHECK(door.contains(placed));
        if (terrain.blocked(placed, Realm::Overworld)) {
            ::testing::reportFailure(__FILE__, __LINE__,
                                     "spawnAt landed in a wall at " +
                                         std::to_string(placed.x) + "," +
                                         std::to_string(placed.y));
            break;
        }
    }
}

TEST(a_player_joins_on_the_beginner_ground) {
    Harness h("spawn-default");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginNew(h, client, "newcomer", "password7"));
    client.joinGame(1280, 720);
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    const Entity body = onlyPlayer(h.server.world());
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) return;
    const Vec2 at = h.server.world().get<Transform>(body).position;

    // Joining with NO choice at all lands in the map's first door in button
    // order, which on the shipped data is its only one.
    CHECK(inSpawnPoint(overworld(h), "garden", at));
    CHECK(!onDangerousGround(overworld(h), at));
    CHECK(!h.server.terrain().blocked(at, Realm::Overworld));
    CHECK(h.server.world().get<Transform>(body).realm == Realm::Overworld);
}

TEST(the_live_server_grows_what_the_ground_under_each_mob_declares) {
    // The real GameServer, the real garden.tmj, a real client joining through
    // the real door -- and every mob the population controller puts around that
    // flower is no rarer than the ground it stands on says. The map declares no
    // `defaultDifficulty`, so all the ground no band covers is the curve's first
    // anchor, fully common: this is the end-to-end form of the anchor a new
    // player actually meets, with the author's own bands allowed to be as hard
    // as they claim and no harder.
    Harness h("spawn-commons");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginNew(h, client, "greenhorn", "password7"));
    client.joinGame(1280, 720);
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    // Long enough for several population passes (they run on a 500ms clock).
    World& world = h.server.world();
    int mobs = 0;
    for (int i = 0; i < 400 && mobs < 20; ++i) {
        h.step(1, {&client});
        Query<MobTag> live{world};
        mobs = static_cast<int>(live.count());
    }
    CHECK(mobs > 0);

    // The door's own ground has to be beginner ground whatever else the author
    // paints: that is the rule a joining flower depends on. On the shipped map
    // the door stands inside a difficulty-0.5 band, so "beginner" is the band's
    // number rather than the map default -- but its blend still bottoms out at
    // common, which is the claim that matters to a level-1 flower.
    const Vec2 spawnedAt = h.server.world().get<Transform>(onlyPlayer(world)).position;
    const double doorDifficulty = difficultyOnMap(overworld(h), spawnedAt);
    CHECK(!onDangerousGround(overworld(h), spawnedAt));
    CHECK(doorDifficulty < kDangerousGroundDifficulty);
    CHECK(tierMixForDifficulty(doorDifficulty).lower == Rarity::Common);

    int commonGround = 0;
    int bandedGround = 0;
    Query<MobTag, MobType, Transform> live{world};
    live.each([&](Entity, MobTag&, MobType& type, Transform& transform) {
        // A mob's own min_rarity still floors it, which is the mob's property
        // rather than the ground's; above both that floor and the hardest tier
        // its square can roll, the ground rolled something it had no licence to.
        const double difficulty = difficultyOnMap(overworld(h), transform.position);
        const MobConfig& config = content().mob(type.configIndex);
        const int ceiling =
            std::max(rarityIndex(tierMixForDifficulty(difficulty).upper), rarityIndex(config.minRarity));
        if (rarityIndex(type.rarity) > ceiling) {
            ::testing::reportFailure(__FILE__, __LINE__,
                                     std::string("the live garden grew a ") + rarityName(type.rarity) +
                                         " " + config.id + " on difficulty-" +
                                         std::to_string(difficulty) + " ground");
        }
        if (difficulty != 0.0) {
            ++bandedGround;
            return;
        }
        ++commonGround;
        if (type.rarity != Rarity::Common && rarityIndex(type.rarity) > rarityIndex(config.minRarity)) {
            ::testing::reportFailure(__FILE__, __LINE__,
                                     std::string("the live garden grew a ") + rarityName(type.rarity) +
                                         " " + config.id + " on difficulty-zero ground");
        }
    });
    // Which of the two arms the population landed in is the author's business --
    // on the shipped map the door's whole neighbourhood is inside one of its
    // bands, so it is all banded ground today. What is pinned is that EVERY live
    // mob was judged against a difficulty rather than skipped by a lookup that
    // found nothing.
    CHECK_EQ(commonGround + bandedGround, mobs);
}

TEST(respawning_returns_to_the_beginner_ground) {
    Harness h("spawn-respawn");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginNew(h, client, "phoenix", "password7"));
    client.joinGame(1280, 720);
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    World& world = h.server.world();
    const Entity body = onlyPlayer(world);
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) return;

    // Kill the body outright. Death is a component, so this is the same state
    // combat would leave behind, without waiting for a real fight.
    world.get<Health>(body).current = 0.0;
    world.add<Dead>(body, Dead{NULL_ENTITY});
    CHECK(h.stepUntil({&client}, [&] { return client.dead(); }));

    client.requestRespawn();
    CHECK(h.stepUntil({&client}, [&] {
        const Entity respawned = onlyPlayer(world);
        return respawned != NULL_ENTITY && !world.has<Dead>(respawned);
    }));

    const Entity respawned = onlyPlayer(world);
    CHECK(respawned != NULL_ENTITY);
    if (respawned == NULL_ENTITY) return;
    const Vec2 at = world.get<Transform>(respawned).position;

    // The whole point: a respawn goes back to the beginner ground, NOT to a
    // band picked from the player's level and not to the middle of the map.
    CHECK(inSpawnPoint(overworld(h), "garden", at));
    CHECK(!onDangerousGround(overworld(h), at));
    // Not the middle of the map, which is what the bug this test was written
    // for did. Measured against the map's OWN extent -- every map says its own
    // size now, and a fixed world constant would stop meaning anything the
    // moment an author resized the garden.
    const Vec2 extent = h.server.terrain().realmExtent(Realm::Overworld);
    CHECK(distance(at, extent * 0.5) > 0.2 * extent.length());
}

namespace {

// ---------------------------------------------------------------------------
// A two-map world, built here rather than taken from the shipped data
// ---------------------------------------------------------------------------
//
// The game ships ONE map with ONE door on it. Everything about several realms
// -- choosing between doors, a pad that lands somewhere, a door an admin may
// name and nobody else may -- therefore needs a world of its own. Bending the
// shipped map into that shape would make the game's art a test fixture and
// stop an author from ever changing it.
//
// `meadow` is the overworld: two pickable doors and a pad into `warren`.
// `warren` is a second, differently-sized realm whose only door is NOT
// pickable, which is exactly the sublevel arrangement the picker's rules are
// written for.

using flix::testsupport::twoMapDataDir;

/// An admin account, seeded before the server opens the database.
void seedAdminAccount(const std::string& path) {
    Database db;
    std::string error;
    db.load(path, error);
    db.setPasswordCost(4);
    CreateResult created = db.createUser("boss", "password7");
    if (created.ok()) created.account->admin = true;
    db.markDirty();
    db.save();
}

Entity bodyNamed(World& world, const std::string& name) {
    Entity found = NULL_ENTITY;
    Query<PlayerTag, PlayerAccount> players{world};
    players.each([&](Entity e, PlayerTag&, PlayerAccount& account) {
        if (account.username == name) found = e;
    });
    return found;
}

} // namespace

TEST(a_chosen_spawn_point_is_honoured_and_survives_a_respawn) {
    const std::string dir = twoMapDataDir("choice");
    CHECK(!dir.empty());
    if (dir.empty()) return;
    Harness h("spawn-choice", {}, dir, 0);
    if (!h.ready) { CHECK(false); flix::testsupport::removeDataDir(dir); return; }
    CHECK(h.server.worldMaps().choice("dunes") != nullptr);

    NetClient client;
    CHECK(loginNew(h, client, "wanderer", "password7"));
    client.joinGame(1280, 720, "dunes");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    World& world = h.server.world();
    Entity body = onlyPlayer(world);
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) { flix::testsupport::removeDataDir(dir); return; }
    CHECK(inSpawnPoint(overworld(h), "dunes", world.get<Transform>(body).position));

    // The choice lives on the session, so dying does not quietly move the
    // player back to the map's first door.
    world.get<Health>(body).current = 0.0;
    world.add<Dead>(body, Dead{NULL_ENTITY});
    CHECK(h.stepUntil({&client}, [&] { return client.dead(); }));
    client.requestRespawn();
    CHECK(h.stepUntil({&client}, [&] {
        const Entity respawned = onlyPlayer(world);
        return respawned != NULL_ENTITY && !world.has<Dead>(respawned);
    }));

    body = onlyPlayer(world);
    CHECK(body != NULL_ENTITY);
    if (body != NULL_ENTITY) {
        CHECK(inSpawnPoint(overworld(h), "dunes", world.get<Transform>(body).position));
    }
    flix::testsupport::removeDataDir(dir);
}

TEST(a_teleporter_carries_a_player_to_another_map) {
    const std::string dir = twoMapDataDir("pad");
    CHECK(!dir.empty());
    if (dir.empty()) return;
    Harness h("spawn-teleporter", {}, dir, 0);
    if (!h.ready) { CHECK(false); flix::testsupport::removeDataDir(dir); return; }

    const MapElement* pad = nullptr;
    for (const MapElement& element : overworld(h).elements()) {
        if (element.kind == MapElementKind::Teleporter && element.targetMap == "warren") {
            pad = &element;
        }
    }
    CHECK(pad != nullptr);
    if (pad == nullptr) { flix::testsupport::removeDataDir(dir); return; }

    NetClient client;
    CHECK(loginNew(h, client, "spelunker", "password7"));
    client.joinGame(1280, 720, {}, "spelunker");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    World& world = h.server.world();
    const Entity body = onlyPlayer(world);
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) { flix::testsupport::removeDataDir(dir); return; }

    // Stand on the pad and wait out the dwell. The pad is HELD, not touched:
    // stepping onto it and straight off again must not fire it.
    world.get<Transform>(body).position = pad->centre();
    CHECK(h.stepUntil({&client}, [&] {
        return world.get<Transform>(body).realm != Realm::Overworld;
    }, 120));

    const Transform& at = world.get<Transform>(body);
    bool found = false;
    const Realm warren = h.server.worldMaps().realmOfId("warren", found);
    CHECK(found);
    CHECK(at.realm == warren);
    const MapData* map = h.server.worldMaps().forRealm(warren);
    CHECK(map != nullptr);
    if (map != nullptr) CHECK(inSpawnPoint(*map, pad->targetSpawn, at.position));
    // The kit came too: nothing of this flower's is left in the overworld.
    Query<Transform, PetalInstance> petals{world};
    petals.each([&](Entity, Transform& petal, PetalInstance& owner) {
        if (owner.owner == body) CHECK(petal.realm == warren);
    });
    // And the client followed: it was sent the second map's grid, which is a
    // DIFFERENT shape from the first's, and drew the arrival there.
    CHECK(h.stepUntil({&client}, [&] { return client.view().realm() == warren; }));
    CHECK_EQ(client.terrain().tileCols(warren), h.server.terrain().tileCols(warren));
    CHECK(h.server.terrain().tileCols(warren) != h.server.terrain().tileCols(Realm::Overworld));
    flix::testsupport::removeDataDir(dir);
}

TEST(a_spawn_choice_the_maps_do_not_define_falls_back) {
    Harness h("spawn-unknown");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginNew(h, client, "lost", "password7"));
    // A spawn point that is not on any map. The join must still succeed, on
    // the beginner ground, rather than being refused or landing nowhere.
    client.joinGame(1280, 720, "atlantis");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    const Entity body = onlyPlayer(h.server.world());
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) return;
    CHECK(inSpawnPoint(overworld(h), "garden", h.server.world().get<Transform>(body).position));
}

// ---------------------------------------------------------------------------
// Zone outlines
// ---------------------------------------------------------------------------
//
// Spawn zones are polygons. These are the geometry that decides which tier of
// mob a point belongs to, so the interesting cases are the ones a rectangle
// never had: a concave notch, and the boundary itself.

namespace {

/// A 4000-unit square with a 2000-unit bite taken out of its bottom-right --
/// an L, which is the smallest shape whose bounding box lies about it.
MapElement lZone() {
    MapElement zone;
    zone.kind = MapElementKind::Spawn;
    zone.polygon = {{0, 0}, {4000, 0}, {4000, 2000}, {2000, 2000}, {2000, 4000}, {0, 4000}};
    zone.bounds = {0, 0, 4000, 4000};
    return zone;
}

} // namespace

TEST(a_zone_outline_excludes_what_its_bounding_box_includes) {
    const MapElement zone = lZone();

    // Inside both.
    CHECK(zone.contains({1000, 1000}));
    CHECK(zone.contains({3000, 1000}));
    CHECK(zone.contains({1000, 3000}));

    // The bite: inside the bounding box, outside the zone. This is the whole
    // difference between the two shapes, and the reason the spawner tests the
    // outline rather than the box it culls with.
    CHECK(zone.bounds.contains({3000, 3000}));
    CHECK(!zone.contains({3000, 3000}));

    // Outside both.
    CHECK(!zone.contains({-1, 1000}));
    CHECK(!zone.contains({5000, 1000}));
}

TEST(a_zone_boundary_counts_as_inside) {
    // The rectangles these replaced were tested inclusively on every edge, so a
    // mob standing exactly on a border was in that zone. A polygon that dropped
    // it would move every seam between two tier bands by a hair, silently.
    const MapElement zone = lZone();
    for (const Vec2 corner : zone.polygon) CHECK(zone.contains(corner));
    CHECK(zone.contains({2000, 0}));        // on the top edge
    CHECK(zone.contains({0, 4000}));        // on a corner
    CHECK(zone.contains({3000, 2000}));     // on the notch's horizontal edge
    CHECK(zone.contains({2000, 3000}));     // on the notch's vertical edge
}

TEST(a_zone_area_is_the_outlines_not_the_boxs) {
    // A zone's mob target is scaled by this. Sizing an L-shaped band by its
    // bounding box would pack it at a third again the density of a rectangular
    // zone next to it.
    const MapElement zone = lZone();
    CHECK_EQ(zone.area(), 4000.0 * 4000.0 - 2000.0 * 2000.0);
    CHECK_EQ(zone.bounds.w * zone.bounds.h, 4000.0 * 4000.0);

    // A zone with no outline is its rectangle, area and all.
    MapElement rect;
    rect.kind = MapElementKind::Spawn;
    rect.bounds = {100, 200, 30, 40};
    CHECK_EQ(rect.area(), 30.0 * 40.0);
    CHECK(rect.contains({130, 240}));       // inclusive, as it always was
    CHECK(!rect.contains({130.5, 240}));
}

TEST(a_spawn_in_a_polygon_zone_lands_inside_it) {
    // Placement samples the bounding box and rejects what falls outside the
    // outline. With an L that is a quarter of the box, so this also says the
    // rejection loop does not simply give up.
    Terrain terrain;   // all ground: the outline is the only thing rejecting
    MapData map;
    Rng rng(12345);
    const MapElement zone = lZone();

    int placed = 0;
    for (int trial = 0; trial < 200; ++trial) {
        Vec2 at;
        if (!map.spawnInElement(zone, rng, terrain, at)) continue;
        ++placed;
        CHECK(zone.contains(at));
    }
    // Not "every attempt succeeded" -- the sampler is allowed to run out of
    // tries -- but it must work the large majority of the time or a zone like
    // this would starve.
    CHECK(placed > 150);
}

// ---------------------------------------------------------------------------
// Zone mob distributions
// ---------------------------------------------------------------------------
//
// A zone says WHAT it spawns as weighted rows of section presets and named
// mobs -- "garden 50% hornet 50%". How DANGEROUS it is, is a separate property
// -- its `difficulty` -- and that is where the map's progression lives.

TEST(a_distribution_parses_the_authored_syntax) {
    std::string warning;
    const std::vector<ZoneMobEntry> rows = parseMobDistribution("garden 50% hornet 50%", &warning);
    CHECK(warning.empty());
    CHECK(rows.size() == 2);

    // Names only. Whether "garden" is a group or "hornet" is a mob is the
    // content's question, answered when the band is built against it: the map
    // layer must not depend on mobs.json to be readable.
    CHECK_EQ(rows[0].name, std::string("garden"));
    CHECK_EQ(rows[0].weight, 50.0);
    CHECK_EQ(rows[1].name, std::string("hornet"));
    CHECK_EQ(rows[1].weight, 50.0);
}

TEST(a_distribution_accepts_the_shapes_an_author_will_type) {
    // Percent signs, commas and separators are all noise; the weights are
    // relative, so nothing has to add up to a hundred.
    const std::vector<ZoneMobEntry> spelled = parseMobDistribution("ocean 20% jellyfish 80%", nullptr);
    const std::vector<ZoneMobEntry> bare = parseMobDistribution("ocean 20, jellyfish 80", nullptr);
    const std::vector<ZoneMobEntry> ratio = parseMobDistribution("ocean 1 jellyfish 4", nullptr);
    for (const auto* rows : {&spelled, &bare, &ratio}) {
        CHECK(rows->size() == 2);
        CHECK_EQ((*rows)[0].name, std::string("ocean"));
        CHECK_EQ((*rows)[1].name, std::string("jellyfish"));
        CHECK((*rows)[1].weight > (*rows)[0].weight);
    }

    // A bare name is a zone of nothing but that.
    const std::vector<ZoneMobEntry> only = parseMobDistribution("hornet", nullptr);
    CHECK(only.size() == 1);
    CHECK_EQ(only[0].name, std::string("hornet"));
    CHECK_EQ(only[0].weight, 1.0);

    // Underscores are part of a name, because "ant_hell" is a group.
    const std::vector<ZoneMobEntry> ants = parseMobDistribution("ant_hell 100%", nullptr);
    CHECK(ants.size() == 1);
    CHECK_EQ(ants[0].name, std::string("ant_hell"));
}

TEST(a_broken_distribution_is_reported_not_guessed_at) {
    // Nothing at all: the spawner reads an empty list as "no distribution" and
    // asks the ground under the band instead.
    CHECK(parseMobDistribution("", nullptr).empty());
    CHECK(parseMobDistribution("   ", nullptr).empty());

    // A weight of zero would make its row unreachable, which is a mistake
    // rather than an intention. Skipped, and said out loud -- a mistyped
    // distribution is otherwise a zone that silently keeps its old behaviour.
    std::string warning;
    const std::vector<ZoneMobEntry> rows = parseMobDistribution("hornet 0 bee 3", &warning);
    CHECK(!warning.empty());
    CHECK(rows.size() == 1);
    CHECK_EQ(rows[0].name, std::string("bee"));
}

TEST(the_shipped_map_falls_back_to_its_own_mob_group_wherever_nothing_says_otherwise) {
    // world.tmj carried two hundred bands and nine regions and the spawner was
    // only ever exercised through them; the shipped map is the other extreme --
    // an author paints art, drops a door, and brushes difficulty onto a few
    // shapes without ever saying WHAT lives in them. So the chain that has to
    // hold is the fallback one: a non-empty default group that the content
    // actually defines, reached by every band that names no roster of its own.
    // A map that resolved to an empty group would spawn nothing at all,
    // silently.
    MapData map;
    std::string error;
    CHECK(map.loadTiled(dataDir() + "/garden.tmj", error));
    CHECK(error.empty());
    CHECK(!map.defaultMobGroup().empty());
    CHECK_EQ(map.defaultMobGroup(), map.biome());
    CHECK(content().mobGroupIndex(map.defaultMobGroup()) != kInvalidIndex);

    // Every band on the map either says nothing about mobs -- and so reaches
    // the default group above -- or names rows the content defines. Neither the
    // number of bands nor their difficulties are pinned here: the author is
    // still balancing them, and this is a test of the FALLBACK, not of the art.
    for (const MapElement& element : map.elements()) {
        if (!element.isSpawnBand() && !element.isMobRegion()) continue;
        for (const ZoneMobEntry& row : element.mobDistribution) {
            CHECK(content().mobGroupIndex(row.name) != kInvalidIndex ||
                  content().mobIndex(row.name) != kInvalidIndex);
        }
    }
}

TEST(an_authored_band_and_region_still_parse) {
    // Bands and regions are not gone, only unused by the shipped art. This is
    // the coverage the old world.tmj gave for free, kept alive against a map
    // written here: a `spawn` object WITH a difficulty is a band that owns a
    // population, one WITHOUT is a region that only says what grows there.
    //
    // The difficulty is authored as an `int`, which is what Tiled's own spinner
    // writes; a `float` lands in the same place. 40 is rare-ish ground on the
    // curve -- mostly rare with some epic in it.
    const std::string objects =
        R"({ "id": 1, "type": "spawn", "visible": true, "rotation": 0,
             "x": 600, "y": 600, "width": 3000, "height": 3000, "properties": [
               { "name": "difficulty", "type": "int", "value": 40 },
               { "name": "mobs", "type": "string", "value": "hornet 100%" } ] },
           { "id": 2, "type": "spawn", "visible": true, "rotation": 0,
             "x": 600, "y": 4200, "width": 3000, "height": 3000, "properties": [
               { "name": "mobs", "type": "string", "value": "ocean 100%" } ] })";
    const std::string banded =
        flix::testsupport::fixtureMap(24, 24, std::string(), std::string(), objects);
    const std::string dir = flix::testsupport::stageDataDir("bands", {{"banded", banded}});
    CHECK(!dir.empty());
    if (dir.empty()) return;

    MapData map;
    std::string error;
    CHECK(map.loadTiled(dir + "/banded.tmj", error));
    int bands = 0;
    int regions = 0;
    for (const MapElement& element : map.elements()) {
        if (element.isSpawnBand()) {
            ++bands;
            CHECK_NEAR(element.difficulty, 40.0, 1e-9);
            CHECK(tierMixForDifficulty(element.difficulty).lower == Rarity::Rare);
            CHECK_EQ(element.mobDistribution.size(), std::size_t{1});
            CHECK_EQ(element.mobDistribution[0].name, std::string("hornet"));
        }
        if (element.isMobRegion()) {
            ++regions;
            CHECK_EQ(element.mobDistribution[0].name, std::string("ocean"));
        }
    }
    CHECK_EQ(bands, 1);
    CHECK_EQ(regions, 1);
    flix::testsupport::removeDataDir(dir);
}

// ---------------------------------------------------------------------------
// Doors: safe ground, and who may use which
// ---------------------------------------------------------------------------

TEST(every_pickable_door_stands_on_safe_open_ground) {
    // The rule the picker relies on: a door the title screen offers puts a
    // fresh flower down on open ground whose difficulty is below the first one
    // that can roll a rare at all. Checked over every staged map, so a hand
    // edit that slides a difficulty-40 band over a door does not ship
    // unnoticed.
    Terrain terrain;
    WorldMaps maps;
    std::string error;
    if (!maps.load(flix::testsupport::dataDir(), &terrain, error)) {
        ::testing::reportFailure(__FILE__, __LINE__, "the shipped maps did not load: " + error);
        return;
    }
    int doors = 0;
    for (const SpawnChoice& choice : maps.spawnChoices()) {
        CHECK(choice.pickable);
        const MapData* map = maps.forRealm(choice.realm);
        if (map == nullptr || choice.element < 0 ||
            choice.element >= static_cast<int>(map->elements().size())) {
            ::testing::reportFailure(__FILE__, __LINE__, "door " + choice.id + " has no rectangle");
            continue;
        }
        const MapElement& door = map->elements()[static_cast<std::size_t>(choice.element)];
        const Vec2 centre = door.centre();
        ++doors;
        if (onDangerousGround(*map, centre)) {
            ::testing::reportFailure(__FILE__, __LINE__,
                                     "door " + choice.id + " on " + map->id() +
                                         " sits under a band of difficulty " +
                                         std::to_string(kDangerousGroundDifficulty) + " or more");
        }
        if (terrain.blocked(centre, choice.realm)) {
            ::testing::reportFailure(__FILE__, __LINE__,
                                     "door " + choice.id + " on " + map->id() +
                                         " is walled over");
        }
    }
    CHECK(doors > 0);
    // Every door, pickable or not, is known by the same ids: a pad arrives at
    // a sublevel door through door(), never through choice().
    CHECK(maps.doors().size() >= maps.spawnChoices().size());
    for (const SpawnChoice& choice : maps.spawnChoices()) {
        const SpawnChoice* same = maps.door(choice.id);
        CHECK(same != nullptr);
        if (same != nullptr) CHECK(same->realm == choice.realm);
    }
}

TEST(a_door_on_another_map_joins_into_that_map) {
    // Naming a door that lives on a SECOND map puts the body in that map's own
    // coordinate space, and the client is told the shape of the grid it is
    // about to draw. The door is not pickable, so this also needs an admin --
    // see the next test for the rule itself.
    const std::string dir = twoMapDataDir("othermap");
    CHECK(!dir.empty());
    if (dir.empty()) return;
    Harness h("spawn-other-map", seedAdminAccount, dir, 0);
    if (!h.ready) { CHECK(false); flix::testsupport::removeDataDir(dir); return; }
    const SpawnChoice* gate = h.server.worldMaps().door("warren_gate");
    CHECK(gate != nullptr);
    if (gate == nullptr) { flix::testsupport::removeDataDir(dir); return; }

    NetClient client;
    CHECK(flix::testsupport::connectClient(h, client));
    client.requestLogin("boss", "password7");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
    client.joinGame(1280, 720, "warren_gate", "boss");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    World& world = h.server.world();
    const Entity body = onlyPlayer(world);
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) { flix::testsupport::removeDataDir(dir); return; }
    const Transform& at = world.get<Transform>(body);
    CHECK(at.realm == gate->realm);
    const MapData* warren = h.server.worldMaps().forRealm(gate->realm);
    CHECK(warren != nullptr);
    if (warren != nullptr) CHECK(inSpawnPoint(*warren, "warren_gate", at.position));
    CHECK(h.stepUntil({&client}, [&] { return client.view().realm() == gate->realm; }));
    CHECK_EQ(client.terrain().tileCols(gate->realm), h.server.terrain().tileCols(gate->realm));
    CHECK_EQ(client.terrain().tileRows(gate->realm), h.server.terrain().tileRows(gate->realm));
    // Its own size, not the overworld's and not any historical default.
    CHECK_EQ(client.terrain().tileCols(gate->realm), 16);
    CHECK(!h.server.terrain().blocked(at.position, at.realm));
    flix::testsupport::removeDataDir(dir);
}

TEST(a_door_that_is_not_pickable_is_joined_only_by_an_admin) {
    // The rule the picker's two lists exist for. `warren_gate` is marked
    // `pickable: false`, so it is a door and not a choice: door() finds it, the
    // title screen never offers it, a player naming it anyway starts at the
    // default, and an admin naming it arrives there.
    //
    // The shipped data has no such door any more -- one map, one door, and it
    // is pickable -- so the arrangement is staged here rather than deleted.
    const std::string dir = twoMapDataDir("sublevel");
    CHECK(!dir.empty());
    if (dir.empty()) return;
    Harness h("spawn-sublevel", seedAdminAccount, dir, 0);
    CHECK(h.ready);
    if (!h.ready) { flix::testsupport::removeDataDir(dir); return; }

    const WorldMaps& maps = h.server.worldMaps();
    const SpawnChoice* door = maps.door("warren_gate");
    CHECK(door != nullptr);
    if (door == nullptr) { flix::testsupport::removeDataDir(dir); return; }
    CHECK(!door->pickable);
    CHECK(maps.choice("warren_gate") == nullptr);
    for (const SpawnChoice& choice : maps.spawnChoices()) CHECK(choice.pickable);
    CHECK_EQ(maps.spawnChoices().size(), std::size_t{2});
    CHECK_EQ(maps.doors().size(), std::size_t{3});

    // A player naming it starts at the default, on the overworld.
    NetClient player;
    CHECK(flix::testsupport::connectClient(h, player));
    player.requestRegister("ratcatcher", "password7");
    CHECK(h.stepUntil({&player}, [&] { return player.status() == NetClient::Status::LoggedIn; }));
    player.joinGame(1280, 720, "warren_gate", "ratcatcher");
    CHECK(h.stepUntil({&player}, [&] { return player.status() == NetClient::Status::Playing; }));
    World& world = h.server.world();
    const Entity body = bodyNamed(world, "ratcatcher");
    CHECK(body != NULL_ENTITY);
    if (body != NULL_ENTITY) {
        const Transform& at = world.get<Transform>(body);
        CHECK(at.realm == Realm::Overworld);
        const MapData* overworldMap = maps.forRealm(Realm::Overworld);
        CHECK(overworldMap != nullptr && inSpawnPoint(*overworldMap, "meadow", at.position));
    }
    CHECK(player.view().realm() == Realm::Overworld);

    // An admin naming it arrives on the second map, at that door.
    NetClient admin;
    CHECK(flix::testsupport::connectClient(h, admin));
    admin.requestLogin("boss", "password7");
    CHECK(h.stepUntil({&admin}, [&] { return admin.status() == NetClient::Status::LoggedIn; }));
    admin.joinGame(1280, 720, "warren_gate", "boss");
    CHECK(h.stepUntil({&player, &admin}, [&] { return admin.status() == NetClient::Status::Playing; }));
    const Entity bossBody = bodyNamed(world, "boss");
    CHECK(bossBody != NULL_ENTITY);
    if (bossBody != NULL_ENTITY) {
        const Transform& at = world.get<Transform>(bossBody);
        CHECK(at.realm == door->realm);
        const MapData* warren = maps.forRealm(door->realm);
        CHECK(warren != nullptr && inSpawnPoint(*warren, "warren_gate", at.position));
    }
    CHECK(h.stepUntil({&player, &admin}, [&] { return admin.view().realm() == door->realm; }));
    flix::testsupport::removeDataDir(dir);
}


// ---------------------------------------------------------------------------
// A map that is mostly wall
// ---------------------------------------------------------------------------
//
// Collision is a property of the LAYER now, and the shipped map ticks it on
// three of its five: about three fifths of garden.tmj is solid. That is the
// author's design, and it moved every placement path from "the map is mostly
// open, a rejected sample is bad luck" to "a rejected sample is the common
// case". These run the whole set on a fixture built to the same density, with
// a deliberately badly-placed door on it, because a path that quietly gives up
// on a dense map does not look like a bug -- it looks like a player standing
// in a wall.

TEST(a_door_drawn_over_solid_ground_still_lands_a_body_on_open_ground) {
    // `cellar` is one 300-unit cell and every point in it is wall. Fifty
    // rejection samples find nothing, which is the case the fallback exists
    // for -- and the fallback used to be the rectangle's own CENTRE, i.e. the
    // middle of that wall. The centre is now where the search STARTS: the
    // nearest ground a body can actually stand on.
    const std::string dir = flix::testsupport::denseMapDataDir("solid-door");
    CHECK(!dir.empty());
    if (dir.empty()) return;

    Terrain terrain;
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dir, &terrain, error));
    const MapData* world = maps.forRealm(Realm::Overworld);
    CHECK(world != nullptr);
    if (world == nullptr) { flix::testsupport::removeDataDir(dir); return; }

    // The fixture really is as solid as the shipped map, or this proves
    // nothing.
    int blocked = 0;
    const int cols = terrain.tileCols(Realm::Overworld);
    const int rows = terrain.tileRows(Realm::Overworld);
    for (int ty = 0; ty < rows; ++ty) {
        for (int tx = 0; tx < cols; ++tx) {
            if (tileBlocks(terrain.atTile(tx, ty, Realm::Overworld))) ++blocked;
        }
    }
    CHECK(blocked * 2 > cols * rows);

    const MapElement* cellar = world->playerSpawn("cellar");
    CHECK(cellar != nullptr);
    if (cellar == nullptr) { flix::testsupport::removeDataDir(dir); return; }
    CHECK(!cellar->pickable);
    // Every corner and the centre: the whole rectangle is wall.
    CHECK(terrain.blocked(cellar->centre(), Realm::Overworld));

    Rng rng(0x5011D);
    for (int i = 0; i < 50; ++i) {
        Vec2 at{};
        CHECK(world->spawnAt("cellar", rng, terrain, at));
        if (terrain.blocked(at, Realm::Overworld)) {
            ::testing::reportFailure(__FILE__, __LINE__,
                                     "a door over solid ground put a body in a wall at " +
                                         std::to_string(at.x) + "," + std::to_string(at.y));
            break;
        }
    }
    // And so does the whole-map default, when the first door in button order
    // is the walled one. `hollow` is first here, so this is the ordinary path;
    // the point is that it still answers with open ground on a dense map.
    for (int i = 0; i < 50; ++i) {
        const Vec2 at = world->defaultSpawn(rng, terrain);
        if (terrain.blocked(at, Realm::Overworld)) {
            ::testing::reportFailure(__FILE__, __LINE__,
                                     "defaultSpawn put a body in a wall at " +
                                         std::to_string(at.x) + "," + std::to_string(at.y));
            break;
        }
    }
    flix::testsupport::removeDataDir(dir);
}

TEST(every_body_a_dense_map_places_stands_on_open_ground) {
    // The live server on the same fixture, with its usual bot population: a
    // joining player, a respawning player, every bot, every mob the density
    // fill and the bands stood, and every drop. None of them may be inside a
    // wall, and the population may not have collapsed because the placement
    // gave up.
    const std::string dir = flix::testsupport::denseMapDataDir("dense-world");
    CHECK(!dir.empty());
    if (dir.empty()) return;
    Harness h("spawn-dense", {}, dir);   // the server's usual bots, deliberately
    CHECK(h.ready);
    if (!h.ready) { flix::testsupport::removeDataDir(dir); return; }

    NetClient client;
    CHECK(loginNew(h, client, "spelunker", "password7"));
    client.joinGame(1280, 720, {}, "spelunker");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));
    h.step(200, {&client});

    World& world = h.server.world();
    const Terrain& terrain = h.server.terrain();
    const MapData* map = h.server.worldMaps().forRealm(Realm::Overworld);
    CHECK(map != nullptr);

    // The join landed in the pickable door, on ground.
    Entity body = NULL_ENTITY;
    Query<PlayerTag, Transform, PlayerAccount> flowers{world};
    flowers.each([&](Entity e, PlayerTag&, Transform&, PlayerAccount& account) {
        if (account.username == "spelunker") body = e;
    });
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) { flix::testsupport::removeDataDir(dir); return; }
    CHECK(map != nullptr && inSpawnPoint(*map, "hollow", world.get<Transform>(body).position));
    CHECK(!terrain.blocked(world.get<Transform>(body).position, Realm::Overworld));

    // Every flower in the world, bots included -- and there ARE bots, because
    // a population that could not be placed would be an empty world that
    // looked like a passing test.
    int flowerCount = 0;
    int inWall = 0;
    flowers.each([&](Entity, PlayerTag&, Transform& transform, PlayerAccount&) {
        ++flowerCount;
        if (terrain.blocked(transform.position, Realm::Overworld)) ++inWall;
    });
    CHECK(flowerCount > 4);
    CHECK_EQ(inWall, 0);

    // Every mob the spawner stood. Water counts as a wall here: tileBlocks()
    // is true of it, and a mob standing in the pond is as stuck as one inside
    // a castle.
    int mobCount = 0;
    int mobsInWall = 0;
    Query<MobTag, Transform> mobs{world};
    mobs.each([&](Entity, MobTag&, Transform& transform) {
        if (transform.realm != Realm::Overworld) return;
        ++mobCount;
        if (terrain.blocked(transform.position, Realm::Overworld)) ++mobsInWall;
    });
    CHECK(mobCount > 0);
    CHECK_EQ(mobsInWall, 0);

    // And a respawn, which takes the same path a second time with a world full
    // of bodies to avoid.
    world.get<Health>(body).current = 0;
    world.add<Dead>(body, Dead{NULL_ENTITY});
    // Out of the bots' reach first. A bot carrying yggdrasil revives a corpse
    // it can get to, and on a map this solid the bots are packed close enough
    // to the only door that the corpse is back on its feet before the client
    // is ever told it died. That is the world working; it is not what is being
    // measured here.
    {
        int farTx = 0;
        int farTy = 0;
        const Vec2 mapExtent = terrain.realmExtent(Realm::Overworld);
        CHECK(terrain.nearestOpenTile({mapExtent.x - kTileSize, mapExtent.y - kTileSize}, farTx,
                                      farTy, Realm::Overworld));
        world.get<Transform>(body).position = Terrain::tileCenter(farTx, farTy);
    }
    CHECK(h.stepUntil({&client}, [&] { return client.dead(); }));
    client.requestRespawn();
    CHECK(h.stepUntil({&client}, [&] {
        Entity reborn = NULL_ENTITY;
        Query<PlayerTag, Transform, PlayerAccount> again{world};
        again.each([&](Entity e, PlayerTag&, Transform&, PlayerAccount& account) {
            if (account.username == "spelunker" && e != body) reborn = e;
        });
        return reborn != NULL_ENTITY;
    }));
    Entity reborn = NULL_ENTITY;
    flowers.each([&](Entity e, PlayerTag&, Transform&, PlayerAccount& account) {
        if (account.username == "spelunker" && e != body) reborn = e;
    });
    CHECK(reborn != NULL_ENTITY);
    if (reborn != NULL_ENTITY) {
        CHECK(!terrain.blocked(world.get<Transform>(reborn).position, Realm::Overworld));
        CHECK(map != nullptr && inSpawnPoint(*map, "hollow", world.get<Transform>(reborn).position));
    }

    // Drops last: they are scattered off a corpse and pushed back out of
    // whatever they landed in, and on a dense map that push is the only thing
    // between a drop and a wall nobody can reach into.
    int drops = 0;
    int dropsInWall = 0;
    Query<DropTag, Transform> loot{world};
    loot.each([&](Entity, DropTag&, Transform& transform) {
        if (transform.realm != Realm::Overworld) return;
        ++drops;
        if (terrain.blocked(transform.position, Realm::Overworld)) ++dropsInWall;
    });
    CHECK_EQ(dropsInWall, 0);
    (void)drops;   // a pass with no kills in it is legitimate

    flix::testsupport::removeDataDir(dir);
}
