// Emits the C++ ECS server's observable derived gameplay values.  The Node
// parity runner compares these JSON tuples with behaviorOracle.ts.
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "shared/core/json.h"
#include "shared/game/config.h"
#include "shared/game/rarity.h"
#include "server/systems/petals.h"
#include "server/systems/combat.h"
#include "server/systems/movement.h"
#include "shared/game/terrain.h"

namespace {

using namespace flix;

void emit(const std::string& path, double value) {
    Json row = Json::array();
    row.push(path);
    row.push(value);
    std::cout << row.dump() << '\n';
}

void emit(const std::string& path, const std::string& value) {
    Json row = Json::array();
    row.push(path);
    row.push(value);
    std::cout << row.dump() << '\n';
}

void emit(const std::string& path, bool value) {
    Json row = Json::array();
    row.push(path);
    row.push(value);
    std::cout << row.dump() << '\n';
}

const char* aiName(AiKind ai) {
    switch (ai) {
        case AiKind::Passive: return "passive";
        case AiKind::Neutral: return "neutral";
        case AiKind::Hostile: return "hostile";
        case AiKind::Sandstorm: return "sandstorm";
        case AiKind::Stationary: return "stationary";
    }
    return "neutral";
}

struct EquippedPetal {
    const char* id;
    Rarity rarity;
    int slot;
};

void emitModifierScenario(const char* name, const ContentRegistry& registry,
                          std::initializer_list<EquippedPetal> equipped) {
    World world;
    CommandBuffer commands{world};
    const Entity player = world.create();
    world.add<PlayerTag>(player);
    world.add<Transform>(player, Transform{{1000.0, 1000.0}, 0.0});
    world.add<Body>(player, Body{kPlayerBaseRadius, 1.0});
    world.add<Health>(player, Health{110.0, 110.0, 0.0, 0.0});
    world.add<PlayerProgress>(player, PlayerProgress{});
    world.add<PlayerModifiers>(player);
    world.add<PlayerInput>(player);
    world.add<Loadout>(player);
    world.add<PetalRing>(player);

    Loadout& loadout = world.get<Loadout>(player);
    for (const EquippedPetal& item : equipped) {
        LoadoutSlot& slot = loadout.slots[static_cast<std::size_t>(item.slot)];
        slot.configIndex = registry.petalIndex(item.id);
        slot.rarity = item.rarity;
    }

    PetalSystem petals;
    petals.run(world, registry, 1000.0, 1.0, commands);
    commands.flush();

    const PlayerModifiers& modifiers = world.get<PlayerModifiers>(player);
    const double rotationSpeed = world.get<PetalRing>(player).spin / kPetalSpinRate;
    const std::string prefix = std::string("scenario/modifiers/") + name;
    emit(prefix + "/damage", modifiers.damageScale);
    emit(prefix + "/max-health", modifiers.maxHealthScale);
    emit(prefix + "/speed", modifiers.speedScale);
    emit(prefix + "/range", modifiers.rangeScale);
    emit(prefix + "/rotation-speed", rotationSpeed);
    emit(prefix + "/player-radius", modifiers.sizeScale);
    emit(prefix + "/luck", modifiers.luck);
    emit(prefix + "/magnetism", modifiers.magnetism);
    emit(prefix + "/aggro-radius", modifiers.aggroRadiusBonus);
    emit(prefix + "/petal-attraction-radius", modifiers.petalAttractionRadius);
    emit(prefix + "/poison-armor", modifiers.poisonArmor);
}

void emitMobHealthScenario() {
    World world;
    CombatSystem combat;

    const Entity player = world.create();
    world.add<PlayerTag>(player);
    world.add<Faction>(player, Faction{Team::Players, false});

    const Entity mob = world.create();
    world.add<MobTag>(mob);
    world.add<Health>(mob, Health{100.0, 100.0, 0.0, 0.0});
    world.add<Faction>(mob, Faction{Team::Hostiles, false});
    world.add<Bounty>(mob, Bounty{});

    combat.applyDamage(world, mob, player, 30.0, 1000.0);
    emit("scenario/mob-health/positive-hit", world.get<Health>(mob).current);
    combat.applyDamage(world, mob, player, -10.0, 1001.0);
    emit("scenario/mob-health/negative-hit", world.get<Health>(mob).current);
    combat.applyDamage(world, mob, player, 500.0, 1002.0);
    emit("scenario/mob-health/lethal-hit", world.get<Health>(mob).current);
    emit("scenario/mob-health/dead", world.has<Dead>(mob));
}

void emitMovementScenario(const char* mapBundlePath) {
    World world;
    Terrain terrain;
    std::string terrainError;
    if (!terrain.loadMapBundle(mapBundlePath, terrainError)) {
        throw std::runtime_error("could not load movement map: " + terrainError);
    }
    struct Sample { const char* name; Vec2 point; };
    const Sample samples[] = {
        {"center", {3000.0, 3000.0}}, {"above", {3000.0, 2990.0}},
        {"below", {3000.0, 3010.0}}, {"right", {3070.0, 3000.0}},
        {"upper-right", {3070.0, 2990.0}},
    };
    for (const Sample& sample : samples) {
        emit(std::string("scenario/movement/tile/") + sample.name,
             static_cast<double>(terrain.at(sample.point)));
    }
    // The oracle's movement scenario is an overworld one: it is the tile map's
    // wall slide that the TypeScript build is compared against.
    const Vec2 resolvedStart = terrain.resolveCircle({3000.0, 2940.0}, 20.0, Realm::Overworld);
    emit("scenario/movement/resolve-start/x", resolvedStart.x);
    emit("scenario/movement/resolve-start/y", resolvedStart.y);
    MovementSystem movement;
    const Entity player = world.create();
    world.add<PlayerTag>(player);
    world.add<Transform>(player, Transform{{3000.0, 2940.0}, 0.0});
    world.add<Motion>(player, Motion{});
    world.add<Body>(player, Body{kPlayerBaseRadius, 1.0});
    world.add<PlayerInput>(player, PlayerInput{});
    world.add<PlayerModifiers>(player, PlayerModifiers{});
    world.add<Afflictions>(player, Afflictions{});
    world.add<Health>(player, Health{100.0, 100.0, 0.0, 0.0});
    world.add<Faction>(player, Faction{Team::Players, false});

    struct Phase { const char* name; double angle; double strength; };
    const Phase phases[] = {
        {"wall-slide", kPi / 4.0, 1.0},
        {"half-up", -kPi / 2.0, 0.5},
        {"release", 0.0, 0.0},
    };
    double nowMillis = 0.0;
    for (const Phase& phase : phases) {
        PlayerInput& input = world.get<PlayerInput>(player);
        input.current.moveAngle = phase.angle;
        input.current.moveStrength = phase.strength;
        for (int tick = 0; tick < 10; ++tick) {
            movement.runPlayerPhase(world, terrain, nowMillis, net::kTickSeconds);
            nowMillis += net::kTickMillis;
            const Transform& tickTransform = world.get<Transform>(player);
            const Motion& tickMotion = world.get<Motion>(player);
            const std::string tickPrefix = std::string("scenario/movement/") + phase.name +
                                           "/tick-" + std::to_string(tick + 1);
            emit(tickPrefix + "/x", tickTransform.position.x);
            emit(tickPrefix + "/y", tickTransform.position.y);
            emit(tickPrefix + "/vx", tickMotion.velocity.x);
            emit(tickPrefix + "/vy", tickMotion.velocity.y);
        }
        const Transform& transform = world.get<Transform>(player);
        const Motion& motion = world.get<Motion>(player);
        const std::string prefix = std::string("scenario/movement/") + phase.name;
        emit(prefix + "/x", transform.position.x);
        emit(prefix + "/y", transform.position.y);
        emit(prefix + "/vx", motion.velocity.x);
        emit(prefix + "/vy", motion.velocity.y);
    }
}

} // namespace

int main(int argc, char** argv) {
    using namespace flix;
    try {
        if (argc != 5) {
            std::cerr << "usage: behavior_oracle <mobs.json> <petals.json> <mob_xp.json> <map_bundle.ts>\n";
            return 2;
        }

        ContentRegistry registry;
        std::string error;
        if (!registry.loadFiles(argv[1], argv[2], argv[3], error)) {
            std::cerr << error << '\n';
            return 2;
        }

        for (int i = 0; i < kRarityCount; ++i) {
            const Rarity rarity = static_cast<Rarity>(i);
            const std::string prefix = std::string("rarity/") + rarityName(rarity);
            emit(prefix + "/craft-percent", craftSuccessChance(rarity) * 100.0);
            emit(prefix + "/drop-upgrade-percent", dropUpgradeChance(rarity) * 100.0);
            emit(prefix + "/drop-downgrade-fraction", dropDowngradeChance(rarity));
            for (int j = 0; j < kRarityCount; ++j) {
                const Rarity target = static_cast<Rarity>(j);
                emit(prefix + "/stall/" + rarityName(target), stallPower(rarity, target));
            }
        }

        for (std::size_t i = 0; i < registry.mobCount(); ++i) {
            const auto index = static_cast<std::uint16_t>(i);
            const MobConfig& config = registry.mob(index);
            for (int tier = 0; tier < kRarityCount; ++tier) {
                const Rarity rarity = static_cast<Rarity>(tier);
                const MobStats stats = registry.mobStats(index, rarity);
                const std::string prefix = "mob/" + config.id + "/" + rarityName(rarity);
                emit(prefix + "/health", stats.health);
                emit(prefix + "/damage", stats.damage);
                emit(prefix + "/radius", stats.radius);
                emit(prefix + "/mass", stats.mass);
                emit(prefix + "/xp", stats.xp);
                emit(prefix + "/aggro-range", stats.aggroRange);
                emit(prefix + "/attack-cooldown-ms", stats.attackCooldownMillis);
                emit(prefix + "/poison-per-second", stats.poisonPerSecond);
                emit(prefix + "/poison-duration-ms", stats.poisonDurationMillis);
                emit(prefix + "/visual-scale", stats.visualScale);
                emit(prefix + "/spawn-weight", stats.spawnWeight);
                emit(prefix + "/ai", std::string(aiName(stats.ai)));
                emit(prefix + "/ambient", stats.ambient ? 1.0 : 0.0);
            }
        }

        for (std::size_t i = 0; i < registry.petalCount(); ++i) {
            const auto index = static_cast<std::uint16_t>(i);
            const PetalConfig& config = registry.petal(index);
            for (int tier = 0; tier < kRarityCount; ++tier) {
                const Rarity rarity = static_cast<Rarity>(tier);
                const PetalStats stats = registry.petalStats(index, rarity);
                const PetalModifiers& modifiers = stats.modifiers;
                const std::string prefix = "petal/" + config.id + "/" + rarityName(rarity);
                emit(prefix + "/damage", stats.damage);
                emit(prefix + "/health", stats.health);
                emit(prefix + "/reload-ms", stats.reloadMillis);
                emit(prefix + "/poison-per-second", stats.poisonPerSecond);
                emit(prefix + "/poison-duration-ms", stats.poisonDurationMillis);
                emit(prefix + "/burst-heal", stats.heal);
                emit(prefix + "/burst-heal-charge-ms", stats.healChargeMillis);
                emit(prefix + "/passive-heal-per-second", stats.passiveHealPerSecond);
                emit(prefix + "/knockback", stats.knockback);
                emit(prefix + "/shield", stats.shield);
                emit(prefix + "/slow-factor", stats.slowFactor);
                emit(prefix + "/slow-duration-ms", stats.slowDurationMillis);
                emit(prefix + "/web-radius", stats.webRadius);
                emit(prefix + "/sponge-duration-ms", stats.spongeDamageDurationMillis);
                emit(prefix + "/attraction-force", stats.attractionForce);
                emit(prefix + "/size", stats.size);
                emit(prefix + "/count", static_cast<double>(stats.count));
                emit(prefix + "/camera-zoom", stats.cameraZoom);
                emit(prefix + "/modifier/damage", modifiers.damage);
                emit(prefix + "/modifier/max-health", modifiers.maxHealth);
                emit(prefix + "/modifier/speed", modifiers.speed);
                emit(prefix + "/modifier/range", modifiers.range);
                emit(prefix + "/modifier/rotation-speed", modifiers.rotationSpeed);
                emit(prefix + "/modifier/player-radius", modifiers.playerRadius);
                emit(prefix + "/modifier/luck", modifiers.luck);
                emit(prefix + "/modifier/magnetism", modifiers.magnetism);
                emit(prefix + "/modifier/aggro-radius", modifiers.aggroRadius);
                emit(prefix + "/modifier/petal-attraction-radius", modifiers.petalAttractionRadius);
                emit(prefix + "/modifier/poison-armor", modifiers.poisonArmor);
            }
        }

        emitModifierScenario("empty", registry, {});
        emitModifierScenario("mixed-active", registry, {
            {"faster", Rarity::Rare, 0}, {"powder", Rarity::Epic, 1},
            {"soil", Rarity::Legendary, 2}, {"air", Rarity::Uncommon, 3},
            {"clover", Rarity::Mythic, 4}, {"lotus", Rarity::Ultra, 5},
            {"lentil", Rarity::Super, 6}, {"basic", Rarity::Common, 7},
            {"faster", Rarity::Common, 9},
        });
        emitModifierScenario("storage-ignored", registry, {
            {"basic", Rarity::Common, 0}, {"faster", Rarity::Apex, 10},
            {"soil", Rarity::Apex, 11}, {"clover", Rarity::Apex, 12},
            {"lentil", Rarity::Apex, 13},
        });
        emitMobHealthScenario();
        emitMovementScenario(argv[4]);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 2;
    }
    return 0;
}
