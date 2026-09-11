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

/// The tier bands the map declares over a point, worst first. A spawn that
/// lands in one of these is a spawn into mobs the player cannot fight.
bool inTierAbove(const MapData& map, Vec2 at, Rarity floor) {
    for (const MapElement& element : map.elements()) {
        if (!element.isSpawnBand()) continue;
        if (rarityIndex(element.spawnTier) < rarityIndex(floor)) continue;
        if (element.contains(at)) return true;
    }
    return false;
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

TEST(the_maps_annotation_layers_load) {
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dataDir(), nullptr, error));
    CHECK(error.empty());
    for (const std::string& warning : maps.warnings()) {
        ::testing::reportFailure(__FILE__, __LINE__, "map warning: " + warning);
    }
    // The shipped manifest names the overworld and the sewers, in that order,
    // then the 45 biome maps (nine biomes x five): the order IS the realm
    // numbering. Regenerating the biome maps or adding one to maps.json must
    // update this number.
    CHECK_EQ(maps.count(), 47);
    CHECK(maps.forRealm(Realm::Overworld) != nullptr);
    CHECK(maps.forRealm(worldRealm(1)) != nullptr);
    CHECK(maps.forRealm(Realm::Arena) == nullptr);
    CHECK(maps.forRealm(Realm::Maze) == nullptr);
    bool found = false;
    CHECK(maps.realmOfId("sewers", found) == worldRealm(1));
    CHECK(found);

    const MapData& world = *maps.forRealm(Realm::Overworld);
    CHECK_EQ(world.id(), std::string("world"));
    CHECK_EQ(world.defaultMobGroup(), std::string("garden"));
    CHECK(world.elements().size() > 100);

    int bands = 0;
    int regions = 0;
    int doors = 0;
    int teleporters = 0;
    for (const MapElement& element : world.elements()) {
        if (element.isSpawnBand()) ++bands;
        if (element.isMobRegion()) ++regions;
        if (element.kind == MapElementKind::PlayerSpawn) ++doors;
        if (element.kind == MapElementKind::Teleporter) ++teleporters;
    }
    CHECK(bands > 150);
    // One per section: the seven sectionAt() used to decide, plus the jungle
    // and the unknown corner, which had no mobs of their own before.
    CHECK_EQ(regions, 9);
    // The nine biome doors plus the sewers gate.
    CHECK_EQ(doors, 10);
    // Every teleporter is a POINT: width and height are both 0. A size test
    // that rejects them takes the dots off the minimap and the glow out of the
    // world, and does it without a word of complaint. The eight authored pads
    // plus one entrance per biome into its first sublevel.
    CHECK_EQ(teleporters, 17);
    // And every one of them says where it leads.
    for (const MapElement& element : world.elements()) {
        if (element.kind != MapElementKind::Teleporter) continue;
        CHECK(!element.targetMap.empty());
    }
}

TEST(the_picker_offers_every_player_spawn_rectangle) {
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dataDir(), nullptr, error));

    // Every door on every map is known, each with a label; the PICKABLE ones
    // are offered, and each of those is reachable by the id the picker sends
    // back. A biome's sublevel doors are not pickable: they are reached
    // through pads from the biome's main area, and only door() knows them.
    std::size_t doors = 0;
    std::size_t pickable = 0;
    for (const MapData& map : maps.maps()) {
        for (const MapElement* point : map.playerSpawns()) {
            ++doors;
            if (point->pickable) ++pickable;
        }
    }
    CHECK_EQ(maps.doors().size(), doors);
    CHECK_EQ(maps.spawnChoices().size(), pickable);
    // Nine biome doors and a gate on the world, two in the sewers, and one on
    // each of the 45 generated biome maps.
    CHECK_EQ(doors, std::size_t{57});
    // A biome is spawnable only from its MAIN area, the overworld door that
    // carries its name: the 45 sublevel doors, the two doors of the
    // hand-authored sewers map and the world's sewer gate (the return point
    // from that map, next to the pad that leads in) are all reached through
    // pads, and only the nine biome doors are offered.
    CHECK_EQ(pickable, std::size_t{9});
    for (const SpawnChoice& choice : maps.spawnChoices()) {
        CHECK(!choice.label.empty());
        CHECK(choice.pickable);
        CHECK(maps.choice(choice.id) == &choice);
        CHECK(isWorldRealm(choice.realm));
    }
    for (const SpawnChoice& door : maps.doors()) {
        CHECK(!door.label.empty());
        CHECK(maps.door(door.id) == &door);
        if (!door.pickable) CHECK(maps.choice(door.id) == nullptr);
    }
    // The garden door is first: it is what a player who chose nothing gets.
    // (Guarded: a world map that failed to load has no doors, and that should
    // read as a failed test, not a crashed binary.)
    const MapData* overworld = maps.forRealm(Realm::Overworld);
    CHECK(overworld != nullptr && !overworld->playerSpawns().empty());
    if (overworld != nullptr && !overworld->playerSpawns().empty()) {
        CHECK_EQ(overworld->playerSpawns().front()->spawnId, std::string("garden"));
    }
    // A door on the second map carries that map's realm, and is known to
    // door() but not offered by choice().
    const SpawnChoice* entrance = maps.door("sewers_entrance");
    CHECK(entrance != nullptr);
    if (entrance != nullptr) CHECK(entrance->realm == worldRealm(1));
    CHECK(maps.choice("sewers_entrance") == nullptr);
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

    CHECK(inSpawnPoint(overworld(h), "garden", at));
    CHECK(!inTierAbove(overworld(h), at, Rarity::Rare));
    // Section 0 is the map's top-left, which is where the beginner ground is.
    CHECK_EQ(sectionAt(at), 0);
    CHECK(!h.server.terrain().blocked(at, Realm::Overworld));
    CHECK(h.server.world().get<Transform>(body).realm == Realm::Overworld);
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
    CHECK(!inTierAbove(overworld(h), at, Rarity::Rare));
    CHECK(distance(at, {kWorldHalf, kWorldHalf}) > 5000.0);
}

TEST(a_chosen_spawn_point_is_honoured_and_survives_a_respawn) {
    Harness h("spawn-choice");
    if (!h.ready) { CHECK(false); return; }
    CHECK(h.server.worldMaps().choice("desert") != nullptr);

    NetClient client;
    CHECK(loginNew(h, client, "wanderer", "password7"));
    client.joinGame(1280, 720, "desert");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    World& world = h.server.world();
    Entity body = onlyPlayer(world);
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) return;
    CHECK(inSpawnPoint(overworld(h), "desert", world.get<Transform>(body).position));

    // The choice lives on the session, so dying does not quietly move the
    // player back to the garden.
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
        CHECK(inSpawnPoint(overworld(h), "desert", world.get<Transform>(body).position));
    }
}

TEST(a_teleporter_carries_a_player_to_another_map) {
    Harness h("spawn-teleporter");
    if (!h.ready) { CHECK(false); return; }

    // The pad in the sewer corner is the door to the sewers map.
    const MapElement* pad = nullptr;
    for (const MapElement& element : overworld(h).elements()) {
        if (element.kind == MapElementKind::Teleporter && element.targetMap == "sewers") pad = &element;
    }
    CHECK(pad != nullptr);
    if (pad == nullptr) return;

    NetClient client;
    CHECK(loginNew(h, client, "spelunker", "password7"));
    client.joinGame(1280, 720);
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    World& world = h.server.world();
    const Entity body = onlyPlayer(world);
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) return;

    // Stand on the pad and wait out the dwell. The pad is HELD, not touched:
    // stepping onto it and straight off again must not fire it.
    world.get<Transform>(body).position = pad->centre();
    CHECK(h.stepUntil({&client}, [&] {
        return world.get<Transform>(body).realm != Realm::Overworld;
    }, 120));

    const Transform& at = world.get<Transform>(body);
    bool found = false;
    const Realm sewers = h.server.worldMaps().realmOfId("sewers", found);
    CHECK(found);
    CHECK(at.realm == sewers);
    const MapData* map = h.server.worldMaps().forRealm(sewers);
    CHECK(map != nullptr);
    if (map != nullptr) CHECK(inSpawnPoint(*map, pad->targetSpawn, at.position));
    // The kit came too: nothing of this flower's is left in the overworld.
    Query<Transform, PetalInstance> petals{world};
    petals.each([&](Entity, Transform& petal, PetalInstance& owner) {
        if (owner.owner == body) CHECK(petal.realm == sewers);
    });
    // And the client followed: it was sent the sewers' grid and drew the
    // arrival there.
    CHECK(h.stepUntil({&client}, [&] { return client.view().realm() == sewers; }));
    CHECK_EQ(client.terrain().tileCols(sewers), h.server.terrain().tileCols(sewers));
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
// mobs -- "garden 50% hornet 50%". The tier it spawns at is a separate
// property, and still where the map's difficulty progression lives.

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

TEST(the_shipped_bands_lean_on_the_regions_under_them) {
    // The tier bands say how dangerous their ground is and nothing else: what
    // grows there is the mob REGION under them, one per section, which is
    // exactly what sectionAt() decided before it was written into the map.
    // Six bands straddle a section boundary, and a band that named a group
    // would have to average the two.
    MapData map;
    std::string error;
    CHECK(map.loadWorldMap(dataDir() + "/world.tmj", error));
    int silentBands = 0;
    int namedBands = 0;
    int regions = 0;
    for (const MapElement& element : map.elements()) {
        if (element.isMobRegion()) {
            ++regions;
            // One group, or a weighted mix -- the unknown corner is half
            // computer and half hel.
            CHECK(!element.mobDistribution.empty());
            continue;
        }
        if (!element.isSpawnBand()) continue;
        if (element.mobDistribution.empty()) ++silentBands;
        else ++namedBands;
    }
    CHECK(silentBands > 100);
    // The old biome rooms -- bee fields, ant nests, the DPS row -- became
    // bands that name their mobs outright.
    CHECK(namedBands > 50);
    CHECK_EQ(regions, 9);
}

// ---------------------------------------------------------------------------
// Doors: safe ground, and who may use which
// ---------------------------------------------------------------------------

TEST(every_pickable_door_stands_on_safe_open_ground) {
    // The rule the picker relies on: a door the title screen offers puts a
    // fresh flower down on open ground with nothing above uncommon over it.
    // Checked over every staged map, so a generator regression or a hand edit
    // that slides a rare band over a door does not ship unnoticed.
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
        if (inTierAbove(*map, centre, Rarity::Rare)) {
            ::testing::reportFailure(__FILE__, __LINE__,
                                     "door " + choice.id + " on " + map->id() +
                                         " sits under a rare-or-better band");
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

namespace {

bool copyFile(const std::string& from, const std::string& to) {
    std::ifstream in(from, std::ios::binary);
    if (!in) return false;
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    out << in.rdbuf();
    return out.good();
}

/// A data directory that is the staged one with the sewers' door marked
/// `pickable = false`: a sublevel door, as the biome maps' are. Built rather
/// than taken from the staged maps so the test does not depend on which maps
/// the generator has marked yet.
std::string stageSublevelDataDir() {
    const std::string src = flix::testsupport::dataDir();
    const std::string dir = "/tmp/florr-itest-sublevel-" + std::to_string(::getpid());
    mkdir(dir.c_str(), 0755);
    for (const char* name : {"mobs.json", "petals.json", "mob_xp.json", "mob_drops.json",
                             "terrain.tsj", "ground.tsj", "world.tmj"}) {
        if (!copyFile(src + "/" + name, dir + "/" + name)) return {};
    }
    std::ifstream in(src + "/sewers.tmj", std::ios::binary);
    if (!in) return {};
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Json map;
    std::string error;
    if (!Json::parse(text, map, error)) return {};
    for (Json& layer : map["layers"].items()) {
        if (layer["name"].asString() != "player_spawns") continue;
        for (Json& object : layer["objects"].items()) {
            bool flagged = false;
            if (object.contains("properties")) {
                for (Json& property : object["properties"].items()) {
                    if (property["name"].asString() == "pickable") { property["value"] = false; flagged = true; }
                }
            }
            if (flagged) continue;
            Json flag = Json::object();
            flag["name"] = "pickable";
            flag["type"] = "bool";
            flag["value"] = false;
            if (!object.contains("properties")) object["properties"] = Json::array();
            object["properties"].push(flag);
        }
    }
    std::ofstream out(dir + "/sewers.tmj", std::ios::binary | std::ios::trunc);
    out << map.dump();
    std::ofstream manifest(dir + "/maps.json", std::ios::binary | std::ios::trunc);
    manifest << R"({"maps": [{"file": "world.tmj"}, {"file": "sewers.tmj"}]})";
    return dir;
}

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

/// The loopback harness over a data directory of the test's choosing.
struct DataDirHarness {
    GameServer server;
    std::string dbPath;
    std::uint16_t port = 0;
    bool ready = false;
    double clock = 0;

    DataDirHarness(const std::string& dataDir, const char* dbName) {
        dbPath = flix::testsupport::tempPath(dbName);
        std::remove(dbPath.c_str());
        seedAdminAccount(dbPath);
        ServerConfig config;
        config.dataDir = dataDir;
        config.databasePath = dbPath;
        config.worldSeed = 12345;
        std::string error;
        for (std::uint16_t candidate = 47100; candidate < 47160; ++candidate) {
            config.port = candidate;
            if (server.start(config, error)) { port = candidate; ready = true; break; }
        }
        if (!ready) std::printf("  harness could not start a server: %s\n", error.c_str());
    }
    ~DataDirHarness() { std::remove(dbPath.c_str()); }

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
    template <class F>
    bool stepUntil(std::vector<NetClient*> clients, F done, int maxTicks = 400) {
        for (int i = 0; i < maxTicks; ++i) {
            step(1, clients);
            if (done()) return true;
        }
        return false;
    }
    bool connect(NetClient& client) {
        client.contentHash = content().contentHash();
        if (!client.connect("127.0.0.1", port)) return false;
        return stepUntil({&client}, [&] { return client.status() == NetClient::Status::Ready; });
    }
};

Entity bodyNamed(World& world, const std::string& name) {
    Entity found = NULL_ENTITY;
    Query<PlayerTag, PlayerAccount> players{world};
    players.each([&](Entity e, PlayerTag&, PlayerAccount& account) {
        if (account.username == name) found = e;
    });
    return found;
}

} // namespace

TEST(a_spawn_point_on_another_map_joins_into_that_map) {
    // The sewers' door is not offered, so it takes an admin to name it; what
    // is checked here is what naming a door on ANOTHER MAP does once the
    // server accepts it.
    Harness h("spawn-other-map", seedAdminAccount);
    if (!h.ready) { CHECK(false); return; }
    const SpawnChoice* entrance = h.server.worldMaps().door("sewers_entrance");
    CHECK(entrance != nullptr);
    if (entrance == nullptr) return;

    NetClient client;
    CHECK(flix::testsupport::connectClient(h, client));
    client.requestLogin("boss", "password7");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
    client.joinGame(1280, 720, "sewers_entrance", "boss");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    World& world = h.server.world();
    const Entity body = onlyPlayer(world);
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) return;
    const Transform& at = world.get<Transform>(body);
    // In the sewers' own coordinate space, inside its door, and the client was
    // told so: the grid it drew is the sewers' shape, not the world's.
    CHECK(at.realm == entrance->realm);
    const MapData* sewers = h.server.worldMaps().forRealm(entrance->realm);
    CHECK(sewers != nullptr);
    if (sewers != nullptr) CHECK(inSpawnPoint(*sewers, "sewers_entrance", at.position));
    CHECK(h.stepUntil({&client}, [&] { return client.view().realm() == entrance->realm; }));
    CHECK_EQ(client.terrain().tileCols(entrance->realm), h.server.terrain().tileCols(entrance->realm));
    CHECK_EQ(client.terrain().tileRows(entrance->realm), h.server.terrain().tileRows(entrance->realm));
    CHECK(client.terrain().tileCols(entrance->realm) != kTilesPerAxis);
    CHECK(!h.server.terrain().blocked(at.position, at.realm));
}

TEST(a_sublevel_door_is_joined_only_by_an_admin) {
    const std::string dataDir = stageSublevelDataDir();
    CHECK(!dataDir.empty());
    if (dataDir.empty()) return;
    DataDirHarness h(dataDir, "spawn-sublevel");
    CHECK(h.ready);
    if (!h.ready) return;

    // The door exists and is not offered.
    const WorldMaps& maps = h.server.worldMaps();
    const SpawnChoice* door = maps.door("sewers_entrance");
    CHECK(door != nullptr);
    if (door == nullptr) return;
    CHECK(!door->pickable);
    CHECK(maps.choice("sewers_entrance") == nullptr);
    for (const SpawnChoice& choice : maps.spawnChoices()) CHECK(choice.pickable);

    // A player naming it starts at the default, on the overworld.
    NetClient player;
    CHECK(h.connect(player));
    player.requestRegister("ratcatcher", "password7");
    CHECK(h.stepUntil({&player}, [&] { return player.status() == NetClient::Status::LoggedIn; }));
    player.joinGame(1280, 720, "sewers_entrance", "ratcatcher");
    CHECK(h.stepUntil({&player}, [&] { return player.status() == NetClient::Status::Playing; }));
    World& world = h.server.world();
    const Entity body = bodyNamed(world, "ratcatcher");
    CHECK(body != NULL_ENTITY);
    if (body != NULL_ENTITY) {
        const Transform& at = world.get<Transform>(body);
        CHECK(at.realm == Realm::Overworld);
        const MapData* overworldMap = maps.forRealm(Realm::Overworld);
        CHECK(overworldMap != nullptr && inSpawnPoint(*overworldMap, "garden", at.position));
    }
    CHECK(player.view().realm() == Realm::Overworld);

    // An admin naming it arrives in the sewers, at that door.
    NetClient admin;
    CHECK(h.connect(admin));
    admin.requestLogin("boss", "password7");
    CHECK(h.stepUntil({&admin}, [&] { return admin.status() == NetClient::Status::LoggedIn; }));
    admin.joinGame(1280, 720, "sewers_entrance", "boss");
    CHECK(h.stepUntil({&player, &admin}, [&] { return admin.status() == NetClient::Status::Playing; }));
    const Entity bossBody = bodyNamed(world, "boss");
    CHECK(bossBody != NULL_ENTITY);
    if (bossBody != NULL_ENTITY) {
        const Transform& at = world.get<Transform>(bossBody);
        CHECK(at.realm == door->realm);
        const MapData* sewers = maps.forRealm(door->realm);
        CHECK(sewers != nullptr && inSpawnPoint(*sewers, "sewers_entrance", at.position));
    }
    CHECK(h.stepUntil({&player, &admin}, [&] { return admin.view().realm() == door->realm; }));
}
