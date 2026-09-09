#include "test.h"

#include "server_harness.h"
#include "shared/game/map_elements.h"

using namespace flix;
using flix::testsupport::connectClient;
using flix::testsupport::dataDir;
using flix::testsupport::Harness;
using flix::testsupport::loginNew;

// Where a player appears.
//
// The map says which ground is the beginner's; the middle of the world is the
// legendary and mythic band, which a level-1 flower cannot survive and cannot
// walk out of. These tests exist because spawning there was not an obviously
// wrong line of code -- it was a plausible-looking "start at the centre".

namespace {

/// True when `at` is inside a spawn zone the map marks `common`.
bool inBeginnerGround(const MapData& map, Vec2 at) {
    for (const MapElement& element : map.elements()) {
        if (element.kind != MapElementKind::Spawn || !element.hasSpawnTier) continue;
        if (element.spawnTier != Rarity::Common) continue;
        if (element.bounds.contains(at)) return true;
    }
    return false;
}

/// The tier bands the map declares over a point, worst first. A spawn that
/// lands in one of these is a spawn into mobs the player cannot fight.
bool inTierAbove(const MapData& map, Vec2 at, Rarity floor) {
    for (const MapElement& element : map.elements()) {
        if (element.kind != MapElementKind::Spawn || !element.hasSpawnTier) continue;
        if (rarityIndex(element.spawnTier) < rarityIndex(floor)) continue;
        if (element.bounds.contains(at)) return true;
    }
    return false;
}

Entity onlyPlayer(World& world) {
    Entity found = NULL_ENTITY;
    Query<PlayerTag, Transform> bodies{world};
    bodies.each([&](Entity e, PlayerTag&, Transform&) { found = e; });
    return found;
}

} // namespace

TEST(the_map_bundles_annotation_layer_loads) {
    MapData map;
    std::string error;
    CHECK(map.load(dataDir() + "/map_bundle.ts", error));
    CHECK(error.empty());

    // The declaration reads `MAP_ELEMENTS: MapElement[] = [...]`, and the first
    // '[' in it belongs to the TYPE. Slicing from there yields an empty array
    // that parses perfectly and leaves every spawn falling back to the map
    // centre -- silently. Hence a count, not just a "did it parse".
    CHECK(map.elements().size() > 100);

    int spawns = 0;
    int biomes = 0;
    int teleporters = 0;
    for (const MapElement& element : map.elements()) {
        if (element.kind == MapElementKind::Spawn) ++spawns;
        if (element.kind == MapElementKind::Biome) ++biomes;
        if (element.kind == MapElementKind::Teleporter) ++teleporters;
    }
    CHECK(spawns > 50);
    CHECK(biomes > 20);
    // Every teleporter in the bundle is a POINT: width and height are both 0.
    // A size test that rejects them takes the dots off the minimap and the
    // glow out of the world, and does it without a word of complaint.
    CHECK_EQ(teleporters, 8);
}

TEST(the_picker_offers_every_named_biome_even_a_dangerous_one) {
    MapData map;
    std::string error;
    CHECK(map.load(dataDir() + "/map_bundle.ts", error));

    // The browser's title screen adds every element.type === 'biome' whose
    // name is neither `garden` nor `unnamed_biome`, with no tier test at all --
    // the safety filter belongs to the server's spawn logic, which is the
    // narrower spawnableBiomes() list.
    CHECK(!map.pickableBiomes().empty());
    CHECK(map.pickableBiomes().size() >= map.spawnableBiomes().size());
    for (const std::string& name : map.pickableBiomes()) {
        CHECK(name != "garden");
        CHECK(name != "unnamed_biome");
    }
    for (const std::string& name : map.spawnableBiomes()) {
        CHECK(std::find(map.pickableBiomes().begin(), map.pickableBiomes().end(), name) !=
              map.pickableBiomes().end());
    }
}

TEST(only_biomes_with_a_safe_spawn_table_are_offered) {
    MapData map;
    std::string error;
    CHECK(map.load(dataDir() + "/map_bundle.ts", error));
    CHECK(!map.spawnableBiomes().empty());

    for (const std::string& name : map.spawnableBiomes()) {
        // The editor's names for the default ground and for unnamed rectangles
        // are not destinations.
        CHECK(name != "garden");
        CHECK(name != "unnamed_biome");

        // Every offered biome must have at least one area a player can be put
        // in, or the picker is offering a choice that silently falls back.
        bool anySafe = false;
        for (const MapElement& element : map.elements()) {
            if (element.kind != MapElementKind::Biome || element.biomeName != name) continue;
            if (MapData::safeForSpawn(element)) anySafe = true;
        }
        CHECK(anySafe);
    }
}

TEST(a_biome_with_no_spawn_table_is_never_safe) {
    // A biome that declares no table inherits the world's tiers, which run all
    // the way up. "No table" reads like "no dangerous mobs" and is the opposite.
    MapElement bare;
    bare.kind = MapElementKind::Biome;
    bare.biomeName = "somewhere";
    CHECK(!MapData::safeForSpawn(bare));

    MapElement safe = bare;
    safe.hasSpawnTable = true;
    safe.spawnTable = {BiomeSpawnEntry{Rarity::Common, 1.0, ""},
                       BiomeSpawnEntry{Rarity::Uncommon, 1.0, ""}};
    CHECK(MapData::safeForSpawn(safe));

    MapElement deadly = safe;
    deadly.spawnTable.push_back(BiomeSpawnEntry{Rarity::Mythic, 1.0, ""});
    CHECK(!MapData::safeForSpawn(deadly));
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

    CHECK(inBeginnerGround(h.server.mapData(), at));
    CHECK(!inTierAbove(h.server.mapData(), at, Rarity::Rare));
    // Section 0 is the map's top-left, which is where the beginner ground is.
    CHECK_EQ(sectionAt(at), 0);
    CHECK(!h.server.terrain().blocked(at, Realm::Overworld));
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
    CHECK(inBeginnerGround(h.server.mapData(), at));
    CHECK(!inTierAbove(h.server.mapData(), at, Rarity::Rare));
    CHECK(distance(at, {kWorldHalf, kWorldHalf}) > 5000.0);
}

TEST(a_chosen_biome_is_honoured_and_survives_a_respawn) {
    Harness h("spawn-biome");
    if (!h.ready) { CHECK(false); return; }
    const std::vector<std::string>& offered = h.server.mapData().spawnableBiomes();
    CHECK(!offered.empty());
    if (offered.empty()) return;
    const std::string biome = offered.front();

    NetClient client;
    CHECK(loginNew(h, client, "wanderer", "password7"));
    client.joinGame(1280, 720, biome);
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    World& world = h.server.world();
    const auto insideChosenBiome = [&](Vec2 at) {
        for (const MapElement& element : h.server.mapData().elements()) {
            if (element.kind != MapElementKind::Biome || element.biomeName != biome) continue;
            if (MapData::safeForSpawn(element) && element.bounds.contains(at)) return true;
        }
        return false;
    };

    Entity body = onlyPlayer(world);
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) return;
    CHECK(insideChosenBiome(world.get<Transform>(body).position));

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
    if (body != NULL_ENTITY) CHECK(insideChosenBiome(world.get<Transform>(body).position));
}

TEST(a_biome_the_map_cannot_place_anyone_in_falls_back) {
    Harness h("spawn-unknown");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginNew(h, client, "lost", "password7"));
    // A biome that is not in the map at all. The join must still succeed, on
    // the beginner ground, rather than being refused or landing nowhere.
    client.joinGame(1280, 720, "atlantis");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    const Entity body = onlyPlayer(h.server.world());
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) return;
    CHECK(inBeginnerGround(h.server.mapData(), h.server.world().get<Transform>(body).position));
}
