#include "server/systems/spawning.h"

#include <algorithm>
#include <cmath>

namespace flix {
namespace {

/// Mobs the recycler is not allowed to touch.
///
/// A boss tier is placed deliberately and is meant to be found and fought, and
/// a target dummy is furniture someone walked away from. Both persist until
/// something kills them, however long nobody looks at them.
bool neverDespawns(const ContentRegistry& content, const MobType& type) {
    if (rarityIndex(type.rarity) >= rarityIndex(Rarity::Ultra)) return true;
    return content.mob(type.configIndex).neverAmbient;
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

/// The largest body that roll can produce. Used to space a spawn against its
/// neighbours before the roll itself has happened.
double sizeJitterCeiling(const MobConfig& config, Rarity rarity) {
    const double fixture = fixtureSizeScale(config, rarity);
    if (!(config.randomSizeMax > config.randomSizeMin) || !(config.size > 0.0)) {
        return std::max(config.randomSizeMin, config.randomSizeMax) * fixture;
    }
    return config.randomSizeMax / config.size * fixture;
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

/// The mobs one player is owed: the reference's world density over their own
/// buffered viewport, which is where the default of 16 comes from
/// (src/server/enemySpawner.ts:573-583).
int viewerMobTarget(const SpawnSystem::Viewer& viewer) {
    const double area = 2.0 * viewer.half.x * 2.0 * viewer.half.y;
    return std::max(1, static_cast<int>(std::ceil(kTargetMobDensity * area)));
}

/// The luck a spawn placed at `at` is charged to: the closest flower to it.
/// What the reference does for a zone fill, which belongs to nobody's viewport
/// (src/server/enemySpawner.ts:855-869).
double nearestViewerLuck(const std::vector<SpawnSystem::Viewer>& viewers, Vec2 at) {
    const SpawnSystem::Viewer* nearest = nullptr;
    double nearestDistSq = 0.0;
    for (const SpawnSystem::Viewer& viewer : viewers) {
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
bool inBorderBand(Vec2 position) {
    return position.x < kWorldBoundaryThreshold ||
           position.x > kWorldSize - kWorldBoundaryThreshold ||
           position.y < kWorldBoundaryThreshold ||
           position.y > kWorldSize - kWorldBoundaryThreshold;
}

/// No spawn lands in anyone's lap, whoever asked for it.
bool nearAnyPlayer(const std::vector<SpawnSystem::Viewer>& viewers, Vec2 position, double radius) {
    const double radiusSq = radius * radius;
    for (const SpawnSystem::Viewer& viewer : viewers) {
        if (distanceSq(viewer.position, position) < radiusSq) return true;
    }
    return false;
}

/// The one-step tier drift every spawn path ends with: one roll up and, only
/// if that misses, one roll down, so the two can never both apply.
Rarity applyTierDrift(Rarity rarity, double luck, Rng& rng) {
    const double upgradeChance = clamp(0.02 + std::max(0.0, luck) * 0.01, 0.0, 1.0);
    if (rng.chance(upgradeChance)) return upgradeRarity(rarity);
    if (rng.chance(dropDowngradeChance(rarity))) return downgradeRarity(rarity);
    return rarity;
}

/// True when a rectangle overlaps any player's buffered viewport. What decides
/// whether a spawn zone is worth stocking at all: the map has 151 of them and
/// only the handful somebody can see are simulated.
bool zoneInView(const Rect& bounds, const std::vector<SpawnSystem::Viewer>& viewers) {
    for (const SpawnSystem::Viewer& viewer : viewers) {
        if (bounds.left() < viewer.position.x + viewer.half.x &&
            bounds.right() > viewer.position.x - viewer.half.x &&
            bounds.top() < viewer.position.y + viewer.half.y &&
            bounds.bottom() > viewer.position.y - viewer.half.y) {
            return true;
        }
    }
    return false;
}

/// Announcements held for a runtime that has not drained them. Two bosses a
/// minute at the very most, so this is a leak guard rather than a queue depth.
constexpr std::size_t kMaxPendingBossSpawns = 16;

/// A uniform point in `bounds`, clamped into the world. The map's rectangles
/// are allowed to hang over the edge and several do.
Vec2 samplePointInRect(const Rect& bounds, Rng& rng) {
    return {clamp(bounds.x + rng.unit() * bounds.w, 0.0, kWorldSize),
            clamp(bounds.y + rng.unit() * bounds.h, 0.0, kWorldSize)};
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

void SpawnSystem::rebuildCandidates(const ContentRegistry& content) {
    if (candidateContent_ == &content && candidateHash_ == content.contentHash()) return;
    candidateContent_ = &content;
    candidateHash_ = content.contentHash();

    for (SectionCandidates& section : candidates_) {
        section.mobs.clear();
        section.cumulative.clear();
    }

    for (std::size_t i = 0; i < content.mobCount(); ++i) {
        const std::uint16_t index = static_cast<std::uint16_t>(i);
        const MobConfig& config = content.mob(index);
        // A zero weight is how the data says "reachable only by other means":
        // centipede body segments belong to a section but are never rolled.
        // `neverAmbient` is the same rule stated by name instead of by weight --
        // the dummy declares no spawn_weight at all, so it would otherwise
        // inherit the default 1.0 and take a fifth of section 7's spawns.
        if (!(config.spawnWeight > 0.0) || config.sectionMask == 0) continue;
        if (config.neverAmbient) continue;
        for (int s = 0; s < kSectionCount; ++s) {
            if ((config.sectionMask & (1u << s)) == 0) continue;
            SectionCandidates& section = candidates_[static_cast<std::size_t>(s)];
            const double running = section.cumulative.empty() ? 0.0 : section.cumulative.back();
            section.mobs.push_back(index);
            section.cumulative.push_back(running + config.spawnWeight);
        }
    }
}

std::uint16_t SpawnSystem::chooseMobType(const ContentRegistry& content, int section, Rng& rng) {
    return chooseMobType(content, section, Rarity::Common, rng);
}

std::uint16_t SpawnSystem::chooseMobType(const ContentRegistry& content, int section,
                                         Rarity rarity, Rng& rng) {
    rebuildCandidates(content);
    if (section < 0 || section >= kSectionCount) return kInvalidIndex;

    const SectionCandidates& candidates = candidates_[static_cast<std::size_t>(section)];
    if (candidates.mobs.empty()) return kInvalidIndex;

    double total = 0.0;
    for (const std::uint16_t index : candidates.mobs) {
        const MobStats stats = content.mobStats(index, rarity);
        if (stats.spawnsIn(section) && stats.spawnWeight > 0.0) total += stats.spawnWeight;
    }
    if (!(total > 0.0)) return kInvalidIndex;

    double roll = rng.unit() * total;
    std::uint16_t last = kInvalidIndex;
    for (const std::uint16_t index : candidates.mobs) {
        const MobStats stats = content.mobStats(index, rarity);
        if (!stats.spawnsIn(section) || !(stats.spawnWeight > 0.0)) continue;
        last = index;
        roll -= stats.spawnWeight;
        if (roll < 0.0) return index;
    }
    return last;
}

bool SpawnSystem::chooseBiomeSpawn(const ContentRegistry& content, const MapElement& biome,
                                   int section, double luck, Rng& rng, std::uint16_t& typeOut,
                                   Rarity& rarityOut) {
    if (biome.spawnTable.empty()) return false;

    double total = 0.0;
    for (const BiomeSpawnEntry& row : biome.spawnTable) total += std::max(0.0, row.weight);
    // Weightless table: the reference's reduce/compare walk falls out of its
    // loop and returns the FIRST row, so a table nobody weighted still spawns
    // something rather than nothing.
    const BiomeSpawnEntry* chosen = &biome.spawnTable.front();
    if (total > 0.0) {
        double roll = rng.unit() * total;
        for (const BiomeSpawnEntry& row : biome.spawnTable) {
            roll -= std::max(0.0, row.weight);
            if (roll <= 0.0) {
                chosen = &row;
                break;
            }
        }
    }

    Rarity tier = chosen->tier;
    std::uint16_t index = kInvalidIndex;
    if (!chosen->mobType.empty()) {
        // Named outright, which bypasses the candidate table entirely -- that
        // table excludes `neverAmbient` mobs, and this is how they get in.
        index = content.mobIndex(chosen->mobType);
    } else {
        // No name: whatever this section admits at the row's tier, weighted
        // exactly as an ordinary ambient roll would weight it.
        index = chooseMobType(content, section, tier, rng);
    }
    if (index == kInvalidIndex || index >= content.mobCount()) return false;

    const MobConfig& config = content.mob(index);
    // A permanent fixture keeps exactly the rarity the map asked for. Drifting
    // off it would produce a tier the one-per-section check below never
    // cleared, and a dummy that slips through is there forever.
    if (!config.neverAmbient) tier = applyTierDrift(tier, luck, rng);

    typeOut = index;
    rarityOut = clampRarity(std::max(rarityIndex(tier), rarityIndex(config.minRarity)));
    return true;
}

bool SpawnSystem::permanentFixtureExists(World& world, std::uint16_t mobIndex, Rarity rarity,
                                         int section) {
    bind(world);
    bool found = false;
    allMobs_->each([&](Entity, MobTag&, Transform& transform, MobType& type) {
        if (found) return;
        if (type.configIndex != mobIndex || type.rarity != rarity) return;
        if (sectionAt(transform.position) != section) return;
        found = true;
    });
    return found;
}

Rarity SpawnSystem::rollRarity(const MobConfig& config, Rng& rng) {
    const Rarity rolled = rollNaturalRarity(-1, 1.0, rng);
    return clampRarity(std::max(rarityIndex(rolled), rarityIndex(config.minRarity)));
}

Rarity SpawnSystem::rollNaturalRarity(int section, double luck, Rng& rng) {
    static constexpr std::array<double, kRarityCount> kEqualRarityWeights = {
        0.94 / 6.0, 0.94 / 6.0, 0.94 / 6.0, 0.94 / 6.0,
        0.94 / 6.0, 0.94 / 6.0, 0.05, 0.001, 0.0, 0.0,
    };
    const auto& weights = section == 7 ? kEqualRarityWeights : kNaturalSpawnWeight;
    double total = 0.0;
    for (const double weight : weights) total += weight;

    int tier = 0;
    if (total > 0.0) {
        double roll = rng.unit() * total;
        for (int i = 0; i < kRarityCount; ++i) {
            if (weights[static_cast<std::size_t>(i)] <= 0.0) continue;
            tier = i;
            roll -= weights[static_cast<std::size_t>(i)];
            if (roll < 0.0) break;
        }
    }

    return applyTierDrift(clampRarity(tier), luck, rng);
}

// ---------------------------------------------------------------------------
// Placing a mob
// ---------------------------------------------------------------------------

Entity SpawnSystem::spawnMob(World& world, const Terrain& terrain, const ContentRegistry& content,
                             std::uint16_t mobIndex, Rarity rarity, Vec2 position, Realm realm,
                             double nowMillis, Rng& rng) {
    return spawnMobAt(world, terrain, content, mobIndex, rarity, position, realm, nowMillis, rng, 0);
}

Entity SpawnSystem::spawnMobAt(World& world, const Terrain& terrain, const ContentRegistry& content,
                               std::uint16_t mobIndex, Rarity rarity, Vec2 position, Realm realm,
                               double nowMillis, Rng& rng, int depth) {
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

    world.add<AmbientMob>(e, AmbientMob{nowMillis});
    world.add<Replicated>(e, Replicated{net::EntityKind::Mob, 0, mobIndex, rarity, 0});
    if (netIds != nullptr) world.add<NetId>(e, NetId{netIds->next()});

    // The census is the OVERWORLD's population. Another realm's mobs are
    // counted by the spawner that fills it, and a maze coordinate would land
    // in a section of a map it is not on.
    if (realm == Realm::Overworld) {
        ++census_.mobs;
        ++census_.spawnedTotal;
        const int section = sectionAt(at);
        if (section >= 0) ++census_.perSection[static_cast<std::size_t>(section)];
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
    const double spacing = bodyStats.radius * 2.0 * kCentipedeSegmentSpacingScale;

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
        const Entity segment = spawnMobAt(world, terrain, content, config.segmentBodyIndex, rarity,
                                          at, realm, nowMillis, rng, depth);
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

Entity SpawnSystem::spawnEscort(World& world, const Terrain& terrain, const ContentRegistry& content,
                                std::uint16_t childIndex, Rarity nestRarity, Vec2 at, Realm realm,
                                Entity parent, double nowMillis, Rng& rng, int depth) {
    if (census_.mobs >= mobCap) return NULL_ENTITY;
    const Entity child =
        spawnMobAt(world, terrain, content, childIndex, nestRarity, at, realm, nowMillis, rng, depth);
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
    rebuildCandidates(content);
    rebuildZones();
    gatherViewers(world, players);

    expireEscorts(dt, commands);
    runNests(world, terrain, content, rng, nowMillis);

    // The census is O(mobs x players) and nothing about a population of a few
    // hundred changes meaningfully inside 200ms.
    if (nowMillis >= nextPopulationMillis_) {
        nextPopulationMillis_ = nowMillis + kPopulationIntervalMillis;
        takeCensus(content, viewers_, nowMillis, commands);
        fillNeighbourhoods(world, terrain, content, viewers_, rng, nowMillis);
    }

    // Three independent clocks in the reference, and they stay independent
    // here: the zones and the bosses read the last census rather than taking
    // one of their own, so neither is tied to the density pass's cadence.
    runSpawnZones(world, terrain, content, viewers_, rng, nowMillis);
    runSpecialMobs(world, terrain, content, viewers_, rng, nowMillis);
}

void SpawnSystem::gatherViewers(World& world, const std::vector<RealmPoint>& players) {
    // Every OVERWORLD flower, with the two facts a coordinate cannot carry. A
    // client that reported nothing keeps the default box, exactly as the
    // reference's `player.viewportWidth || VIEWPORT_WIDTH` does.
    worldViewers_.clear();
    playerBodies_->each([&](Entity e, PlayerTag&, Transform& transform) {
        if (transform.realm != Realm::Overworld) return;
        Viewer viewer;
        viewer.position = transform.position;
        if (const PlayerLocation* location = world.tryGet<PlayerLocation>(e)) {
            viewer.half = {location->viewport.x * 0.5 + kViewportBuffer,
                           location->viewport.y * 0.5 + kViewportBuffer};
        }
        if (const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(e)) {
            viewer.luck = modifiers->luck;
        }
        worldViewers_.push_back(viewer);
    });

    // The caller's list stays the list -- it decides WHO the population is kept
    // for -- and each entry is only paired with the flower standing on it. One
    // that pairs with nothing is a bare coordinate from a harness, and keeps
    // the defaults above.
    viewers_.clear();
    viewers_.reserve(players.size());
    realmOccupied_.fill(false);
    for (const RealmPoint& player : players) {
        realmOccupied_[realmIndex(player.realm)] = true;
        if (player.realm != Realm::Overworld) continue;
        const Vec2 position = player.position;
        Viewer viewer;
        viewer.position = position;
        double nearestDistSq = kViewerMatchRadius * kViewerMatchRadius;
        for (const Viewer& candidate : worldViewers_) {
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
    census_.perSection.fill(0);
    neighbours_.assign(viewers.size(), 0);
    doomed_.clear();

    mobPlacements_.clear();

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
        if (transform.realm != Realm::Overworld) {
            if (realmOccupied_[realmIndex(transform.realm)]) {
                ambient.lastNearPlayerMillis = nowMillis;
            } else if (nowMillis - ambient.lastNearPlayerMillis >= kMobDespawnDelayMillis) {
                doomed_.push_back(e);
            }
            return;
        }
        bool nearAnyone = unattended;
        for (std::size_t i = 0; i < viewers.size(); ++i) {
            // Each flower's OWN box, not one 1920x1080 rectangle for everybody:
            // a mob at the edge of an ultrawide screen is being drawn, and
            // starting its recycle clock is what makes it blink out in front of
            // its owner (src/server/playerState.ts:1041).
            const Vec2 offset = transform.position - viewers[i].position;
            if (std::abs(offset.x) <= viewers[i].half.x &&
                std::abs(offset.y) <= viewers[i].half.y) {
                ++neighbours_[i];
                nearAnyone = true;
            }
        }

        if (nearAnyone) {
            ambient.lastNearPlayerMillis = nowMillis;
        } else if (nowMillis - ambient.lastNearPlayerMillis >= kMobDespawnDelayMillis &&
                   !neverDespawns(content, type)) {
            // Left out of the counts on purpose: it is on its way out, and
            // counting it would suppress the replacement spawn for one pass.
            doomed_.push_back(e);
            return;
        }

        ++census_.mobs;
        mobPlacements_.push_back(MobPlacement{transform.position, body.radius});
        const int section = sectionAt(transform.position);
        if (section >= 0) ++census_.perSection[static_cast<std::size_t>(section)];
    });

    for (const Entity e : doomed_) commands.destroy(e);
    census_.despawnedTotal += static_cast<int>(doomed_.size());
}

bool SpawnSystem::placementAllowed(const Terrain& terrain, const std::vector<Viewer>& viewers,
                                   Vec2 position, int& sectionOut) const {
    sectionOut = sectionAt(position);
    if (sectionOut < 0) return false;
    // The border band is refused outright, before anything else is asked about
    // the point.
    if (inBorderBand(position)) return false;
    if (terrain.blocked(position, Realm::Overworld)) return false;
    // A spawn rectangle owns its own population, at its own tier. The density
    // fill stays out of one entirely: letting it in is what fills a legendary
    // zone with commons, because this pass rolls the natural spread and knows
    // nothing about the map (src/server/enemySpawner.ts:752-756).
    if (inAnySpawnZone(position, sectionOut)) return false;
    if (nearAnyPlayer(viewers, position, kMinSpawnDistance)) return false;
    return !crowdedAt(position, kPreliminarySpawnRadius, kMinMobSpawnSpacing);
}

bool SpawnSystem::crowdedAt(Vec2 position, double halfSize, double extraGap) const {
    for (const MobPlacement& mob : mobPlacements_) {
        const double reach = halfSize + mob.radius + extraGap;
        if (distanceSq(mob.position, position) < reach * reach) return true;
    }
    return false;
}

bool SpawnSystem::inAnySpawnZone(Vec2 position, int section) const {
    if (section < 0 || section >= kSectionCount) return false;
    const std::uint16_t bit = static_cast<std::uint16_t>(1u << section);
    for (const SpawnZone& zone : zones_) {
        if ((zone.sections & bit) == 0) continue;
        // Inclusive on every edge, as the reference's own rectangle test is.
        if (position.x >= zone.bounds.left() && position.x <= zone.bounds.right() &&
            position.y >= zone.bounds.top() && position.y <= zone.bounds.bottom()) {
            return true;
        }
    }
    return false;
}

void SpawnSystem::fillNeighbourhoods(World& world, const Terrain& terrain,
                                     const ContentRegistry& content,
                                     const std::vector<Viewer>& viewers, Rng& rng,
                                     double nowMillis) {
    for (std::size_t i = 0; i < viewers.size(); ++i) {
        const Viewer& viewer = viewers[i];
        // What this player is owed follows the size of their own screen: the
        // reference multiplies the world's density by each buffered viewport
        // it is asked to keep populated, so a bigger window is a bigger
        // neighbourhood rather than a thinner one.
        const int deficit = viewerMobTarget(viewer) - neighbours_[i];
        if (deficit <= 0) continue;

        const int budget = std::min(deficit, kMaxSpawnsPerPass);
        for (int n = 0; n < budget; ++n) {
            if (census_.mobs >= mobCap) return;

            Vec2 at;
            int section = -1;
            bool placed = false;
            for (int attempt = 0; attempt < kSpawnPlacementAttempts; ++attempt) {
                // Sampled, then accepted or REJECTED -- never moved. Nudging a
                // blocked point to the nearest open ground is what turns a lake
                // into a halo of mobs around its shore and holds the population
                // flat where the reference lets it genuinely thin out.
                const Vec2 candidate = viewer.position +
                                       Vec2{rng.range(-viewer.half.x, viewer.half.x),
                                            rng.range(-viewer.half.y, viewer.half.y)};
                if (placementAllowed(terrain, viewers, candidate, section)) {
                    at = candidate;
                    placed = true;
                    break;
                }
            }
            // Every sample landed in a wall, a lake or another player's lap.
            // Give up on this player for the pass rather than burning the rest
            // of the budget on the same geometry.
            if (!placed) break;

            // Ant Hell throttle. Placed exactly where the reference places it:
            // after the position is final and before anything is rolled for it,
            // so a rejected attempt costs this player one of its three spawns
            // for the pass rather than being retried somewhere else.
            if (section == kAntHellSection && rng.unit() > kAntHellSpawnScale) continue;

            const std::size_t bucket = static_cast<std::size_t>(section);
            if (census_.perSection[bucket] >= std::min(kSectionTargetPopulation, kMaxMobsPerSection)) {
                continue;
            }

            // The reference rolls tier before type because both section
            // membership and spawn_weight may be overridden per rarity, and it
            // charges the roll to the player whose neighbourhood asked for the
            // mob: luck is what a clover loadout buys, and a spawn owned by
            // nobody would never feel it (src/server/enemySpawner.ts:775-777).
            //
            // Unless a biome declares its own table, which overrides both the
            // tier roll and the type roll for anything standing inside it.
            Rarity rarity = Rarity::Common;
            std::uint16_t type = kInvalidIndex;
            const MapElement* biome = mapData != nullptr ? mapData->biomeAt(at) : nullptr;
            if (biome != nullptr && !biome->spawnTable.empty()) {
                if (!chooseBiomeSpawn(content, *biome, section, viewer.luck, rng, type, rarity)) {
                    continue;   // the table asked for something that cannot spawn here
                }
            } else {
                rarity = rollNaturalRarity(section, viewer.luck, rng);
                type = chooseMobType(content, section, rarity, rng);
                if (type == kInvalidIndex) break;   // nothing lives in this section
                rarity = clampRarity(std::max(rarityIndex(rarity),
                                              rarityIndex(content.mob(type).minRarity)));
            }

            // A permanent fixture is admitted once per tier per section. It is
            // never despawned and effectively unkillable, so a duplicate would
            // stand there for the life of the server.
            if (content.mob(type).neverAmbient &&
                permanentFixtureExists(world, type, rarity, section)) {
                continue;
            }
            // Phase two used the same 20-unit preliminary body as TypeScript.
            // Its finalizer then repeats the overlap test with the chosen
            // rarity's actual body, which matters for mythic-and-up mobs.
            const MobStats finalStats = content.mobStats(type, rarity);
            const double finalRadius =
                finalStats.radius * sizeJitterCeiling(content.mob(type), rarity);
            if (crowdedAt(at, finalRadius, 0.0)) continue;

            const Entity spawned = spawnMob(world, terrain, content, type, rarity, at,
                                            Realm::Overworld, nowMillis, rng);
            if (spawned == NULL_ENTITY) {
                break;
            }
            if (const Transform* transform = world.tryGet<Transform>(spawned)) {
                const Body* body = world.tryGet<Body>(spawned);
                mobPlacements_.push_back(MobPlacement{transform->position,
                                                      body != nullptr ? body->radius : 0.0});
            }
            ++neighbours_[i];
        }
    }
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

void SpawnSystem::rebuildZones() {
    if (zoneMap_ == mapData) return;
    zoneMap_ = mapData;
    zones_.clear();
    if (mapData == nullptr) return;

    for (const MapElement& element : mapData->elements()) {
        if (element.kind != MapElementKind::Spawn || !element.hasSpawnTier) continue;
        SpawnZone zone;
        zone.bounds = element.bounds;
        zone.tier = element.spawnTier;
        // Rounded UP and never zero: the smallest rectangles on the map are a
        // few hundred units across and would otherwise be permanently empty.
        zone.targetMobs = std::max(
            1, static_cast<int>(std::ceil(kTargetMobDensity * element.bounds.w * element.bounds.h)));
        for (int section = 0; section < kSectionCount; ++section) {
            const Rect bounds{static_cast<double>(section % kSectionsPerAxis) * kSectionSize,
                              static_cast<double>(section / kSectionsPerAxis) * kSectionSize,
                              kSectionSize, kSectionSize};
            if (zone.bounds.intersects(bounds)) {
                zone.sections |= static_cast<std::uint16_t>(1u << section);
            }
        }
        zones_.push_back(zone);
    }
}

int SpawnSystem::countMobsInZone(const Rect& bounds) const {
    int count = 0;
    for (const MobPlacement& mob : mobPlacements_) {
        if (mob.position.x >= bounds.left() && mob.position.x <= bounds.right() &&
            mob.position.y >= bounds.top() && mob.position.y <= bounds.bottom()) {
            ++count;
        }
    }
    return count;
}

void SpawnSystem::runSpawnZones(World& world, const Terrain& terrain,
                                const ContentRegistry& content,
                                const std::vector<Viewer>& viewers, Rng& rng, double nowMillis) {
    if (zones_.empty()) return;
    if (nowMillis < nextZoneMillis_) return;
    nextZoneMillis_ = nowMillis + kZoneIntervalMillis;

    // Nobody online: every zone forgets where it was, so the next arrival gets
    // a full rectangle rather than walking into one mid-trickle.
    if (viewers.empty()) {
        for (SpawnZone& zone : zones_) {
            zone.initialized = false;
            zone.pendingFill = 0;
        }
        return;
    }

    for (SpawnZone& zone : zones_) {
        if (!zoneInView(zone.bounds, viewers)) {
            zone.initialized = false;
            zone.pendingFill = 0;
            continue;
        }

        if (!zone.initialized) {
            zone.pendingFill = std::max(0, zone.targetMobs - countMobsInZone(zone.bounds));
            zone.initialized = true;
            zone.lastWaveMillis = nowMillis;
            zone.lastTrickleMillis = nowMillis;
        }

        // A fill is drained a chunk at a time so a large rectangle entering
        // view is spread over several seconds instead of arriving as one
        // packet. A pass that placed nothing drops the rest of the debt rather
        // than spinning on a rectangle the terrain has since walled over.
        if (zone.pendingFill > 0) {
            const int chunk = std::min(zone.pendingFill, kZoneSpawnsPerPass);
            int spawned = 0;
            for (int i = 0; i < chunk; ++i) {
                if (spawnInZone(world, terrain, content, zone, viewers, rng, nowMillis) ==
                    NULL_ENTITY) {
                    break;
                }
                ++spawned;
            }
            zone.pendingFill = spawned == 0 ? 0 : std::max(0, zone.pendingFill - spawned);
            continue;
        }

        if (nowMillis - zone.lastWaveMillis >= kZoneWaveIntervalMillis) {
            const int deficit = std::max(0, zone.targetMobs - countMobsInZone(zone.bounds));
            zone.pendingFill = std::min(deficit, kZoneSpawnsPerPass * 4);
            zone.lastWaveMillis = nowMillis;
            zone.lastTrickleMillis = nowMillis;
            continue;
        }

        // Between waves, one or two at a time, and only while the rectangle is
        // below its target -- a player culling a zone sees it seep back rather
        // than snap back.
        if (nowMillis - zone.lastTrickleMillis >= kZoneTrickleIntervalMillis) {
            zone.lastTrickleMillis = nowMillis;
            const int current = countMobsInZone(zone.bounds);
            if (current >= zone.targetMobs) continue;
            const int rolled =
                kZoneTrickleMin +
                static_cast<int>(rng.below(kZoneTrickleMax - kZoneTrickleMin + 1));
            const int count = std::min(rolled, zone.targetMobs - current);
            for (int i = 0; i < count; ++i) {
                if (spawnInZone(world, terrain, content, zone, viewers, rng, nowMillis) ==
                    NULL_ENTITY) {
                    break;
                }
            }
        }
    }
}

Entity SpawnSystem::spawnInZone(World& world, const Terrain& terrain,
                                const ContentRegistry& content, const SpawnZone& zone,
                                const std::vector<Viewer>& viewers, Rng& rng, double nowMillis) {
    if (census_.mobs >= mobCap) return NULL_ENTITY;

    Vec2 at;
    bool placed = false;
    for (int attempt = 0; attempt < kZonePlacementAttempts; ++attempt) {
        const Vec2 candidate = samplePointInRect(zone.bounds, rng);
        if (inBorderBand(candidate)) continue;
        if (terrain.blocked(candidate, Realm::Overworld)) continue;
        if (nearAnyPlayer(viewers, candidate, kMinSpawnDistance)) continue;
        if (crowdedAt(candidate, kPreliminarySpawnRadius, kMinMobSpawnSpacing)) continue;
        at = candidate;
        placed = true;
        break;
    }
    if (!placed) return NULL_ENTITY;

    // A rectangle belongs to nobody's viewport, so anything charged to luck is
    // charged to whoever is standing nearest its centre -- the reference's own
    // attribution rule for a zone fill.
    const Vec2 centre{zone.bounds.x + zone.bounds.w * 0.5, zone.bounds.y + zone.bounds.h * 0.5};
    const double luck = nearestViewerLuck(viewers, centre);
    const int section = sectionAt(at);

    Rarity rarity = Rarity::Common;
    std::uint16_t type = kInvalidIndex;
    // The biome wins over the rectangle it sits in, exactly as it does in the
    // neighbourhood fill: the reference's createEnemyInZone tests the biome
    // before it looks at the zone's own tier at all
    // (src/server/enemySpawner.ts:917). Every one of the map's target-dummy
    // biomes lies inside the big common rectangle, so a zone fill that ignored
    // this is a zone fill that never builds the DPS row.
    const MapElement* biome = mapData != nullptr ? mapData->biomeAt(at) : nullptr;
    if (biome != nullptr && !biome->spawnTable.empty()) {
        if (!chooseBiomeSpawn(content, *biome, section, luck, rng, type, rarity)) {
            return NULL_ENTITY;
        }
    } else {
        // The rectangle's own tier, never a natural roll: this is where the
        // map's rarity progression comes from. An ultra rectangle is also the
        // only place `super` appears without the boss pass -- one roll in a
        // hundred.
        rarity = zone.tier;
        if (rarity == Rarity::Ultra) {
            rarity = rng.chance(kUltraZoneSuperChance) ? Rarity::Super : Rarity::Ultra;
        } else {
            rarity = applyTierDrift(rarity, luck, rng);
        }
        type = chooseMobType(content, section, rarity, rng);
        if (type == kInvalidIndex) return NULL_ENTITY;
    }

    if (content.mob(type).neverAmbient &&
        permanentFixtureExists(world, type, rarity, section)) {
        return NULL_ENTITY;
    }

    const Entity spawned = spawnMob(world, terrain, content, type, rarity, at, Realm::Overworld, nowMillis, rng);
    if (spawned == NULL_ENTITY) return NULL_ENTITY;
    // Counted straight away, so the rest of this pass spaces itself against
    // what it has just placed rather than against the last census alone.
    if (const Transform* transform = world.tryGet<Transform>(spawned)) {
        const Body* body = world.tryGet<Body>(spawned);
        mobPlacements_.push_back(
            MobPlacement{transform->position, body != nullptr ? body->radius : 0.0});
    }
    return spawned;
}

// ---------------------------------------------------------------------------
// Bosses
// ---------------------------------------------------------------------------

void SpawnSystem::runSpecialMobs(World& world, const Terrain& terrain,
                                 const ContentRegistry& content,
                                 const std::vector<Viewer>& viewers, Rng& rng, double nowMillis) {
    if (zones_.empty()) return;
    if (nowMillis < nextBossMillis_) return;
    const bool startup = !bossPassRan_;
    bossPassRan_ = true;
    nextBossMillis_ = nowMillis + kBossIntervalMillis;
    // The reference stocks the world once as it boots, whatever it looks like,
    // and after that only while somebody is online -- its timer keeps running
    // over an empty server but skips its body, so a boss killed with nobody
    // watching is not replaced until someone comes back.
    if (!startup && viewers.empty()) return;

    int ultras = 0;
    int supers = 0;
    int uniques = 0;
    // Recomputed from the world every pass rather than tracked: a boss dying
    // anywhere would otherwise leak its section's slot forever.
    std::array<bool, kSectionCount> superSections{};
    allMobs_->each([&](Entity, MobTag&, Transform& transform, MobType& type) {
        // A target dummy is a permanent fixture, not an event, and one parked
        // in an ultra plot would suppress the world's real ultra.
        if (content.mob(type.configIndex).neverAmbient) return;
        if (type.rarity == Rarity::Ultra) {
            ++ultras;
        } else if (type.rarity == Rarity::Super) {
            ++supers;
            const int section = sectionAt(transform.position);
            if (section >= 0) superSections[static_cast<std::size_t>(section)] = true;
        } else if (type.rarity == Rarity::Unique) {
            ++uniques;
        }
    });

    // Exactly one ultra is kept alive, anywhere on the map, and it is never
    // announced -- it is found rather than advertised.
    if (ultras == 0) {
        spawnSpecialMob(world, terrain, content, Rarity::Ultra, -1, nullptr, viewers, rng,
                        nowMillis);
    }

    for (int section = 0; section < kSectionCount; ++section) {
        if (superSections[static_cast<std::size_t>(section)]) continue;
        const Entity boss = spawnSpecialMob(world, terrain, content, Rarity::Super, section,
                                            &superSections, viewers, rng, nowMillis);
        if (boss == NULL_ENTITY) continue;
        ++supers;
        // Three supers in four come out of an ultra rectangle, which is not
        // section-bound, so the one that lands claims whatever section it fell
        // in rather than the one being filled.
        const Transform* transform = world.tryGet<Transform>(boss);
        const MobType* type = world.tryGet<MobType>(boss);
        if (transform == nullptr || type == nullptr) continue;
        const int landed = sectionAt(transform->position);
        if (landed >= 0) superSections[static_cast<std::size_t>(landed)] = true;
        announceBoss(type->configIndex, type->rarity, transform->position);
    }

    // A unique only exists alongside a super, and only one pass in four even
    // tries: it is the rarest thing the world produces on its own.
    if (supers > 0 && uniques == 0 && rng.chance(kUniqueSpawnChance)) {
        const Entity boss = spawnSpecialMob(world, terrain, content, Rarity::Unique, -1, nullptr,
                                            viewers, rng, nowMillis);
        if (boss == NULL_ENTITY) return;
        const Transform* transform = world.tryGet<Transform>(boss);
        const MobType* type = world.tryGet<MobType>(boss);
        if (transform != nullptr && type != nullptr) {
            announceBoss(type->configIndex, type->rarity, transform->position);
        }
    }
}

void SpawnSystem::announceBoss(std::uint16_t mobIndex, Rarity rarity, Vec2 position) {
    // Oldest first, so a server that never drains this keeps the announcements
    // somebody might still care about instead of the ones from an hour ago.
    if (bossSpawns.size() >= kMaxPendingBossSpawns) bossSpawns.erase(bossSpawns.begin());
    bossSpawns.push_back(BossSpawn{mobIndex, rarity, position});
}

Entity SpawnSystem::spawnSpecialMob(World& world, const Terrain& terrain,
                                    const ContentRegistry& content, Rarity tier, int targetSection,
                                    const std::array<bool, kSectionCount>* superSections,
                                    const std::vector<Viewer>& viewers, Rng& rng,
                                    double nowMillis) {
    if (census_.mobs >= mobCap) return NULL_ENTITY;

    // Where the tier lives on the map. Ultras and uniques are ultra-rectangle
    // only; three supers in four join them and the fourth takes a mythic one,
    // which is what spreads bosses beyond the map's nine ultra plots.
    const Rarity zoneTier = tier == Rarity::Super && !rng.chance(kSuperInUltraZoneChance)
                                ? Rarity::Mythic
                                : Rarity::Ultra;

    Vec2 at;
    bool placed = false;
    if (tier == Rarity::Super && targetSection >= 0) {
        // Only the mythic branch is held to the section being filled: there is
        // no ultra rectangle in every section, so an ultra-branch super is
        // allowed to land wherever the map has one.
        placed = zoneTier == Rarity::Mythic
                     ? randomPointInZoneTypeInSection(Rarity::Mythic, targetSection, rng, at)
                     : randomPointInZoneType(Rarity::Ultra, rng, at);
        if (!placed) {
            placed = zoneTier == Rarity::Mythic
                         ? randomPointInZoneType(Rarity::Ultra, rng, at)
                         : randomPointInZoneTypeInSection(Rarity::Mythic, targetSection, rng, at);
        }
    } else {
        placed = randomPointInZoneType(zoneTier, rng, at);
    }
    if (!placed) return NULL_ENTITY;

    // Rolled against the FIRST position's section, before the retries below
    // may move the boss: the reference picks its species once and then only
    // looks for somewhere to stand it.
    const std::uint16_t type = chooseMobType(content, sectionAt(at), tier, rng);
    if (type == kInvalidIndex) return NULL_ENTITY;
    const MobStats stats = content.mobStats(type, tier);

    // A rectangle is allowed to hang over the border band. One retry, then the
    // boss is given up on until the next pass.
    if (inBorderBand(at)) {
        Vec2 retry;
        if (!randomPointInZoneType(zoneTier, rng, retry) || inBorderBand(retry)) return NULL_ENTITY;
        at = retry;
    }

    // Never in someone's lap: a boss materialising inside a player's petals is
    // a free kill for whichever side gets the first tick.
    if (nearAnyPlayer(viewers, at, kMinSpawnDistance)) {
        bool moved = false;
        for (int attempt = 0; attempt < kBossPlacementAttempts; ++attempt) {
            Vec2 retry;
            if (!randomPointInZoneType(zoneTier, rng, retry)) continue;
            if (nearAnyPlayer(viewers, retry, kMinSpawnDistance)) continue;
            at = retry;
            moved = true;
            break;
        }
        if (!moved) return NULL_ENTITY;
    }

    // The final test uses the boss's own body rather than the 20-unit stand-in,
    // which matters: an ultra is several times the size of the mob it displaces.
    if (crowdedAt(at, stats.radius, 0.0)) return NULL_ENTITY;

    // Last, because it is a veto on the FINAL position. Dropping an
    // already-admitted boss instead would leave its entity in the world.
    if (superSections != nullptr) {
        const int landing = sectionAt(at);
        if (landing < 0 || (*superSections)[static_cast<std::size_t>(landing)]) return NULL_ENTITY;
    }

    const Entity spawned = spawnMob(world, terrain, content, type, tier, at, Realm::Overworld, nowMillis, rng);
    if (spawned == NULL_ENTITY) return NULL_ENTITY;
    if (const Transform* transform = world.tryGet<Transform>(spawned)) {
        const Body* body = world.tryGet<Body>(spawned);
        mobPlacements_.push_back(
            MobPlacement{transform->position, body != nullptr ? body->radius : 0.0});
    }
    return spawned;
}

bool SpawnSystem::randomPointInZoneType(Rarity tier, Rng& rng, Vec2& out) const {
    int matches = 0;
    for (const SpawnZone& zone : zones_) {
        if (zone.tier == tier) ++matches;
    }
    if (matches == 0) return false;

    // Walked to rather than indexed: nine ultra and forty-one mythic
    // rectangles is a short list, and a second per-tier index would be one
    // more thing to keep in step with the map.
    const auto nth = [&](int skip) -> const SpawnZone& {
        for (const SpawnZone& zone : zones_) {
            if (zone.tier != tier) continue;
            if (skip-- == 0) return zone;
        }
        return zones_.front();
    };

    const int picked = static_cast<int>(rng.below(static_cast<std::uint32_t>(matches)));
    Vec2 candidate = samplePointInRect(nth(picked).bounds, rng);
    if (!inBorderBand(candidate)) {
        out = candidate;
        return true;
    }
    // Exactly one retry, and in a DIFFERENT rectangle -- resampling the same
    // one is how a zone drawn along the map edge starves the boss pass.
    if (matches < 2) return false;
    candidate = samplePointInRect(nth(picked == 0 ? 1 : 0).bounds, rng);
    if (inBorderBand(candidate)) return false;
    out = candidate;
    return true;
}

bool SpawnSystem::randomPointInZoneTypeInSection(Rarity tier, int section, Rng& rng,
                                                 Vec2& out) const {
    if (section < 0 || section >= kSectionCount) return false;
    const Rect sectionRect{static_cast<double>(section % kSectionsPerAxis) * kSectionSize,
                           static_cast<double>(section / kSectionsPerAxis) * kSectionSize,
                           kSectionSize, kSectionSize};

    int matches = 0;
    for (const SpawnZone& zone : zones_) {
        if (zone.tier == tier && zone.bounds.intersects(sectionRect)) ++matches;
    }
    if (matches == 0) return false;

    const auto nth = [&](int skip) -> const SpawnZone& {
        for (const SpawnZone& zone : zones_) {
            if (zone.tier != tier || !zone.bounds.intersects(sectionRect)) continue;
            if (skip-- == 0) return zone;
        }
        return zones_.front();
    };

    for (int attempt = 0; attempt < kZoneSectionAttempts; ++attempt) {
        const Rect& bounds = nth(static_cast<int>(rng.below(
                                     static_cast<std::uint32_t>(matches))))
                                 .bounds;
        // The slice of the rectangle that lies in this section: a zone may
        // straddle two, and the super is being placed for one of them.
        const double minX = std::max(bounds.left(), sectionRect.left());
        const double maxX = std::min(bounds.right(), sectionRect.right());
        const double minY = std::max(bounds.top(), sectionRect.top());
        const double maxY = std::min(bounds.bottom(), sectionRect.bottom());
        if (minX >= maxX || minY >= maxY) continue;

        const Vec2 candidate = samplePointInRect(Rect{minX, minY, maxX - minX, maxY - minY}, rng);
        if (inBorderBand(candidate)) continue;
        out = candidate;
        return true;
    }
    return false;
}

} // namespace flix
