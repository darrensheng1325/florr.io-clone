#include "test.h"

#include "client/interpolation.h"
#include "client/world_view.h"
#include "server/replication.h"
#include "server/systems/petals.h"
#include "shared/game/components.h"

#include <string>

using namespace flix;

namespace {

/// A world with one player and a way to add mobs, so each test starts from the
/// same known shape instead of repeating twenty lines of setup.
struct Fixture {
    World world;
    NetIdAllocator ids;
    Replicator replicator;
    ClientView view;
    EventQueue events;
    Entity viewer = NULL_ENTITY;

    Fixture() { viewer = addPlayer("bob", {1000, 1000}); }

    Entity addPlayer(const std::string& name, Vec2 at) {
        const Entity e = world.create();
        world.add<PlayerTag>(e);
        world.add<Transform>(e, Transform{at, 0.0});
        world.add<Motion>(e);
        world.add<Body>(e, Body{kPlayerBaseRadius, 1.0});
        world.add<Health>(e, Health{100, 100, 0, 0});
        world.add<PlayerInput>(e);
        world.add<PlayerProgress>(e, PlayerProgress{500, 5, 3, false});
        world.add<PlayerLocation>(e);
        world.add<PlayerAccount>(e, PlayerAccount{"u-" + name, name, 1, false});
        world.add<NetId>(e, NetId{ids.next()});
        Replicated rep;
        rep.kind = net::EntityKind::Player;
        world.add<Replicated>(e, rep);
        return e;
    }

    Entity addMob(Vec2 at, Rarity rarity = Rarity::Rare, double radius = 30) {
        const Entity e = world.create();
        world.add<MobTag>(e);
        world.add<Transform>(e, Transform{at, 0.5});
        world.add<Body>(e, Body{radius, radius * radius});
        world.add<Health>(e, Health{50, 100, 0, 0});
        world.add<NetId>(e, NetId{ids.next()});
        Replicated rep;
        rep.kind = net::EntityKind::Mob;
        rep.typeIndex = 7;
        rep.rarity = rarity;
        world.add<Replicated>(e, rep);
        return e;
    }

    /// Builds one snapshot and feeds it to `into`, returning the payload size.
    std::size_t tick(WorldView& into, std::uint32_t tickNumber, double nowMillis) {
        ByteWriter out;
        Replicator::Frame frame;
        frame.tick = tickNumber;
        frame.nowMillis = nowMillis;
        frame.events = &events;
        replicator.build(world, viewer, view, frame, out);

        ByteReader reader(out.data(), out.size());
        const std::uint8_t id = reader.u8();
        CHECK_EQ(id, static_cast<std::uint8_t>(net::ServerMessage::Snapshot));
        CHECK(into.applySnapshot(reader));
        CHECK(reader.ok());
        return out.size();
    }
};

std::uint32_t netIdOf(World& w, Entity e) { return w.get<NetId>(e).value; }

} // namespace

// ---------------------------------------------------------------------------

TEST(snapshot_round_trips_a_spawn) {
    Fixture f;
    const Entity mob = f.addMob({1100, 1000});
    WorldView client;
    f.tick(client, 1, 1000);

    // The viewer and the mob both arrive.
    CHECK_EQ(client.entities().size(), std::size_t(2));
    const auto& e = client.entities().at(netIdOf(f.world, mob));
    CHECK_EQ(static_cast<int>(e.kind), static_cast<int>(net::EntityKind::Mob));
    CHECK_EQ(static_cast<int>(e.rarity), static_cast<int>(Rarity::Rare));
    CHECK_EQ(e.typeIndex, std::uint16_t(7));
    CHECK_NEAR(e.targetPosition.x, 1100, 0.05);
    CHECK_NEAR(e.radius, 30.0, 0.05);

    // The viewer's own record is flagged, named, and drives SelfState.
    const auto& me = client.entities().at(netIdOf(f.world, f.viewer));
    CHECK(me.isSelf());
    CHECK_EQ(me.name, std::string("bob"));
    CHECK_EQ(client.self().netId, netIdOf(f.world, f.viewer));
    CHECK_NEAR(client.self().health, 100.0, 1e-3);
    CHECK_EQ(client.self().level, 5);
    CHECK_EQ(client.self().stars, 3);
}

TEST(spawn_is_sent_once_and_not_repeated) {
    Fixture f;
    f.addMob({1100, 1000});
    WorldView client;
    const std::size_t first = f.tick(client, 1, 1000);
    const std::size_t second = f.tick(client, 2, 1040);
    // Nothing moved, so the second snapshot carries no spawns and no updates
    // -- only the fixed header. It must be markedly smaller.
    CHECK(second < first);
    CHECK_EQ(client.entities().size(), std::size_t(2));
}

TEST(only_changed_fields_are_sent) {
    Fixture f;
    const Entity mob = f.addMob({1100, 1000});
    WorldView client;
    f.tick(client, 1, 1000);
    const std::size_t idle = f.tick(client, 2, 1040);

    f.world.get<Transform>(mob).position.x += 25;
    const std::size_t moved = f.tick(client, 3, 1080);
    CHECK(moved > idle);
    CHECK_NEAR(client.entities().at(netIdOf(f.world, mob)).targetPosition.x, 1125, 0.05);

    // A sub-tolerance nudge is not worth bytes and must be suppressed.
    f.world.get<Transform>(mob).position.x += 0.001;
    const std::size_t nudged = f.tick(client, 4, 1120);
    CHECK_EQ(nudged, idle);
}

TEST(health_and_state_changes_replicate) {
    Fixture f;
    const Entity mob = f.addMob({1100, 1000});
    WorldView client;
    f.tick(client, 1, 1000);
    CHECK_NEAR(client.entities().at(netIdOf(f.world, mob)).healthFraction, 0.5, 0.01);

    Health& health = f.world.get<Health>(mob);
    health.current = 25;
    health.flashUntilMillis = 1200;
    f.tick(client, 2, 1040);

    const auto& e = client.entities().at(netIdOf(f.world, mob));
    CHECK_NEAR(e.healthFraction, 0.25, 0.01);
    CHECK((e.state & net::StateHurt) != 0);

    // The flash expires on its own once the deadline passes.
    f.tick(client, 3, 1300);
    CHECK((client.entities().at(netIdOf(f.world, mob)).state & net::StateHurt) == 0);
}

TEST(attack_and_defend_reach_the_client) {
    Fixture f;
    WorldView client;
    f.tick(client, 1, 1000);

    f.world.get<PlayerInput>(f.viewer).current.flags = net::InputAttack;
    f.tick(client, 2, 1040);
    CHECK((client.entities().at(client.self().netId).state & net::StateAttacking) != 0);

    f.world.get<PlayerInput>(f.viewer).current.flags = net::InputDefend;
    f.tick(client, 3, 1080);
    const std::uint8_t state = client.entities().at(client.self().netId).state;
    CHECK((state & net::StateDefending) != 0);
    CHECK((state & net::StateAttacking) == 0);
}

TEST(player_visual_flags_round_trip_and_update_independently) {
    Fixture f;
    f.world.add<PlayerVisuals>(f.viewer, PlayerVisuals{
        static_cast<std::uint8_t>(FaceDandelioned | FaceSquareEyes | FaceHasCorruption),
        static_cast<std::uint8_t>(EquipCutter | EquipThirdEye | EquipObserver |
                                  EquipAntennae | EquipTest1),
        static_cast<std::uint32_t>(PlayerRenderPumpkin | PlayerRenderRobot),
        true,
    });
    Afflictions poisoned;
    poisoned.poisonPerSecond = 1;
    poisoned.poisonUntilMillis = 2000;
    f.world.add<Afflictions>(f.viewer, poisoned);
    f.world.add<Dead>(f.viewer);
    f.world.get<PlayerInput>(f.viewer).current.flags = net::InputDefend;

    WorldView client;
    f.tick(client, 1, 1000);
    const RemoteEntity& first = client.entities().at(client.self().netId);
    // Face flags combine stored effects with live poison, death and action.
    CHECK((first.faceFlags & FacePoisoned) != 0);
    CHECK((first.faceFlags & FaceDandelioned) != 0);
    CHECK((first.faceFlags & FaceDeadEyes) != 0);
    CHECK((first.faceFlags & FaceSquareEyes) != 0);
    CHECK((first.faceFlags & FaceDefending) != 0);
    CHECK((first.faceFlags & FaceHasCorruption) != 0);
    CHECK_EQ(first.equipFlags, static_cast<std::uint8_t>(EquipCutter | EquipThirdEye |
                                                          EquipObserver | EquipAntennae |
                                                          EquipTest1));
    CHECK_EQ(first.renderFlags, static_cast<std::uint32_t>(PlayerRenderPumpkin |
                                                            PlayerRenderRobot |
                                                            PlayerRenderGlitch));

    // Visual updates use their own snapshot field: movement and health can
    // remain unchanged while the renderer receives a new face/skin state.
    PlayerVisuals& visuals = f.world.get<PlayerVisuals>(f.viewer);
    visuals.faceFlags = FaceNone;
    visuals.equipFlags = EquipNone;
    visuals.renderFlags = PlayerRenderRobot;
    visuals.glitched = false;
    f.world.get<Afflictions>(f.viewer).poisonUntilMillis = 0;
    f.world.get<PlayerInput>(f.viewer).current.flags = net::InputAttack;
    f.world.remove<Dead>(f.viewer);
    f.tick(client, 2, 1040);
    const RemoteEntity& second = client.entities().at(client.self().netId);
    CHECK((second.faceFlags & FaceAttacking) != 0);
    CHECK((second.faceFlags & FaceDefending) == 0);
    CHECK((second.faceFlags & (FacePoisoned | FaceDeadEyes | FaceDandelioned |
                               FaceSquareEyes | FaceHasCorruption)) == 0);
    CHECK_EQ(second.equipFlags, std::uint8_t(EquipNone));
    CHECK_EQ(second.renderFlags, std::uint32_t(PlayerRenderRobot));
}

TEST(entities_leaving_view_are_removed) {
    Fixture f;
    const Entity mob = f.addMob({1100, 1000});
    WorldView client;
    f.tick(client, 1, 1000);
    CHECK_EQ(client.entities().size(), std::size_t(2));

    // Well beyond the viewport plus margin.
    f.world.get<Transform>(mob).position = {40000, 40000};
    f.tick(client, 2, 1040);
    CHECK_EQ(client.entities().size(), std::size_t(1));
    CHECK(client.entities().count(netIdOf(f.world, mob)) == 0);

    // Coming back re-spawns it, with its immutable data intact.
    f.world.get<Transform>(mob).position = {1100, 1000};
    f.tick(client, 3, 1080);
    CHECK_EQ(client.entities().size(), std::size_t(2));
    CHECK_EQ(client.entities().at(netIdOf(f.world, mob)).typeIndex, std::uint16_t(7));
}

TEST(destroyed_entities_are_removed) {
    Fixture f;
    const Entity mob = f.addMob({1100, 1000});
    const std::uint32_t id = netIdOf(f.world, mob);
    WorldView client;
    f.tick(client, 1, 1000);

    f.world.destroy(mob);
    f.tick(client, 2, 1040);
    CHECK(client.entities().count(id) == 0);
}

TEST(net_ids_are_never_reused) {
    Fixture f;
    const Entity first = f.addMob({1100, 1000});
    const std::uint32_t firstId = netIdOf(f.world, first);
    f.world.destroy(first);
    const Entity second = f.addMob({1100, 1000});
    // The ECS slot is recycled, but the wire id must not be, or a client that
    // has not yet processed the removal would update the wrong entity.
    CHECK(netIdOf(f.world, second) != firstId);
}

TEST(snapshot_is_capped_and_keeps_the_nearest) {
    Fixture f;
    f.replicator.maxEntities = 10;
    for (int i = 0; i < 60; ++i) {
        f.addMob({1000 + static_cast<double>(i) * 5.0, 1000});
    }
    WorldView client;
    f.tick(client, 1, 1000);
    CHECK(client.entities().size() <= std::size_t(10));

    // The nearest mob must have survived the cull; the furthest must not.
    bool sawNear = false;
    for (const auto& entry : client.entities()) {
        if (entry.second.kind != net::EntityKind::Mob) continue;
        CHECK(entry.second.targetPosition.x < 1000 + 60 * 5.0);
        if (entry.second.targetPosition.x < 1020) sawNear = true;
    }
    CHECK(sawNear);
}

TEST(events_are_scoped_to_what_the_client_can_see) {
    Fixture f;
    const Entity mob = f.addMob({1100, 1000});
    WorldView client;
    f.tick(client, 1, 1000);
    client.events().clear();

    f.events.damage(netIdOf(f.world, mob), 37, {1100, 1000}, Realm::Overworld);
    f.events.damage(999, 12, {50000, 50000}, Realm::Overworld);   // far away
    f.tick(client, 2, 1040);

    CHECK_EQ(client.events().size(), std::size_t(1));
    CHECK_EQ(static_cast<int>(client.events()[0].kind), static_cast<int>(net::EventKind::Damage));
    CHECK_NEAR(client.events()[0].amount, 37.0, 0.01);
}

TEST(a_lightning_strike_carries_its_bolts_and_the_next_event_still_decodes) {
    Fixture f;
    WorldView client;
    f.tick(client, 1, 1000);
    client.events().clear();

    // A strike, then an ordinary event behind it. The second one is the point:
    // Lightning is the only kind with a variable tail, and a reader that does
    // not consume it reads the tail as the next event's header.
    f.events.lightning({1000, 1000}, 1000.0, Realm::Overworld,
                       {{1100, 1000}, {900, 1050}, {1000, 1200}});
    f.events.damage(4242, 9, {1010, 1010}, Realm::Overworld);
    f.tick(client, 2, 1040);

    CHECK_EQ(client.events().size(), std::size_t(2));
    const ViewEvent& strike = client.events()[0];
    CHECK_EQ(static_cast<int>(strike.kind), static_cast<int>(net::EventKind::Lightning));
    CHECK_NEAR(strike.position.x, 1000.0, 0.05);
    CHECK_NEAR(strike.radius, 1000.0, 0.05);
    CHECK_EQ(strike.points.size(), std::size_t(3));
    CHECK_NEAR(strike.points[0].x, 1100.0, 0.05);
    CHECK_NEAR(strike.points[2].y, 1200.0, 0.05);

    CHECK_EQ(static_cast<int>(client.events()[1].kind), static_cast<int>(net::EventKind::Damage));
    CHECK_EQ(client.events()[1].netId, std::uint32_t(4242));
    CHECK(client.events()[1].points.empty());
}

TEST(a_strike_reports_no_more_bolts_than_the_wire_cap) {
    Fixture f;
    WorldView client;
    f.tick(client, 1, 1000);
    client.events().clear();

    std::vector<Vec2> targets;
    for (std::size_t i = 0; i < net::kMaxLightningTargets + 8; ++i) {
        targets.push_back({1000.0 + static_cast<double>(i), 1000.0});
    }
    f.events.lightning({1000, 1000}, 1000.0, Realm::Overworld, targets);
    f.tick(client, 2, 1040);

    CHECK_EQ(client.events().size(), std::size_t(1));
    CHECK_EQ(client.events()[0].points.size(), net::kMaxLightningTargets);
}

TEST(truncated_snapshot_is_rejected_wholesale) {
    Fixture f;
    f.addMob({1100, 1000});
    ByteWriter out;
    Replicator::Frame frame;
    frame.tick = 1;
    frame.nowMillis = 1000;
    f.replicator.build(f.world, f.viewer, f.view, frame, out);

    WorldView client;
    // Chop the payload short: the decoder must apply nothing rather than
    // leaving the view half-populated.
    ByteReader reader(out.data(), out.size() - 12);
    reader.u8();
    CHECK(!client.applySnapshot(reader));
    CHECK_EQ(client.entities().size(), std::size_t(0));
}

TEST(stale_snapshots_are_ignored) {
    Fixture f;
    const Entity mob = f.addMob({1100, 1000});
    WorldView client;
    f.tick(client, 5, 1000);

    f.world.get<Transform>(mob).position.x = 1300;
    f.tick(client, 6, 1040);
    CHECK_NEAR(client.entities().at(netIdOf(f.world, mob)).targetPosition.x, 1300, 0.05);

    // Replaying an older tick must not rewind the world.
    ByteWriter out;
    Replicator::Frame frame;
    frame.tick = 3;
    frame.nowMillis = 900;
    ClientView fresh;
    f.replicator.build(f.world, f.viewer, fresh, frame, out);
    ByteReader reader(out.data(), out.size());
    reader.u8();
    client.applySnapshot(reader);
    CHECK_EQ(client.tick(), std::uint32_t(6));
}

// ---------------------------------------------------------------------------
// Interpolation
// ---------------------------------------------------------------------------

namespace {

/// A view holding one flower at the origin whose server position is `target`.
/// Built by hand rather than through the replicator: these tests are about the
/// ease law, and seeding it directly is what lets them assert exact numbers.
RemoteEntity easedFlower(Vec2 drawn, Vec2 target) {
    RemoteEntity e;
    e.netId = 1;
    e.kind = net::EntityKind::Player;
    e.position = drawn;
    e.targetPosition = target;
    e.needsSnap = false;
    return e;
}

} // namespace

TEST(the_ease_rate_is_frame_rate_independent) {
    // Two frames of half the step must land where one whole one does, or the
    // flower's speed would depend on the frame rate.
    const double rate = easeRateFromAmount(kDefaultInterpolationAmount);
    const double whole = easeAmount(rate, 1.0 / 30.0);
    const double half = easeAmount(rate, 1.0 / 60.0);
    CHECK_NEAR(1.0 - (1.0 - half) * (1.0 - half), whole, 1e-12);

    // And the amount is the browser build's definition: the fraction of the
    // gap closed in one 60 fps frame.
    CHECK_NEAR(easeAmount(easeRateFromAmount(0.3), 1.0 / 60.0), 0.3, 1e-12);
}

TEST(a_stalled_frame_does_not_become_a_snap) {
    // A resize or a breakpoint hands back a dt of seconds. Easing the whole
    // gap on the frame the window resumes is a teleport on screen.
    const double rate = easeRateFromAmount(kDefaultInterpolationAmount);
    CHECK(easeAmount(rate, 30.0) < 0.999);
    CHECK_NEAR(easeAmount(rate, 30.0), easeAmount(rate, 0.1), 1e-12);
}

TEST(a_flower_eases_toward_the_server_and_never_past_it) {
    WorldView view;
    view.seedForTest(easedFlower({0, 0}, {100, 0}));
    const double rate = view.easeRatePerSecond;

    view.interpolate(1000, 1.0 / 60.0);
    const double afterOne = view.entities().at(1).position.x;
    CHECK_NEAR(afterOne, 100.0 * easeAmount(rate, 1.0 / 60.0), 1e-9);

    // Asymptotic, so it approaches without ever overshooting -- an overshoot
    // is what makes a corrected position visibly spring.
    for (int i = 0; i < 600; ++i) view.interpolate(1000, 1.0 / 60.0);
    CHECK(view.entities().at(1).position.x <= 100.0);
    CHECK_NEAR(view.entities().at(1).position.x, 100.0, 0.01);
}

TEST(a_flower_cuts_rather_than_glides_across_a_teleport) {
    WorldView view;
    view.seedForTest(easedFlower({0, 0}, {0, kTeleportSnapDistance + 1}));
    view.interpolate(1000, 1.0 / 60.0);
    // A portal, a respawn or the maze at (200000, 200000): easing would drag
    // the flower across every screen in between.
    CHECK_NEAR(view.entities().at(1).position.y, kTeleportSnapDistance + 1, 1e-9);
}

TEST(a_flower_settles_exactly_instead_of_asymptoting) {
    WorldView view;
    view.seedForTest(easedFlower({0, 0}, {kSettleEpsilon / 2, 0}));
    view.interpolate(1000, 1.0 / 60.0);
    CHECK_NEAR(view.entities().at(1).position.x, kSettleEpsilon / 2, 1e-12);
}

TEST(a_flowers_facing_comes_straight_off_the_wire) {
    // It drives the eyes. Easing it makes the pupils swim behind the cursor.
    WorldView view;
    RemoteEntity e = easedFlower({0, 0}, {0, 0});
    e.angle = 0;
    e.targetAngle = kPi / 2;
    view.seedForTest(e);
    view.interpolate(1000, 1.0 / 60.0);
    CHECK_NEAR(view.entities().at(1).angle, kPi / 2, 1e-12);
}

TEST(a_mobs_facing_eases_instead_of_snapping) {
    // Passive AI turns up to 180 degrees in one server step.
    WorldView view;
    RemoteEntity e;
    e.netId = 2;
    e.kind = net::EntityKind::Mob;
    e.needsSnap = false;
    e.angle = 0;
    e.targetAngle = kPi / 2;
    view.seedForTest(e);
    view.interpolate(1000, 1.0 / 60.0);
    const double after = view.entities().at(2).angle;
    CHECK(after > 0.0);
    CHECK(after < kPi / 2);
}

TEST(mob_playback_runs_behind_the_render_clock) {
    WorldView view;
    RemoteEntity e;
    e.netId = 3;
    e.kind = net::EntityKind::Mob;
    e.needsSnap = false;
    e.targetPosition = {100, 0};
    // Two samples 50 ms apart, so the midpoint of the pair is at t = 1025.
    e.samples.push_back({1000, {0, 0}});
    e.samples.push_back({1050, {100, 0}});
    view.seedForTest(e);

    // Render at 1025 + the delay: playback lands on the midpoint.
    view.interpolate(1025 + kMobRenderDelayMillis, 1.0 / 60.0);
    CHECK_NEAR(view.entities().at(3).position.x, 50.0, 1e-9);
}

TEST(mob_playback_extrapolates_at_most_one_sample_when_starved) {
    WorldView view;
    RemoteEntity e;
    e.netId = 4;
    e.kind = net::EntityKind::Mob;
    e.needsSnap = false;
    e.samples.push_back({1000, {0, 0}});
    e.samples.push_back({1050, {100, 0}});
    view.seedForTest(e);

    // Far past the newest sample: it guesses one span forward and stops, which
    // rides out a late packet without sending the mob through a wall.
    view.interpolate(9000 + kMobRenderDelayMillis, 1.0 / 60.0);
    CHECK_NEAR(view.entities().at(4).position.x, 200.0, 1e-9);
}

TEST(a_mob_with_no_history_eases_rather_than_freezing) {
    // One sample cannot bracket anything. Freezing on it would stall every mob
    // for the whole playback delay after it comes into view.
    WorldView view;
    RemoteEntity e;
    e.netId = 5;
    e.kind = net::EntityKind::Mob;
    e.needsSnap = false;
    e.targetPosition = {100, 0};
    e.samples.push_back({1000, {0, 0}});
    view.seedForTest(e);
    view.interpolate(1000, 1.0 / 60.0);
    CHECK(view.entities().at(5).position.x > 0.0);
}

TEST(a_freshly_spawned_entity_does_not_slide_in) {
    Fixture f;
    const Entity mob = f.addMob({1200, 1000});
    WorldView client;
    f.tick(client, 1, 1000);
    client.interpolate(99999, 1.0 / 60.0);
    // Drawn where it spawned rather than sliding in from wherever a recycled
    // record left the slot.
    CHECK_NEAR(client.entities().at(netIdOf(f.world, mob)).position.x, 1200, 0.05);
}

TEST(the_viewers_flower_eases_and_the_camera_gets_one_answer) {
    Fixture f;
    WorldView client;
    f.tick(client, 1, 1000);
    // The join snaps: there is no continuity to preserve.
    client.interpolate(1000, 1.0 / 60.0);
    const Vec2 spawned = client.selfDrawnPosition();
    CHECK_NEAR(spawned.x, client.self().position.x, 1e-9);

    f.world.get<Transform>(f.viewer).position.x += 100;
    f.tick(client, 2, 1040);
    client.interpolate(1040, 1.0 / 60.0);

    // Eased, not cut: the camera is pinned to this, so anything that jumped
    // here would jolt the whole world.
    const Vec2 drawn = client.selfDrawnPosition();
    CHECK(drawn.x > spawned.x);
    CHECK(drawn.x < client.self().position.x);
    // And the self ENTITY reads the same, so the petal ring anchored to it
    // cannot drift off the body by a fraction of a pixel.
    CHECK_NEAR(client.entities().at(client.self().netId).position.x, drawn.x, 1e-12);
}

TEST(snapAll_cuts_every_entity_onto_the_server_position) {
    WorldView view;
    view.seedForTest(easedFlower({0, 0}, {100, 0}));
    view.snapAll();
    view.interpolate(1000, 1.0 / 60.0);
    CHECK_NEAR(view.entities().at(1).position.x, 100.0, 1e-12);
}

TEST(a_rigid_ring_stays_rigid_while_its_flower_walks) {
    // THE petal-jitter regression. A petal's drawn place in its flower's ring
    // must depend only on where the server put it relative to that flower --
    // never on the flower's own interpolation state.
    //
    // Snapshots arrive at 20 Hz and frames are drawn faster, so anything
    // derived from a raw authoritative position is a staircase. The renderer
    // used to add `owner.position - owner.targetPosition` to every petal to
    // re-anchor the ring; that is a smooth value minus a staircase, i.e. a
    // sawtooth, and it shook every petal at the beat between the two rates.
    //
    // Here the ring is deliberately RIGID -- the offset never changes -- while
    // the flower walks in snapshot-sized steps. Any motion in the drawn offset
    // is manufactured by the client.
    WorldView view;

    RemoteEntity owner;
    owner.netId = 1;
    owner.kind = net::EntityKind::Player;
    owner.position = owner.targetPosition = {1000, 1000};
    owner.needsSnap = false;
    view.seedForTest(owner);

    const Vec2 ring{60, 0};
    RemoteEntity petal;
    petal.netId = 2;
    petal.kind = net::EntityKind::Petal;
    petal.ownerNetId = 1;
    petal.position = petal.targetPosition = owner.targetPosition + ring;
    petal.ownerOffset = ring;
    petal.needsSnap = false;
    view.seedForTest(petal);

    // Three frames per snapshot, which is the ratio that produced the visible
    // shake: the staircase and the frame clock do not divide evenly.
    Vec2 walked = owner.targetPosition;
    double worstDrift = 0;
    for (int snapshot = 0; snapshot < 40; ++snapshot) {
        walked += {5.0, 0};                       // one 20 Hz step of travel
        view.setTargetForTest(1, walked);
        view.setTargetForTest(2, walked + ring);
        for (int frame = 0; frame < 3; ++frame) {
            view.interpolate(1000 + snapshot * 50.0 + frame * 16.7, 1.0 / 60.0);
            const Vec2 drawn = view.entities().at(2).position - view.entities().at(1).position;
            // Skip the first snapshot: the ease is still settling from the seed.
            if (snapshot > 0) worstDrift = std::max(worstDrift, distance(drawn, ring));
        }
    }
    CHECK_NEAR(worstDrift, 0.0, 1e-9);
}

TEST(a_petal_ring_is_smoothed_in_its_flowers_frame_not_in_the_world) {
    // The ring's own motion still has to be interpolated -- it arrives at the
    // snapshot rate like everything else -- but in the flower's frame, so
    // smoothing a rotation cannot also smear the flower's translation into it.
    WorldView view;

    RemoteEntity owner;
    owner.netId = 1;
    owner.kind = net::EntityKind::Player;
    owner.position = owner.targetPosition = {1000, 1000};
    owner.needsSnap = false;
    view.seedForTest(owner);

    RemoteEntity petal;
    petal.netId = 2;
    petal.kind = net::EntityKind::Petal;
    petal.ownerNetId = 1;
    petal.position = petal.targetPosition = {1060, 1000};
    petal.ownerOffset = {60, 0};
    petal.needsSnap = false;
    view.seedForTest(petal);

    // The ring swings a quarter turn in one step while the flower stands still.
    view.setTargetForTest(2, {1000, 1060});
    view.interpolate(1000, 1.0 / 60.0);

    const Vec2 drawn = view.entities().at(2).position - view.entities().at(1).position;
    // Part of the way round, not all of it and not none of it.
    CHECK(drawn.y > 0.0);
    CHECK(drawn.y < 60.0);
    CHECK(drawn.x < 60.0);
    CHECK(drawn.x > 0.0);
}

TEST(a_petal_with_no_owner_on_screen_still_interpolates) {
    // The frame or two before an owner's spawn record lands. Anchoring is not
    // possible; drawing the petal at a stale position is worse than easing it.
    WorldView view;
    RemoteEntity petal;
    petal.netId = 7;
    petal.kind = net::EntityKind::Petal;
    petal.ownerNetId = 999;            // nothing under this id
    petal.position = {0, 0};
    petal.targetPosition = {100, 0};
    petal.needsSnap = false;
    view.seedForTest(petal);
    view.interpolate(1000, 1.0 / 60.0);
    CHECK(view.entities().at(7).position.x > 0.0);
    CHECK(view.entities().at(7).position.x < 100.0);
}

TEST(snapshot_reports_the_viewers_reloading_slots) {
    Fixture f;
    Loadout loadout;
    loadout.slots[0] = LoadoutSlot{0, Rarity::Common, 0.0, false};
    loadout.slots[2] = LoadoutSlot{0, Rarity::Common, 3200.0, true};
    f.world.add<Loadout>(f.viewer, loadout);

    WorldView client;
    f.tick(client, 1, 2000);

    // What is LEFT of the reload, not when it ends: the bar counts down from
    // it on its own clock between snapshots.
    CHECK_NEAR(client.self().slotReloadRemainingMillis[2], 1200.0, 1.0);
    CHECK_EQ(client.self().slotReloadRemainingMillis[0], 0.0);
    CHECK_EQ(client.self().slotReloadRemainingMillis[1], 0.0);

    // The frame clock carries it forward, so the wedge sweeps at display rate
    // rather than in twenty steps a second.
    client.interpolate(2000, 0.2);
    CHECK_NEAR(client.self().slotReloadRemainingMillis[2], 1000.0, 1.0);
}

TEST(a_slot_that_finished_reloading_drops_off_the_wire) {
    // Streamed state, not a one-shot event: the wedge clears because the slot
    // stops being reported, so a client that missed a frame is not left with a
    // petal that sweeps forever.
    Fixture f;
    Loadout loadout;
    loadout.slots[2] = LoadoutSlot{0, Rarity::Common, 3200.0, true};
    f.world.add<Loadout>(f.viewer, loadout);

    WorldView client;
    f.tick(client, 1, 2000);
    CHECK(client.self().slotReloadRemainingMillis[2] > 0.0);

    LoadoutSlot& slot = f.world.get<Loadout>(f.viewer).slots[2];
    slot.broken = false;
    slot.reloadReadyAtMillis = 0;
    f.tick(client, 2, 3300);
    CHECK_EQ(client.self().slotReloadRemainingMillis[2], 0.0);
}

TEST(an_untouched_loadout_costs_the_snapshot_one_byte) {
    // The list leads with a count so the overwhelmingly common case -- nothing
    // broken and nothing hurt -- is a single zero rather than an entry per slot.
    Fixture f;
    WorldView client;
    const std::size_t bare = f.tick(client, 1, 1000);

    Fixture g;
    Loadout loadout;
    loadout.slots[1] = LoadoutSlot{0, Rarity::Common, 2000.0, true};
    g.world.add<Loadout>(g.viewer, loadout);
    WorldView other;
    const std::size_t withOne = g.tick(other, 1, 1000);

    CHECK_EQ(withOne - bare, std::size_t(4));
}

TEST(a_clump_reports_a_reload_while_any_grain_is_missing) {
    // Sand, light and dahlia break one grain at a time, so `LoadoutSlot::broken`
    // -- which only goes up once the LAST one is gone -- is the wrong signal for
    // the bar. One missing grain is a reload the wedge should be sweeping.
    Fixture f;
    Loadout loadout;
    loadout.slots[3] = LoadoutSlot{0, Rarity::Common, 0.0, false};
    f.world.add<Loadout>(f.viewer, loadout);

    PetalSlotState slotState;
    PetalSlotState::Slot& clump = slotState.slots[3];
    clump.independent = true;
    clump.populated = true;
    // Four grains: two out, two still orbiting.
    clump.instanceReadyAtMillis = {2600.0, 0.0, 2200.0, 0.0};
    f.world.add<PetalSlotState>(f.viewer, slotState);

    WorldView client;
    f.tick(client, 1, 2000);

    // Sized by the grain that comes back LAST. The soonest would finish the
    // sweep with a grain still missing and then snap it backwards.
    CHECK_NEAR(client.self().slotReloadRemainingMillis[3], 600.0, 1.0);
}

TEST(a_whole_clump_back_on_the_field_clears_the_wedge) {
    Fixture f;
    Loadout loadout;
    loadout.slots[3] = LoadoutSlot{0, Rarity::Common, 0.0, false};
    f.world.add<Loadout>(f.viewer, loadout);

    PetalSlotState slotState;
    PetalSlotState::Slot& clump = slotState.slots[3];
    clump.independent = true;
    clump.populated = true;
    clump.instanceReadyAtMillis = {2600.0, 0.0, 2200.0, 0.0};
    f.world.add<PetalSlotState>(f.viewer, slotState);

    WorldView client;
    f.tick(client, 1, 2000);
    CHECK(client.self().slotReloadRemainingMillis[3] > 0.0);

    // Respawning a grain zeroes its entry, which is what ends the sweep.
    for (double& ready : f.world.get<PetalSlotState>(f.viewer).slots[3].instanceReadyAtMillis) {
        ready = 0.0;
    }
    f.tick(client, 2, 2700);
    CHECK_EQ(client.self().slotReloadRemainingMillis[3], 0.0);
}

TEST(a_hurt_slot_reaches_the_client_without_being_broken) {
    // Damage and reload travel in one entry, so a petal that is merely hurt is
    // reported even though nothing about it is on cooldown.
    Fixture f;
    Loadout loadout;
    loadout.slots[1] = LoadoutSlot{0, Rarity::Common, 0.0, false};
    f.world.add<Loadout>(f.viewer, loadout);

    PetalSlotState slotState;
    slotState.slots[1].healthFraction = 0.4;
    f.world.add<PetalSlotState>(f.viewer, slotState);

    WorldView client;
    f.tick(client, 1, 2000);

    // A byte of ratio, so a step of 1/255 is the most it can be out.
    CHECK_NEAR(client.self().slotHealthFraction[1], 0.4, 1.0 / 255.0);
    CHECK_EQ(client.self().slotReloadRemainingMillis[1], 0.0);
    // Everything else reads as untouched rather than as empty.
    CHECK_EQ(client.self().slotHealthFraction[0], 1.0);
}

TEST(a_healed_slot_stops_being_reported_and_reads_full_again) {
    Fixture f;
    Loadout loadout;
    loadout.slots[1] = LoadoutSlot{0, Rarity::Common, 0.0, false};
    f.world.add<Loadout>(f.viewer, loadout);

    PetalSlotState slotState;
    slotState.slots[1].healthFraction = 0.4;
    f.world.add<PetalSlotState>(f.viewer, slotState);

    WorldView client;
    f.tick(client, 1, 2000);
    CHECK(client.self().slotHealthFraction[1] < 1.0);

    f.world.get<PetalSlotState>(f.viewer).slots[1].healthFraction = 1.0;
    f.tick(client, 2, 2050);
    CHECK_EQ(client.self().slotHealthFraction[1], 1.0);
}
