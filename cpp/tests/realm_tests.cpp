#include "test.h"

#include "client/camera.h"
#include "client/net_client.h"
#include "client/render/world_renderer.h"
#include "server/game_server.h"
#include "server/replication.h"
#include "server/systems/combat.h"
#include "server/systems/mode_spawning.h"
#include "server/systems/spawning.h"
#include "server_harness.h"
#include "shared/game/components.h"
#include "shared/game/realm.h"
#include "shared/game/spatial.h"
#include "shared/game/terrain.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

using namespace flix;

// The three realms are separate coordinate spaces (realm.h). These tests pin
// the two properties everything else rests on -- a body is kept inside its own
// realm's geometry, and nothing in one realm can see or touch another -- and
// then walk a real client into the maze and the arena over loopback.

namespace {

using flix::testsupport::connectClient;
using flix::testsupport::Harness;

std::size_t playersVisibleTo(const NetClient& client) {
    std::size_t n = 0;
    for (const auto& e : client.view().entities()) {
        if (e.second.kind == net::EntityKind::Player) ++n;
    }
    return n;
}

Entity bodyOf(World& world, const std::string& name) {
    Entity found = NULL_ENTITY;
    Query<PlayerTag, PlayerAccount> players{world};
    players.each([&](Entity e, PlayerTag&, PlayerAccount& account) {
        if (account.username == name) found = e;
    });
    return found;
}

/// Logs a fresh account in and joins with a spawn choice.
bool joinAs(Harness& h, NetClient& client, const char* name, const std::string& choice) {
    if (!connectClient(h, client)) return false;
    client.requestRegister(name, "password9");
    if (!h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; })) {
        return false;
    }
    client.joinGame(1280, 720, choice, name);
    if (!h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; })) {
        return false;
    }
    return h.stepUntil({&client}, [&] { return client.view().self().netId != 0; });
}

} // namespace

// ---------------------------------------------------------------------------
// Terrain answers per realm
// ---------------------------------------------------------------------------

TEST(the_arena_is_a_ring_and_the_terrain_keeps_bodies_inside_it) {
    Terrain terrain;   // ungenerated: the overworld is all ground here
    // The centre is open; far outside the ring is wall, whatever the tile grid
    // would say about the same numbers in the overworld.
    CHECK(!terrain.blocked(kArenaCentre, Realm::Arena));
    CHECK(terrain.blocked({kArenaCentre.x + kArenaRadius + 1.0, kArenaCentre.y}, Realm::Arena));
    CHECK(!terrain.blocked({kArenaCentre.x + kArenaRadius + 1.0, kArenaCentre.y}, Realm::Overworld));

    // The clamp is radial and leaves a body of `radius` tangent to the edge.
    const Vec2 outside{kArenaCentre.x + kArenaRadius + 900.0, kArenaCentre.y + 50.0};
    const Vec2 held = terrain.clampInside(outside, 20.0, Realm::Arena);
    CHECK_NEAR(distance(held, kArenaCentre), kArenaRadius - 20.0, 1e-9);
    // resolveCircle does the same, and reports the contact.
    const Terrain::WallResolution wall = terrain.resolveWall(outside, 20.0, Realm::Arena);
    CHECK(wall.collided);
    CHECK_NEAR(distance(wall.position, kArenaCentre), kArenaRadius - 20.0, 1e-9);
    CHECK(!terrain.resolveWall(kArenaCentre, 20.0, Realm::Arena).collided);

    // Sight is never blocked on open floor; the ring is not a wall to see past.
    CHECK(!terrain.segmentBlocked(kArenaCentre, kArenaSpawn, Realm::Arena));
    CHECK(terrain.hasLineOfSight(kArenaCentre, kArenaSpawn, Realm::Arena));
    CHECK(!terrain.outside(kArenaSpawn, Realm::Arena));
    CHECK(terrain.outside(outside, Realm::Arena));
}

TEST(the_maze_lives_at_its_own_origin_and_its_walls_answer_for_it) {
    Terrain terrain;
    setActiveMazeDay(3);   // garden
    const Maze& maze = activeMaze();
    CHECK_NEAR(kMazeOriginX, 0.0, 1e-12);
    CHECK_NEAR(kMazeOriginY, 0.0, 1e-12);
    CHECK(maze.contains(maze.spawn()));
    CHECK(maze.isFloor(maze.spawn()));

    // The entrance is floor in the maze realm. The very same numbers in the
    // overworld are answered by the tile grid (all ground on an ungenerated
    // map), and in the arena by the ring.
    CHECK(!terrain.blocked(maze.spawn(), Realm::Maze));
    CHECK(!terrain.blocked(maze.spawn(), Realm::Overworld));

    // Some cell is void; a point in it is wall to the maze and nothing else.
    Vec2 voidPoint;
    bool foundVoid = false;
    for (int gy = 0; gy < maze.gridDim() && !foundVoid; ++gy) {
        for (int gx = 0; gx < maze.gridDim() && !foundVoid; ++gx) {
            if (maze.cellValue(gx, gy) != 0) continue;
            voidPoint = {(gx + 0.5) * kMazeCellSize, (gy + 0.5) * kMazeCellSize};
            foundVoid = true;
        }
    }
    CHECK(foundVoid);
    if (foundVoid) {
        CHECK(terrain.blocked(voidPoint, Realm::Maze));
        CHECK(!terrain.blocked(voidPoint, Realm::Overworld));
        // A body placed in the void is pushed onto floor, inside the square.
        const Vec2 pushed = terrain.resolveCircle(voidPoint, 20.0, Realm::Maze);
        CHECK(!maze.blocksPoint(pushed));
        CHECK(!terrain.outside(pushed, Realm::Maze));
    }

    // findOpenSpawn in the maze hands back floor, near where it was asked.
    Rng rng(77);
    for (int i = 0; i < 20; ++i) {
        const Vec2 at = terrain.findOpenSpawn(rng, maze.spawn(), kMazeCellSize * 0.3, Realm::Maze);
        CHECK(maze.isFloor(at));
        CHECK(distance(at, maze.spawn()) < kMazeCellSize);
    }

    // The realm's square is what a body is clamped to, and the layer size the
    // broadphase is built from.
    CHECK_NEAR(terrain.realmSize(Realm::Maze), maze.worldSize(), 1e-9);
    const Vec2 far{maze.worldSize() + 5000.0, -3000.0};
    const Vec2 held = terrain.clampInside(far, 20.0, Realm::Maze);
    CHECK(!terrain.outside(held, Realm::Maze));
}

// ---------------------------------------------------------------------------
// Nothing crosses a realm
// ---------------------------------------------------------------------------

TEST(replication_streams_a_viewer_only_its_own_realm) {
    World world;
    NetIdAllocator ids;
    Replicator replicator;
    ClientView view;
    EventQueue events;

    const auto addBody = [&](Realm realm, bool player) {
        const Entity e = world.create();
        if (player) world.add<PlayerTag>(e);
        else world.add<MobTag>(e);
        world.add<Transform>(e, Transform{{1000, 1000}, 0.0, realm});
        world.add<Body>(e, Body{20, 1});
        world.add<Health>(e, Health{50, 100, 0, 0});
        if (player) {
            world.add<PlayerInput>(e);
            world.add<PlayerLocation>(e);
        }
        world.add<NetId>(e, NetId{ids.next()});
        Replicated rep;
        rep.kind = player ? net::EntityKind::Player : net::EntityKind::Mob;
        world.add<Replicated>(e, rep);
        return e;
    };

    const Entity viewer = addBody(Realm::Maze, true);
    const Entity mazeMob = addBody(Realm::Maze, false);
    const Entity overworldMob = addBody(Realm::Overworld, false);
    const Entity arenaMob = addBody(Realm::Arena, false);
    // A damage number in each realm, all at the viewer's own coordinates.
    events.damage(world.get<NetId>(mazeMob).value, 5, {1000, 1000}, Realm::Maze);
    events.damage(world.get<NetId>(overworldMob).value, 5, {1000, 1000}, Realm::Overworld);
    events.damage(world.get<NetId>(arenaMob).value, 5, {1000, 1000}, Realm::Arena);

    ByteWriter out;
    Replicator::Frame frame;
    frame.tick = 1;
    frame.nowMillis = 1000.0;
    frame.events = &events;
    replicator.build(world, viewer, view, frame, out);

    WorldView client;
    ByteReader reader(out.data(), out.size());
    CHECK_EQ(reader.u8(), static_cast<std::uint8_t>(net::ServerMessage::Snapshot));
    CHECK(client.applySnapshot(reader));

    // The viewer and the maze mob, and nothing standing on the same numbers in
    // another space.
    CHECK_EQ(client.entities().size(), std::size_t(2));
    CHECK_EQ(client.entities().count(world.get<NetId>(mazeMob).value), std::size_t(1));
    CHECK_EQ(client.entities().count(world.get<NetId>(overworldMob).value), std::size_t(0));
    CHECK_EQ(client.entities().count(world.get<NetId>(arenaMob).value), std::size_t(0));
    CHECK_EQ(client.events().size(), std::size_t(1));
    if (!client.events().empty()) {
        CHECK_EQ(client.events().front().netId, world.get<NetId>(mazeMob).value);
    }
}

TEST(the_mode_spawner_fills_only_the_realms_someone_is_in) {
    World world;
    CommandBuffer commands{world};
    Terrain terrain;
    SpawnSystem spawner;
    ModeSpawner modes;
    SpatialGrid grid;
    Rng rng(0xBEEF);
    setActiveMazeDay(3);

    const auto rebuildGrid = [&] {
        grid.clear();
        Query<Transform, Body> bodies{world};
        bodies.each([&](Entity e, Transform& t, Body& b) { grid.insert(e, t.realm, t.position, b.radius); });
    };
    const auto countIn = [&](Realm realm) {
        int n = 0;
        Query<MobTag, Transform> mobs{world};
        mobs.each([&](Entity, MobTag&, Transform& t) { if (t.realm == realm) ++n; });
        return n;
    };

    // Nobody anywhere: nothing appears, however long the clock runs.
    double now = 0;
    for (int pass = 0; pass < 6; ++pass) {
        now += kModeSpawnIntervalMillis;
        modes.run(world, terrain, content(), spawner, grid, {}, rng, now);
        commands.flush();
    }
    CHECK_EQ(countIn(Realm::Arena), 0);
    CHECK_EQ(countIn(Realm::Maze), 0);

    // One flower in the ring: the arena fills towards twelve, every mob inside
    // the ring and in the arena realm, none in the overworld or the maze.
    const std::vector<RealmPoint> duellist{{kArenaSpawn, Realm::Arena}};
    for (int pass = 0; pass < 12; ++pass) {
        now += kModeSpawnIntervalMillis;
        rebuildGrid();
        modes.run(world, terrain, content(), spawner, grid, duellist, rng, now);
        commands.flush();
    }
    const int arenaMobs = countIn(Realm::Arena);
    CHECK(arenaMobs > 0);
    CHECK(arenaMobs <= kArenaMobsPerPlayer);
    CHECK_EQ(countIn(Realm::Overworld), 0);
    CHECK_EQ(countIn(Realm::Maze), 0);
    Query<MobTag, Transform, Body> arenaBodies{world};
    arenaBodies.each([&](Entity, MobTag&, Transform& t, Body& body) {
        if (t.realm != Realm::Arena) return;
        CHECK(distance(t.position, kArenaCentre) + body.radius <= kArenaRadius + 1e-6);
        CHECK(distance(t.position, kArenaSpawn) >= kArenaSpawnPlayerGap - 1e-6);
    });

    // One flower at the maze entrance: corridors fill, every mob on floor, in
    // the maze realm, and never within a viewport of the flower.
    const Maze& maze = activeMaze();
    const std::vector<RealmPoint> explorer{{maze.spawn(), Realm::Maze}};
    for (int pass = 0; pass < 8; ++pass) {
        now += kModeSpawnIntervalMillis;
        rebuildGrid();
        modes.run(world, terrain, content(), spawner, grid, explorer, rng, now);
        commands.flush();
    }
    CHECK(countIn(Realm::Maze) > 0);
    CHECK(countIn(Realm::Maze) <= ModeSpawner::mazePopulationTarget());
    CHECK_EQ(countIn(Realm::Overworld), 0);
    Query<MobTag, Transform, MobType> mazeBodies{world};
    int bosses = 0;
    mazeBodies.each([&](Entity e, MobTag&, Transform& t, MobType& type) {
        if (t.realm != Realm::Maze) return;
        CHECK(!maze.blocksPoint(t.position));
        const bool boss = rarityIndex(type.rarity) >= rarityIndex(Rarity::Ultra);
        if (boss && !(world.has<BodySegment>(e) && !world.get<BodySegment>(e).head)) ++bosses;
        if (!boss) CHECK(distance(t.position, maze.spawn()) >= kMazeSpawnPlayerGap - 1e-6);
    });
    // The two boss rooms are stocked, and with nothing above what they hold.
    CHECK_EQ(bosses, std::min<int>(kMazeBossCount, static_cast<int>(maze.bossSpots().size())));

    // Rotation clears the maze outright, and the arena keeps its crowd.
    modes.clearMaze(world, commands);
    commands.flush();
    CHECK_EQ(countIn(Realm::Maze), 0);
    CHECK_EQ(countIn(Realm::Arena), arenaMobs);
}

// ---------------------------------------------------------------------------
// Over the wire
// ---------------------------------------------------------------------------

TEST(a_client_that_picks_the_maze_arrives_in_it_and_cannot_walk_through_its_walls) {
    Harness h("maze-join");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(joinAs(h, client, "theseus", kMazeSpawnChoice));
    CHECK(client.view().realm() == Realm::Maze);

    World& world = h.server.world();
    const Entity body = bodyOf(world, "theseus");
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) return;
    const Maze& maze = activeMaze();
    CHECK(world.get<Transform>(body).realm == Realm::Maze);
    CHECK(maze.isFloor(world.get<Transform>(body).position));
    CHECK(distance(world.get<Transform>(body).position, maze.spawn()) < kMazeCellSize);
    // Not hostile to other flowers: only the ring is.
    CHECK(!world.get<Faction>(body).friendlyFireEnabled);

    // Run into the walls in every direction. The body may never end up inside
    // one, and it may never leave the maze's square: the containment that
    // used to be a world clamp 140000 units away is the realm's own now.
    net::InputFrame input;
    input.moveStrength = 1.0;
    std::uint32_t sequence = 0;
    for (int dir = 0; dir < 4; ++dir) {
        input.moveAngle = dir * kPi * 0.5;
        for (int i = 0; i < 45; ++i) {
            input.sequence = ++sequence;
            client.sendInput(input);
            h.step(1, {&client});
            const Vec2 at = world.get<Transform>(body).position;
            CHECK(!maze.blocksPoint(at));
            CHECK(!h.server.terrain().outside(at, Realm::Maze));
        }
    }

    // The corridors get populated while someone is inside: the WHOLE maze,
    // not a bubble around the flower. Population spread is what says so --
    // a fill that only served the viewport could not put mobs on the far side
    // of the layout -- and every one of them stands on floor, in the maze.
    // (The spawn stand-off is a placement rule, not an invariant: a mob that
    // has since walked over to the flower is a mob doing its job. The
    // ModeSpawner test above measures the stand-off where it applies.)
    Query<MobTag, Transform> mobs{world};
    const auto countMazeMobs = [&] {
        int n = 0;
        mobs.each([&](Entity, MobTag&, Transform& t) { if (t.realm == Realm::Maze) ++n; });
        return n;
    };
    CHECK(h.stepUntil({&client}, [&] { return countMazeMobs() > 100; }, 900));
    const Vec2 me = world.get<Transform>(body).position;
    Entity nearest = NULL_ENTITY;
    double nearestDist = 1e18;
    double farthestDist = 0;
    mobs.each([&](Entity e, MobTag&, Transform& t) {
        if (t.realm != Realm::Maze) return;
        CHECK(!maze.blocksPoint(t.position));
        const double d = distance(t.position, me);
        if (d < nearestDist) { nearestDist = d; nearest = e; }
        farthestDist = std::max(farthestDist, d);
    });
    CHECK(farthestDist > maze.worldSize() * 0.4);

    // Brought next to the flower, a maze mob is streamed to the client: it is
    // in the client's realm. An overworld mob at the same numbers never is --
    // that is what the two-realm test below pins.
    CHECK(nearest != NULL_ENTITY);
    if (nearest != NULL_ENTITY) {
        world.get<Transform>(nearest).position =
            h.server.terrain().findOpenSpawn(h.probeRng, me, 200.0, Realm::Maze);
        const std::uint32_t nearestNetId = world.get<NetId>(nearest).value;
        CHECK(h.stepUntil({&client}, [&] {
            return client.view().entities().count(nearestNetId) == 1;
        }));
    }
}

TEST(a_client_that_picks_pvp_fights_in_the_ring_with_the_arena_kit) {
    Harness h("arena-join");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(joinAs(h, client, "gladiator", kArenaSpawnChoice));
    CHECK(client.view().realm() == Realm::Arena);

    World& world = h.server.world();
    const Entity body = bodyOf(world, "gladiator");
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) return;
    CHECK(world.get<Transform>(body).realm == Realm::Arena);
    CHECK_NEAR(world.get<Transform>(body).position.x, kArenaSpawn.x, 1e-6);
    CHECK_NEAR(world.get<Transform>(body).position.y, kArenaSpawn.y, 1e-6);
    // The ring's rules: players hurt each other, health is a flat pool, and
    // the kit is the starter ring with an empty bag whatever the account owns.
    CHECK(world.get<Faction>(body).friendlyFireEnabled);
    CHECK_NEAR(world.get<Health>(body).max, kArenaMaxHealth, 1e-9);
    CHECK(world.has<ArenaScore>(body));
    CHECK(h.stepUntil({&client}, [&] { return client.profile().loadout.size() == kLoadoutSlots; }));
    const std::uint16_t basic = content().petalIndex("basic");
    int basics = 0;
    for (std::size_t i = 0; i < client.profile().loadout.size() && i < kLoadoutActiveSlots; ++i) {
        if (!client.profile().loadout[i].empty() && client.profile().loadout[i].petalIndex == basic) {
            ++basics;
        }
    }
    CHECK_EQ(basics, 5);
    CHECK(client.profile().inventory.empty());

    // Running east from the spawn meets the ring inside two seconds. The body
    // is held on its inside face and never leaves the arena's space.
    net::InputFrame input;
    input.moveStrength = 1.0;
    input.moveAngle = 0.0;
    for (int i = 0; i < 150; ++i) {
        input.sequence = static_cast<std::uint32_t>(i + 1);
        client.sendInput(input);
        h.step(1, {&client});
        const Vec2 at = world.get<Transform>(body).position;
        CHECK(world.get<Transform>(body).realm == Realm::Arena);
        // The ring holds the flower's own MOVEMENT absolutely: stepCollide
        // clamps every step into the disc. A mob walking into it is the one
        // thing that can carry it past the line, because the contact shove is
        // an immediate 25-unit displacement with no wall resolve behind it
        // (combat.h, applyMobContactKnockback) -- deliberately, and the next
        // movement step pulls it back. So the bound is the ring plus that one
        // shove, and it is a bound on how far OUT a body can be carried rather
        // than on whether the arena leaks.
        CHECK(distance(at, kArenaCentre) <= kArenaRadius + kMobContactKnockback + 1e-6);
    }
    CHECK(distance(world.get<Transform>(body).position, kArenaCentre) >
          kArenaRadius - world.get<Body>(body).radius - 1.0);

    // Leaving the ring ends the run: the account's own loadout is back and
    // the arena kit is gone with the body.
    client.leaveGame();
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
    CHECK(bodyOf(world, "gladiator") == NULL_ENTITY);
}

TEST(a_maze_run_is_played_one_rarity_down_on_the_mazes_own_track) {
    // The maze plays the account's ring a tier lower, locks it for the run,
    // and keeps its XP and talents on a track of their own -- so a maze run
    // can neither inflate nor spend the outside level.
    Harness h("maze-track", [](const std::string& path) {
        // An account with a rare ring, a super in an orbiting slot, and a
        // super in the storage row behind it. Shifted down a tier the rare
        // becomes uncommon; the orbiting super would become an ultra, which is
        // over the maze's mythic ceiling, so it is benched -- while the one in
        // storage shifts and stays, because storage orbits nothing.
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << R"({"users":{"weaver":{"id":"u-weaver","username":"weaver",)"
            << R"("password":"mazepass9"}},)"
            << R"("players":{"u-weaver":{"totalXP":1000000,"mazeTotalXP":0,)"
            << R"("inventory":{"common":{"petal_basic":5}},"loadout":[)"
            << R"({"type":"petal","rarity":"rare","petalType":"basic"},)"
            << R"({"type":"petal","rarity":"super","petalType":"basic"},)"
            << R"(null,null,null,null,null,null,null,null,)"
            << R"({"type":"petal","rarity":"super","petalType":"basic"}]}}})";
    });
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(connectClient(h, client));
    client.requestLogin("weaver", "mazepass9");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
    const int outsideLevel = client.profile().level;
    CHECK(outsideLevel > 1);

    client.joinGame(1280, 720, kMazeSpawnChoice, "weaver");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));
    CHECK(h.stepUntil({&client}, [&] { return client.view().self().netId != 0; }));

    World& world = h.server.world();
    const Entity body = bodyOf(world, "weaver");
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) return;
    CHECK(world.get<Transform>(body).realm == Realm::Maze);

    // The body's ring: rare down to uncommon, the orbiting super benched, and
    // the storage row's super shifted to ultra but never benched.
    const Loadout& worn = world.get<Loadout>(body);
    CHECK(!worn.slots[0].empty());
    CHECK(worn.slots[0].rarity == Rarity::Uncommon);
    CHECK(worn.slots[1].empty());
    CHECK(!worn.slots[kLoadoutActiveSlots].empty());
    CHECK(worn.slots[kLoadoutActiveSlots].rarity == Rarity::Ultra);

    // And the bar the client draws says the same thing, or it would advertise
    // a rarity the flower is not swinging.
    CHECK(h.stepUntil({&client}, [&] { return client.profile().loadout.size() == kLoadoutSlots; }));
    CHECK(client.profile().loadout[0].rarity == Rarity::Uncommon);
    CHECK(client.profile().loadout[1].empty());

    // The maze track starts at level one however high the outside one is, and
    // the loadout is locked for the run.
    CHECK_EQ(client.profile().level, 1);
    CHECK_EQ(world.get<PlayerProgress>(body).level, 1);
    client.setLoadoutSlot(0, kNoPetal, Rarity::Common);
    h.step(10, {&client});
    CHECK(!world.get<Loadout>(body).slots[0].empty());
    const PlayerRecord* account = h.server.database().findProgress("u-weaver");
    CHECK(account != nullptr);
    if (account != nullptr) {
        CHECK(account->loadout[0].has_value());
        CHECK(account->loadout[0]->rarity == Rarity::Rare);   // untouched by the run
    }

    // XP earned inside lands on the maze track, never on the outside total.
    world.get<PlayerProgress>(body).totalXp += 5000;
    client.leaveGame();
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
    const PlayerRecord* saved = h.server.database().findProgress("u-weaver");
    CHECK(saved != nullptr);
    if (saved != nullptr) {
        CHECK_NEAR(saved->totalXp, 1000000.0, 1e-6);
        CHECK_NEAR(saved->mazeTotalXp, 5000.0, 1e-6);
    }
    // Back on the title screen the account's own ring is what is shown again.
    CHECK(h.stepUntil({&client}, [&] {
        return !client.profile().loadout.empty() &&
               client.profile().loadout[0].rarity == Rarity::Rare;
    }));
    CHECK_EQ(client.profile().level, outsideLevel);
}

TEST(an_arena_run_never_touches_the_account_it_is_played_from) {
    // The run is a scratch copy of the account: what is looted, crafted or
    // lost in the ring belongs to the run, and a quarter of what survives it
    // reaches the account on the way out. Nothing else does.
    Harness h("arena-account");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(joinAs(h, client, "duellist", kArenaSpawnChoice));
    World& world = h.server.world();
    const Entity body = bodyOf(world, "duellist");
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) return;

    // The account keeps the starter kit it registered with while the ring
    // hands out its own; the client is shown the ring's.
    const std::string userId = world.get<PlayerAccount>(body).userId;
    const std::uint16_t basic = content().petalIndex("basic");
    const std::uint16_t rose = content().petalIndex("rose");
    CHECK(basic != kInvalidIndex);
    CHECK(rose != kInvalidIndex);
    const auto accountItems = [&](std::uint16_t petalIndex) {
        const PlayerRecord* record = h.server.database().findProgress(userId);
        return record != nullptr
                   ? record->itemCount(Rarity::Common, "petal_" + content().petal(petalIndex).id)
                   : -1;
    };
    const int accountBasics = accountItems(basic);
    CHECK(accountBasics > 0);
    CHECK(h.stepUntil({&client}, [&] { return !client.profile().loadout.empty(); }));
    CHECK_EQ(client.profile().stackCount(basic, Rarity::Common), std::uint32_t(0));

    // Twelve roses looted in the ring -- through the real pickup path, which
    // is what banks them -- reach the run's bag and never the account's.
    for (int i = 0; i < 12; ++i) {
        const Vec2 at = world.get<Transform>(body).position;
        const Entity drop = world.create();
        world.add<DropTag>(drop);
        world.add<Transform>(drop, Transform{at, 0.0, Realm::Arena});
        world.add<Body>(drop, Body{10.0, 1.0});
        DropItem item;
        item.configIndex = rose;
        item.rarity = Rarity::Common;
        world.add<DropItem>(drop, std::move(item));
        world.add<Lifetime>(drop, Lifetime{30.0});
        world.add<Replicated>(drop, Replicated{net::EntityKind::Drop, 0, rose, Rarity::Common, 0});
        h.step(2, {&client});
    }
    CHECK(h.stepUntil({&client}, [&] {
        return client.profile().stackCount(rose, Rarity::Common) == 12;
    }));
    CHECK_EQ(accountItems(rose), 0);

    // Out through the door: a quarter of the run's twelve, and the account's
    // own kit back on the client.
    client.leaveGame();
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
    CHECK_EQ(accountItems(rose), 3);
    CHECK(h.stepUntil({&client}, [&] {
        return client.profile().stackCount(basic, Rarity::Common) ==
               static_cast<std::uint32_t>(accountBasics);
    }));
    CHECK_EQ(client.profile().stackCount(rose, Rarity::Common), std::uint32_t(3));
}

TEST(a_maze_flower_and_an_overworld_flower_never_see_each_other) {
    // No bots: the world is one map with one door on it, so the bot
    // population stands exactly where a joining player is put down and "each
    // sees exactly one flower" could not otherwise be said at all.
    Harness h("two-realms", {}, flix::testsupport::dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient outside;
    NetClient inside;
    CHECK(joinAs(h, outside, "gardener", ""));
    CHECK(joinAs(h, inside, "minotaur", kMazeSpawnChoice));
    h.step(30, {&outside, &inside});

    // The two share numbers -- both are near their own origins -- but not a
    // space. Each sees exactly one flower: itself.
    CHECK(outside.view().realm() == Realm::Overworld);
    CHECK(inside.view().realm() == Realm::Maze);
    CHECK_EQ(playersVisibleTo(outside), std::size_t(1));
    CHECK_EQ(playersVisibleTo(inside), std::size_t(1));
    CHECK_EQ(outside.view().entities().count(inside.view().self().netId), std::size_t(0));
    CHECK_EQ(inside.view().entities().count(outside.view().self().netId), std::size_t(0));

    // Respawning keeps the choice: a flower that died in the maze comes back
    // to its entrance, still in the maze.
    World& world = h.server.world();
    const Entity body = bodyOf(world, "minotaur");
    CHECK(body != NULL_ENTITY);
    if (body != NULL_ENTITY) {
        world.get<Health>(body).current = 0;
        world.add<Dead>(body, Dead{NULL_ENTITY});
        CHECK(h.stepUntil({&outside, &inside}, [&] { return inside.dead(); }));
        inside.requestRespawn();
        CHECK(h.stepUntil({&outside, &inside}, [&] {
            const Entity reborn = bodyOf(world, "minotaur");
            return reborn != NULL_ENTITY && reborn != body && world.has<Transform>(reborn);
        }));
        const Entity reborn = bodyOf(world, "minotaur");
        CHECK(reborn != NULL_ENTITY);
        if (reborn != NULL_ENTITY) {
            CHECK(world.get<Transform>(reborn).realm == Realm::Maze);
            CHECK(activeMaze().isFloor(world.get<Transform>(reborn).position));
        }
        CHECK(inside.view().realm() == Realm::Maze);
    }
}

// ---------------------------------------------------------------------------
// What is drawn under each realm
// ---------------------------------------------------------------------------

namespace {

/// The fraction of a rendered frame that is darker than `threshold` on every
/// channel. Used for the two places a realm paints something genuinely dark:
/// the void beyond a realm's bounds, which is the black the frame is cleared
/// to, and the arena's surround.
double darkFraction(Canvas& canvas, int threshold) {
    const std::vector<std::uint8_t> pixels =
        canvas.getImageData(0, 0, canvas.width(), canvas.height());
    if (pixels.size() < 4) return 0.0;
    std::size_t dark = 0;
    const std::size_t count = pixels.size() / 4;
    for (std::size_t i = 0; i < count; ++i) {
        if (pixels[i * 4] < threshold && pixels[i * 4 + 1] < threshold &&
            pixels[i * 4 + 2] < threshold) {
            ++dark;
        }
    }
    return static_cast<double>(dark) / static_cast<double>(count);
}

/// Mean brightness of a frame, 0..255. A maze wall is a 20% black WASH over
/// the biome ground rather than a dark colour of its own, so no threshold
/// separates it from the corridor beside it -- what does is that the wash is
/// four fifths as bright as what it covers.
double meanBrightness(Canvas& canvas) {
    const std::vector<std::uint8_t> pixels =
        canvas.getImageData(0, 0, canvas.width(), canvas.height());
    if (pixels.size() < 4) return 0.0;
    const std::size_t count = pixels.size() / 4;
    double total = 0;
    for (std::size_t i = 0; i < count; ++i) {
        total += (pixels[i * 4] + pixels[i * 4 + 1] + pixels[i * 4 + 2]) / 3.0;
    }
    return total / static_cast<double>(count);
}

/// Renders one frame of `realm` with the camera centred on `at` and nothing in
/// the world. No content and no sprites: the ground falls back to the biome's
/// flat colour, which is all these assertions need.
///
/// The frame is small by default because most of these questions are about
/// what a whole screenful comes out like; the size widens for the ones that
/// are about what a real 1920-wide window sees of a 1000-unit maze cell.
Canvas renderRealm(Realm realm, Vec2 at, double zoom = 1.0, int width = 320, int height = 180) {
    Canvas canvas = Canvas::createVirtual(width, height);
    Camera camera;
    camera.setViewport(width, height);
    camera.userZoom = zoom;
    camera.snapTo(at);
    WorldRenderer renderer;
    WorldView view;
    view.setRealm(realm);
    renderer.draw(canvas, view, camera, at, 0.0);
    return canvas;
}

double renderDark(Realm realm, Vec2 at, int threshold, double zoom = 1.0, int width = 320,
                  int height = 180) {
    Canvas canvas = renderRealm(realm, at, zoom, width, height);
    return darkFraction(canvas, threshold);
}

double renderBrightness(Realm realm, Vec2 at, double zoom = 1.0, int width = 320,
                        int height = 180) {
    Canvas canvas = renderRealm(realm, at, zoom, width, height);
    return meanBrightness(canvas);
}

/// A cell of `value` well inside the maze, so a frame centred on it is not
/// half filled with the black beyond the square. Returns false when the
/// layout has none, which no shipped one does.
bool findCell(const Maze& maze, std::uint8_t value, int margin, Vec2& out) {
    for (int gy = margin; gy < maze.gridDim() - margin; ++gy) {
        for (int gx = margin; gx < maze.gridDim() - margin; ++gx) {
            if (maze.cellValue(gx, gy) != value) continue;
            out = {(gx + 0.5) * kMazeCellSize, (gy + 0.5) * kMazeCellSize};
            return true;
        }
    }
    return false;
}

} // namespace

TEST(each_realm_draws_its_own_ground) {
    setActiveMazeDay(3);
    const Maze& maze = activeMaze();

    // A maze wall is a wash over the biome ground, so a viewpoint standing in
    // solid void comes out dimmer than one standing in a corridor -- by about
    // the fifth the wash takes off. Both points are taken from well inside the
    // layout, or the black beyond the square would be what was measured.
    Vec2 voidPoint;
    Vec2 floorPoint;
    CHECK(findCell(maze, 0, 6, voidPoint));
    CHECK(findCell(maze, 1, 6, floorPoint));
    const double wallLit = renderBrightness(Realm::Maze, voidPoint);
    const double corridorLit = renderBrightness(Realm::Maze, floorPoint);
    CHECK(corridorLit > 0.0);
    CHECK(wallLit < corridorLit);
    CHECK_NEAR(wallLit / corridorLit, 0.8, 0.05);

    // And at the client's own size, standing on a floor cell whose neighbour
    // is void: a player walking the corridors has to SEE the wall beside them,
    // not just collide with it. A 1920-wide window covers nearly two cells, so
    // both the corridor and the wall half a cell away are on screen.
    Vec2 besideWall;
    bool foundEdge = false;
    for (int gy = 6; gy < maze.gridDim() - 6 && !foundEdge; ++gy) {
        for (int gx = 6; gx < maze.gridDim() - 6 && !foundEdge; ++gx) {
            if (maze.cellValue(gx, gy) != 1) continue;
            if (maze.cellValue(gx + 1, gy) != 0 && maze.cellValue(gx, gy + 1) != 0) continue;
            besideWall = {(gx + 0.5) * kMazeCellSize, (gy + 0.5) * kMazeCellSize};
            foundEdge = true;
        }
    }
    CHECK(foundEdge);
    const double edgeLit = renderBrightness(Realm::Maze, besideWall, 1.0, 1920, 1080);
    const double openLit = renderBrightness(Realm::Maze, floorPoint, 1.0, 1920, 1080);
    CHECK(edgeLit < openLit);          // some of the frame is wall
    CHECK(edgeLit > openLit * 0.85);   // and most of it is still corridor

    // Outside the maze square there is no ground at all: the frame stays the
    // black it was cleared to.
    const double outsideDark =
        renderDark(Realm::Maze, {maze.worldSize() + 50000.0, maze.worldSize() + 50000.0}, 16);
    CHECK(outsideDark > 0.99);

    // The arena: the ring's floor is grey, and beyond the ring is the near
    // black void. The centre is all floor, and a point far outside is not.
    const double arenaFloorDark = renderDark(Realm::Arena, kArenaCentre, 40);
    const double arenaVoidDark =
        renderDark(Realm::Arena, {kArenaCentre.x + kArenaRadius * 4.0, kArenaCentre.y}, 40);
    CHECK(arenaFloorDark < 0.01);
    CHECK(arenaVoidDark > 0.99);

    // The overworld, with NO map installed, is void everywhere.
    //
    // This is the change: the overworld's ground used to be a colour the
    // engine chose from the section you stood in, so a renderer with no map at
    // all still painted grass. It is the MAP's own tile layers now -- the art
    // the author painted, read out of the staged map file -- and a renderer
    // holding neither a map nor a terrain has nothing to draw. The maze and
    // the arena are unaffected above because they are geometry rather than
    // tiles, and they still answer for their own floor.
    const double noMapDark = renderDark(Realm::Overworld, {kWorldHalf, kWorldHalf}, 16);
    const double offMapDark = renderDark(Realm::Overworld, {-40000.0, -40000.0}, 16);
    CHECK(noMapDark > 0.99);
    CHECK(offMapDark > 0.99);
}

// ---------------------------------------------------------------------------
// A respawn that changes realm restates the map
// ---------------------------------------------------------------------------

TEST(a_respawn_into_another_realm_sends_the_client_that_realms_map) {
    // Join the overworld, take the pad into the second map, die there,
    // respawn: the spawn choice is still the default, so the new body is on
    // the overworld -- and the client has to be told, or it keeps drawing the
    // second map under a flower that is walking the first.
    //
    // A fixture world, because the shipped data is ONE map with no pads on it.
    // What is under test is the RealmChange a cross-realm respawn sends, which
    // needs two realms and does not care which maps they are.
    const std::string dir = flix::testsupport::twoMapDataDir("respawnrealm");
    CHECK(!dir.empty());
    if (dir.empty()) return;
    Harness h("respawn-realm", {}, dir, 0);
    if (!h.ready) { CHECK(false); flix::testsupport::removeDataDir(dir); return; }
    NetClient client;
    CHECK(joinAs(h, client, "spelunker", ""));

    const MapData* overworld = h.server.worldMaps().forRealm(Realm::Overworld);
    const MapElement* pad = nullptr;
    // NOT a ternary over `overworld->elements()` and an empty vector: the two
    // branches have no common reference type, so the conditional yields a
    // COPY, the range-for binds to that temporary, and every pointer taken
    // into it dangles the moment the loop ends. It read as a pad at (0, 0)
    // that no flower could ever stand on.
    static const std::vector<MapElement> kNoElements;
    for (const MapElement& element :
         overworld != nullptr ? overworld->elements() : kNoElements) {
        if (element.kind == MapElementKind::Teleporter && element.targetMap == "warren") {
            pad = &element;
        }
    }
    CHECK(pad != nullptr);
    if (pad == nullptr) { flix::testsupport::removeDataDir(dir); return; }
    bool found = false;
    const Realm sewers = h.server.worldMaps().realmOfId("warren", found);
    CHECK(found);

    World& world = h.server.world();
    const Entity body = bodyOf(world, "spelunker");
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) return;
    world.get<Transform>(body).position = pad->centre();
    CHECK(h.stepUntil({&client}, [&] { return world.get<Transform>(body).realm == sewers; }, 120));
    CHECK(h.stepUntil({&client}, [&] { return client.view().realm() == sewers; }));
    CHECK_EQ(client.terrain().tileCols(sewers), h.server.terrain().tileCols(sewers));
    // The client's drawn body catches up with the arrival: it is placed by a
    // snapshot, and until then the arrival stands in for it.
    CHECK(h.stepUntil({&client}, [&] { return client.selfPlaced(); }));

    // Death in the sewers.
    world.get<Health>(body).current = 0;
    world.add<Dead>(body, Dead{NULL_ENTITY});
    CHECK(h.stepUntil({&client}, [&] { return client.dead(); }));
    client.requestRespawn();
    CHECK(h.stepUntil({&client}, [&] {
        const Entity reborn = bodyOf(world, "spelunker");
        return reborn != NULL_ENTITY && reborn != body && world.has<Transform>(reborn);
    }));
    const Entity reborn = bodyOf(world, "spelunker");
    CHECK(reborn != NULL_ENTITY);
    if (reborn == NULL_ENTITY) return;
    CHECK(world.get<Transform>(reborn).realm == Realm::Overworld);

    // The client followed: its realm, its grid and its drawn body all belong
    // to the overworld again, and the arrival it was handed is the new
    // body's spawn.
    CHECK(h.stepUntil({&client}, [&] { return client.view().realm() == Realm::Overworld; }));
    CHECK_EQ(client.terrain().tileCols(Realm::Overworld), h.server.terrain().tileCols(Realm::Overworld));
    CHECK_EQ(client.terrain().tileRows(Realm::Overworld), h.server.terrain().tileRows(Realm::Overworld));
    CHECK(h.stepUntil({&client}, [&] { return client.selfPlaced(); }));
    CHECK_NEAR(client.arrival().x, world.get<Transform>(reborn).position.x, 1.0);
    CHECK_NEAR(client.arrival().y, world.get<Transform>(reborn).position.y, 1.0);
    flix::testsupport::removeDataDir(dir);
}
