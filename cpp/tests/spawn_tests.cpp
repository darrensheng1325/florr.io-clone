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
        // The outline, which is what the spawner and the join handler ask. A
        // bounding-box test here would pass a spawn that landed in the corner a
        // polygon zone does not cover -- the exact bug this shape change is
        // meant to make impossible.
        if (element.contains(at)) return true;
    }
    return false;
}

/// The tier bands the map declares over a point, worst first. A spawn that
/// lands in one of these is a spawn into mobs the player cannot fight.
bool inTierAbove(const MapData& map, Vec2 at, Rarity floor) {
    for (const MapElement& element : map.elements()) {
        if (element.kind != MapElementKind::Spawn || !element.hasSpawnTier) continue;
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
            if (MapData::safeForSpawn(element) && element.contains(at)) return true;
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

    // "garden" is one of the nine mob-spawn sections, so it is a preset: this
    // row defers to whatever the Garden's ambient table holds.
    CHECK(rows[0].isPreset());
    CHECK(rows[0].presetSection == 0);
    CHECK(rows[0].mobType.empty());
    CHECK_EQ(rows[0].weight, 50.0);

    // "hornet" is not a section, so it is a mob named outright.
    CHECK(!rows[1].isPreset());
    CHECK_EQ(rows[1].mobType, std::string("hornet"));
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
        CHECK((*rows)[0].presetSection == 3);          // Ocean
        CHECK_EQ((*rows)[1].mobType, std::string("jellyfish"));
        CHECK((*rows)[1].weight > (*rows)[0].weight);
    }

    // A bare name is a zone of nothing but that.
    const std::vector<ZoneMobEntry> only = parseMobDistribution("hornet", nullptr);
    CHECK(only.size() == 1);
    CHECK_EQ(only[0].mobType, std::string("hornet"));
    CHECK_EQ(only[0].weight, 1.0);

    // Underscored section names, because "Ant Hell" is a section.
    const std::vector<ZoneMobEntry> ants = parseMobDistribution("ant_hell 100%", nullptr);
    CHECK(ants.size() == 1);
    CHECK(ants[0].presetSection == 4);
}

TEST(a_broken_distribution_is_reported_not_guessed_at) {
    // Nothing at all: the spawner reads an empty list as "no distribution" and
    // does the ambient roll it always did.
    CHECK(parseMobDistribution("", nullptr).empty());
    CHECK(parseMobDistribution("   ", nullptr).empty());

    // A weight of zero would make its row unreachable, which is a mistake
    // rather than an intention. Skipped, and said out loud -- a mistyped
    // distribution is otherwise a zone that silently keeps its old behaviour.
    std::string warning;
    const std::vector<ZoneMobEntry> rows = parseMobDistribution("hornet 0 bee 3", &warning);
    CHECK(!warning.empty());
    CHECK(rows.size() == 1);
    CHECK_EQ(rows[0].mobType, std::string("bee"));

    // Every section name resolves, and nothing else does.
    CHECK(sectionIndexByName("garden") == 0);
    CHECK(sectionIndexByName("sewers") == 6);
    CHECK(sectionIndexByName("unknown") == 8);
    CHECK(sectionIndexByName("hornet") == -1);
    CHECK(sectionIndexByName("Garden") == -1);   // ids are lower case
}

TEST(the_shipped_zones_keep_the_ambient_roll) {
    // No zone on the map declares a distribution yet, and that is the point of
    // the empty case: 151 zones go on spawning the ambient table of whichever
    // section the mob lands in, exactly as they did. Six of them straddle two
    // sections, and for those "the section the mob landed in" is not a constant
    // -- which is why the default is not written out as a preset.
    MapData map;
    std::string error;
    CHECK(map.loadWorldMap(dataDir() + "/world.tmj", error));
    int zones = 0;
    for (const MapElement& element : map.elements()) {
        if (element.kind != MapElementKind::Spawn) continue;
        ++zones;
        CHECK(element.mobDistribution.empty());
    }
    CHECK(zones > 100);
}
