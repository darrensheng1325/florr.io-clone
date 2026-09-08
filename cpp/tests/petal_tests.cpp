#include "test.h"

#include "server/systems/petals.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace flix;

namespace {

// The ring is driven entirely by content, so these tests run on content of
// their own: a handful of petals chosen to isolate one rule each. Using the
// shipped data instead would tie every assertion to a balance number that is
// allowed to change.

const char* const kPetalsJson = R"JSON({
  "basic":    {"name":"Basic","damage":10,"health":10,"size":2,"cooldown":1200,"count":1,"color":"#90EE90"},
  "sandy":    {"name":"Sandy","damage":4,"health":12,"size":1,"cooldown":800,"count":4,"clumped":true,"color":"#8B0000"},
  "shards":   {"name":"Shards","damage":3,"health":6,"size":1,"cooldown":500,"count":3,"independentHealth":true,"color":"#CCCCCC"},
  "rock":     {"name":"Rock","damage":1,"size":2,"cooldown":1000,"count":1,"color":"#777777"},
  "healer":   {"name":"Healer","damage":1,"health":5,"size":1,"cooldown":3500,"count":1,"burstHeal":10,"burstHealChargeMs":1000,"defendOnly":true,"color":"#FF69B4"},
  "shell":    {"name":"Shell","damage":1,"health":5,"size":1,"cooldown":3500,"count":1,"burstShield":22,"burstHealChargeMs":1000,"defendOnly":true,"color":"#FCDD86"},
  "bubble":   {"name":"Bubble","damage":0,"health":1,"size":1,"cooldown":1000,"count":1,"color":"#FFFFFF"},
  "web":      {"name":"Web","damage":5,"health":10,"size":1.2,"cooldown":3000,"count":1,"defendOnly":true,"webRadius":90,"color":"#FFFFFF"},
  "pollen":   {"name":"Pollen","damage":10,"health":10,"size":1,"cooldown":1000,"count":1,"color":"#FFE763"},
  "sponge":   {"name":"Sponge","damage":10,"health":10,"size":1,"cooldown":2000,"count":1,"spongeDamageDuration":1000,"color":"#FF96E0"},
  "peas":     {"name":"Peas","damage":6,"health":5,"size":1,"cooldown":1000,"count":1,"projectile":{"count":3,"spreadAngle":0.5,"speed":800,"distance":1000},"color":"#00FF00"},
  "peaclump": {"name":"Peaclump","damage":6,"health":5,"size":1,"cooldown":1000,"count":4,"clumped":true,"projectile":{"count":1,"spreadAngle":0,"speed":800,"distance":1000},"color":"#00FF00"},
  "lucky":    {"name":"Lucky","damage":1,"health":5,"size":1,"cooldown":2000,"count":1,"playerModifiers":{"luck":2,"speed":1.5,"magnetism":50},"color":"#FFD700"},
  "reacher":  {"name":"Reacher","damage":1,"health":5,"size":1,"cooldown":2000,"count":1,"playerModifiers":{"range":1.5},"color":"#00FFFF"},
  "anchor":   {"name":"Anchor","damage":1,"health":5,"size":1,"cooldown":1000,"count":1,"playerModifiers":{"rotationSpeed":0},"color":"#888888"},
  "inflator": {"name":"Inflator","damage":1,"health":5,"size":1,"cooldown":2000,"count":1,"playerModifiers":{"playerRadius":1.5},"color":"#FF00FF"},
  "summoner": {"name":"Summoner","damage":1,"health":4,"size":1,"cooldown":1000,"count":1,"petMobType":"critter","petMobRarity":"common","petCount":2,"color":"#AA00AA"},
  "toxic":    {"name":"Toxic","damage":2,"health":5,"size":1,"cooldown":1000,"count":1,"poison":0.05,"poisonDuration":3000,"color":"#00AA00"}
})JSON";

const char* const kMobsJson = R"JSON({
  "critter": {"name":"Critter","health":10,"damage":1,"size":1,"speed":1,"range":300,"cooldown":500,"color":"#FF0000","section":[0],"ai_type":"hostile"},
  "brute":   {"name":"Brute","health":500,"damage":5,"size":6,"speed":1,"range":300,"cooldown":500,"color":"#AA3300","section":[0],"ai_type":"hostile"}
})JSON";

std::string tempDir() {
    const char* env = std::getenv("TMPDIR");
    std::string base = (env != nullptr && *env != '\0') ? env : "/tmp";
    if (base.back() != '/') base.push_back('/');
    base += "flix_petal_tests";
    mkdir(base.c_str(), 0755);   // already there is fine
    return base;
}

bool writeText(const std::string& path, const char* text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return out.good();
}

struct Fixture {
    ContentRegistry registry;
    bool ok = false;
    std::string error;
};

const Fixture& fixture() {
    static const Fixture state = [] {
        Fixture f;
        const std::string dir = tempDir();
        const std::string mobs = dir + "/mobs.json";
        const std::string petals = dir + "/petals.json";
        if (!writeText(mobs, kMobsJson) || !writeText(petals, kPetalsJson)) {
            f.error = "could not write the fixture content into " + dir;
            return f;
        }
        // No XP table: mob XP is irrelevant here and loadFiles tolerates it.
        f.ok = f.registry.loadFiles(mobs, petals, "", f.error);
        return f;
    }();
    return state;
}

bool contentLoaded() {
    if (!fixture().ok) {
        ::testing::reportFailure(__FILE__, __LINE__,
                                 "fixture content failed to load: " + fixture().error);
    }
    return fixture().ok;
}

std::uint16_t petalId(const char* id) { return fixture().registry.petalIndex(id); }

/// A world with one player, and the system driven at the real tick rate.
struct Rig {
    World world;
    CommandBuffer commands{world};
    PetalSystem system;
    Entity player = NULL_ENTITY;
    // Deliberately not zero: a timer that was never set reads as 0, and a
    // clock starting there would make that bug look like a working one.
    double now = 1000.0;

    Rig() {
        player = world.create();
        world.add<PlayerTag>(player);
        world.add<Transform>(player, Transform{{1000.0, 1000.0}, 0.0});
        world.add<Motion>(player);
        world.add<Knockback>(player);
        world.add<Body>(player, Body{kPlayerBaseRadius, 1.0});
        world.add<Health>(player, Health{maxHealthForLevel(1), maxHealthForLevel(1), 0.0, 0.0});
        world.add<Faction>(player, Faction{Team::Players, false});
        world.add<PlayerProgress>(player, PlayerProgress{});
        world.add<PlayerInput>(player);
        world.add<PlayerModifiers>(player);
        world.add<PlayerLocation>(player);
        world.add<Loadout>(player);
        world.add<PetalRing>(player);
        world.add<Afflictions>(player);
        world.add<HitCooldowns>(player);
    }

    void equip(int slot, const char* id, Rarity rarity = Rarity::Common) {
        LoadoutSlot& s = world.get<Loadout>(player).slots[static_cast<std::size_t>(slot)];
        s.configIndex = petalId(id);
        s.rarity = rarity;
    }

    void unequip(int slot) {
        LoadoutSlot& s = world.get<Loadout>(player).slots[static_cast<std::size_t>(slot)];
        s.configIndex = kNoPetal;
    }

    void setFlags(std::uint8_t flags) { world.get<PlayerInput>(player).current.flags = flags; }

    /// Stops the ring turning, by equipping a petal whose rotationSpeed
    /// modifier is zero -- the reference sums those as `+= modifier - 1`, so
    /// one of them cancels the base rate exactly.
    ///
    /// A petal is not welded to its place on the ring: it is SPRUNG toward it
    /// (PETAL_SPRING_FORCE), so on a turning ring it settles into a small fixed
    /// lead and a slightly wider orbit and never sits on its target point. A
    /// test about WHERE the ring puts a petal freezes the ring first and then
    /// lets the spring arrive; a test about the turning itself does not, and
    /// asserts against the steady state instead.
    ///
    /// It costs the LAST active slot, and one place on the ring with it.
    static constexpr int kAnchorSlot = kLoadoutActiveSlots - 1;
    void freezeRing() { equip(kAnchorSlot, "anchor"); }

    /// Steps until the spring has carried every petal onto its target. With a
    /// frozen ring the target does not move, so this converges to the point
    /// itself rather than to an orbit around it.
    void settleRing(int ticks = 120) { tick(ticks); }

    void tick(int count = 1) {
        for (int i = 0; i < count; ++i) {
            now += net::kTickMillis;
            system.run(world, fixture().registry, now, net::kTickSeconds, commands);
            commands.flush();
        }
    }

    /// Steps until `done` holds, so a test can wait on a reload without
    /// hard-coding how many ticks that is.
    template <class F>
    bool tickUntil(F done, int maxTicks = 2000) {
        for (int i = 0; i < maxTicks; ++i) {
            tick();
            if (done()) return true;
        }
        return false;
    }

    /// Equipping a petal puts its slot on a full reload before the petal ever
    /// appears -- the reference's `updateLoadout` stamps `onCooldown` and a
    /// `cooldownEndTime` on every newly equipped slot, and the spawn loadout is
    /// built the same way. A test whose subject is something else waits that
    /// out here instead of restating each petal's cooldown in ticks.
    void settleEquips(int maxTicks = 400) {
        // One tick first: `broken` reads false until the system has actually
        // seen the edit and stamped the slot's reload, so testing it before
        // that would call an unserved cooldown "ready".
        tick();
        const auto ready = [&] {
            for (int i = 0; i < kLoadoutActiveSlots; ++i) {
                if (slot(i).empty()) continue;
                if (slot(i).broken) return false;
            }
            return true;
        };
        if (ready()) return;
        if (!tickUntil(ready, maxTicks)) {
            ::testing::reportFailure(__FILE__, __LINE__,
                                     "the equip reload never finished");
        }
    }

    const LoadoutSlot& slot(int index) const {
        return const_cast<World&>(world).get<Loadout>(player).slots[static_cast<std::size_t>(index)];
    }

    /// What the loadout bar drains its tile by: how much of the slot is still
    /// standing, whichever of the two health models it runs on.
    double slotHealth(int index) const {
        return const_cast<World&>(world)
            .get<PetalSlotState>(player)
            .slots[static_cast<std::size_t>(index)]
            .healthFraction;
    }

    const PetalRing& ring() const { return const_cast<World&>(world).get<PetalRing>(player); }
    const PlayerModifiers& modifiers() const {
        return const_cast<World&>(world).get<PlayerModifiers>(player);
    }

    std::vector<Entity> petals(int slotIndex = -1) {
        std::vector<Entity> out;
        Query<PetalTag, PetalInstance> query{world};
        query.each([&](Entity e, PetalTag&, PetalInstance& instance) {
            if (instance.owner != player) return;
            if (slotIndex >= 0 && instance.slot != slotIndex) return;
            out.push_back(e);
        });
        std::sort(out.begin(), out.end(), [&](Entity a, Entity b) {
            return world.get<PetalInstance>(a).subIndex < world.get<PetalInstance>(b).subIndex;
        });
        return out;
    }

    Entity petalWithSub(int slotIndex, int subIndex) {
        for (const Entity e : petals(slotIndex)) {
            if (world.get<PetalInstance>(e).subIndex == subIndex) return e;
        }
        return NULL_ENTITY;
    }

    std::size_t countOf(net::EntityKind kind) {
        std::size_t n = 0;
        Query<Replicated> query{world};
        query.each([&](Entity, Replicated& replicated) {
            if (replicated.kind == kind) ++n;
        });
        return n;
    }

    std::size_t petCount() {
        Query<Pet> query{world};
        return query.count();
    }

    Vec2 position(Entity e) { return world.get<Transform>(e).position; }
    double radiusOf(Entity petal) { return (position(petal) - position(player)).length(); }
    double angleOf(Entity petal) { return (position(petal) - position(player)).angle(); }
    double healthOf(Entity e) { return world.get<Health>(e).current; }
    void damage(Entity e, double amount) { world.get<Health>(e).current -= amount; }
};

/// Angular distance, so a comparison across the -pi/+pi seam is not a failure.
double angularGap(double a, double b) { return std::fabs(wrapAngle(a - b)); }

} // namespace

// ---------------------------------------------------------------------------
// Ring placement
// ---------------------------------------------------------------------------

TEST(petal_ring_places_one_petal_per_slot_evenly) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "basic");
    rig.equip(3, "basic");
    rig.equip(7, "basic");
    rig.freezeRing();
    rig.settleEquips();
    rig.settleRing();

    const std::vector<Entity> petals = rig.petals();
    CHECK_EQ(petals.size(), std::size_t(4));   // three basics and the anchor

    // Slots 0, 3, 7 and 9 are the first, second, third and fourth occupied
    // slots, so they take quarters of the ring regardless of the empty slots
    // between them.
    const double wedge = kTau / 4.0;
    const double spin = rig.ring().spin;
    for (const Entity petal : petals) {
        const int slot = rig.world.get<PetalInstance>(petal).slot;
        const int ordinal = slot == 0 ? 0 : (slot == 3 ? 1 : (slot == 7 ? 2 : 3));
        CHECK_NEAR(angularGap(rig.angleOf(petal), spin + wedge * ordinal), 0.0, 1e-6);
        CHECK_NEAR(rig.radiusOf(petal), rig.ring().radius, 1e-6);
    }
}

TEST(petal_ring_rotates_at_the_spin_rate) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "basic");
    rig.equip(1, "basic");
    rig.settleEquips();
    // No freeze here -- the turning IS the subject. The spring settles into a
    // steady state on the moving ring: a fixed lead angle and a fixed slightly
    // wider orbit, both constant from then on. What must hold in that state is
    // that the petal keeps station with the ring rather than drifting round it.
    rig.settleRing();

    const Entity first = rig.petals(0).front();
    const double before = rig.angleOf(first);
    const double radiusBefore = rig.radiusOf(first);
    const double spinBefore = rig.ring().spin;

    rig.tick(5);
    const double expected = kPetalSpinRate * net::kTickSeconds * 5.0;
    CHECK_NEAR(angularGap(rig.ring().spin, spinBefore + expected), 0.0, 1e-9);
    CHECK_NEAR(angularGap(rig.angleOf(rig.petals(0).front()), before + expected), 0.0, 1e-9);
    CHECK_NEAR(rig.radiusOf(rig.petals(0).front()), radiusBefore, 1e-6);

    // The gap between the two petals is fixed by the layout: rotation must
    // carry the whole ring, not slide one petal along it.
    CHECK_NEAR(angularGap(rig.angleOf(rig.petals(0).front()), rig.angleOf(rig.petals(1).front())),
               kPi, 1e-9);
}

TEST(attacking_and_defending_move_the_ring_smoothly) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "basic");
    rig.settleEquips();

    const double rest = kPetalOrbitRestRadius;
    CHECK_NEAR(rest, 60.0, 1e-9);
    CHECK_NEAR(rig.ring().radius, rest, 1e-9);

    rig.settleRing();
    const double restOrbitRatio = rig.radiusOf(rig.petals(0).front()) / rig.ring().radius;
    CHECK_NEAR(restOrbitRatio, 1.0, 0.01);

    rig.setFlags(net::InputAttack);
    rig.tick();
    const double attackTarget = kPetalOrbitRestRadius * kPetalOrbitAttackExtension;
    CHECK_NEAR(attackTarget, 120.0, 1e-9);
    CHECK_NEAR(rig.ring().targetRadius, attackTarget, 1e-9);
    // Eased: one tick moves toward the target without arriving at it.
    CHECK(rig.ring().radius > rest);
    CHECK(rig.ring().radius < attackTarget);
    const double afterOne = rig.ring().radius;
    rig.tick();
    CHECK(rig.ring().radius > afterOne);

    rig.tick(200);
    CHECK_NEAR(rig.ring().radius, attackTarget, 1e-6);
    // The petal is sprung toward the ring, not welded to it, so on a turning
    // ring it holds station a fixed fraction outside its target point. What
    // matters is that it followed the ring out: it orbits at the same
    // proportion of the extended radius that it did of the resting one.
    CHECK_NEAR(rig.radiusOf(rig.petals(0).front()) / rig.ring().radius, restOrbitRatio, 1e-6);

    rig.setFlags(net::InputDefend);
    rig.tick();
    const double defendTarget = kPetalOrbitRestRadius * kPetalOrbitDefendExtension;
    CHECK_NEAR(defendTarget, 42.0, 1e-9);
    CHECK_NEAR(rig.ring().targetRadius, defendTarget, 1e-9);
    CHECK(rig.ring().radius < attackTarget);
    CHECK(rig.ring().radius > defendTarget);
    rig.tick(200);
    CHECK_NEAR(rig.ring().radius, defendTarget, 1e-6);

    // Both buttons at once is a lunge, not a block. The reference tests the
    // extend button FIRST -- `if (extendPressed) ... else if (retractPressed)`
    // -- and only reaches the retract branch when extend is up, so a player
    // holding both throws the ring out rather than pulling it in.
    rig.setFlags(net::InputAttack | net::InputDefend);
    rig.tick();
    CHECK_NEAR(rig.ring().targetRadius, attackTarget, 1e-9);
}

TEST(a_range_modifier_widens_the_ring) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "reacher");
    rig.tick();
    CHECK_NEAR(rig.ring().targetRadius, kPetalOrbitRestRadius * 1.5, 1e-9);
    rig.tick(200);
    CHECK_NEAR(rig.ring().radius, kPetalOrbitRestRadius * 1.5, 1e-6);
}

TEST(a_player_radius_modifier_scales_the_body_and_preserves_petal_clearance) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "inflator");
    rig.tick();

    const double playerRadius = rig.world.get<Body>(rig.player).radius;
    CHECK_NEAR(playerRadius, kPlayerBaseRadius * 1.5, 1e-9);
    // TypeScript grows the neutral ring from 60 to 70, not to 90: the extra
    // body radius is added once, preserving the gap from the flower's edge.
    CHECK_NEAR(rig.ring().targetRadius,
               kPetalOrbitRestRadius + playerRadius - kPlayerBaseRadius, 1e-9);
}

TEST(a_clumped_slot_spawns_a_cluster_around_one_ring_position) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "sandy");
    rig.freezeRing();
    rig.settleEquips();
    rig.settleRing();

    const std::vector<Entity> grains = rig.petals(0);
    CHECK_EQ(grains.size(), std::size_t(4));

    // All four hang off the SAME ring position -- that is what clumped means.
    const double reach = rig.ring().radius;
    const Vec2 slotPoint = rig.position(rig.player) + Vec2::fromAngle(rig.ring().spin, reach);
    // `effectiveSize * 40 * 0.5` in the reference, which is exactly the
    // petal's own hit radius -- the grains sit one radius out from the shared
    // centre, not one diameter.
    const double spacing = fixture().registry.petalStats(petalId("sandy"), Rarity::Common).radius;

    std::vector<double> subAngles;
    for (const Entity grain : grains) {
        CHECK_NEAR((rig.position(grain) - slotPoint).length(), spacing, 1e-6);
        CHECK_EQ(int(rig.world.get<PetalInstance>(grain).subCount), 4);
        subAngles.push_back((rig.position(grain) - slotPoint).angle());
    }
    // Four distinct directions out of the cluster centre, not four petals piled
    // on the same point.
    std::sort(subAngles.begin(), subAngles.end());
    for (std::size_t i = 1; i < subAngles.size(); ++i) {
        CHECK_NEAR(subAngles[i] - subAngles[i - 1], kTau / 4.0, 1e-6);
    }
}

TEST(an_empty_loadout_places_nothing_and_leaves_modifiers_neutral) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.tick(5);
    CHECK_EQ(rig.petals().size(), std::size_t(0));
    CHECK_NEAR(rig.modifiers().speedScale, 1.0, 1e-12);
    CHECK_NEAR(rig.modifiers().luck, 1.0, 1e-12);
    CHECK_NEAR(rig.ring().radius, kPetalOrbitRestRadius, 1e-9);
}

// ---------------------------------------------------------------------------
// Attraction
// ---------------------------------------------------------------------------

TEST(an_attracted_petal_is_projected_inside_the_body_the_hit_test_uses) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "basic");
    rig.freezeRing();
    rig.settleRing();

    const Entity petal = rig.petals(0).front();
    const double petalRadius = rig.world.get<Body>(petal).radius;

    // A mob that rolled SMALL. Its body is a little over half the tier radius
    // the stat table quotes, and that gap is the whole bug: projecting onto the
    // TIER radius parks the petal outside the circle the melee pass tests, so
    // the ring whips around a mob it can never touch -- no damage out, and no
    // health off the petal either, because a petal pays for its swing only when
    // the swing lands. Nothing else about the capture looks wrong, which is why
    // it read as "petal health sometimes does not go down".
    const std::uint16_t brute = fixture().registry.mobIndex("brute");
    const double statRadius = fixture().registry.mobStats(brute, Rarity::Common).radius;
    const double jitter = 0.55;
    const double bodyRadius = statRadius * jitter;
    // The trap only exists for a roll this small; assert the fixture is in it.
    CHECK(statRadius * kMobOrbitRadiusScale > bodyRadius + petalRadius);

    const Vec2 orbit = rig.world.get<Transform>(petal).position;
    const Entity mob = rig.world.create();
    rig.world.add<MobTag>(mob);
    rig.world.add<MobType>(mob, MobType{brute, Rarity::Common, jitter});
    rig.world.add<Transform>(mob, Transform{orbit + Vec2{20.0, 0.0}, 0.0});
    rig.world.add<Body>(mob, Body{bodyRadius, 1.0});
    rig.world.add<Health>(mob, Health{500.0, 500.0, 0.0, 0.0});
    rig.world.add<Faction>(mob, Faction{Team::Hostiles, false});

    rig.tick(60);

    CHECK(rig.world.get<PetalInstance>(petal).attractedTo == mob);
    const double gap = (rig.world.get<Transform>(petal).position -
                        rig.world.get<Transform>(mob).position).length();
    CHECK(gap < bodyRadius + petalRadius);
    // And on the edge of the body it actually has, not buried in its middle.
    CHECK(gap > bodyRadius * 0.5);
}

// ---------------------------------------------------------------------------
// Breaking and reloading
// ---------------------------------------------------------------------------

TEST(a_petal_at_zero_health_breaks_and_returns_on_its_cooldown) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "basic");
    rig.settleEquips();
    CHECK_EQ(rig.petals(0).size(), std::size_t(1));
    CHECK_NEAR(rig.healthOf(rig.petals(0).front()), 10.0, 1e-9);

    rig.damage(rig.petals(0).front(), 10.0);
    const double brokenAt = rig.now + net::kTickMillis;
    rig.tick();

    CHECK(rig.slot(0).broken);
    CHECK_EQ(rig.petals(0).size(), std::size_t(0));
    CHECK_NEAR(rig.slot(0).reloadReadyAtMillis, brokenAt + 1200.0, 1e-9);

    // Not a tick early: the cooldown is the whole cost of losing a petal.
    while (rig.now + net::kTickMillis < rig.slot(0).reloadReadyAtMillis) {
        rig.tick();
        CHECK(rig.slot(0).broken);
        CHECK_EQ(rig.petals(0).size(), std::size_t(0));
    }
    const double readyAt = rig.slot(0).reloadReadyAtMillis;
    rig.tick();
    CHECK(rig.now >= readyAt);
    CHECK(rig.now < readyAt + net::kTickMillis + 1e-9);
    CHECK(!rig.slot(0).broken);
    CHECK_EQ(rig.petals(0).size(), std::size_t(1));
    // It comes back whole, not at whatever health it died with.
    CHECK_NEAR(rig.healthOf(rig.petals(0).front()), 10.0, 1e-9);
}

TEST(partial_damage_does_not_break_a_petal) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "basic");
    rig.settleEquips();
    rig.damage(rig.petals(0).front(), 9.5);
    rig.tick(20);
    CHECK(!rig.slot(0).broken);
    CHECK_EQ(rig.petals(0).size(), std::size_t(1));
    // Damage is kept, not silently healed by the mirror that keeps a cluster
    // in step.
    CHECK_NEAR(rig.healthOf(rig.petals(0).front()), 0.5, 1e-9);
}

TEST(a_clumped_slot_gives_every_grain_its_own_health) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "sandy");
    rig.tick();
    for (const Entity grain : rig.petals(0)) CHECK_NEAR(rig.healthOf(grain), 12.0, 1e-9);

    // `clumped` is the second, and by far the commoner, way the reference
    // declares independent instances: `(clumped || independentHealth) &&
    // count > 1`. A four-grain clump of sand is four petals that share one
    // place on the ring, NOT one health bar shared four ways -- so a hit on
    // one grain stays on that grain.
    rig.damage(rig.petalWithSub(0, 0), 5.0);
    rig.tick();
    CHECK_EQ(rig.petals(0).size(), std::size_t(4));
    CHECK_NEAR(rig.healthOf(rig.petalWithSub(0, 0)), 7.0, 1e-9);
    CHECK_NEAR(rig.healthOf(rig.petalWithSub(0, 1)), 12.0, 1e-9);

    // Two grains hit in the same tick take their own damage and nobody else's.
    rig.damage(rig.petalWithSub(0, 0), 3.0);
    rig.damage(rig.petalWithSub(0, 1), 4.0);
    rig.tick();
    CHECK_EQ(rig.petals(0).size(), std::size_t(4));
    CHECK_NEAR(rig.healthOf(rig.petalWithSub(0, 0)), 4.0, 1e-9);
    CHECK_NEAR(rig.healthOf(rig.petalWithSub(0, 1)), 8.0, 1e-9);
    CHECK(!rig.slot(0).broken);

    // And one grain breaking leaves the other three on the field, which is
    // what a shared pool could never do.
    rig.damage(rig.petalWithSub(0, 0), 4.0);
    rig.tick();
    CHECK_EQ(rig.petals(0).size(), std::size_t(3));
    CHECK(!rig.slot(0).broken);

    CHECK(rig.tickUntil([&] { return rig.petals(0).size() == 4; }));
    CHECK_NEAR(rig.healthOf(rig.petalWithSub(0, 0)), 12.0, 1e-9);
    CHECK_NEAR(rig.healthOf(rig.petalWithSub(0, 1)), 8.0, 1e-9);
}

TEST(independent_health_petals_break_one_at_a_time) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "shards");
    rig.tick();
    CHECK_EQ(rig.petals(0).size(), std::size_t(3));

    // Damage on one shard stays on that shard.
    rig.damage(rig.petalWithSub(0, 0), 4.0);
    rig.tick();
    CHECK_NEAR(rig.healthOf(rig.petalWithSub(0, 0)), 2.0, 1e-9);
    CHECK_NEAR(rig.healthOf(rig.petalWithSub(0, 1)), 6.0, 1e-9);

    rig.damage(rig.petalWithSub(0, 1), 6.0);
    const double brokenAt = rig.now + net::kTickMillis;
    rig.tick();
    // One down, two still out: the slot is not on cooldown.
    CHECK_EQ(rig.petals(0).size(), std::size_t(2));
    CHECK(!rig.slot(0).broken);
    CHECK(rig.petalWithSub(0, 1) == NULL_ENTITY);
    const PetalSlotState& state = rig.world.get<PetalSlotState>(rig.player);
    CHECK_NEAR(state.slots[0].instanceReadyAtMillis[1], brokenAt + 500.0, 1e-9);

    CHECK(rig.tickUntil([&] { return rig.petals(0).size() == 3; }));
    CHECK(!rig.slot(0).broken);
    // The one that came back is whole; the one that was only grazed is not
    // healed by its neighbour's return.
    CHECK_NEAR(rig.healthOf(rig.petalWithSub(0, 1)), 6.0, 1e-9);
    CHECK_NEAR(rig.healthOf(rig.petalWithSub(0, 0)), 2.0, 1e-9);
}

TEST(a_combat_killed_independent_petal_still_pays_its_reload) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "shards");
    rig.tick();

    const Entity shard = rig.petalWithSub(0, 1);
    const double brokenAt = rig.now + net::kTickMillis;
    rig.world.add<Dead>(shard);
    rig.tick();

    CHECK_EQ(rig.petals(0).size(), std::size_t(2));
    const PetalSlotState& state = rig.world.get<PetalSlotState>(rig.player);
    CHECK_NEAR(state.slots[0].instanceReadyAtMillis[1], brokenAt + 500.0, 1e-9);
    rig.tick(5);
    CHECK_EQ(rig.petals(0).size(), std::size_t(2));
}

TEST(an_independent_slot_reads_as_broken_only_when_all_of_it_is_down) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "shards");
    rig.tick();
    for (const Entity shard : rig.petals(0)) rig.damage(shard, 6.0);
    rig.tick();
    CHECK_EQ(rig.petals(0).size(), std::size_t(0));
    CHECK(rig.slot(0).broken);
    CHECK(rig.slot(0).reloadReadyAtMillis > rig.now);

    CHECK(rig.tickUntil([&] { return !rig.slot(0).broken; }));
    CHECK_EQ(rig.petals(0).size(), std::size_t(3));
}

TEST(a_petal_with_no_health_pool_can_never_break) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "rock");
    rig.settleEquips();
    const std::vector<Entity> petals = rig.petals(0);
    CHECK_EQ(petals.size(), std::size_t(1));
    // No Health component at all: an unbreakable petal is not one with zero
    // hit points, which would break on its first tick.
    CHECK(!rig.world.has<Health>(petals.front()));

    rig.tick(200);
    CHECK(!rig.slot(0).broken);
    CHECK_EQ(rig.petals(0).size(), std::size_t(1));
}

TEST(a_broken_petal_leaves_its_gap_open) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "basic");
    rig.equip(1, "basic");
    rig.freezeRing();
    rig.settleEquips();
    rig.settleRing();
    CHECK_EQ(rig.petals().size(), std::size_t(3));   // two basics and the anchor

    rig.damage(rig.petals(0).front(), 10.0);
    rig.tick();
    CHECK_EQ(rig.petals().size(), std::size_t(2));

    // The survivor keeps the third of the ring it already had. Re-spacing the
    // remaining petals would make every break a visible lurch.
    const Entity survivor = rig.petals(1).front();
    rig.settleRing();
    CHECK_NEAR(angularGap(rig.angleOf(survivor), rig.ring().spin + kTau / 3.0), 0.0, 1e-6);
}

TEST(swapping_a_petal_rebuilds_the_slot_from_scratch) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "basic");
    rig.settleEquips();
    rig.damage(rig.petals(0).front(), 8.0);
    rig.tick();
    CHECK_NEAR(rig.healthOf(rig.petals(0).front()), 2.0, 1e-9);

    rig.equip(0, "sandy");
    rig.tick();
    const std::vector<Entity> grains = rig.petals(0);
    CHECK_EQ(grains.size(), std::size_t(4));
    for (const Entity grain : grains) CHECK_NEAR(rig.healthOf(grain), 12.0, 1e-9);
    CHECK(!rig.slot(0).broken);
}

TEST(re_equipping_over_a_broken_slot_clears_its_cooldown) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "basic");
    rig.settleEquips();
    rig.damage(rig.petals(0).front(), 10.0);
    rig.tick();
    CHECK(rig.slot(0).broken);

    rig.equip(0, "shards");
    rig.tick();
    CHECK(!rig.slot(0).broken);
    CHECK_NEAR(rig.slot(0).reloadReadyAtMillis, 0.0, 1e-12);
    CHECK_EQ(rig.petals(0).size(), std::size_t(3));
}

TEST(unequipping_removes_the_petals_immediately) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "sandy");
    rig.tick();
    CHECK_EQ(rig.petals().size(), std::size_t(4));
    rig.unequip(0);
    rig.tick();
    CHECK_EQ(rig.petals().size(), std::size_t(0));
    CHECK(rig.world.get<Loadout>(rig.player).spawned.empty());
}

TEST(death_clears_the_ring_and_respawning_rebuilds_it) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "basic");
    rig.equip(1, "sandy");
    rig.settleEquips();
    CHECK_EQ(rig.petals().size(), std::size_t(5));

    rig.world.get<Health>(rig.player).current = 0.0;
    rig.tick();
    CHECK_EQ(rig.petals().size(), std::size_t(0));
    CHECK(rig.world.get<Loadout>(rig.player).spawned.empty());
    // The reload debt dies with the body: coming back mid-cooldown on every
    // slot would be a harsher punishment than losing the petals was.
    CHECK(!rig.slot(0).broken);

    rig.world.get<Health>(rig.player).current = 100.0;
    rig.tick();
    CHECK_EQ(rig.petals().size(), std::size_t(5));
}

// ---------------------------------------------------------------------------
// Modifiers
// ---------------------------------------------------------------------------

TEST(modifiers_are_summed_from_scratch_and_vanish_when_unequipped) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "lucky");
    rig.tick();
    CHECK_NEAR(rig.modifiers().luck, 3.0, 1e-9);
    CHECK_NEAR(rig.modifiers().speedScale, 1.5, 1e-9);
    CHECK_NEAR(rig.modifiers().magnetism, 50.0, 1e-9);

    // A hundred ticks must not accumulate a hundred bonuses.
    rig.tick(100);
    CHECK_NEAR(rig.modifiers().luck, 3.0, 1e-9);
    CHECK_NEAR(rig.modifiers().speedScale, 1.5, 1e-9);

    rig.equip(1, "lucky");
    rig.tick();
    CHECK_NEAR(rig.modifiers().luck, 5.0, 1e-9);
    CHECK_NEAR(rig.modifiers().speedScale, 1.5 * 1.5, 1e-9);
    CHECK_NEAR(rig.modifiers().magnetism, 100.0, 1e-9);

    rig.unequip(0);
    rig.unequip(1);
    rig.tick();
    CHECK_NEAR(rig.modifiers().luck, 1.0, 1e-12);
    CHECK_NEAR(rig.modifiers().speedScale, 1.0, 1e-12);
    CHECK_NEAR(rig.modifiers().magnetism, kBaseMagnetism, 1e-12);
}

TEST(a_broken_petal_keeps_its_equipment_modifier_while_reloading) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "lucky");
    rig.settleEquips();
    CHECK_NEAR(rig.modifiers().luck, 3.0, 1e-9);

    rig.damage(rig.petals(0).front(), 5.0);
    rig.tick();
    CHECK(rig.slot(0).broken);
    CHECK_NEAR(rig.modifiers().luck, 3.0, 1e-12);
    CHECK_NEAR(rig.modifiers().speedScale, 1.5, 1e-12);

    CHECK(rig.tickUntil([&] { return !rig.slot(0).broken; }));
    CHECK_NEAR(rig.modifiers().luck, 3.0, 1e-9);
}

TEST(a_sponge_defers_damage_only_while_its_body_is_alive) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "sponge", Rarity::Rare);
    rig.settleEquips();
    CHECK_NEAR(rig.modifiers().spongeDamageDurationMillis, 2000.0, 1e-9);

    rig.damage(rig.petals(0).front(), 10.0 * petalStatScale(Rarity::Rare));
    rig.tick();
    CHECK(rig.slot(0).broken);
    CHECK_NEAR(rig.modifiers().spongeDamageDurationMillis, 0.0, 1e-12);
}

TEST(a_clump_pays_its_modifier_once_not_once_per_grain) {
    if (!contentLoaded()) return;
    Rig rig;
    // sandy carries no modifiers, so this checks the slot-vs-instance rule
    // through the one number a four-count petal would inflate: the count of
    // contributions, seen here as the range scale staying neutral.
    rig.equip(0, "sandy");
    rig.equip(1, "reacher");
    rig.settleEquips();
    CHECK_EQ(rig.petals().size(), std::size_t(5));
    CHECK_NEAR(rig.modifiers().rangeScale, 1.5, 1e-9);
}

TEST(rarity_scales_a_petals_published_numbers) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "toxic", Rarity::Rare);
    rig.settleEquips();
    const Entity petal = rig.petals(0).front();

    // Rare is two tiers up: a flat 3x per tier on damage and poison.
    const double scale = petalStatScale(Rarity::Rare);
    CHECK_NEAR(rig.world.get<ContactDamage>(petal).amount, 2.0 * scale, 1e-9);
    const PetalEffect& effect = rig.world.get<PetalEffect>(petal);
    CHECK_NEAR(effect.poisonPerSecond, 50.0 * scale, 1e-6);
    // Duration is flat: rarity buys damage, not a longer debuff.
    CHECK_NEAR(effect.poisonDurationMillis, 3000.0, 1e-9);
    CHECK_NEAR(rig.healthOf(petal), 5.0 * scale, 1e-9);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

TEST(a_burst_heal_charges_homes_consumes_and_reloads) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "healer");
    rig.settleEquips();
    rig.world.get<Health>(rig.player).current = 50.0;
    rig.setFlags(net::InputAttack);
    rig.tick(60);   // well past the petal's 1000ms charge

    // Input does not gate a rose: after charging it flies home because health
    // is missing, delivers one burst, and enters its normal break cooldown.
    CHECK_NEAR(rig.world.get<Health>(rig.player).current, 62.0, 1e-9);
    CHECK(rig.slot(0).broken);
    CHECK_EQ(rig.petals(0).size(), std::size_t(0));

    CHECK(rig.tickUntil([&] { return !rig.slot(0).broken; }));
    CHECK_EQ(rig.petals(0).size(), std::size_t(1));
    CHECK(rig.tickUntil([&] { return rig.world.get<Health>(rig.player).current > 70.0; }, 50));
}

TEST(a_shell_homes_grants_one_temporary_shield_and_is_consumed) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "shell");
    rig.settleEquips();
    rig.tick(40);

    const ShieldState& shield = rig.world.get<ShieldState>(rig.player);
    CHECK_NEAR(shield.amount, 22.0, 1e-9);
    CHECK(shield.active(rig.now));
    CHECK(rig.slot(0).broken);
    CHECK_EQ(rig.petals(0).size(), std::size_t(0));
}

TEST(defending_pops_a_bubble_and_pushes_the_flower) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "bubble");
    rig.settleEquips();
    const Vec2 before = rig.position(rig.player);
    rig.setFlags(net::InputDefend);
    rig.tick();
    CHECK_NEAR(distance(before, rig.position(rig.player)), 60.0, 1e-9);
    rig.tick();
    CHECK(rig.slot(0).broken);
    CHECK_EQ(rig.petals(0).size(), std::size_t(0));
}

TEST(attacking_throws_one_web_and_consumes_its_petal) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "web");
    rig.freezeRing();
    rig.settleEquips();
    rig.settleRing();
    rig.setFlags(net::InputAttack);
    rig.tick();
    const Entity spentWeb = rig.petals(0).front();
    const double bearing = rig.angleOf(spentWeb);

    Query<GroundEffect, Transform, Lifetime> fields{rig.world};
    CHECK_EQ(fields.count(), std::size_t(1));
    fields.each([&](Entity, GroundEffect& field, Transform& transform, Lifetime& lifetime) {
        CHECK_EQ(field.kind, GroundEffectKind::Web);
        CHECK_NEAR(field.radius, 90.0, 1e-9);
        CHECK_NEAR(field.slowFactor, 0.5, 1e-12);
        CHECK_NEAR(lifetime.remainingSeconds, 10.0, 1e-12);
        CHECK_NEAR(angularGap((transform.position - rig.position(rig.player)).angle(), bearing),
                   0.0, 1e-6);
        CHECK_NEAR(distance(transform.position, rig.position(rig.player)),
                   rig.radiusOf(spentWeb) + 620.0, 1e-6);
    });
    CHECK(rig.world.has<Dead>(spentWeb));
    rig.tick();
    CHECK(rig.slot(0).broken);
}

TEST(defending_drops_pollen_with_discrete_damage_and_consumes_it) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "pollen", Rarity::Rare);
    rig.settleEquips();
    rig.setFlags(net::InputDefend);
    rig.tick();

    Query<GroundEffect, Lifetime> fields{rig.world};
    // Rare pollen has two independently-consumed instances.
    CHECK_EQ(fields.count(), std::size_t(2));
    fields.each([&](Entity, GroundEffect& field, Lifetime& lifetime) {
        CHECK_EQ(field.kind, GroundEffectKind::Poison);
        CHECK_NEAR(field.damagePerHit, 10.0 * petalStatScale(Rarity::Rare), 1e-9);
        CHECK_NEAR(field.damageIntervalMillis, 500.0, 1e-9);
        CHECK_NEAR(lifetime.remainingSeconds, 5.0, 1e-12);
    });
    CHECK(rig.world.has<Dead>(rig.petals(0).front()));
}

TEST(a_burst_heal_never_overshoots_the_health_bar) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "healer");
    rig.settleEquips();
    rig.world.get<Health>(rig.player).current = 105.0;
    rig.setFlags(net::InputDefend);
    rig.tick(60);
    CHECK_NEAR(rig.world.get<Health>(rig.player).current, 110.0, 1e-9);
}

TEST(a_projectile_petal_fires_its_fan_and_respects_its_cooldown) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "peas");
    rig.freezeRing();
    rig.settleEquips();
    rig.settleRing();
    rig.tick(40);
    // Projectile timers do not advance while the ring is not attacking.
    CHECK_EQ(rig.countOf(net::EntityKind::Projectile), std::size_t(0));

    rig.setFlags(net::InputAttack);
    rig.tick();
    CHECK_EQ(rig.countOf(net::EntityKind::Projectile), std::size_t(3));
    const double nextShotAt =
        rig.world.get<PetalInstance>(rig.petals(0).front()).nextProjectileMillis;
    CHECK_NEAR(nextShotAt, rig.now + 1000.0, 1e-9);

    const double petalHeading = rig.angleOf(rig.petals(0).front());
    std::vector<double> offsets;
    Query<ProjectileTag, Motion, Projectile> shots{rig.world};
    shots.each([&](Entity, ProjectileTag&, Motion& motion, Projectile& projectile) {
        offsets.push_back(wrapAngle(motion.velocity.angle() - petalHeading));
        CHECK_NEAR(motion.velocity.length(), 800.0, 1e-6);
        CHECK_NEAR(projectile.damage, 6.0, 1e-9);
        CHECK_NEAR(projectile.remainingDistance, 1000.0, 1e-9);
        CHECK(projectile.creditTo == rig.player);
        CHECK(projectile.owner == rig.player);
    });
    CHECK_EQ(offsets.size(), std::size_t(3));
    std::sort(offsets.begin(), offsets.end());
    // spreadAngle is the STEP between adjacent shots, and the fan is centred
    // on the petal's outward heading.
    CHECK_NEAR(offsets[0], -0.5, 1e-6);
    CHECK_NEAR(offsets[1], 0.0, 1e-6);
    CHECK_NEAR(offsets[2], 0.5, 1e-6);

    // No second volley until the cooldown is up, then exactly three more.
    rig.tick(20);
    CHECK_EQ(rig.countOf(net::EntityKind::Projectile), std::size_t(3));
    CHECK(rig.tickUntil([&] { return rig.countOf(net::EntityKind::Projectile) > 3; }));
    CHECK_EQ(rig.countOf(net::EntityKind::Projectile), std::size_t(6));
}

TEST(each_grain_of_a_clump_fires_along_its_own_facing) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "peaclump");
    rig.freezeRing();
    rig.settleEquips();
    rig.settleRing();
    rig.setFlags(net::InputAttack);
    rig.tick();

    // Four grains on one ring place, so the volley is four shots -- and the
    // point of the test is that they leave in four DIFFERENT directions,
    // spaced a quarter turn apart around the clump, rather than stacking on
    // the slot's shared bearing.
    CHECK_EQ(rig.countOf(net::EntityKind::Projectile), std::size_t(4));
    const double slotBearing = rig.world.get<Transform>(rig.petals(0).front()).angle;
    std::vector<double> offsets;
    Query<ProjectileTag, Motion> shots{rig.world};
    shots.each([&](Entity, ProjectileTag&, Motion& motion) {
        offsets.push_back(wrapAngle(motion.velocity.angle() - slotBearing));
    });
    CHECK_EQ(offsets.size(), std::size_t(4));
    std::sort(offsets.begin(), offsets.end());
    CHECK_NEAR(offsets[0], -kTau * 0.25, 1e-6);
    CHECK_NEAR(offsets[1], 0.0, 1e-6);
    CHECK_NEAR(offsets[2], kTau * 0.25, 1e-6);
    CHECK_NEAR(std::abs(offsets[3]), kTau * 0.5, 1e-6);
}

TEST(a_projectile_petal_that_breaks_stops_firing) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "peas");
    rig.setFlags(net::InputAttack);
    CHECK(rig.tickUntil([&] { return rig.countOf(net::EntityKind::Projectile) == 3; }));
    rig.damage(rig.petals(0).front(), 5.0);
    rig.tick();
    CHECK_EQ(rig.petals(0).size(), std::size_t(0));
    rig.tick(20);
    CHECK_EQ(rig.countOf(net::EntityKind::Projectile), std::size_t(3));
}

TEST(summons_outlive_the_egg_that_broke_under_them) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "summoner");
    CHECK(rig.tickUntil([&] { return rig.petCount() == 2; }));

    Query<Pet> pets{rig.world};
    std::vector<Entity> summoned = pets.collect();
    CHECK_EQ(summoned.size(), std::size_t(2));
    for (const Entity pet : summoned) {
        CHECK(rig.world.get<Pet>(pet).owner == rig.player);
        CHECK_EQ(int(rig.world.get<Pet>(pet).slot), 0);
        CHECK(rig.world.has<MobTag>(pet));
        // Fights for the player, and is worth nothing to whoever kills it.
        CHECK(rig.world.get<Faction>(pet).team == Team::Players);
        CHECK(!rig.world.has<Bounty>(pet));
    }

    // No runaway summoning: the cap holds tick after tick.
    rig.tick(100);
    CHECK_EQ(rig.petCount(), std::size_t(2));

    rig.damage(rig.petals(0).front(), 4.0);
    rig.tick();
    CHECK(rig.slot(0).broken);
    // The squad STAYS. Over there a pet is despawned only when the slot's
    // petal actually changes, when a duplicate replaces it, or when it drifts
    // off its owner's screen -- never by the egg breaking in combat -- and the
    // reload then finds the pets still out and hatches nothing.
    CHECK_EQ(rig.petCount(), std::size_t(2));
    // Not killed either: no corpse is left for the reaper to award.
    Query<Dead> dead{rig.world};
    CHECK_EQ(dead.count(), std::size_t(0));

    CHECK(rig.tickUntil([&] { return !rig.slot(0).broken; }));
    rig.tick(5);
    CHECK_EQ(rig.petCount(), std::size_t(2));
}

TEST(a_pet_killed_in_the_field_is_resummoned) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "summoner");
    CHECK(rig.tickUntil([&] { return rig.petCount() == 2; }));

    Query<Pet> pets{rig.world};
    const Entity victim = pets.collect().front();
    rig.world.destroy(victim);
    CHECK_EQ(rig.petCount(), std::size_t(1));

    CHECK(rig.tickUntil([&] { return rig.petCount() == 2; }));
}

TEST(unequipping_a_summoner_recalls_its_pets) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "summoner");
    CHECK(rig.tickUntil([&] { return rig.petCount() == 2; }));
    rig.unequip(0);
    rig.tick();
    CHECK_EQ(rig.petCount(), std::size_t(0));
}

// ---------------------------------------------------------------------------
// Wiring
// ---------------------------------------------------------------------------

TEST(spawned_entities_take_net_ids_when_an_allocator_is_installed) {
    if (!contentLoaded()) return;
    Rig rig;
    std::uint32_t counter = 0;
    rig.system.allocateNetId = [&counter] { return ++counter; };
    rig.equip(0, "peas");
    rig.settleEquips();
    rig.setFlags(net::InputAttack);
    rig.tick();

    const Entity petal = rig.petals(0).front();
    CHECK(rig.world.has<NetId>(petal));
    CHECK_EQ(rig.world.get<NetId>(petal).value, std::uint32_t(1));
    CHECK_EQ(rig.world.get<Replicated>(petal).kind, net::EntityKind::Petal);
    CHECK_EQ(rig.world.get<Replicated>(petal).typeIndex, petalId("peas"));

    CHECK(rig.tickUntil([&] { return rig.countOf(net::EntityKind::Projectile) == 3; }));
    Query<ProjectileTag, NetId> shots{rig.world};
    CHECK_EQ(shots.count(), std::size_t(3));
    CHECK_EQ(counter, std::uint32_t(4));
}

TEST(petals_carry_no_motion_so_movement_cannot_fight_the_ring) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "basic");
    rig.settleEquips();
    const Entity petal = rig.petals(0).front();
    CHECK(!rig.world.has<Motion>(petal));
    CHECK(!rig.world.has<Knockback>(petal));
    CHECK(rig.world.has<Body>(petal));
    CHECK(rig.world.has<ContactDamage>(petal));
    CHECK(rig.world.has<HitCooldowns>(petal));
    CHECK(rig.world.get<Faction>(petal).team == Team::Players);
}

TEST(the_ring_follows_the_flower_it_belongs_to) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "basic");
    rig.freezeRing();
    rig.settleEquips();
    rig.world.get<Transform>(rig.player).position = Vec2{5000.0, -2000.0};
    // The ring is sprung to its owner and centred on where that owner was at
    // the end of the LAST tick, so a flower that jumps five thousand units
    // drags its petals after it rather than teleporting them: they arrive over
    // the following ticks.
    rig.tick();
    CHECK(rig.radiusOf(rig.petals(0).front()) > rig.ring().radius * 10.0);
    rig.settleRing();
    CHECK_NEAR(rig.radiusOf(rig.petals(0).front()), rig.ring().radius, 1e-4);
    CHECK_NEAR(rig.position(rig.petals(0).front()).x - 5000.0,
               std::cos(rig.ring().spin) * rig.ring().radius, 1e-4);
}

TEST(two_players_keep_separate_rings) {
    if (!contentLoaded()) return;
    Rig rig;
    Rig other;   // a second world, to prove nothing here is process-wide state
    rig.equip(0, "basic");
    rig.freezeRing();
    rig.settleEquips();
    rig.settleRing();
    other.equip(0, "sandy");
    other.freezeRing();
    other.settleEquips();
    other.settleRing();
    CHECK_EQ(rig.petals().size(), std::size_t(2));    // basic and the anchor
    CHECK_EQ(other.petals().size(), std::size_t(5));  // four grains and the anchor

    // ...and a second flower in the SAME world.
    const Entity second = rig.world.create();
    rig.world.add<PlayerTag>(second);
    rig.world.add<Transform>(second, Transform{{-500.0, 400.0}, 0.0});
    rig.world.add<Body>(second, Body{kPlayerBaseRadius, 1.0});
    rig.world.add<Health>(second, Health{100.0, 100.0, 0.0, 0.0});
    rig.world.add<Faction>(second, Faction{Team::Players, false});
    rig.world.add<PlayerInput>(second);
    rig.world.add<PlayerModifiers>(second);
    rig.world.add<Loadout>(second);
    rig.world.add<PetalRing>(second);
    rig.world.get<Loadout>(second).slots[0].configIndex = petalId("shards");
    rig.settleRing();

    CHECK_EQ(rig.petals().size(), std::size_t(2));
    Query<PetalInstance> all{rig.world};
    CHECK_EQ(all.count(), std::size_t(5));            // plus the second flower's three shards
    for (const Entity petal : all.collect()) {
        const PetalInstance& instance = rig.world.get<PetalInstance>(petal);
        const Vec2 owner = rig.world.get<Transform>(instance.owner).position;
        // The second flower's ring still turns -- it wears no anchor -- so its
        // shards hold the steady orbit a spring settles into rather than the
        // ring radius exactly.
        const double slack = instance.owner == rig.player ? 1e-6 : 1.0;
        CHECK_NEAR((rig.world.get<Transform>(petal).position - owner).length(),
                   rig.world.get<PetalRing>(instance.owner).radius, slack);
    }
}


// ---------------------------------------------------------------------------
// The fraction the loadout bar drains its tile by
// ---------------------------------------------------------------------------

TEST(a_hurt_petal_reports_a_part_full_slot) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "basic");
    rig.settleEquips();
    CHECK_NEAR(rig.slotHealth(0), 1.0, 1e-9);

    rig.damage(rig.petals(0).front(), 4.0);
    rig.tick();
    CHECK_NEAR(rig.slotHealth(0), 0.6, 1e-9);

    // A broken slot is empty, not merely low: the tile drains all the way to
    // the plate colour and refills when the reload hands the pool back.
    rig.damage(rig.petals(0).front(), 6.0);
    rig.tick();
    CHECK(rig.slot(0).broken);
    CHECK_NEAR(rig.slotHealth(0), 0.0, 1e-9);

    CHECK(rig.tickUntil([&] { return !rig.slot(0).broken; }));
    CHECK_NEAR(rig.slotHealth(0), 1.0, 1e-9);
}

TEST(a_clump_reports_the_share_of_its_grains_still_standing) {
    if (!contentLoaded()) return;
    Rig rig;
    rig.equip(0, "sandy");
    rig.tick();
    CHECK_EQ(rig.petals(0).size(), std::size_t(4));
    CHECK_NEAR(rig.slotHealth(0), 1.0, 1e-9);

    // A grain off the field counts as none of its own health rather than as
    // absent, so three whole grains of four is a tile three quarters full --
    // which is the only reading that makes a half-broken clump legible.
    rig.damage(rig.petalWithSub(0, 0), 12.0);
    rig.tick();
    CHECK_EQ(rig.petals(0).size(), std::size_t(3));
    CHECK_NEAR(rig.slotHealth(0), 0.75, 1e-9);

    // Damage on a surviving grain is its own share of the same total.
    rig.damage(rig.petalWithSub(0, 1), 6.0);
    rig.tick();
    CHECK_NEAR(rig.slotHealth(0), 0.625, 1e-9);
}

TEST(an_unbreakable_petal_never_drains_its_tile) {
    if (!contentLoaded()) return;
    // Rock has no health at all. Reading a pool it does not own would leave its
    // tile permanently empty.
    Rig rig;
    rig.equip(0, "rock");
    rig.settleEquips();
    rig.tick(20);
    CHECK_NEAR(rig.slotHealth(0), 1.0, 1e-9);
}
