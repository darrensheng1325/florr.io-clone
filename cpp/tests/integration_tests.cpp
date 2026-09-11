#include "test.h"

#include "client/net_client.h"
#include "client/interpolation.h"
#include "server/db.h"
#include "server/game_server.h"
#include "server/systems/mob_ai.h"
#include "server_harness.h"
#include "shared/game/config.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace flix;

// These tests answer "is it still a multiplayer game": a real server object,
// real sockets on loopback, real clients speaking the real protocol.
// Everything below the socket is the shipping code path.

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

} // namespace

TEST(a_client_connects_registers_and_joins) {
    Harness h("join");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(connectClient(h, client));

    client.requestRegister("alice", "hunter2!");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
    CHECK(!client.sessionToken().empty());
    CHECK_EQ(client.profile().username, std::string("alice"));

    client.joinGame(1280, 720);
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    // Rendering uses this local copy, so it must be byte-for-byte the server
    // terrain that movement and collisions are using.
    CHECK(std::equal(client.terrain().tiles(),
                     client.terrain().tiles() + client.terrain().tileCount(),
                     h.server.terrain().tiles()));

    CHECK(h.stepUntil({&client}, [&] { return client.view().self().netId != 0; }));
    CHECK(client.view().self().maxHealth > 0);
    CHECK(client.view().self().health > 0);
    CHECK_EQ(client.view().self().level, 1);
}

TEST(a_wrong_password_is_refused_and_a_right_one_is_not) {
    Harness h("auth");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(connectClient(h, client));
    client.requestRegister("bob", "correct-horse");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));

    NetClient wrong;
    CHECK(connectClient(h, wrong));
    wrong.authAnswered = false;
    wrong.requestLogin("bob", "wrong");
    CHECK(h.stepUntil({&wrong}, [&] { return wrong.authAnswered; }));
    CHECK_EQ(static_cast<int>(wrong.authStatus), static_cast<int>(net::AuthStatus::BadCredentials));
    CHECK(wrong.status() != NetClient::Status::LoggedIn);

    NetClient right;
    CHECK(connectClient(h, right));
    right.authAnswered = false;
    right.requestLogin("bob", "correct-horse");
    CHECK(h.stepUntil({&right}, [&] { return right.authAnswered; }));
    CHECK_EQ(static_cast<int>(right.authStatus), static_cast<int>(net::AuthStatus::Ok));
}

TEST(a_content_mismatch_is_reported_rather_than_misparsed) {
    Harness h("proto");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    // Content the server does not have. The handshake must reject it outright
    // instead of letting the two sides disagree about every stat all session.
    client.contentHash = 0xDEADBEEF;
    CHECK(client.connect("127.0.0.1", h.port));
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Failed; }));
    CHECK(!client.lastError().empty());
}

TEST(a_refused_handshake_is_not_redialled) {
    Harness h("proto-noretry");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    client.contentHash = 0xDEADBEEF;
    CHECK(client.connect("127.0.0.1", h.port));
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Failed; }));

    // A server that refuses this build refuses it every time, so nothing
    // redials. What has to change is the build, which is what the flag says.
    CHECK(!client.reconnecting());
    CHECK(client.staleBuild);
}

TEST(a_client_redials_a_server_that_restarted) {
    const auto seed = [](const std::string& path) {
        Database db;
        std::string error;
        db.load(path, error);
        db.setPasswordCost(4);
        db.createUser("returner", "password7");
        db.markDirty();
        db.save();
    };

    NetClient client;
    std::uint16_t port = 0;
    {
        Harness first("restart-a", seed);
        if (!first.ready) { CHECK(false); return; }
        port = first.port;
        CHECK(connectClient(first, client));
        client.requestLogin("returner", "password7");
        CHECK(first.stepUntil({&client},
                              [&] { return client.status() == NetClient::Status::LoggedIn; }));
    }
    // The server process is gone -- which is exactly what a restart looks like
    // from here.
    for (int i = 0; i < 200 && client.status() != NetClient::Status::Failed; ++i) {
        client.poll(1);
    }
    CHECK(client.status() == NetClient::Status::Failed);
    CHECK(client.reconnecting());
    CHECK(!client.reconnected);

    // The same port, because the harness takes the lowest free one and the
    // first server has just released it. A client that redialled somewhere
    // else would not be testing anything.
    Harness second("restart-b", seed);
    if (!second.ready) { CHECK(false); return; }
    CHECK(second.port == port);

    // The backoff starts at a second, so this is real elapsed time rather than
    // simulated ticks; the budget covers two or three attempts.
    const bool back = second.stepUntil({&client},
                                       [&] { return client.status() == NetClient::Status::Ready ||
                                                    client.status() == NetClient::Status::LoggedIn; },
                                       2000);
    CHECK(back);
    CHECK(client.reconnected);

    // And the socket is genuinely usable again, not merely open: the account
    // goes back in over it.
    //
    // With a password rather than the token this client is still holding.
    // Sessions live in the database, and these two harnesses own separate
    // files -- on a real restart the same inventory.json comes back up and the
    // token resumes, which is what App::onReconnected presents.
    client.requestLogin("returner", "password7");
    CHECK(second.stepUntil({&client},
                           [&] { return client.status() == NetClient::Status::LoggedIn; }));
}

TEST(two_players_see_each_other_move) {
    Harness h("twoplayers");
    if (!h.ready) { CHECK(false); return; }

    NetClient alice, bob;
    CHECK(connectClient(h, alice));
    CHECK(connectClient(h, bob));

    alice.requestRegister("alice", "password1");
    bob.requestRegister("bob", "password2");
    CHECK(h.stepUntil({&alice, &bob}, [&] {
        return alice.status() == NetClient::Status::LoggedIn &&
               bob.status() == NetClient::Status::LoggedIn;
    }));

    // The nameplate carries the flower's name, not the account's, so the two
    // bodies are told apart below by the name sent here.
    alice.joinGame(1280, 720, {}, "alice");
    bob.joinGame(1280, 720, {}, "bob");
    CHECK(h.stepUntil({&alice, &bob}, [&] {
        return alice.status() == NetClient::Status::Playing &&
               bob.status() == NetClient::Status::Playing;
    }));

    // Stand them next to each other. Spawns are scattered across the map's
    // beginner zone, which is thousands of units wide -- far enough apart that
    // whether two arrivals can see one another is a coin toss, and this test is
    // about replication, not about where the map puts people.
    World& world = h.server.world();
    Entity bobBody = NULL_ENTITY;
    Vec2 aliceAt{};
    Query<PlayerTag, PlayerAccount, Transform> bodies{world};
    bodies.each([&](Entity e, PlayerTag&, PlayerAccount& account, Transform& transform) {
        if (account.username == "alice") aliceAt = transform.position;
        else if (account.username == "bob") bobBody = e;
    });
    CHECK(bobBody != NULL_ENTITY);
    if (bobBody != NULL_ENTITY) world.get<Transform>(bobBody).position = aliceAt + Vec2{60, 0};

    // And clear the ground in front of them. The door is a single tile inside
    // the garden's common band, which the band fill stocked before anyone
    // arrived, and a mob standing (or wandering) in Alice's lane shoves her
    // sideways as she runs through it -- a real collision, not a replication
    // fault, and not what this test measures.
    std::vector<Entity> bystanders;
    Query<MobTag, Transform> mobs{world};
    mobs.each([&](Entity e, MobTag&, Transform& transform) {
        if (distanceSq(transform.position, aliceAt) < 4000.0 * 4000.0) bystanders.push_back(e);
    });
    for (Entity e : bystanders) world.destroy(e);

    CHECK(h.stepUntil({&alice, &bob}, [&] {
        return playersVisibleTo(alice) >= 2 && playersVisibleTo(bob) >= 2;
    }));

    // Find Alice's body in Bob's view, drive Alice, and confirm Bob sees the
    // movement. That is the whole multiplayer contract in one assertion.
    const std::uint32_t aliceNetId = alice.view().self().netId;
    CHECK(aliceNetId != 0);
    CHECK(bob.view().entities().count(aliceNetId) == 1);
    const Vec2 before = bob.view().entities().at(aliceNetId).targetPosition;

    net::InputFrame input;
    input.moveAngle = 0;          // straight along +x
    input.moveStrength = 1.0;
    for (int i = 0; i < 40; ++i) {
        input.sequence = static_cast<std::uint32_t>(i + 1);
        alice.sendInput(input);
        h.step(1, {&alice, &bob});
    }

    CHECK(bob.view().entities().count(aliceNetId) == 1);
    const Vec2 after = bob.view().entities().at(aliceNetId).targetPosition;
    // Forty ticks at full speed is well over a second of running; less than a
    // hundred units means input is not reaching the simulation.
    CHECK(after.x - before.x > 100.0);
    CHECK_NEAR(after.y, before.y, 60.0);
}

TEST(chat_reaches_the_other_player) {
    Harness h("chat");
    if (!h.ready) { CHECK(false); return; }

    NetClient alice, bob;
    CHECK(connectClient(h, alice));
    CHECK(connectClient(h, bob));
    alice.requestRegister("alice", "password1");
    bob.requestRegister("bob", "password2");
    CHECK(h.stepUntil({&alice, &bob}, [&] {
        return alice.status() == NetClient::Status::LoggedIn &&
               bob.status() == NetClient::Status::LoggedIn;
    }));

    const std::size_t before = bob.chat().size();
    alice.sendChat("hello bob");
    CHECK(h.stepUntil({&alice, &bob}, [&] { return bob.chat().size() > before; }));

    bool found = false;
    for (const ChatLine& line : bob.chat()) {
        if (line.text == "hello bob" && line.author == "alice") found = true;
    }
    CHECK(found);
}

TEST(chat_flooding_is_rate_limited) {
    Harness h("flood");
    if (!h.ready) { CHECK(false); return; }

    NetClient alice, bob;
    CHECK(connectClient(h, alice));
    CHECK(connectClient(h, bob));
    alice.requestRegister("alice", "password1");
    bob.requestRegister("bob", "password2");
    CHECK(h.stepUntil({&alice, &bob}, [&] {
        return alice.status() == NetClient::Status::LoggedIn &&
               bob.status() == NetClient::Status::LoggedIn;
    }));

    const std::size_t before = bob.chat().size();
    for (int i = 0; i < 50; ++i) alice.sendChat("spam " + std::to_string(i));
    h.step(40, {&alice, &bob});

    // A client controls how often it sends; the server controls how often it
    // will listen. Far fewer than fifty lines must get through.
    CHECK(bob.chat().size() - before < 20);
}

TEST(the_drawn_flower_tracks_the_server_without_ever_stepping_backwards) {
    // The property that makes the world look still while you run through it.
    // The client simulates nothing, so its drawn position is a low-pass of a
    // monotonically advancing authoritative one -- and a low-pass of a
    // monotonic signal is monotonic. Any backwards step along the heading is
    // a correction, and because the camera is pinned to this position, a
    // correction is a jolt of the entire world.
    Harness h("drawn-flower");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(connectClient(h, client));
    client.requestRegister("carol", "password3");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
    client.joinGame(1280, 720);
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));
    CHECK(h.stepUntil({&client}, [&] { return client.view().self().netId != 0; }));

    net::InputFrame input;
    input.moveAngle = 0.7;
    input.moveStrength = 1.0;

    // Put the body where the whole run is provably clear of walls: a wall stop
    // is a legitimate reversal of the SERVER position, and this test is about
    // what the client adds on top of it.
    const double reach = kPlayerMaxSpeed * net::kTickSeconds * 60.0 + kPlayerBaseRadius * 2;
    const Vec2 heading = Vec2::fromAngle(input.moveAngle, reach);
    World& world = h.server.world();
    Entity body = NULL_ENTITY;
    Query<PlayerTag, Transform> bodies{world};
    bodies.each([&](Entity e, PlayerTag&, Transform&) { body = e; });
    CHECK(body != NULL_ENTITY);
    if (body != NULL_ENTITY) {
        Vec2 open = world.get<Transform>(body).position;
        for (int attempt = 0; attempt < 400; ++attempt) {
            if (!h.server.terrain().blocked(open, Realm::Overworld) &&
                !h.server.terrain().segmentBlocked(open, open + heading, Realm::Overworld)) {
                break;
            }
            open = h.server.terrain().findOpenSpawn(h.probeRng, {kWorldHalf * 0.2, kWorldHalf * 0.6},
                                                    4000.0, Realm::Overworld);
        }
        world.get<Transform>(body).position = open;
    }
    CHECK(h.stepUntil({&client}, [&] {
        return distance(client.view().self().position, world.get<Transform>(body).position) < 1.0;
    }));

    // A mob touching the flower displaces it 25 units on the spot, which is a
    // real server-side reversal and not what this measures. Sweep the lane.
    const auto clearLane = [&] {
        if (body == NULL_ENTITY) return;
        const Vec2 at = world.get<Transform>(body).position;
        std::vector<Entity> doomed;
        Query<MobTag, Transform> mobs{world};
        mobs.each([&](Entity e, MobTag&, Transform& transform) {
            if (distance(transform.position, at) < 2000.0) doomed.push_back(e);
        });
        for (const Entity e : doomed) world.destroy(e);
    };
    clearLane();

    WorldView& view = client.view();
    view.snapAll();
    const Vec2 forward = Vec2::fromAngle(input.moveAngle, 1.0);
    const auto along = [&forward](Vec2 p) { return p.x * forward.x + p.y * forward.y; };
    double previousAlong = along(view.selfDrawnPosition());
    double worstBackwards = 0;
    double worstLag = 0;

    for (int i = 0; i < 60; ++i) {
        clearLane();
        input.sequence = static_cast<std::uint32_t>(i + 1);
        client.sendInput(input);
        h.step(1, {&client});
        // Two rendered frames per server tick, as a 60 Hz client gets.
        view.interpolate(1000.0 + i * net::kTickMillis, net::kTickSeconds / 2.0);
        view.interpolate(1000.0 + i * net::kTickMillis + net::kTickMillis / 2.0,
                         net::kTickSeconds / 2.0);

        const double now = along(view.selfDrawnPosition());
        worstBackwards = std::max(worstBackwards, previousAlong - now);
        previousAlong = now;
        // Settled in after a few ticks, before which the client is still
        // learning where it spawned.
        if (i > 10) {
            worstLag = std::max(worstLag,
                                distance(view.selfDrawnPosition(), view.self().position));
        }
    }

    // Not "small": zero. The ease only ever moves toward the target.
    CHECK_NEAR(worstBackwards, 0.0, 1e-9);
    // And it stays within a few frames of travel of the truth rather than
    // drifting away from it.
    CHECK(worstLag < kPlayerMaxSpeed * 0.5);
}

TEST(a_disconnect_removes_the_player_from_everyone_else) {
    // No bots: the world is one map with one small door, so the bot
    // population stands in the same place a joining player does. "Alice sees
    // exactly one flower once Bob is gone" is a statement about the
    // disconnect, and it cannot be made in a crowd.
    Harness h("disconnect", {}, flix::testsupport::dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient alice;
    std::uint32_t aliceNetId = 0;
    {
        NetClient bob;
        CHECK(connectClient(h, alice));
        CHECK(connectClient(h, bob));
        alice.requestRegister("alice", "password1");
        bob.requestRegister("bob", "password2");
        CHECK(h.stepUntil({&alice, &bob}, [&] {
            return alice.status() == NetClient::Status::LoggedIn &&
                   bob.status() == NetClient::Status::LoggedIn;
        }));
        alice.joinGame(1280, 720, {}, "alice");
        bob.joinGame(1280, 720, {}, "bob");
        CHECK(h.stepUntil({&alice, &bob}, [&] {
            return alice.status() == NetClient::Status::Playing &&
                   bob.status() == NetClient::Status::Playing;
        }));
        // Spawns scatter across a zone thousands of units wide; stand them
        // together so this tests the disconnect, not the spawn scatter.
        World& world = h.server.world();
        Vec2 aliceAt{};
        Entity bobBody = NULL_ENTITY;
        Query<PlayerTag, PlayerAccount, Transform> bodies{world};
        bodies.each([&](Entity e, PlayerTag&, PlayerAccount& account, Transform& transform) {
            if (account.username == "alice") aliceAt = transform.position;
            else if (account.username == "bob") bobBody = e;
        });
        if (bobBody != NULL_ENTITY) world.get<Transform>(bobBody).position = aliceAt + Vec2{60, 0};

        CHECK(h.stepUntil({&alice, &bob}, [&] { return playersVisibleTo(alice) >= 2; }));
        aliceNetId = alice.view().self().netId;
        CHECK(aliceNetId != 0);
        bob.disconnect();
    }

    // Bob's socket is gone. The server must drop his body and tell Alice,
    // rather than leaving a ghost standing there forever.
    CHECK(h.stepUntil({&alice}, [&] { return playersVisibleTo(alice) == 1; }));
}

TEST(a_session_token_lets_a_returning_player_back_in) {
    Harness h("persist");
    if (!h.ready) { CHECK(false); return; }

    std::string token;
    {
        NetClient client;
        CHECK(connectClient(h, client));
        client.requestRegister("dave", "password4");
        CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
        token = client.sessionToken();
        CHECK(!token.empty());
    }

    // A returning client presents only its token; the password is never stored
    // anywhere but in the player's head.
    NetClient returning;
    CHECK(connectClient(h, returning));
    returning.authAnswered = false;
    returning.resumeSession(token);
    CHECK(h.stepUntil({&returning}, [&] { return returning.authAnswered; }));
    CHECK_EQ(static_cast<int>(returning.authStatus), static_cast<int>(net::AuthStatus::Ok));
    CHECK_EQ(returning.profile().username, std::string("dave"));

    // A made-up token must not be accepted.
    NetClient forged;
    CHECK(connectClient(h, forged));
    forged.authAnswered = false;
    forged.resumeSession("not-a-real-token");
    CHECK(h.stepUntil({&forged}, [&] { return forged.authAnswered; }));
    CHECK_EQ(static_cast<int>(forged.authStatus), static_cast<int>(net::AuthStatus::SessionExpired));
}

TEST(mobs_spawn_and_are_replicated_to_a_player) {
    Harness h("mobs");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(connectClient(h, client));
    client.requestRegister("erin", "password5");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
    // Use the largest accepted viewport so newly spawned ring mobs enter the
    // replication prefetch region without relying on random AI movement.
    client.joinGame(2600, 2600);
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    const bool sawMobs = h.stepUntil({&client}, [&] {
        for (const auto& e : client.view().entities()) {
            if (e.second.kind == net::EntityKind::Mob) return true;
        }
        return false;
    }, 600);
    CHECK(sawMobs);

    // Whatever spawned must be drawable: a valid config index and a real size.
    for (const auto& entry : client.view().entities()) {
        if (entry.second.kind != net::EntityKind::Mob) continue;
        CHECK(entry.second.typeIndex < content().mobCount());
        CHECK(entry.second.radius > 0);
        CHECK(entry.second.healthFraction > 0);
    }
}

TEST(a_player_who_leaves_to_the_menu_keeps_their_account) {
    Harness h("leave");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(connectClient(h, client));
    client.requestRegister("frank", "password6");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
    client.joinGame(1280, 720);
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    client.leaveGame();
    h.step(10, {&client});
    // Leaving drops the body but not the login: the connection is still
    // authenticated and can join again without another password.
    CHECK(client.status() != NetClient::Status::Playing);
    CHECK_EQ(client.profile().username, std::string("frank"));

    client.joinGame(1280, 720);
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));
}

TEST(logging_out_ends_the_session_at_both_ends) {
    Harness h("logout");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(connectClient(h, client));
    client.requestRegister("grace", "password7");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
    const std::string token = client.sessionToken();
    CHECK(!token.empty());

    client.joinGame(1280, 720);
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));
    CHECK(h.stepUntil({&client}, [&] { return client.view().self().netId != 0; }));

    client.logout();
    h.step(10, {&client});

    // The client keeps nothing of the account: no token to resume with, no
    // profile for a panel to draw, and no body left on the screen. The socket
    // itself is untouched -- it is back where a fresh connection sits, ready
    // for the login form.
    CHECK(client.sessionToken().empty());
    CHECK(client.profile().username.empty());
    CHECK_EQ(playersVisibleTo(client), std::size_t(0));
    CHECK(client.status() == NetClient::Status::Ready);

    // The server dropped the body with it, rather than leaving a flower nobody
    // is steering.
    CHECK_EQ(h.server.playerCount(), std::size_t(0));

    // The token is revoked, not merely forgotten: a copy of it kept anywhere
    // else is now worthless.
    NetClient stale;
    CHECK(connectClient(h, stale));
    stale.authAnswered = false;
    stale.resumeSession(token);
    CHECK(h.stepUntil({&stale}, [&] { return stale.authAnswered; }));
    CHECK_EQ(static_cast<int>(stale.authStatus), static_cast<int>(net::AuthStatus::SessionExpired));

    // And the same socket can log straight back in, which is what the form
    // behind the Log Out button expects: no reconnect between the two.
    client.authAnswered = false;
    client.requestLogin("grace", "password7");
    CHECK(h.stepUntil({&client}, [&] { return client.authAnswered; }));
    CHECK_EQ(static_cast<int>(client.authStatus), static_cast<int>(net::AuthStatus::Ok));
    CHECK_EQ(client.profile().username, std::string("grace"));
    CHECK(client.sessionToken() != token);
}

TEST(a_hornets_missile_reaches_the_client_at_the_size_it_was_fired_at) {
    // No bots: they join at the same door this flower does now, and a farming
    // bot kills the hornet under test before it ever fires.
    Harness h("volley", {}, flix::testsupport::dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginNew(h, client, "hornetwatch", "password9"));
    client.joinGame(2600, 2600);
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    // Where THIS client's flower stands, so the hornet can be put in its face.
    //
    // Matched by the viewer's own net id rather than by taking the first
    // PlayerTag in the world: even with the bots off, the only flower that
    // must be found is this client's. A hornet anchored on somebody else
    // fires shots this viewer's stream never carries.
    World& world = h.server.world();
    const std::uint32_t selfNetId = client.view().self().netId;
    Query<PlayerTag, NetId, Transform> flowers(world);
    Vec2 at{0, 0};
    bool found = false;
    flowers.each([&](Entity, PlayerTag&, NetId& id, Transform& transform) {
        if (id.value == selfNetId) { at = transform.position; found = true; }
    });
    CHECK(found);

    // A hornet built the way the spawner builds one -- the shipping AI, the
    // shipping content -- close enough that it has a target immediately.
    const std::uint16_t hornetIndex = content().mobIndex("hornet");
    const MobStats stats = content().mobStats(hornetIndex, Rarity::Common);
    const Entity hornet = world.create();
    world.add<MobTag>(hornet);
    world.add<Transform>(hornet, Transform{at + Vec2{160, 0}, 0.0});
    world.add<Motion>(hornet);
    world.add<Body>(hornet, Body{stats.radius, stats.mass});
    world.add<Health>(hornet, Health{stats.health, stats.health, 0, 0});
    world.add<MobType>(hornet, MobType{hornetIndex, Rarity::Common, 1.0});
    world.add<Faction>(hornet, Faction{Team::Hostiles, false});
    world.add<ContactDamage>(hornet, ContactDamage{stats.damage, kMobHitIntervalMillis});
    MobAi brain;
    brain.kind = stats.ai;
    brain.anchor = at + Vec2{160, 0};
    brain.aggroRange = stats.aggroRange;
    world.add<MobAi>(hornet, brain);

    // What the wire should carry, worked out from the same two inputs the
    // server has: the ammunition's body and the SHOOTER's, which is where the
    // hornet's 1.3 size enters.
    const std::uint16_t ammo = content().petalIndex("hornet_missile");
    const PetalStats ammoStats = content().petalStats(ammo, Rarity::Common);
    const double ownerScale = stats.radius / kMobBaseRadius;
    const double expected = std::max(
        1.0, ammoStats.size * kProjectileRadiusPerSize * ownerScale / kProjectileSizeDivisor);

    // THIS hornet's shot, found through the server's Projectile.owner and
    // its net id, not the first projectile the stream happens to carry: the
    // flower stands at the garden door, where the ambient fill puts hornets
    // of other tiers within sight, and an uncommon one firing first would
    // deliver a shot 1.3x this size that has nothing to do with the port
    // under test.
    double seenRadius = -1.0;
    std::uint16_t seenType = 0xFFFF;
    Query<Projectile, NetId> shots(world);
    const bool sawShot = h.stepUntil({&client}, [&] {
        bool seen = false;
        shots.each([&](Entity, Projectile& shot, NetId& id) {
            if (seen || shot.owner != hornet) return;
            const auto entry = client.view().entities().find(id.value);
            if (entry == client.view().entities().end()) return;
            if (entry->second.kind != net::EntityKind::Projectile) return;
            seenRadius = entry->second.radius;
            seenType = entry->second.typeIndex;
            seen = true;
        });
        return seen;
    }, 600);
    CHECK(sawShot);

    // The client draws a shot at twice this, so a radius that arrived wrong is
    // artwork drawn at the wrong calibre. Tolerance is the f32 the wire uses.
    CHECK_NEAR(seenRadius, expected, 1e-3);
    CHECK_EQ(seenType, ammo);
    // And it really is smaller than the ammunition petal's own body: a shot is
    // half its petal, then the shooter's scale over the divisor.
    CHECK(seenRadius < ammoStats.radius);
}

TEST(persist_all_writes_the_database_and_keeps_serving) {
    Harness h("persist-all");
    if (!h.ready) { CHECK(false); return; }

    // A registration is dirty in memory and, left alone, reaches the disk
    // only on the periodic save. persistAll() is the on-demand flush: the
    // offline page calls it from the tab's unload handler, where nothing
    // else would ever write the file.
    NetClient client;
    CHECK(flix::testsupport::loginNew(h, client, "carol", "hunter2!"));
    // Nobody is in the world yet, so no player is written -- but the database is.
    CHECK_EQ(h.server.persistAll(), std::size_t{0});
    {
        Database probe;
        std::string error;
        CHECK(probe.load(h.dbPath, error));
        CHECK(probe.findUser("carol") != nullptr);
    }

    // Unlike shutdown(), the listener is still up afterwards.
    NetClient second;
    CHECK(connectClient(h, second));

    client.joinGame(1280, 720);
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));
    CHECK_EQ(h.server.persistAll(), std::size_t{1});
}
