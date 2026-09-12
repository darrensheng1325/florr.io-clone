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

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

    const Vec2 extent = h.server.terrain().realmExtent(Realm::Overworld);
    Vec2 centre{0, 0};
    for (const Entity bot : bots) {
        const Vec2 at = world.get<Transform>(bot).position;
        // Inside the map, with room for the boundary margin the wander picker
        // clamps to. A bot outside this is one whose steering escaped.
        // Measured against the OVERWORLD'S OWN extent: every map states its
        // size now, and the shipped one is nothing like the historical square.
        CHECK(at.x > 0.0 && at.x < extent.x);
        CHECK(at.y > 0.0 && at.y < extent.y);
        centre += at;
    }
    centre = centre / static_cast<double>(bots.size());

    // Separation and a hunting ground chosen per bot mean the population must
    // not be a single knot. A crowd collapsed onto one point is the classic
    // failure of a controller whose anchor is the same for everyone -- which
    // is exactly what the per-band "farm the zone matching your gear" rule
    // this replaced produced, once every bot's gear pointed at the same band.
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

    // Then put it out of everyone's reach, in the far corner of the map.
    //
    // Not tidying: a bot carrying yggdrasil REVIVES a corpse it can reach, and
    // bots actively path to each other's corpses to do it. The shipped map is
    // about three fifths solid now, so the walkable ground -- and the bots
    // standing on it -- is packed close enough that a corpse left where it
    // fell is revived long before the three-second replacement deadline. That
    // is correct behaviour and it is covered by
    // bot_traversal_petals_are_handed_back; what it is not is a test of the
    // REPLACEMENT path, which only runs on a corpse nobody saved. Nineteen
    // thousand units away, with three seconds on the clock, is out of reach.
    const Terrain& terrain = h.server.terrain();
    const Vec2 extent = terrain.realmExtent(Realm::Overworld);
    int farTx = 0;
    int farTy = 0;
    CHECK(terrain.nearestOpenTile({extent.x - kTileSize, extent.y - kTileSize}, farTx, farTy,
                                  Realm::Overworld));
    world.get<Transform>(corpse).position = Terrain::tileCenter(farTx, farTy);

    // The population pass takes it away and builds a replacement.
    CHECK(h.stepUntil({&client}, [&] { return !world.isAlive(corpse); }, 400));
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
    //
    // A fixture world, because the shipped map has no pads on it: `meadow`
    // with a pad into `warren`. What is under test is the BOT, and the bots
    // populate whatever overworld they are given.
    const std::string dir = twoMapDataDir("botpad");
    CHECK(!dir.empty());
    if (dir.empty()) return;
    Harness h("bots-pads", {}, dir);
    CHECK(h.ready);
    if (!h.ready) { removeDataDir(dir); return; }

    NetClient client;
    CHECK(loginNew(h, client, "padwatcher", "hunter22"));
    client.joinGame(1280, 720, {}, "padwatcher");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));
    h.step(200, {&client});

    World& world = h.server.world();
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
    const std::vector<Entity> bots = botBodies(world);
    CHECK(!bots.empty());
    if (pad == nullptr || bots.empty()) { removeDataDir(dir); return; }

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

    // The same pad, the same hold, a player: gone to the other map.
    Entity player = NULL_ENTITY;
    Query<PlayerTag, PlayerAccount> players{world};
    players.each([&](Entity e, PlayerTag&, PlayerAccount& account) {
        if (account.username == "padwatcher") player = e;
    });
    CHECK(player != NULL_ENTITY);
    if (player == NULL_ENTITY) { removeDataDir(dir); return; }
    world.get<Transform>(player).position = pad->centre();
    CHECK(h.stepUntil({&client}, [&] {
        return world.get<Transform>(player).realm != Realm::Overworld;
    }, ticks));
    removeDataDir(dir);
}

// ---------------------------------------------------------------------------
// The two things the controller exists to do
// ---------------------------------------------------------------------------
//
// Everything above pins that bots EXIST and move. These pin that they PLAY.
// Both were false of the controller these replaced -- a bot would walk through
// a mob without its petals ever coming out, and a boss could stand in the
// middle of the population unbothered -- and neither is visible in any of the
// properties above, because a bot walking in a straight line past a beetle
// satisfies all of them.

namespace {

/// Every live ambient mob in the overworld, with its health.
std::unordered_map<Entity, double> mobHealths(World& world) {
    std::unordered_map<Entity, double> out;
    Query<MobTag, Transform, Health> mobs{world};
    mobs.each([&](Entity e, MobTag&, Transform& transform, Health& health) {
        if (world.has<Pet>(e) || world.has<Dead>(e)) return;
        if (transform.realm != Realm::Overworld) return;
        out[e] = health.current;
    });
    return out;
}

/// `/admin spawn <mob> <rarity> <x> <y> <count>`, which is the console an
/// operator uses -- so a test that stages a fight stages it the way the game
/// can, rather than by assembling a mob out of components and hoping it
/// carries everything combat needs.
void adminSpawn(NetClient& client, const char* mob, const char* rarity, Vec2 at, int count) {
    client.sendChat("/admin spawn " + std::string(mob) + " " + rarity + " " +
                    std::to_string(static_cast<int>(at.x)) + " " +
                    std::to_string(static_cast<int>(at.y)) + " " + std::to_string(count));
}

} // namespace

TEST(a_bot_fights_what_is_put_in_front_of_it) {
    Harness h("bots-fight", seedAdmin);
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
    std::vector<Entity> bots = botBodies(world);
    CHECK(bots.size() > 4);
    if (bots.size() < 5) return;

    // The five bots FURTHEST from the admin's own flower. Bots deliberately
    // leave the mobs around a human alone (kBotPlayerClaimRadius) -- that rule
    // is what stops two dozen of them stripping the screen of whoever came to
    // play -- so a fight staged on the player's doorstep would be testing that
    // rule rather than this one.
    Entity me = NULL_ENTITY;
    Query<PlayerTag, PlayerAccount> people{world};
    people.each([&](Entity e, PlayerTag&, PlayerAccount& account) {
        if (account.username == "boss") me = e;
    });
    CHECK(me != NULL_ENTITY);
    if (me == NULL_ENTITY) return;
    const Vec2 human = world.get<Transform>(me).position;
    std::sort(bots.begin(), bots.end(), [&](Entity a, Entity b) {
        return distanceSq(world.get<Transform>(a).position, human) >
               distanceSq(world.get<Transform>(b).position, human);
    });

    // One mob dropped right on top of each of five bots. Five rather than one
    // because a bot may legitimately be doing something else at that instant
    // -- running from something, standing on a drop -- and the claim is about
    // the controller, not about any single tick of any single bot.
    //
    // LEGENDARY, not rare. A rare beetle dropped on a bot is dead on the tick
    // it lands -- which is the claim, emphatically, but it leaves nothing to
    // measure: the mob is killed and reaped inside the same tick the console
    // created it, so no observer between ticks ever sees it at all. A
    // legendary one takes a few seconds to chew through, and its health
    // dropping is the same statement made where it can be read.
    //
    // They are also picked out by WHAT and WHERE rather than by being new to
    // the world, because the world stocks itself now: mobs appear beside a bot
    // on their own the whole time this runs, and "everything that was not here
    // a moment ago" would be measuring the spawner.
    const std::uint16_t beetle = content().mobIndex("beetle");
    std::unordered_set<Entity> known;
    {
        Query<MobTag, MobType> mobs{world};
        mobs.each([&](Entity e, MobTag&, MobType& type) {
            if (type.configIndex == beetle) known.insert(e);
        });
    }

    std::vector<Entity> staged;
    std::unordered_map<Entity, double> stagedHealth;
    // Five staged, not five attempted: a bot may have died since the list was
    // taken, and a console line now and then does not land. Walking the roster
    // until five beetles are actually standing beside a bot is what stops the
    // measurement below quietly shrinking to two or three mobs.
    for (const Entity bot : bots) {
        if (staged.size() >= 5) break;
        if (!world.isAlive(bot) || world.has<Dead>(bot)) continue;
        const Vec2 at = world.get<Transform>(bot).position;
        const Vec2 spot = at + Vec2{90, 0};
        adminSpawn(client, "beetle", "legendary", spot, 1);
        // A third of a second between lines. The console is rate limited like
        // any other chat (session.h: commandAllowance), and five commands on
        // consecutive ticks is four commands the server never reads.
        for (int step = 0; step < 10; ++step) {
            h.step(1, {&client});
            Query<MobTag, MobType, Transform, Health> mobs{world};
            mobs.each([&](Entity e, MobTag&, MobType& type, Transform& where, Health& health) {
                if (type.configIndex != beetle || type.rarity != Rarity::Legendary) return;
                if (where.realm != Realm::Overworld) return;
                if (distance(where.position, spot) > 250.0) return;
                if (!known.insert(e).second) return;
                staged.push_back(e);
                stagedHealth[e] = health.current;
            });
        }
    }
    h.step(3, {&client});

    // Loudly, rather than measuring whatever happened to be lying around: a
    // staging step that quietly places nothing turns the assertion below into
    // a statement about an empty list.
    CHECK(staged.size() >= 4);
    if (staged.empty()) return;

    // Four seconds: long enough for the reaction delay, for the bot to close
    // the last few units and for a petal to come round.
    h.step(120, {&client});

    int hurt = 0;
    for (const Entity mob : staged) {
        // Gone counts: a beetle a bot finished off is the strongest form of
        // the claim.
        if (!world.isAlive(mob) || world.has<Dead>(mob)) { ++hurt; continue; }
        if (world.get<Health>(mob).current < stagedHealth[mob] - 1e-6) ++hurt;
    }
    CHECK(hurt >= 3);
}

TEST(bots_rally_onto_a_boss) {
    Harness h("bots-boss", seedAdmin);
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
    // THE BOTS IN THIS PLAYER'S REALM, not every bot on the server. Maps are
    // separate coordinate spaces, so averaging positions across two of them
    // names a point in neither and the boss lands nowhere near anybody --
    // which would read as the controller ignoring it.
    Entity watcher = NULL_ENTITY;
    Query<PlayerTag, PlayerAccount, Transform> watchers{world};
    watchers.each([&](Entity e, PlayerTag&, PlayerAccount& account, Transform&) {
        if (!account.userId.empty()) watcher = e;
    });
    CHECK(watcher != NULL_ENTITY);
    if (watcher == NULL_ENTITY) return;
    const Realm realm = world.get<Transform>(watcher).realm;

    std::vector<Entity> bots;
    for (const Entity bot : botBodies(world)) {
        if (world.get<Transform>(bot).realm == realm) bots.push_back(bot);
    }
    CHECK(bots.size() > 4);
    if (bots.size() < 5) return;

    // The middle of the population, so the boss lands inside the rally range
    // of most of it rather than off in a corner.
    Vec2 centre{0, 0};
    for (const Entity bot : bots) centre += world.get<Transform>(bot).position;
    centre = centre / static_cast<double>(bots.size());

    const std::unordered_map<Entity, double> before = mobHealths(world);
    adminSpawn(client, "beetle", "super", centre, 1);
    h.step(12, {&client});

    // The Super THIS spawn made: in the player's realm, and the nearest one to
    // where it was asked for. A map whose bands reach ultra rolls the odd
    // Super of its own, and one of those two maps away is not the boss the
    // bots are being asked to answer.
    Entity boss = NULL_ENTITY;
    double bossDistance = 0;
    Query<MobTag, MobType, Transform> mobs{world};
    mobs.each([&](Entity e, MobTag&, MobType& type, Transform& at) {
        if (before.count(e) != 0) return;
        if (type.rarity != Rarity::Super || at.realm != realm) return;
        const double distance = distanceSq(at.position, centre);
        if (boss == NULL_ENTITY || distance < bossDistance) {
            boss = e;
            bossDistance = distance;
        }
    });
    CHECK(boss != NULL_ENTITY);
    if (boss == NULL_ENTITY) return;

    const auto crowdAround = [&](double radius) {
        if (!world.isAlive(boss)) return 0;
        const Vec2 at = world.get<Transform>(boss).position;
        int count = 0;
        for (const Entity bot : bots) {
            if (!world.isAlive(bot) || world.has<Dead>(bot)) continue;
            if (distanceSq(world.get<Transform>(bot).position, at) < radius * radius) ++count;
        }
        return count;
    };

    const double bossHealth = world.get<Health>(boss).current;

    // Sampled over the whole window rather than read once at the end. A raid
    // is a crowd that GATHERS: taking one reading twelve seconds later asks
    // whether the bots happened to be on the near side of their orbit at that
    // instant, which is a coin toss, not the property under test.
    int peakCrowd = crowdAround(1000.0);
    for (int i = 0; i < 30; ++i) {
        h.step(15, {&client});
        if (!world.isAlive(boss) || world.has<Dead>(boss)) break;
        peakCrowd = std::max(peakCrowd, crowdAround(1000.0));
    }

    // Either the crowd gathered and chewed on it, or it gathered and killed it
    // -- both are the boss being answered rather than ignored, which is what
    // the old controller did with it.
    const bool killed = !world.isAlive(boss) || world.has<Dead>(boss);
    CHECK(peakCrowd >= 3);
    if (!killed) CHECK(world.get<Health>(boss).current < bossHealth);
}
