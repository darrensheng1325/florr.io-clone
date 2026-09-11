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

} // namespace

int main(int argc, char** argv) {
    using namespace flix;
    try {
        if (argc != 4) {
            std::cerr << "usage: behavior_oracle <mobs.json> <petals.json> <mob_xp.json>\n";
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
        // There used to be a movement scenario here, walked over the tile grid
        // in src/map_bundle.ts so both builds could step the same walls. The
        // map is a Tiled file now and the frozen TypeScript build cannot read
        // one, so there is no map both sides can agree on any more -- and an
        // oracle that stepped a DIFFERENT map would report a parity failure
        // that means nothing. Movement against real terrain is covered by
        // cpp/tests/movement_tests.cpp instead; behavior-parity.js skips the
        // keys the TypeScript side still emits for it, and says why.
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 2;
    }
    return 0;
}
