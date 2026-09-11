// The bots, driven by a real server with a real player in the world.
//
// What is worth pinning here is not any one decision -- the decision tree is
// full of randomised timers and personas, and a test that asserted a
// particular heading would fail on the next tuning change. It is the
// PROPERTIES the reference's controller has and a broken port loses, each of
// which has actually gone wrong at some point in the browser build:
//
//   * bots exist and keep existing while a player is online
//   * they MOVE -- an input path that never writes moveStrength produces a
//     field of flowers standing perfectly still, which is what a crashed or
//     short-circuited controller looks like from outside
//   * they spread out rather than converging on one point
//   * they stay inside the map
//   * a corpse is replaced rather than left standing
//   * the loadout swaps hand the original petals back
//
// The tests reach into the server through its public world handle rather than
// through the wire: a bot is a plain player entity, so it is findable by
// having a PlayerTag and no session behind it.

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "server/bot_ai.h"
#include "server/db.h"
#include "test.h"
#include "server_harness.h"

using namespace flix;
using namespace flix::testsupport;

namespace {

/// Every bot body in the world: a flower with a nameplate and no account.
std::vector<Entity> botBodies(World& world) {
    std::vector<Entity> out;
    Query<PlayerTag, PlayerAccount, Transform> players{world};
    players.each([&](Entity e, PlayerTag&, PlayerAccount& account, Transform&) {
        if (account.userId.empty()) out.push_back(e);
    });
    return out;
}

} // namespace

TEST(bots_populate_and_move) {
    Harness h("bots-move");
    CHECK(h.ready);
    if (!h.ready) return;

    NetClient client;
    CHECK(loginNew(h, client, "botwatcher", "hunter22"));
    client.joinGame(1280, 720, {}, "botwatcher");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    // Two maintenance passes plus a little, so the burst cap has had time to
    // fill more than one batch.
    h.step(200, {&client});

    World& world = h.server.world();
    const std::vector<Entity> bots = botBodies(world);
    CHECK(bots.size() > 4);
    if (bots.empty()) return;

    // Where everyone is now, and where they are two seconds later.
    std::unordered_map<Entity, Vec2> before;
    for (const Entity bot : bots) before[bot] = world.get<Transform>(bot).position;
    h.step(60, {&client});

    int moved = 0;
    int alive = 0;
    for (const Entity bot : bots) {
        if (!world.isAlive(bot)) continue;
        ++alive;
        const Vec2 now = world.get<Transform>(bot).position;
        // Ten units over two seconds is a very low bar deliberately: a bot
        // standing on a wander target on purpose, or orbiting tightly, still
        // has to be distinguishable from one no controller is driving.
        if ((now - before[bot]).length() > 10.0) ++moved;
    }
    CHECK(alive > 0);
    // Not every bot at once -- idle pauses are part of the behaviour -- but a
    // clear majority of a two-dozen population must be going somewhere.
    CHECK(moved * 2 > alive);
}

TEST(bots_spread_out_and_stay_in_the_world) {
    Harness h("bots-spread");
    CHECK(h.ready);
    if (!h.ready) return;

    NetClient client;
    CHECK(loginNew(h, client, "botspread", "hunter22"));
    client.joinGame(1280, 720, {}, "botspread");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));
    h.step(300, {&client});

    World& world = h.server.world();
    const std::vector<Entity> bots = botBodies(world);
    CHECK(bots.size() > 4);
    if (bots.size() < 2) return;

    Vec2 centre{0, 0};
    for (const Entity bot : bots) {
        const Vec2 at = world.get<Transform>(bot).position;
        // Inside the map, with room for the boundary margin the wander picker
        // clamps to. A bot outside this is one whose steering escaped.
        CHECK(at.x > 0.0 && at.x < kWorldSize);
        CHECK(at.y > 0.0 && at.y < kWorldSize);
        centre += at;
    }
    centre = centre / static_cast<double>(bots.size());

    // Separation and the per-band farming zones between them mean the
    // population must not be a single knot. A crowd that has collapsed onto
    // one point is the classic failure of a controller whose anchor is the
    // same for everyone.
    double spread = 0;
    for (const Entity bot : bots) {
        spread = std::max(spread, (world.get<Transform>(bot).position - centre).length());
    }
    CHECK(spread > kBotSeparationRadius * 2.0);
}

TEST(bot_corpses_are_replaced) {
    Harness h("bots-respawn");
    CHECK(h.ready);
    if (!h.ready) return;

    NetClient client;
    CHECK(loginNew(h, client, "botreaper", "hunter22"));
    client.joinGame(1280, 720, {}, "botreaper");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));
    h.step(200, {&client});

    World& world = h.server.world();
    std::vector<Entity> bots = botBodies(world);
    CHECK(!bots.empty());
    if (bots.empty()) return;
    const std::size_t populated = bots.size();

    // Kill one outright, the way combat does: `Dead` is the signal, and
    // zeroing health alone is not one.
    const Entity corpse = bots.front();
    world.get<Health>(corpse).current = 0;
    world.add<Dead>(corpse, Dead{NULL_ENTITY});

    // The corpse LINGERS -- other bots carry yggdrasil and path to it -- so it
    // is still there on the next tick rather than gone the instant it died.
    h.step(2, {&client});
    CHECK(world.isAlive(corpse));
    CHECK(world.has<Dead>(corpse));

    // Then the population pass takes it away and builds a replacement.
    h.step(300, {&client});
    CHECK(!world.isAlive(corpse));
    // The population is back where it was: a bot that died and was never
    // replaced is a slot the world quietly loses for the rest of the session.
    CHECK(botBodies(world).size() >= populated);
}

TEST(bot_traversal_petals_are_handed_back) {
    Harness h("bots-swap");
    CHECK(h.ready);
    if (!h.ready) return;

    NetClient client;
    CHECK(loginNew(h, client, "botswap", "hunter22"));
    client.joinGame(1280, 720, {}, "botswap");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));
    h.step(400, {&client});

    // Bots swap slot 0 for powder while crossing to their farming ground and
    // slot 1 for yggdrasil while another bot is close. Neither may become the
    // build: after a long run no bot may be carrying BOTH swaps in a loadout
    // that also lost its original petals -- and no slot may be left empty by a
    // restore that wrote a default-constructed slot back.
    World& world = h.server.world();
    const std::vector<Entity> bots = botBodies(world);
    CHECK(!bots.empty());

    int emptyPrimarySlots = 0;
    for (const Entity bot : bots) {
        const Loadout& loadout = world.get<Loadout>(bot);
        for (int i = 0; i < kLoadoutActiveSlots; ++i) {
            if (loadout.slots[static_cast<std::size_t>(i)].empty()) ++emptyPrimarySlots;
        }
    }
    // Every bot is built with all ten active slots filled, and nothing in the
    // controller may empty one.
    CHECK_EQ(emptyPrimarySlots, 0);
}

namespace {

/// An admin account, seeded before the server opens the database.
void seedAdmin(const std::string& path) {
    Database db;
    std::string error;
    db.load(path, error);
    db.setPasswordCost(4);   // the default cost makes this the slowest test
    CreateResult created = db.createUser("boss", "password7");
    if (created.ok()) created.account->admin = true;
    db.markDirty();
    db.save();
}

int countBots(World& world) {
    int count = 0;
    Query<PlayerTag, PlayerAccount> players{world};
    players.each([&](Entity, PlayerTag&, PlayerAccount& account) {
        if (account.userId.empty()) ++count;
    });
    return count;
}

} // namespace

TEST(set_bot_count_moves_the_population_at_once) {
    // The console test beside this one asserts what `set_bot_count` SAYS. This
    // asserts what it DOES, which is the half that was never covered: an
    // operator has no way to tell a target that was recorded but never acted
    // on from one that works, and "the command does nothing" is exactly what a
    // recorded-only target looks like.
    Harness h("bots-count", seedAdmin);
    CHECK(h.ready);
    if (!h.ready) return;

    NetClient client;
    CHECK(connectClient(h, client));
    client.requestLogin("boss", "password7");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
    client.joinGame(1280, 720, {}, "boss");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));
    h.step(200, {&client});

    World& world = h.server.world();
    CHECK(countBots(world) > 0);

    // Up to the ceiling, and there on the NEXT TICK rather than dribbled out
    // at the restart burst rate. The burst cap exists to make a restart look
    // like players arriving; an operator typing a number is not a restart, and
    // filling a hundred bots four at a time takes half a minute of looking
    // like a command that did nothing.
    client.sendChat("/admin set_bot_count " + std::to_string(kMaxBots));
    h.step(3, {&client});
    CHECK_EQ(countBots(world), kMaxBots);

    // And back down, just as promptly.
    client.sendChat("/admin set_bot_count 3");
    h.step(3, {&client});
    CHECK_EQ(countBots(world), 3);

    // Over the ceiling clamps to it rather than being refused.
    client.sendChat("/admin set_bot_count " + std::to_string(kMaxBots + 50));
    h.step(3, {&client});
    CHECK_EQ(countBots(world), kMaxBots);

    // `default` hands the population back to the formula, which targets far
    // fewer than the ceiling with one player online.
    client.sendChat("/admin set_bot_count default");
    h.step(3, {&client});
    CHECK(countBots(world) < kMaxBots);
}

TEST(a_bot_never_takes_a_pad) {
    // Bots exist to populate the overworld, and their controller reads only
    // that map's grid. A bot carried through a pad would steer around walls
    // it is not standing among, so a pad simply does not take one -- however
    // long it stands there -- while the same pad takes a player.
    Harness h("bots-pads");
    CHECK(h.ready);
    if (!h.ready) return;

    NetClient client;
    CHECK(loginNew(h, client, "padwatcher", "hunter22"));
    client.joinGame(1280, 720, {}, "padwatcher");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));
    h.step(200, {&client});

    World& world = h.server.world();
    const MapData* overworld = h.server.worldMaps().forRealm(Realm::Overworld);
    const MapElement* pad = nullptr;
    for (const MapElement& element : overworld != nullptr ? overworld->elements()
                                                          : std::vector<MapElement>{}) {
        if (element.kind == MapElementKind::Teleporter && element.targetMap == "sewers") {
            pad = &element;
        }
    }
    CHECK(pad != nullptr);
    const std::vector<Entity> bots = botBodies(world);
    CHECK(!bots.empty());
    if (pad == nullptr || bots.empty()) return;

    // Held on the pad's centre every tick for three dwell periods: a player
    // would have been sent to the sewers three times over.
    const Entity bot = bots.front();
    const int ticks = static_cast<int>(3.0 * kTeleporterDwellMillis / net::kTickMillis);
    for (int i = 0; i < ticks && world.isAlive(bot); ++i) {
        world.get<Transform>(bot).position = pad->centre();
        h.step(1, {&client});
    }
    CHECK(world.isAlive(bot));
    if (world.isAlive(bot)) {
        CHECK(world.get<Transform>(bot).realm == Realm::Overworld);
        // Not even charging: the pad never began to take it.
        if (const TeleporterState* state = world.tryGet<TeleporterState>(bot)) {
            CHECK(state->pad < 0);
        }
    }

    // The same pad, the same hold, a player: gone to the sewers.
    Entity player = NULL_ENTITY;
    Query<PlayerTag, PlayerAccount> players{world};
    players.each([&](Entity e, PlayerTag&, PlayerAccount& account) {
        if (account.username == "padwatcher") player = e;
    });
    CHECK(player != NULL_ENTITY);
    if (player == NULL_ENTITY) return;
    world.get<Transform>(player).position = pad->centre();
    CHECK(h.stepUntil({&client}, [&] {
        return world.get<Transform>(player).realm != Realm::Overworld;
    }, ticks));
}
