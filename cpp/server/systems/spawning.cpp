#include "server/systems/spawning.h"

#include <algorithm>
#include <cmath>

namespace flix {
namespace {

/// Mobs that are never put to sleep and never recycled: they stay live
/// entities wherever they stand, however long nobody looks at them.
///
/// A BOSS is an event -- the whole server was told it appeared and the bots
/// were sent at it, so it has to be there when somebody arrives, doing what it
/// was doing. A target dummy is furniture someone walked away from, and the
/// DPS row has to be standing when they come back to it.
///
/// Nothing else qualifies, ULTRAS INCLUDED. An ultra out of everyone's sight
/// goes back to being a record like any other mob, which is not the same as
/// being destroyed: it is still on the map, at the same spot, and it comes
/// back to life when somebody returns. It costs nothing while nobody is there,
/// which is the whole point -- a difficulty-100 band is nearly all ultras, and
/// keeping that band's population simulated forever is exactly the bill this
/// system exists not to pay.
bool alwaysAwake(const ContentRegistry& content, std::uint16_t mobIndex, Rarity rarity) {
    if (isBossRarity(rarity)) return true;
    return mobIndex < content.mobCount() && content.mob(mobIndex).neverAmbient;
}

bool alwaysAwake(const ContentRegistry& content, const MobType& type) {
    return alwaysAwake(content, type.configIndex, type.rarity);
}

/// How much smaller a permanent fixture is than the wild mob of its tier.
///
/// A common dummy matches a common mob exactly; by unique it is pulled down to
/// three quarters of a wild unique, so the practice target does not become an
/// enormous wall at the top rarities. Linear in the rarity index over the
/// common..unique span, which is the reference's buildSizeRamp
/// (src/mobs.ts:190, DUMMY_SIZE_SCALE_AT_UNIQUE).
double fixtureSizeScale(const MobConfig& config, Rarity rarity) {
    if (!config.neverAmbient) return 1.0;
    constexpr double kScaleAtUnique = 0.75;
    constexpr double kUniqueIndex = static_cast<double>(rarityIndex(Rarity::Unique));
    return 1.0 - (1.0 - kScaleAtUnique) * (static_cast<double>(rarityIndex(rarity)) / kUniqueIndex);
}

/// The per-spawn size multiplier on the mob's nominal size: the `random_size`
/// roll, times the fixture ramp above.
///
/// The JSON range is an ABSOLUTE size rather than a factor, so the reference
/// divides it by the config's own `size`: a cactus (size 1.5, random_size
/// [1, 2]) comes out between 0.667x and 1.333x, not between 1x and 2x. Both
/// factors ride in the one number because the reference resolves them in the
/// one function (getEnemySizeScale), and everything that turns a mob's stat
/// size into world units multiplies by exactly that.
double rollSizeJitter(const MobConfig& config, Rarity rarity, Rng& rng) {
    const double fixture = fixtureSizeScale(config, rarity);
    if (!(config.randomSizeMax > config.randomSizeMin) || !(config.size > 0.0)) {
        return config.randomSizeMin * fixture;
    }
    return rng.range(config.randomSizeMin, config.randomSizeMax) / config.size * fixture;
}

/// A mob whose body cannot hurt a player.
///
/// Matched by id because the reference states the rule that way -- there is no
/// JSON field for it, only `enemy.type !== 'item_spawner'` guarding the
/// contact-damage branch (src/server/playerState.ts:1695).
bool harmlessOnContact(const MobConfig& config) {
    return config.id == "item_spawner";
}

/// Where an escort placed AROUND its nest stands: a bearing of its own,
/// between `gap` and `gap + anchorRadius` units clear of the nest's body.
Vec2 escortRingPoint(Vec2 anchor, double anchorRadius, double gap, Rng& rng) {
    return anchor + Vec2::fromAngle(rng.angle(), anchorRadius + gap + rng.unit() * anchorRadius);
}

/// How far a coordinate may sit from a flower and still BE that flower.
///
/// The runtime builds its position list out of these very transforms earlier
/// in the same tick, so the pairing is normally exact; the slack is for a
/// caller that snapshotted a moment before. Tighter than a flower's own body,
/// so the only pair it can confuse is two players standing inside each other,
/// who are owed the same neighbourhood anyway.
constexpr double kViewerMatchRadius = 24.0;

/// The luck a spawn placed at `at` is charged to: the closest flower to it in
/// the SAME realm. What the reference does for a zone fill, which belongs to
/// nobody's viewport (src/server/enemySpawner.ts:855-869). Distances are only
/// meaningful inside one coordinate space, so a flower standing at the same
/// numbers on another map is not "near" anything here.
double nearestViewerLuck(const std::vector<SpawnSystem::Viewer>& viewers, Realm realm, Vec2 at) {
    const SpawnSystem::Viewer* nearest = nullptr;
    double nearestDistSq = 0.0;
    for (const SpawnSystem::Viewer& viewer : viewers) {
        if (viewer.realm != realm) continue;
        const double distSq = distanceSq(viewer.position, at);
        if (nearest != nullptr && distSq >= nearestDistSq) continue;
        nearest = &viewer;
        nearestDistSq = distSq;
    }
    return nearest == nullptr ? kNeutralSpawnLuck : nearest->luck;
}

/// The border band, which is the thickness of the boundary wall. A mob
/// standing in it is half inside the edge of the world, so the reference
/// refuses the point outright rather than moving it
/// (isInOutOfBoundsZone, src/server/shared/positions.ts:25-30).
///
/// `extent` is the rectangle of the realm the point is in: every map is its
/// own coordinate space with its own size -- the shipped world is 64 tiles
/// square -- so a single world constant is the wrong number for all but one
/// of them.
bool inBorderBand(Vec2 position, Vec2 extent) {
    return position.x < kWorldBoundaryThreshold ||
           position.x > extent.x - kWorldBoundaryThreshold ||
           position.y < kWorldBoundaryThreshold ||
           position.y > extent.y - kWorldBoundaryThreshold;
}

/// No spawn lands in anyone's lap, whoever asked for it.
bool nearAnyPlayer(const std::vector<SpawnSystem::Viewer>& viewers, Realm realm, Vec2 position,
                   double radius) {
    const double radiusSq = radius * radius;
    for (const SpawnSystem::Viewer& viewer : viewers) {
        if (viewer.realm != realm) continue;
        if (distanceSq(viewer.position, position) < radiusSq) return true;
    }
    return false;
}

/// True when a band's box overlaps any player's buffered viewport, grown by
/// `margin`.
///
/// The cheap half of waking the world: a band nobody is anywhere near is one
/// rectangle test per viewer, and its records are never walked at all.
bool zoneInView(const Rect& bounds, Realm realm,
                const std::vector<SpawnSystem::Viewer>& viewers, double margin) {
    for (const SpawnSystem::Viewer& viewer : viewers) {
        // Same map first: two maps' coordinates overlap numerically, so a
        // viewport test alone would wake a second map because somebody was
        // standing at the same numbers in the first.
        if (viewer.realm != realm) continue;
        if (bounds.left() < viewer.position.x + viewer.half.x + margin &&
            bounds.right() > viewer.position.x - viewer.half.x - margin &&
            bounds.top() < viewer.position.y + viewer.half.y + margin &&
            bounds.bottom() > viewer.position.y - viewer.half.y - margin) {
            return true;
        }
    }
    return false;
}

/// Announcements held for a runtime that has not drained them. Two bosses a
/// minute at the very most, so this is a leak guard rather than a queue depth.
constexpr std::size_t kMaxPendingBossSpawns = 16;

/// A uniform point in `bounds`, clamped into the realm's own rectangle
/// (`extent`). The map's rectangles are allowed to hang over the edge and
/// several do.
Vec2 samplePointInRect(const Rect& bounds, Vec2 extent, Rng& rng) {
    return {clamp(bounds.x + rng.unit() * bounds.w, 0.0, extent.x),
            clamp(bounds.y + rng.unit() * bounds.h, 0.0, extent.y)};
}

/// A uniform point within `radius` of `around`, in the same rectangle.
Vec2 samplePointNear(Vec2 around, double radius, Vec2 extent, Rng& rng) {
    const Vec2 at = around + rng.insideCircle(radius);
    return {clamp(at.x, 0.0, extent.x), clamp(at.y, 0.0, extent.y)};
}

} // namespace

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

void SpawnSystem::bind(World& world) {
    if (boundWorld_ == &world) return;
    boundWorld_ = &world;
    // Queries cache their matched archetypes and are meant to outlive a tick,
    // but they need a world to be constructed against and this system is built
    // before the server has one. Rebinding also covers a test that runs the
    // same system over a second world.
    ambient_.emplace(world);
    ambient_->without<Dead>();
    casualties_.emplace(world);
    escorts_.emplace(world);
    escorts_->without<Dead>();
    spawners_.emplace(world);
    spawners_->without<Dead>();
    waveNests_.emplace(world);
    waveNests_->without<Dead>();
    allMobs_.emplace(world);
    allMobs_->without<Dead>();
    playerBodies_.emplace(world);
}

// ---------------------------------------------------------------------------
// Type and tier selection
// ---------------------------------------------------------------------------

std::uint16_t SpawnSystem::chooseGroupMob(const ContentRegistry& content, std::uint16_t group,
                                         Rarity rarity, Rng& rng) const {
    if (group == kInvalidIndex || group >= content.mobGroupCount()) return kInvalidIndex;
    const MobGroup& members = content.mobGroup(group);
    if (members.members.empty()) return kInvalidIndex;

    // Two walks rather than a cached cumulative table, because eligibility is
    // per TIER: a mob with a min_rarity is in the group at legendary and out
    // of it at common, so a table built once would have to be built per tier
    // per group. A group holds a handful of mobs and this runs a few times a
    // second, so two passes over it is not worth caching away.
    //
    // A zero weight is how the data says "in this group, but never rolled":
    // centipede body segments belong to the garden and are only ever placed by
    // the head that owns them.
    double total = 0.0;
    for (const MobGroupMember& member : members.members) {
        if (!(member.weight > 0.0)) continue;
        if (!content.mobStats(member.mob, rarity).ambient) continue;
        total += member.weight;
    }
    if (!(total > 0.0)) return kInvalidIndex;

    double roll = rng.unit() * total;
    std::uint16_t last = kInvalidIndex;
    for (const MobGroupMember& member : members.members) {
        if (!(member.weight > 0.0)) continue;
        if (!content.mobStats(member.mob, rarity).ambient) continue;
        last = member.mob;
        roll -= member.weight;
        if (roll < 0.0) return member.mob;
    }
    return last;
}

std::uint16_t SpawnSystem::chooseRegionMobAt(const ContentRegistry& content, Realm realm, Vec2 at,
                                             Rarity rarity, Rng& rng) {
    // The region standing on this ground. First match in map order, as every
    // other shape lookup here is.
    for (const SpawnZone& region : regions_) {
        if (region.realm != realm) continue;
        if (!zoneContains(region.bounds, region.polygon, at)) continue;
        return rollResolvedRows(content, region.resolved, rarity, rng);
    }
    // Outside every region: the map's own default group, which is the only
    // thing left that can say what belongs here.
    //
    // There is no arm below this one. A map is always in hand by the time we
    // get here: this is reached only from chooseZoneMobType, which is reached
    // only from spawnInZone, which needs a band -- and a band exists only
    // because rebuildZones read it off a map in `worldMaps`, whose realm
    // therefore resolves. A harness with no map has no band, so it spawns
    // nothing at all and never arrives here; that is what spawning.h's
    // `worldMaps` says, and it used to be untrue while the density fill could
    // reach this function with a bare Terrain.
    const MapData* map = worldMaps != nullptr ? worldMaps->forRealm(realm) : nullptr;
    if (map == nullptr) return kInvalidIndex;
    return chooseGroupMob(content, content.mobGroupIndex(map->defaultMobGroup()), rarity, rng);
}

bool SpawnSystem::permanentFixtureExists(World& world, std::uint16_t mobIndex, Rarity rarity,
                                         Realm realm, int section) {
    bind(world);
    bool found = false;
    allMobs_->each([&](Entity, MobTag&, Transform& transform, MobType& type) {
        if (found) return;
        if (type.configIndex != mobIndex || type.rarity != rarity) return;
        // Same map first: a dummy on the overworld says nothing about whether
        // a biome map has one, and the numeric section is the OVERWORLD's
        // grid. On any other map the fixture is one per tier per map.
        if (transform.realm != realm) return;
        if (realm == Realm::Overworld && sectionAt(transform.position) != section) return;
        found = true;
    });
    return found;
}

Rarity SpawnSystem::rollRarity(const MobConfig& config, double difficulty, double luck,
                               Rng& rng) {
    const Rarity rolled = rollSpawnRarity(difficulty, luck, rng);
    return clampRarity(std::max(rarityIndex(rolled), rarityIndex(config.minRarity)));
}

double SpawnSystem::difficultyAt(Realm realm, Vec2 at) const {
    // A band, or nothing. The author drawing danger onto a shape is the only
    // way a square of the world is dangerous; a square no band covers grows no
    // mob at all, and zero is what that reads as. First match in map order, as
    // every other shape lookup here is.
    for (const SpawnZone& zone : zones_) {
        if (zone.realm != realm) continue;
        if (!zoneContains(zone.bounds, zone.polygon, at)) continue;
        return zone.difficulty;
    }
    return 0.0;
}

// ---------------------------------------------------------------------------
// Placing a mob
// ---------------------------------------------------------------------------

Entity SpawnSystem::spawnMob(World& world, const Terrain& terrain, const ContentRegistry& content,
                             std::uint16_t mobIndex, Rarity rarity, Vec2 position, Realm realm,
                             double nowMillis, Rng& rng) {
    // No band owns a mob somebody else asked for -- the arena, the maze, a
    // script, an operator's console -- so it is counted against no band's
    // target and is recycled the old way rather than going back to a record
    // there is no band to hold.
    return spawnMobAt(world, terrain, content, mobIndex, rarity, position, realm, nowMillis, rng, 0,
                      kInvalidIndex);
}

Entity SpawnSystem::spawnMobAt(World& world, const Terrain& terrain, const ContentRegistry& content,
                               std::uint16_t mobIndex, Rarity rarity, Vec2 position, Realm realm,
                               double nowMillis, Rng& rng, int depth, std::uint16_t zone) {
    if (mobIndex >= content.mobCount()) return NULL_ENTITY;

    const MobConfig& config = content.mob(mobIndex);
    // A mob does not exist below its min_rarity, whoever asked for it. Enforced
    // here rather than at each call site so a nest, a script and the ambient
    // roll cannot disagree about it.
    rarity = clampRarity(std::max(rarityIndex(rarity), rarityIndex(config.minRarity)));
    const MobStats stats = content.mobStats(mobIndex, rarity);

    const double jitter = rollSizeJitter(config, rarity, rng);
    const double radius = stats.radius * jitter;

    // resolveCircle, not a blocked() test: the caller hands over a point and
    // the mob is a body, so a spot one unit from a wall is legal as a point and
    // embedded as a circle.
    const Vec2 at = terrain.resolveCircle(position, radius, realm);

    // Held rather than passed straight through: a centipede's body is laid out
    // along its head's facing, and the chain is built once the head is whole.
    const double angle = rng.angle();

    const Entity e = world.create();
    world.add<MobTag>(e);
    world.add<Transform>(e, Transform{at, angle, realm});
    world.add<Motion>(e);
    // Mass is area, but it is the TIER's area: the reference derives mass from
    // the config size and the rarity step alone, so a mob that rolled a big
    // body is no harder to knock back than one that rolled a small one.
    world.add<Body>(e, Body{radius, stats.mass});
    world.add<Knockback>(e);
    world.add<Faction>(e, Faction{Team::Hostiles, false});
    world.add<Health>(e, Health{stats.health, stats.health, 0.0, 0.0});
    // The config's cooldown is the gap between deliberate ATTACKS, which the AI
    // owns; touching a mob is throttled by the same rule for every mob.
    //
    // The item spawner is the one mob that is furniture rather than an enemy:
    // the reference excludes it from the contact-damage branch by name, so its
    // `damage: 50` never lands on anyone who walks into it.
    if (!harmlessOnContact(config)) {
        world.add<ContactDamage>(e, ContactDamage{stats.damage, kMobHitIntervalMillis});
    }
    world.add<HitCooldowns>(e);
    world.add<Afflictions>(e);
    world.add<MobType>(e, MobType{mobIndex, rarity, jitter});

    const bool chainHead = config.segmentCount > 0 && config.segmentBodyIndex != kInvalidIndex;
    if (chainHead) {
        // A head is a segment of its own chain. The follow pass walks from
        // whatever has nothing ahead of it, so a head carrying no link would
        // not be a chain root at all and its body would never be placed.
        BodySegment link;
        link.head = true;
        link.chainHead = e;
        world.add<BodySegment>(e, link);
    }

    Bounty bounty;
    bounty.xp = stats.xp;
    world.add<Bounty>(e, std::move(bounty));

    MobAi ai;
    ai.kind = stats.ai;
    ai.anchor = at;
    ai.aggroRange = stats.aggroRange;
    ai.wanderAngle = rng.angle();
    ai.nextDecisionMillis = nowMillis;
    world.add<MobAi>(e, std::move(ai));

    world.add<AmbientMob>(e, AmbientMob{nowMillis, zone});
    world.add<Replicated>(e, Replicated{net::EntityKind::Mob, 0, mobIndex, rarity, 0});
    if (netIds != nullptr) world.add<NetId>(e, NetId{netIds->next()});

    // The census is the OVERWORLD's population. Another realm's mobs are
    // counted by the spawner that fills it.
    if (realm == Realm::Overworld) {
        ++census_.mobs;
        ++census_.spawnedTotal;
    }

    if (depth < kMaxNestDepth) {
        if (config.periodicSpawn.present) {
            Spawner spawner;
            spawner.childConfigIndex = config.periodicSpawn.mobIndex;
            spawner.rarityOffset = config.periodicSpawn.rarityOffset;
            spawner.intervalMillis = config.periodicSpawn.intervalMillis;
            // Due immediately. The reference starts the clock at zero against a
            // wall-clock `now`, so a queen has a soldier out on the tick she
            // appears rather than standing alone for her first interval.
            spawner.nextSpawnMillis = nowMillis;
            spawner.childLifetimeMillis = config.periodicSpawn.lifetimeMillis;
            spawner.maxAlive = config.periodicSpawn.maxAlive;
            world.add<Spawner>(e, std::move(spawner));
        }
        if (!config.spawnWaves.empty()) {
            NestWaves waves;
            waves.mobIndex = mobIndex;
            // Seeded full: the first band fires on the first damage the hole
            // takes, and never before it.
            waves.previousHealth = stats.health;
            world.add<NestWaves>(e, std::move(waves));
        }
    }

    // Last, because each of these is a create() that can relocate the rows the
    // adds above were writing into. Nothing may touch `e` after this -- the
    // chain takes it by value, as a link, and never reads its components.
    //
    // A body is laid out whatever the nesting depth: it is not nest content but
    // the rest of the same animal, so a centipede that is itself an escort
    // still arrives whole rather than as a floating head.
    if (chainHead) {
        spawnBodyChain(world, terrain, content, e, config, rarity, at, realm, angle, nowMillis, rng,
                       depth + 1);
    }
    if (depth < kMaxNestDepth) {
        for (const std::uint16_t child : config.initialSpawns) {
            spawnEscort(world, terrain, content, child, rarity,
                        escortRingPoint(at, radius, kInitialEscortGap, rng), realm, e, nowMillis,
                        rng, depth + 1);
        }
    }

    // EVERY path that puts a mob in the world comes through here, so this is
    // the one place a boss can be announced from and there is no way to add a
    // spawn path that quietly produces one in silence.
    //
    // Roots only. A nest's escorts and a centipede's segments carry their
    // parent's tier, and announcing those would put a line in chat for every
    // bead of a super centipede rather than one for the animal.
    if (depth == 0) announceIfNotable(content, e, mobIndex, rarity, at, realm);

    return e;
}

void SpawnSystem::spawnBodyChain(World& world, const Terrain& terrain,
                                 const ContentRegistry& content, Entity head,
                                 const MobConfig& config, Rarity rarity, Vec2 headPosition,
                                 Realm realm, double headAngle, double nowMillis, Rng& rng,
                                 int depth) {
    // The body's own stats at the HEAD's tier: a mythic centipede is one long
    // mythic animal, not a big head towing a string of common beads.
    const MobStats bodyStats = content.mobStats(config.segmentBodyIndex, rarity);
    if (!(bodyStats.radius > 0.0)) return;
    const double spacing = bodyStats.radius * kSegmentSpacingPerRadius;

    // Straight back from the head's facing, and stepped from the REQUESTED
    // points rather than the resolved ones: a segment nudged out of a wall must
    // not bend the rest of the body around it. The chain pass owns the shape
    // from the next tick onwards, and it starts from a straight animal.
    const Vec2 step = Vec2::fromAngle(headAngle + kPi, spacing);

    Entity ahead = head;
    Vec2 at = headPosition;
    for (int i = 1; i <= config.segmentCount; ++i) {
        if (census_.mobs >= mobCap) break;
        at = at + step;
        // Segments belong to the animal, not to the band: the head is the one
        // thing the band counted, and the chain is rebuilt from scratch every
        // time that head comes back to life.
        const Entity segment = spawnMobAt(world, terrain, content, config.segmentBodyIndex, rarity,
                                          at, realm, nowMillis, rng, depth, kInvalidIndex);
        if (segment == NULL_ENTITY) break;

        BodySegment link;
        link.ahead = ahead;
        link.spacing = spacing;
        link.chainHead = head;
        link.segmentIndex = i;
        world.add<BodySegment>(segment, link);
        // After the add, which relocated the row the spawn had just written.
        // A segment faces the way its head does or the body starts out kinked.
        if (Transform* transform = world.tryGet<Transform>(segment)) transform->angle = headAngle;
        ahead = segment;
    }
}

// An escort is placed by its NEST, not by a band. The ring it stands on is
// centred on the parent and routinely reaches over the band's edge onto ground
// no band covers -- which is legal, and deliberately ungated: the band chose
// the nest, and the nest chose these. The same is true of a centipede's body
// segments (spawnBodyChain).
Entity SpawnSystem::spawnEscort(World& world, const Terrain& terrain, const ContentRegistry& content,
                                std::uint16_t childIndex, Rarity nestRarity, Vec2 at, Realm realm,
                                Entity parent, double nowMillis, Rng& rng, int depth) {
    if (census_.mobs >= mobCap) return NULL_ENTITY;
    const Entity child = spawnMobAt(world, terrain, content, childIndex, nestRarity, at, realm,
                                    nowMillis, rng, depth, kInvalidIndex);
    if (child == NULL_ENTITY || parent == NULL_ENTITY) return child;

    // The leash, on all three paths that put a child into the world. Dragged
    // past the retreat radius from whatever made it, an escort drops its target
    // and walks back, so a hole cannot be stripped of its defenders by leading
    // them away one at a time (src/ecs/systems/enemyAI.ts:485).
    //
    // The parent's position is read out before the add, which relocates the row
    // it points into.
    Vec2 home = at;
    if (const Transform* anchor = world.tryGet<Transform>(parent)) home = anchor->position;
    world.add<HoleTether>(child, HoleTether{parent, home, false});
    return child;
}

// ---------------------------------------------------------------------------
// The tick
// ---------------------------------------------------------------------------

void SpawnSystem::run(World& world, const Terrain& terrain, const ContentRegistry& content,
                      const std::vector<RealmPoint>& players, Rng& rng, double nowMillis, double dt,
                      CommandBuffer& commands) {
    bind(world);
    rebuildZones(content);
    gatherViewers(world, players);

    expireEscorts(dt, commands);
    runNests(world, terrain, content, rng, nowMillis);

    // The census is O(mobs x players) and nothing about a population of a few
    // hundred changes meaningfully inside half a second. Waking rides with it:
    // the two are the same question asked in the two directions, they need the
    // same viewer list, and kLatentWakeMargin is what pays for the cadence.
    // EVERY tick, unlike everything else here, because a corpse does not wait:
    // combat marks a mob dead earlier in this very tick and the runtime reaps
    // it at the end of it, so a band that only looked twice a second would
    // miss nineteen kills in twenty and hand their slots to the band-wide
    // top-up instead -- which is to say, to somewhere else entirely.
    bankCasualties(world, terrain, content, viewers_, rng, nowMillis);

    if (nowMillis >= nextPopulationMillis_) {
        nextPopulationMillis_ = nowMillis + kPopulationIntervalMillis;
        takeCensus(content, viewers_, nowMillis, commands);
        promoteLatent(world, terrain, content, viewers_, rng, nowMillis);
    }

    // The one ambient spawn path, and it runs whether or not anybody is
    // anywhere near: the map is full at all times, and that is the whole
    // difference between this and a spawner that follows the players around.
    // Its own clock, and it reads the last census rather than taking one of
    // its own, so it is not tied to that cadence.
    stockSpawnZones(world, terrain, content, viewers_, rng, nowMillis);

    // And that is all of it. There is no second pass behind the bands covering
    // the ground the author left unbanded -- a square no band covers grows
    // nothing, which is the point.
    //
    // The ARENA and the MAZE are not that rule's business: they are generated
    // realms with no object layer to draw a band on, and ModeSpawner
    // (mode_spawning.h) fills each one whole, every tick, through spawnMob().
    // Their mobs are kept alive by the realm-occupied branch of takeCensus
    // rather than by any band, and none of them is ever latent.
}

void SpawnSystem::gatherViewers(World& world, const std::vector<RealmPoint>& players) {
    // Every flower on an authored map, with the two facts a coordinate cannot
    // carry. A client that reported nothing keeps the default box, exactly as
    // the reference's `player.viewportWidth || VIEWPORT_WIDTH` does.
    worldViewers_.clear();
    playerBodies_->each([&](Entity e, PlayerTag&, Transform& transform) {
        // Every flower on an authored MAP, not only the overworld's: spawn
        // bands exist on every map and each one is stocked for the people
        // looking at it. The arena and the maze are populated whole, by
        // ModeSpawner, so their flowers are not viewers here.
        if (!isWorldRealm(transform.realm)) return;
        Viewer viewer;
        viewer.position = transform.position;
        viewer.realm = transform.realm;
        if (const PlayerLocation* location = world.tryGet<PlayerLocation>(e)) {
            viewer.half = {location->viewport.x * 0.5 + kViewportBuffer,
                           location->viewport.y * 0.5 + kViewportBuffer};
        }
        if (const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(e)) {
            viewer.luck = modifiers->luck;
        }
        worldViewers_.push_back(viewer);
    });

    // The caller's list stays the list -- it decides WHO the bands are stocked
    // and the population kept for -- and each entry is only paired with the
    // flower standing on it. One that pairs with nothing is a bare coordinate
    // from a harness, and keeps the defaults above.
    viewers_.clear();
    viewers_.reserve(players.size());
    realmOccupied_.fill(false);
    for (const RealmPoint& player : players) {
        realmOccupied_[realmIndex(player.realm)] = true;
        if (!isWorldRealm(player.realm)) continue;
        const Vec2 position = player.position;
        Viewer viewer;
        viewer.position = position;
        // The realm is the caller's fact about the point, not something the
        // pairing below discovers: a coordinate on a second map's door and
        // the same numbers in the overworld are two different places, and a
        // viewer filed under the wrong one stocks the wrong map.
        viewer.realm = player.realm;
        double nearestDistSq = kViewerMatchRadius * kViewerMatchRadius;
        for (const Viewer& candidate : worldViewers_) {
            if (candidate.realm != player.realm) continue;
            const double distSq = distanceSq(candidate.position, position);
            if (distSq > nearestDistSq) continue;
            nearestDistSq = distSq;
            viewer.half = candidate.half;
            viewer.luck = candidate.luck;
        }
        viewers_.push_back(viewer);
    }
}

void SpawnSystem::expireEscorts(double dt, CommandBuffer& commands) {
    doomed_.clear();
    escorts_->each([&](Entity e, AmbientMob&, Lifetime& lifetime) {
        // A zero remainder means "no timer": only nest escorts are given one,
        // and an ambient mob must not evaporate because the field defaulted.
        if (lifetime.remainingSeconds <= 0.0) return;
        lifetime.remainingSeconds -= dt;
        if (lifetime.remainingSeconds <= 0.0) doomed_.push_back(e);
    });
    // Destroyed, not killed: an escort running out of time is bookkeeping, and
    // marking it Dead would pay out XP and loot for a mob nobody fought.
    for (const Entity e : doomed_) commands.destroy(e);
}

void SpawnSystem::takeCensus(const ContentRegistry& content, const std::vector<Viewer>& viewers,
                             double nowMillis, CommandBuffer& commands) {
    census_.mobs = 0;
    doomed_.clear();

    mobPlacements_.clear();
    // Recounted from the world rather than tracked through every death: a mob
    // dies in combat, is reaped by the runtime and is never reported here, so
    // a running total would drift downward forever and its band would stop
    // replacing anything.
    for (SpawnZone& zone : zones_) zone.liveMobs = 0;

    // Nobody connected means nobody has failed to see anything. The reference's
    // near-a-player test answers true when its box list is empty, and that
    // permissive default is what keeps an unattended server populated instead
    // of emptying itself and handing the next arrival a barren map.
    const bool unattended = viewers.empty();

    ambient_->each([&](Entity e, MobTag&, Transform& transform, Body& body, MobType& type,
                       AmbientMob& ambient) {
        // An arena or maze mob is kept for as long as anyone is in its realm:
        // those realms are populated whole rather than by viewport, and the
        // reference exempts their mobs from the distance despawn on the same
        // condition (mazeSpawner.ts hasMazePlayers). Once the realm empties
        // the usual grace period runs and the population drains.
        if (!isWorldRealm(transform.realm)) {
            if (realmOccupied_[realmIndex(transform.realm)]) {
                ambient.lastNearPlayerMillis = nowMillis;
            } else if (nowMillis - ambient.lastNearPlayerMillis >= kMobDespawnDelayMillis) {
                doomed_.push_back(e);
            }
            return;
        }
        bool nearAnyone = unattended;
        for (const Viewer& viewer : viewers) {
            // Same map first: a mob is only ever "seen" by somebody standing
            // in its own coordinate space.
            if (viewer.realm != transform.realm) continue;
            // Each flower's OWN box, not one 1920x1080 rectangle for everybody:
            // a mob at the edge of an ultrawide screen is being drawn, and
            // starting its recycle clock is what makes it blink out in front of
            // its owner (src/server/playerState.ts:1041).
            const Vec2 offset = transform.position - viewer.position;
            if (std::abs(offset.x) <= viewer.half.x && std::abs(offset.y) <= viewer.half.y) {
                nearAnyone = true;
                break;
            }
        }

        if (nearAnyone) {
            ambient.lastNearPlayerMillis = nowMillis;
        } else if (nowMillis - ambient.lastNearPlayerMillis >= kMobDespawnDelayMillis &&
                   !alwaysAwake(content, type)) {
            // Nobody has been near it for the grace period, so it stops being
            // an entity. Whether that is the END of it depends on whether a
            // band is holding a slot for it: one the band placed goes back
            // into that band's records AT THE POSITION IT WANDERED TO, so the
            // map stays exactly as full as it was and the mob is still there
            // to be found. A nest's escort, a centipede's segment, an arena
            // mob or something an operator conjured has no band, and is simply
            // destroyed as it always was.
            if (ambient.zone < zones_.size()) {
                SpawnZone& zone = zones_[ambient.zone];
                zone.latent.push_back(
                    LatentMob{transform.position, nowMillis, type.configIndex, type.rarity});
                ++census_.demotedTotal;
            }
            // Left out of the counts on purpose: it is on its way out, and
            // counting it would suppress the replacement spawn for one pass.
            doomed_.push_back(e);
            return;
        }

        ++census_.mobs;
        if (ambient.zone < zones_.size()) ++zones_[ambient.zone].liveMobs;
        // The placement record the stocking pass spaces its next spawns
        // against.
        mobPlacements_.push_back(MobPlacement{transform.position, body.radius, transform.realm});
    });

    for (const Entity e : doomed_) commands.destroy(e);
    census_.despawnedTotal += static_cast<int>(doomed_.size());

    census_.latent = latentCount();
}

bool SpawnSystem::crowdedAt(Realm realm, Vec2 position, double halfSize, double extraGap,
                            const SpawnZone& zone) const {
    for (const MobPlacement& mob : mobPlacements_) {
        if (mob.realm != realm) continue;
        const double reach = halfSize + mob.radius + extraGap;
        if (distanceSq(mob.position, position) < reach * reach) return true;
    }
    // The band's own records. They carry no radius -- a record is a type and a
    // point, and the body it will grow is rolled when it wakes -- so they are
    // spaced by the same nominal gap two spawn ATTEMPTS are, which is what
    // keeps a stocked band from settling into clumps.
    const double reach = halfSize + kPreliminarySpawnRadius + extraGap;
    const double reachSq = reach * reach;
    for (const LatentMob& record : zone.latent) {
        if (distanceSq(record.position, position) < reachSq) return true;
    }
    return false;
}

bool SpawnSystem::seenBy(const std::vector<Viewer>& viewers, Realm realm, Vec2 position,
                         double margin) const {
    for (const Viewer& viewer : viewers) {
        if (viewer.realm != realm) continue;
        const Vec2 offset = position - viewer.position;
        if (std::abs(offset.x) <= viewer.half.x + margin &&
            std::abs(offset.y) <= viewer.half.y + margin) {
            return true;
        }
    }
    return false;
}

void SpawnSystem::runNests(World& world, const Terrain& terrain, const ContentRegistry& content,
                           Rng& rng, double nowMillis) {
    // Both loops snapshot their nests first: spawning an escort creates
    // entities, which relocates the very columns the query would be walking.
    spawners_->collect(scratchChildren_);
    for (const Entity nest : scratchChildren_) {
        Spawner* spawner = world.tryGet<Spawner>(nest);
        const Transform* transform = world.tryGet<Transform>(nest);
        const MobType* type = world.tryGet<MobType>(nest);
        if (spawner == nullptr || transform == nullptr || type == nullptr) continue;

        // Handles rather than a counter: a counter leaks a slot every time a
        // child dies somewhere else, and the nest goes quiet forever.
        std::size_t live = 0;
        for (const Entity child : spawner->children) {
            if (world.isAlive(child)) spawner->children[live++] = child;
        }
        spawner->children.resize(live);

        if (nowMillis < spawner->nextSpawnMillis) continue;
        spawner->nextSpawnMillis = nowMillis + std::max(1.0, spawner->intervalMillis);
        if (static_cast<int>(live) >= spawner->maxAlive) continue;

        // Read everything out before spawning: `spawner` points into an
        // archetype column and does not survive a create().
        const std::uint16_t childIndex = spawner->childConfigIndex;
        const Rarity childRarity = clampRarity(rarityIndex(type->rarity) + spawner->rarityOffset);
        const double lifetimeMillis = spawner->childLifetimeMillis;
        const Vec2 anchor = transform->position;
        const Realm nestRealm = transform->realm;
        const double facing = transform->angle;
        const Body* body = world.tryGet<Body>(nest);
        const double anchorRadius = body != nullptr ? body->radius : kMobBaseRadius;

        // Out of the queen's abdomen: one body radius directly behind her,
        // never on a bearing of its own. Soldiers trailing her is the whole
        // read of the fight, and a random ring puts them in front of her.
        const Vec2 at = anchor - Vec2::fromAngle(facing, anchorRadius);
        const Entity child = spawnEscort(world, terrain, content, childIndex, childRarity, at,
                                         nestRealm, nest, nowMillis, rng, 1);
        if (child == NULL_ENTITY) continue;
        if (lifetimeMillis > 0.0) {
            world.add<Lifetime>(child, Lifetime{lifetimeMillis / 1000.0});
        }
        if (Spawner* again = world.tryGet<Spawner>(nest)) again->children.push_back(child);
    }

    waveNests_->collect(scratchChildren_);
    for (const Entity nest : scratchChildren_) {
        NestWaves* waves = world.tryGet<NestWaves>(nest);
        const Transform* transform = world.tryGet<Transform>(nest);
        const MobType* type = world.tryGet<MobType>(nest);
        const Health* health = world.tryGet<Health>(nest);
        if (waves == nullptr || transform == nullptr || type == nullptr || health == nullptr) {
            continue;
        }

        std::size_t live = 0;
        for (const Entity child : waves->children) {
            if (world.isAlive(child)) waves->children[live++] = child;
        }
        waves->children.resize(live);

        // A hole answers damage, not a clock. Healing (or being spawned) only
        // moves the mark, so an untouched hole never sends anything at all.
        const double current = health->current;
        const double previous = waves->previousHealth;
        waves->previousHealth = current;
        if (current >= previous) continue;

        const MobConfig& config = content.mob(waves->mobIndex);
        if (config.spawnWaves.empty()) continue;

        // Everything the nest owns is read out here: `waves`, `health` and
        // `transform` all point into an archetype column and do not survive the
        // first create() below. The wave lists live in the registry rather than
        // in the world, so they are safe to walk while entities are appearing.
        const int lastWave = static_cast<int>(config.spawnWaves.size()) - 1;
        const double maxHealth = health->max > 0.0 ? health->max : 1.0;
        const Rarity nestRarity = type->rarity;
        const Vec2 anchor = transform->position;
        const Realm nestRealm = transform->realm;
        const Body* body = world.tryGet<Body>(nest);
        const double anchorRadius = body != nullptr ? body->radius : kMobBaseRadius;

        // Both ends are clamped into the list. An overkill drives `current` far
        // negative, and an unclamped end index turns the loop below into
        // millions of iterations that all just skip -- a flat-heap CPU hang.
        const int startBand =
            std::min(lastWave, static_cast<int>(std::floor(previous / maxHealth * lastWave)));
        const int endBand =
            std::max(0, static_cast<int>(std::ceil(current / maxHealth * lastWave)));

        // Counted DOWN from the health the hole had, so the band escalates as it
        // is worn away and one big hit releases every band it crossed at once.
        for (int band = startBand; band >= endBand; --band) {
            const int index = lastWave - band;
            if (index < 0 || index > lastWave) continue;
            for (const std::uint16_t member : config.spawnWaves[static_cast<std::size_t>(index)]) {
                const Entity child =
                    spawnEscort(world, terrain, content, member, nestRarity,
                                escortRingPoint(anchor, anchorRadius, kWaveEscortGap, rng),
                                nestRealm, nest, nowMillis, rng, 1);
                if (child == NULL_ENTITY) break;   // the global cap, nothing else
                if (NestWaves* again = world.tryGet<NestWaves>(nest)) {
                    again->children.push_back(child);
                }
            }
        }

        if (NestWaves* again = world.tryGet<NestWaves>(nest)) {
            again->nextWave = static_cast<std::uint16_t>(std::max(0, lastWave - endBand));
        }
    }
}

// ---------------------------------------------------------------------------
// Spawn rectangles
// ---------------------------------------------------------------------------

void SpawnSystem::rebuildZones(const ContentRegistry& content) {
    if (zoneMaps_ == worldMaps && zoneContentHash_ == content.contentHash()) return;
    zoneMaps_ = worldMaps;
    zoneContentHash_ = content.contentHash();
    zones_.clear();
    regions_.clear();
    if (worldMaps == nullptr) return;

    // Every staged map's bands, each tagged with the realm it belongs to. One
    // flat list rather than a list per map: the passes that walk it are already
    // filtering (by viewport, by realm, by tier), and a second level of
    // indirection would buy nothing at these counts.
    for (const MapData& map : worldMaps->maps()) {
        for (const MapElement& element : map.elements()) {
            if (!element.isSpawnBand() && !element.isMobRegion()) continue;
            SpawnZone zone;
            zone.bounds = element.bounds;
            zone.polygon = element.polygon;
            zone.difficulty = element.difficulty;
            zone.realm = map.realm();
            zone.mobs = element.mobDistribution;

            // The rows are resolved to indices ONCE, here, rather than on every
            // spawn: a group name is a hash probe and a busy band rolls several
            // times a second. It is also the only moment both halves are in
            // hand -- the map, which has the names, and the content, which has
            // the groups -- so it is where a typo can be reported.
            zone.resolved.reserve(zone.mobs.size());
            for (const ZoneMobEntry& row : zone.mobs) {
                SpawnZone::ResolvedRow resolved;
                resolved.weight = row.weight;
                resolved.group = content.mobGroupIndex(row.name);
                // Groups win over mob ids, so naming a group is unambiguous
                // even if some mob shares its name.
                if (resolved.group == kInvalidIndex) resolved.mob = content.mobIndex(row.name);
                if (resolved.group == kInvalidIndex && resolved.mob == kInvalidIndex &&
                    unknownZoneMobs_.insert(row.name).second) {
                    std::fprintf(stderr,
                                 "[spawn] map \"%s\" names \"%s\", which is neither a mob group "
                                 "nor a mob\n",
                                 map.id().c_str(), row.name.c_str());
                }
                zone.resolved.push_back(resolved);
            }
            if (element.isMobRegion()) {
                // A region owns no population, so none of the bookkeeping
                // below applies to it: no target, no fill, no section mask.
                regions_.push_back(std::move(zone));
                continue;
            }

            // The OUTLINE's area, not the bounding box's: a diagonal band
            // covers about half its box, and sizing its population by the box
            // would pack it at twice the density of a rectangular band next
            // door.
            //
            // Rounded UP and never zero: the smallest bands on the map are a
            // few hundred units across and would otherwise be permanently
            // empty.
            zone.targetMobs =
                std::max(1, static_cast<int>(std::ceil(kTargetMobDensity * element.area())));
            zones_.push_back(std::move(zone));
        }

        // A map with no band on it grows NOTHING. That is the rule, not a
        // failure -- the author draws where the mobs are -- but the symptom is
        // an empty world, which reads as a bug in the spawner rather than as a
        // gap in the data, so it is said out loud here as well as on the map's
        // own load line.
        //
        // Not at BOOT, though: this runs from SpawnSystem::run, and the server
        // returns out of its tick while nobody is playing, so an idle server
        // never reaches it. What an author staging a map sees in the boot log
        // is the map's own `[map] ... NO SPAWN BANDS` line (map_elements.cpp);
        // this one adds what to DO about it, the first time anyone joins.
        const bool hasBand = std::any_of(zones_.begin(), zones_.end(), [&](const SpawnZone& band) {
            return band.realm == map.realm();
        });
        if (!hasBand) {
            if (unknownZoneMobs_.insert("<" + map.id() + ":bands>").second) {
                std::fprintf(stderr,
                             "[spawn] map \"%s\" has no spawn band on it, so no mob will ever "
                             "spawn there -- draw a `spawn` object with a `difficulty` to "
                             "populate it\n",
                             map.id().c_str());
            }
            continue;
        }

        // It has bands, but nothing anywhere says WHAT they grow: no band
        // names a roster, no region draws over the map and it declares no
        // `defaultMobGroup`. Every fill will roll kInvalidIndex and place
        // nothing, which looks identical to having no bands at all.
        const bool saysSomething =
            !map.defaultMobGroup().empty() ||
            std::any_of(regions_.begin(), regions_.end(),
                        [&](const SpawnZone& region) { return region.realm == map.realm(); }) ||
            std::any_of(zones_.begin(), zones_.end(), [&](const SpawnZone& band) {
                return band.realm == map.realm() && !band.resolved.empty();
            });
        if (!saysSomething && unknownZoneMobs_.insert("<" + map.id() + ":default>").second) {
            std::fprintf(stderr,
                         "[spawn] map \"%s\" names no mob group anywhere -- no `mobs` on any "
                         "band, no mob region and no `defaultMobGroup`; its bands will stay "
                         "empty\n",
                         map.id().c_str());
        }
    }
}

std::uint16_t SpawnSystem::rollResolvedRows(const ContentRegistry& content,
                                           const std::vector<SpawnZone::ResolvedRow>& rows,
                                           Rarity rarity, Rng& rng) const {
    if (rows.empty()) return kInvalidIndex;

    double total = 0.0;
    for (const SpawnZone::ResolvedRow& row : rows) total += std::max(0.0, row.weight);
    // A table nobody weighted still spawns something rather than nothing.
    const SpawnZone::ResolvedRow* chosen = &rows.front();
    if (total > 0.0) {
        double roll = rng.unit() * total;
        for (const SpawnZone::ResolvedRow& row : rows) {
            roll -= std::max(0.0, row.weight);
            if (roll <= 0.0) {
                chosen = &row;
                break;
            }
        }
    }

    // A group defers to that group's own weights, so `ocean` in a garden band
    // spawns exactly what the ocean would.
    if (chosen->group != kInvalidIndex) {
        return chooseGroupMob(content, chosen->group, rarity, rng);
    }
    // Named outright, which bypasses the group roll entirely -- that roll
    // excludes `neverAmbient` mobs, and this is how one reaches the world.
    if (chosen->mob == kInvalidIndex || chosen->mob >= content.mobCount()) return kInvalidIndex;
    return chosen->mob;
}

std::uint16_t SpawnSystem::chooseZoneMobType(const ContentRegistry& content,
                                            const SpawnZone& zone, Vec2 at, Rarity rarity,
                                            Rng& rng) {
    // No distribution of its own: whatever this ground grows anyway. That is
    // what a band saying only `difficulty: 50` means -- the same mobs as the
    // meadow beside it, three tiers up.
    if (zone.resolved.empty()) return chooseRegionMobAt(content, zone.realm, at, rarity, rng);
    return rollResolvedRows(content, zone.resolved, rarity, rng);
}

// ---------------------------------------------------------------------------
// Stocking the map
// ---------------------------------------------------------------------------

void SpawnSystem::stockSpawnZones(World& world, const Terrain& terrain,
                                  const ContentRegistry& content,
                                  const std::vector<Viewer>& viewers, Rng& rng,
                                  double nowMillis) {
    // No band anywhere means no ambient mob anywhere. There is no second pass
    // behind this one that would cover the ground the author left unbanded.
    if (zones_.empty()) return;
    if (nowMillis < nextZoneMillis_) return;
    nextZoneMillis_ = nowMillis + kZoneIntervalMillis;

    for (std::size_t index = 0; index < zones_.size(); ++index) {
        SpawnZone& zone = zones_[index];
        // The band's whole population, awake and asleep. Nothing about a
        // viewport appears in this test: a band on the far side of the map is
        // stocked to exactly the same number as the one under the player's
        // feet, which is the change this whole file is built around.
        //
        // A full band costs one subtraction per pass, which is what makes it
        // affordable to ask the question of every band on every map every
        // second.
        int owed = zone.targetMobs - static_cast<int>(zone.latent.size()) - zone.liveMobs;
        if (owed <= 0) continue;
        owed = std::min(owed, kZoneStockPerPass);
        for (int n = 0; n < owed; ++n) {
            if (!stockZone(world, terrain, content, zone, static_cast<std::uint16_t>(index),
                           viewers, rng, nowMillis)) {
                // The outline had nowhere to put this one, so it has nowhere
                // to put the next either: drop the rest of the debt rather
                // than spinning on a band the terrain has since walled over.
                break;
            }
        }
    }
}

bool SpawnSystem::stockZone(World& world, const Terrain& terrain, const ContentRegistry& content,
                            SpawnZone& zone, std::uint16_t zoneIndex,
                            const std::vector<Viewer>& viewers, Rng& rng, double nowMillis,
                            Vec2 anchor, double scatter) {
    // Everything about this happens in the band's OWN realm: the map it is
    // drawn on has its own size, its own walls and its own population, and the
    // same numbers on the overworld describe somewhere else entirely.
    const Vec2 extent = terrain.realmExtent(zone.realm);
    Vec2 at;
    bool placed = false;
    for (int attempt = 0; attempt < kZonePlacementAttempts; ++attempt) {
        // Near where the last one was lost, or anywhere in the outline. See
        // kRespawnScatter: the difference between the two is the difference
        // between a band that is evenly full and one that is full on paper.
        const Vec2 candidate = scatter > 0.0
                                   ? samplePointNear(anchor, scatter, extent, rng)
                                   : samplePointInRect(zone.bounds, extent, rng);
        // Rejection sampling over the bounding box keeps the distribution
        // uniform over the outline. A candidate in a corner the polygon does
        // not cover is thrown away like any other unusable one, so a zone that
        // fills little of its box simply spends more of its attempts -- which
        // is why there are attempts rather than one shot.
        if (!zoneContains(zone.bounds, zone.polygon, candidate)) continue;
        if (inBorderBand(candidate, extent)) continue;
        if (terrain.blocked(candidate, zone.realm)) continue;
        if (nearAnyPlayer(viewers, zone.realm, candidate, kMinSpawnDistance)) continue;
        if (crowdedAt(zone.realm, candidate, kPreliminarySpawnRadius, kMinMobSpawnSpacing, zone)) {
            continue;
        }
        at = candidate;
        placed = true;
        break;
    }
    if (!placed) return false;

    // A band belongs to nobody's viewport, so anything charged to luck is
    // charged to whoever is standing nearest its centre -- the reference's own
    // attribution rule for a zone fill. The bounding box's centre, which for a
    // concave band is not inside it; it is an attribution tiebreak, not a
    // placement, so that costs nothing. With nobody near the band at all --
    // which is now the usual case, because a band stocks itself whether or not
    // anyone is there -- this is neutral luck, and neutral luck is exactly
    // what the difficulty anchors are stated at.
    const Vec2 centre{zone.bounds.x + zone.bounds.w * 0.5, zone.bounds.y + zone.bounds.h * 0.5};
    const double luck = nearestViewerLuck(viewers, zone.realm, centre);

    // The band's own DIFFICULTY, run through the one curve: this is where the
    // map's rarity progression comes from, and where every boss in the world
    // now comes from -- a difficulty-200 band is full of supers because its
    // author said so, not because a separate pass decided the world was owed
    // one.
    Rarity rarity = rollSpawnRarity(zone.difficulty, luck, rng);
    std::uint16_t type = chooseZoneMobType(content, zone, at, rarity, rng);
    if (type == kInvalidIndex) return false;
    // A band naming a mob outright can name one below its own tier; the mob's
    // floor wins, exactly as it does on every other spawn path. Read the tier
    // AFTER the floor, because that is the tier that decides whether this is a
    // record or an event.
    rarity = clampRarity(std::max(rarityIndex(rarity), rarityIndex(content.mob(type).minRarity)));

    if (!alwaysAwake(content, type, rarity)) {
        // The ordinary case, and the whole reason the map can be full: a
        // position, a type and a tier. No entity, no components, no wire id.
        //
        // WHEN it may wake is the one subtlety. A record placed where nobody
        // was looking is ready at once, so an unvisited band is full the
        // moment somebody walks into it. A record placed inside a live
        // viewport is a REPLACEMENT for something that just died there, and it
        // waits: without the wait, clearing the mobs around you would refill
        // them in front of you within half a second.
        const double delay = seenBy(viewers, zone.realm, at, 0.0)
                                 ? rng.range(kInViewRespawnMinMillis, kInViewRespawnMaxMillis)
                                 : 0.0;
        zone.latent.push_back(LatentMob{at, nowMillis + delay, type, rarity});
        return true;
    }

    // A boss, or a permanent fixture. Neither is ever a record: a boss is an
    // event the whole server is told about the moment it happens and has to be
    // standing where it was announced, and a dummy is the DPS row.
    const int section = sectionAt(at);
    if (content.mob(type).neverAmbient &&
        permanentFixtureExists(world, type, rarity, zone.realm, section)) {
        return false;
    }
    if (census_.mobs >= mobCap) return false;

    const Entity spawned = spawnMobAt(world, terrain, content, type, rarity, at, zone.realm,
                                      nowMillis, rng, 0, zoneIndex);
    if (spawned == NULL_ENTITY) return false;
    ++zone.liveMobs;
    // Counted straight away, so the rest of this pass spaces itself against
    // what it has just placed rather than against the last census alone --
    // in the band's realm, or crowdedAt() would never see it.
    if (const Transform* transform = world.tryGet<Transform>(spawned)) {
        const Body* body = world.tryGet<Body>(spawned);
        mobPlacements_.push_back(MobPlacement{transform->position,
                                              body != nullptr ? body->radius : 0.0,
                                              transform->realm});
    }
    return true;
}

void SpawnSystem::bankCasualties(World& world, const Terrain& terrain,
                                 const ContentRegistry& content,
                                 const std::vector<Viewer>& viewers, Rng& rng, double nowMillis) {
    if (zones_.empty()) return;

    casualtyList_.clear();
    casualties_->each([&](Entity, MobTag&, Transform& transform, AmbientMob& ambient, Dead&) {
        if (ambient.zone >= zones_.size()) return;
        casualtyList_.push_back(Casualty{transform.position, ambient.zone});
        // Claimed, once and for all. A corpse lies around for a while before
        // the reaper takes it, and a band that banked it on every pass in
        // between would breed a mob per pass out of one kill.
        ambient.zone = kInvalidIndex;
    });

    // Collected first, spent afterwards: stocking a boss creates an entity,
    // and a create() relocates the very rows the walk above is holding.
    //
    // A slot this cannot place -- it died in a pocket of wall, or against a
    // crowd -- is simply not placed. The band is then one short of its target,
    // which is exactly what the top-up pass exists to notice, so the mob comes
    // back somewhere else in the band rather than being lost.
    for (const Casualty& casualty : casualtyList_) {
        SpawnZone& zone = zones_[casualty.zone];
        stockZone(world, terrain, content, zone, casualty.zone, viewers, rng, nowMillis,
                  casualty.position, kRespawnScatter);
    }
}

// ---------------------------------------------------------------------------
// Waking it up
// ---------------------------------------------------------------------------

void SpawnSystem::promoteLatent(World& world, const Terrain& terrain,
                                const ContentRegistry& content,
                                const std::vector<Viewer>& viewers, Rng& rng, double nowMillis) {
    // Nobody is looking at anything, so nothing has to be awake. This is also
    // what keeps an unattended server from paying for the population it holds:
    // the whole map is stocked and not one of it is simulated.
    if (viewers.empty()) return;

    for (std::size_t index = 0; index < zones_.size(); ++index) {
        SpawnZone& zone = zones_[index];
        if (zone.latent.empty()) continue;
        // The cheap rejection, and the reason this is affordable at all: one
        // rectangle test per viewer says whether the band's records are worth
        // walking, and on a map with two dozen bands almost none of them are.
        if (!zoneInView(zone.bounds, zone.realm, viewers, kLatentWakeMargin)) continue;

        int budget = kZoneWakePerPass;
        for (std::size_t i = 0; i < zone.latent.size() && budget > 0;) {
            const LatentMob& record = zone.latent[i];
            if (nowMillis < record.readyMillis ||
                !seenBy(viewers, zone.realm, record.position, kLatentWakeMargin)) {
                ++i;
                continue;
            }
            if (census_.mobs >= mobCap) return;

            const Entity spawned =
                spawnMobAt(world, terrain, content, record.mobIndex, record.rarity,
                           record.position, zone.realm, nowMillis, rng, 0,
                           static_cast<std::uint16_t>(index));
            // Gone from the records either way. A record whose type the
            // content no longer defines cannot be woken and must not be
            // retried every pass forever; dropping it lets the band stock a
            // replacement it can actually place.
            zone.latent[i] = zone.latent.back();
            zone.latent.pop_back();
            if (spawned == NULL_ENTITY) continue;
            // census_.mobs is spawnMobAt's to raise -- it is the cap every
            // spawn path shares, and counting it twice here would halve the
            // ceiling for everything else in the same pass.
            ++zone.liveMobs;
            ++census_.promotedTotal;
            --budget;
            if (const Transform* transform = world.tryGet<Transform>(spawned)) {
                const Body* body = world.tryGet<Body>(spawned);
                mobPlacements_.push_back(MobPlacement{transform->position,
                                                      body != nullptr ? body->radius : 0.0,
                                                      transform->realm});
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Reading the latent population back
// ---------------------------------------------------------------------------

int SpawnSystem::latentCount() const {
    std::size_t total = 0;
    for (const SpawnZone& zone : zones_) total += zone.latent.size();
    return static_cast<int>(total);
}

void SpawnSystem::latentSites(Realm realm, std::vector<LatentSite>& out) const {
    for (const SpawnZone& zone : zones_) {
        if (zone.realm != realm) continue;
        for (const LatentMob& record : zone.latent) {
            out.push_back(
                LatentSite{record.position, record.mobIndex, record.rarity, zone.difficulty});
        }
    }
}

// ---------------------------------------------------------------------------
// Announcements
// ---------------------------------------------------------------------------

void SpawnSystem::announceIfNotable(const ContentRegistry& content, Entity entity,
                                    std::uint16_t mobIndex, Rarity rarity, Vec2 position,
                                    Realm realm) {
    if (rarityIndex(rarity) < rarityIndex(kAnnouncedRarity)) return;
    // A permanent fixture is not an event. The DPS row is built out of bands
    // that name the target dummy outright, up to unique, and announcing those
    // would put a line in chat for a post nobody has to fight.
    if (mobIndex >= content.mobCount() || content.mob(mobIndex).neverAmbient) return;
    // Oldest first, so a server that never drains this keeps the announcements
    // somebody might still care about instead of the ones from an hour ago.
    if (bossSpawns.size() >= kMaxPendingBossSpawns) bossSpawns.erase(bossSpawns.begin());
    bossSpawns.push_back(BossSpawn{entity, mobIndex, rarity, position, realm});
}

} // namespace flix
