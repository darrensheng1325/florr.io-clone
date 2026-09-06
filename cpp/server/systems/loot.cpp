#include "server/systems/loot.h"

#include <algorithm>
#include <fstream>

#include "server/loot_eligibility.h"
#include "shared/core/json.h"

namespace flix {

// ---------------------------------------------------------------------------
// DropTables
// ---------------------------------------------------------------------------

bool DropTables::linkedTo(const ContentRegistry& content) const {
    return loaded_ && content_ == &content && contentHash_ == content.contentHash();
}

bool DropTables::load(const ContentRegistry& content, const std::string& path, std::string& errorOut) {
    Json root;
    std::string parseError;
    if (!Json::parseFile(path, root, parseError)) {
        errorOut = path + ": " + parseError;
        return false;
    }
    if (!root.isObject()) {
        errorOut = path + ": top level is not an object";
        return false;
    }

    std::vector<SourceEntry> loaded;
    for (const std::string& mobId : root.keys()) {
        const Json& table = root[mobId];
        if (!table.isObject() || !table["drops"].isArray()) {
            errorOut = path + ": table for '" + mobId + "' must contain a drops array";
            return false;
        }

        for (const Json& entry : table["drops"].items()) {
            if (!entry.isObject()) {
                errorOut = path + ": a drop for '" + mobId + "' is not an object";
                return false;
            }
            const std::string type = entry["type"].asString();
            const std::string itemId = entry["itemType"].asString();
            const std::string rarity = entry["rarity"].asString();
            if (itemId.empty() || rarity.empty() || !entry["probability"].isNumber()) {
                errorOut = path + ": a drop for '" + mobId + "' is missing itemType, rarity, or probability";
                return false;
            }

            int rarityOffset = -1;
            for (int i = 0; i < kRarityCount; ++i) {
                if (rarity == kRarityNames[static_cast<std::size_t>(i)]) {
                    rarityOffset = i;
                    break;
                }
            }
            if (rarityOffset < 0) {
                errorOut = path + ": drop '" + itemId + "' for '" + mobId + "' has an unknown rarity";
                return false;
            }

            SourceEntry source;
            source.mobId = mobId;
            source.petalId = itemId;
            // Anything not explicitly a consumable is a petal, which is how the
            // reference reads the field. Every row is kept whatever its kind:
            // the row list is the weighted denominator above uncommon.
            source.kind = type == "consumable" ? Kind::Consumable
                          : itemId == "random" ? Kind::RandomPetal
                                               : Kind::Petal;
            source.rarityOffset = rarityOffset;
            source.probability = clamp(entry["probability"].asDouble(), 0.0, 1.0);
            source.minCount = std::max(1, entry["minQuantity"].asInt(1));
            source.maxCount = std::max(source.minCount, entry["maxQuantity"].asInt(source.minCount));
            loaded.push_back(std::move(source));
        }
    }

    source_ = std::move(loaded);
    loaded_ = true;
    content_ = nullptr;
    contentHash_ = 0;
    resolve(content);
    errorOut.clear();
    return true;
}

void DropTables::loadDefault(const ContentRegistry& content) {
    static constexpr const char* kCandidates[] = {
        "data/mob_drops.json",
        "src/mob_drops.json",
        "../src/mob_drops.json",
        "../../src/mob_drops.json",
    };

    for (const char* candidate : kCandidates) {
        std::ifstream probe(candidate, std::ios::binary);
        if (!probe) continue;
        std::string ignored;
        if (load(content, candidate, ignored)) return;
    }

    // A standalone system test can have no data directory at all. Mark the
    // attempt so the steady-state tick does not repeatedly hit the filesystem.
    loaded_ = true;
    resolve(content);
}

void DropTables::link(const ContentRegistry& content) {
    if (!loaded_) loadDefault(content);
    if (linkedTo(content)) return;
    resolve(content);
}

void DropTables::resolve(const ContentRegistry& content) {
    content_ = &content;
    contentHash_ = content.contentHash();

    byMob_.assign(content.mobCount(), std::vector<Entry>{});
    unresolved_.clear();

    for (const SourceEntry& source : source_) {
        const std::uint16_t mobIndex = content.mobIndex(source.mobId);
        if (mobIndex == kInvalidIndex || mobIndex >= byMob_.size()) {
            unresolved_.push_back(std::string("mob '") + source.mobId +
                                  "' has a drop table but no config");
            continue;
        }

        Entry resolved;
        // A row whose item this build cannot hand out -- a consumable, the
        // Random sentinel, an id the petal registry does not know -- still
        // belongs in the table. Above uncommon it is a weight and below it an
        // independent roll; only the payout is missing, and kNoPetal is
        // already what spawnDrop treats as "nothing".
        resolved.kind = source.kind;
        resolved.petalIndex =
            source.kind == Kind::Petal ? content.petalIndex(source.petalId) : kNoPetal;
        resolved.rarityOffset = source.rarityOffset;
        resolved.probability = source.probability;
        resolved.minCount = source.minCount;
        resolved.maxCount = source.maxCount;
        byMob_[mobIndex].push_back(resolved);
    }

    // The TypeScript registry auto-generates a guaranteed common egg row for
    // every non-pet mob that permits eggs. It is runtime content, so it is not
    // present in mob_drops.json and must be restored after linking.
    for (std::size_t i = 0; i < content.mobCount(); ++i) {
        const MobConfig& mob = content.mob(static_cast<std::uint16_t>(i));
        if (mob.noEggDrop ||
            (mob.id.size() >= 4 && mob.id.compare(mob.id.size() - 4, 4, "_pet") == 0)) continue;
        const std::uint16_t egg = content.petalIndex(mob.id + "_egg");
        if (egg == kInvalidIndex) continue;
        auto& rows = byMob_[i];
        auto existing = std::find_if(rows.begin(), rows.end(), [&](const Entry& row) {
            return row.kind == Kind::Petal && row.petalIndex == egg &&
                   row.rarityOffset == rarityIndex(Rarity::Common);
        });
        if (existing != rows.end()) {
            existing->probability = 1.0;
        } else {
            Entry row;
            row.petalIndex = egg;
            row.rarityOffset = rarityIndex(Rarity::Common);
            row.probability = 1.0;
            rows.insert(rows.begin(), row);
        }
    }

    // What a `random` row may turn into. A property of the content rather than
    // of a kill, so it is derived here once: admin petals, the two cutters and
    // the eggs of mobs that never lay one are excluded however they are
    // authored, and every other petal is equally likely.
    droppable_.clear();
    for (const std::uint16_t index : content.petalDisplayOrder()) {
        const PetalConfig& petal = content.petal(index);
        if (petal.isAdminPetal) continue;
        if (petal.id == "cutter" || petal.id == "lightning_cutter") continue;
        if (petal.id.size() > 4 && petal.id.compare(petal.id.size() - 4, 4, "_egg") == 0) {
            const std::uint16_t layer = content.mobIndex(petal.id.substr(0, petal.id.size() - 4));
            if (layer != kInvalidIndex && content.mob(layer).noEggDrop) continue;
        }
        droppable_.push_back(index);
    }
    basicPetal_ = content.petalIndex("basic");
}

const std::vector<DropTables::Entry>& DropTables::forMob(std::uint16_t mobIndex) const {
    static const std::vector<Entry> kNothing;
    if (mobIndex >= byMob_.size()) return kNothing;
    return byMob_[mobIndex];
}

std::uint16_t DropTables::randomPetal(Rng& rng) const {
    if (droppable_.empty()) return basicPetal_;
    return droppable_[rng.below(static_cast<std::uint32_t>(droppable_.size()))];
}

// ---------------------------------------------------------------------------
// Rolls
// ---------------------------------------------------------------------------

Rarity LootSystem::scaleDropRarity(Rarity authoredRarity, Rarity mobRarity, Rng& rng) {
    // Above uncommon, 90% of selected rows first become one tier below the
    // mob; common and uncommon rows keep their authored table rarity.
    if (rarityIndex(mobRarity) > rarityIndex(Rarity::Uncommon) && rng.chance(0.9)) {
        return clampRarity(rarityIndex(mobRarity) - 1);
    }
    return authoredRarity;
}

Rarity LootSystem::finishDropRarity(Rarity baseRarity, Rarity mobRarity, Rng& rng) {
    Rarity base = baseRarity;
    double upgrade = dropUpgradeChance(base);
    if (mobRarity == Rarity::Ultra) upgrade *= 20.0;
    upgrade = clamp(upgrade, 0.0, 1.0);
    if (rng.chance(upgrade)) {
        base = upgradeRarity(base);
    } else if (rng.chance(dropDowngradeChance(base))) {
        base = downgradeRarity(base);
    }

    // Rare mobs floor at tier-1; epic and above floor at tier-2.
    const int mobTier = rarityIndex(mobRarity);
    if (mobTier >= rarityIndex(Rarity::Rare)) {
        const int floor = mobTier >= rarityIndex(Rarity::Epic) ? mobTier - 2 : mobTier - 1;
        if (rarityIndex(base) < floor) base = clampRarity(floor);
    }
    // Apex mobs explicitly cap item rarity at unique.
    if (mobRarity == Rarity::Apex && base == Rarity::Apex) base = Rarity::Unique;
    return base;
}

Rarity LootSystem::rollDropRarity(Rarity authoredRarity, Rarity mobRarity, Rng& rng) {
    return finishDropRarity(scaleDropRarity(authoredRarity, mobRarity, rng), mobRarity, rng);
}

bool LootSystem::mayPickUp(const DropItem& drop, Entity player, double nowMillis) {
    (void)nowMillis;
    if (std::find(drop.pickedUpBy.begin(), drop.pickedUpBy.end(), player) != drop.pickedUpBy.end()) {
        return false;
    }
    return drop.eligible.empty() ||
           std::find(drop.eligible.begin(), drop.eligible.end(), player) != drop.eligible.end();
}

// ---------------------------------------------------------------------------
// Drops
// ---------------------------------------------------------------------------

Entity LootSystem::spawnDrop(World& world, std::uint16_t petalIndex, Rarity rarity, Vec2 position,
                             const std::vector<Entity>& eligible, double nowMillis) {
    if (petalIndex == kNoPetal) return NULL_ENTITY;

    const Entity e = world.create();
    world.add<DropTag>(e);
    world.add<Transform>(e, Transform{position, 0.0});
    // No Motion: a drop is furniture. The body is here only so the broadphase
    // files it and the pickup query can find it.
    world.add<Body>(e, Body{kDropBodyRadius, 1.0});

    DropItem item;
    item.configIndex = petalIndex;
    item.rarity = rarity;
    item.eligible = eligible;
    world.add<DropItem>(e, std::move(item));

    world.add<Lifetime>(e, Lifetime{kDropLifetimeByRarity[static_cast<std::size_t>(rarityIndex(rarity))]});
    world.add<Replicated>(e, Replicated{net::EntityKind::Drop, 0, petalIndex, rarity, 0});
    if (netIds != nullptr) world.add<NetId>(e, NetId{netIds->next()});
    return e;
}

// ---------------------------------------------------------------------------
// The tick
// ---------------------------------------------------------------------------

namespace {

/// Outside the playable rectangle, where nothing can ever be reached.
///
/// The reference exempts the PVP arena and the maze, which sit outside the
/// world rect on purpose; this build has neither, so the rectangle is the whole
/// rule. Tested after the wall push, which is what normally pulls an escaping
/// drop back inside -- what reaches here is a drop the resolver could not save.
bool outsideWorld(Vec2 p) {
    return p.x < 0.0 || p.x >= kWorldSize || p.y < 0.0 || p.y >= kWorldSize;
}

/// How far a flower reaches for loot.
///
/// Magnetism widens the reach and nothing else. Pulling the drop in would
/// consume it before a snapshot ever carried it.
double pickupReach(const World& world, Entity player, const PlayerModifiers& mods) {
    const Body* body = world.tryGet<Body>(player);
    const double base = body != nullptr ? body->radius * 2.0 : kDropPickupRadius;
    return base + std::max(0.0, mods.magnetism);
}

} // namespace

void LootSystem::bind(World& world) {
    if (boundWorld_ == &world) return;
    boundWorld_ = &world;
    collectors_.emplace(world);
    collectors_->without<Dead>();
    drops_.emplace(world);
    corpses_.emplace(world);
    // A pet dying is its owner's petal breaking, not a kill, and a corpse that
    // has already paid out must never be walked again.
    corpses_->without<LootAwarded, Pet>();
}

void LootSystem::run(World& world, const SpatialGrid& grid, const ContentRegistry& content,
                     Rng& rng, double nowMillis, double dt, CommandBuffer& commands,
                     EventQueue& events) {
    bind(world);
    tables_.link(content);
    pickups_.clear();

    maintainDrops(dt, commands);
    collectPickups(world, grid, commands, events, nowMillis);
    awardDeaths(world, rng, nowMillis);
    // Deaths pay out after the broadphase pass, but their loot is still
    // collectable this tick: the reference rolls a mob's drops inside the very
    // player step that then tests pickups, so a magnet flower standing on its
    // own kill takes the item before any snapshot could have carried it. The
    // client is not left with nothing to animate -- the pickup cue carries the
    // drop's position and look for exactly this case.
    collectFresh(world, commands, events, nowMillis);
}

void LootSystem::maintainDrops(double dt, CommandBuffer& commands) {
    expired_.clear();
    drops_->each([&](Entity e, DropTag&, Transform& transform, Lifetime& lifetime) {
        // Nothing resolves the +-50 spawn scatter -- neither server does, by
        // design -- so this push is the only way a drop that landed inside a
        // rock, a wall or water ever becomes reachable again. Pickup is a plain
        // distance test, and a tile face is far wider than its reach.
        if (terrain != nullptr) {
            transform.position = terrain->resolveCircle(transform.position, kDropWallRadius);
        }
        lifetime.remainingSeconds -= dt;
        if (lifetime.remainingSeconds <= 0.0 || outsideWorld(transform.position)) {
            expired_.push_back(e);
        }
    });
    for (const Entity e : expired_) commands.destroy(e);
}

void LootSystem::collectPickups(World& world, const SpatialGrid& grid, CommandBuffer& commands,
                                EventQueue& events, double nowMillis) {
    collectors_->each([&](Entity player, PlayerTag&, Transform& transform, PlayerModifiers& mods) {
        const Health* health = world.tryGet<Health>(player);
        if (health != nullptr && !health->alive()) return;

        const double reach = pickupReach(world, player, mods);
        grid.query(transform.position, reach, candidates_);
        for (const Entity candidate : candidates_) {
            tryCollect(world, player, transform.position, reach * reach, candidate, commands,
                       events, nowMillis);
        }
    });
}

void LootSystem::collectFresh(World& world, CommandBuffer& commands, EventQueue& events,
                              double nowMillis) {
    if (fresh_.empty()) return;
    collectors_->each([&](Entity player, PlayerTag&, Transform& transform, PlayerModifiers& mods) {
        const Health* health = world.tryGet<Health>(player);
        if (health != nullptr && !health->alive()) return;

        const double reach = pickupReach(world, player, mods);
        for (const Entity candidate : fresh_) {
            tryCollect(world, player, transform.position, reach * reach, candidate, commands,
                       events, nowMillis);
        }
    });
}

void LootSystem::tryCollect(World& world, Entity player, Vec2 playerPosition, double reachSq,
                            Entity candidate, CommandBuffer& commands, EventQueue& events,
                            double nowMillis) {
    DropItem* drop = world.tryGet<DropItem>(candidate);
    if (drop == nullptr) return;
    const Transform* at = world.tryGet<Transform>(candidate);
    if (at == nullptr) return;
    if (distanceSq(at->position, playerPosition) > reachSq) return;
    if (!mayPickUp(*drop, player, nowMillis)) return;

    const Pickup pickup{player, drop->configIndex, drop->rarity};
    pickups_.push_back(pickup);
    if (onPickup) onPickup(pickup);

    const NetId* dropId = world.tryGet<NetId>(candidate);
    const NetId* playerId = world.tryGet<NetId>(player);
    if (dropId != nullptr && playerId != nullptr) {
        events.pickedUp(dropId->value, playerId->value, at->position);
    }
    drop->pickedUpBy.push_back(player);
    bool finished = false;
    if (!drop->eligible.empty()) {
        finished = std::all_of(drop->eligible.begin(), drop->eligible.end(), [&](Entity e) {
            return std::find(drop->pickedUpBy.begin(), drop->pickedUpBy.end(), e) !=
                   drop->pickedUpBy.end();
        });
    }
    if (finished) commands.destroy(candidate);
}

void LootSystem::awardDeaths(World& world, Rng& rng, double nowMillis) {
    fresh_.clear();
    corpses_->collect(corpseList_);
    for (const Entity corpse : corpseList_) {
        const MobType* type = world.tryGet<MobType>(corpse);
        const Transform* transform = world.tryGet<Transform>(corpse);
        if (type == nullptr || transform == nullptr) continue;

        // Read the corpse out in full first. Everything below is a structural
        // change, and these pointers are into an archetype column.
        const std::uint16_t mobIndex = type->configIndex;
        const Rarity mobRarity = type->rarity;
        const Vec2 at = transform->position;
        const Dead* dead = world.tryGet<Dead>(corpse);
        const Entity killer = dead != nullptr ? dead->killer : NULL_ENTITY;

        // Loot slots go to the highest positive contributors, just like XP.
        ranked_.clear();
        eligible_.clear();
        if (const Bounty* bounty = world.tryGet<Bounty>(corpse)) {
            for (const Bounty::Share& share : bounty->contributors) {
                if (share.damage <= 0.0) continue;
                // Damage is the whole test on the reference's side, where the
                // tally holds nothing but player ids. So a contributor whose
                // entity is GONE still burns one of the tier's slots: nobody is
                // promoted into the gap, the next live player below the cut
                // gets nothing, and a drop reserved for someone who left can
                // never be completed and simply expires. Only a live entity
                // that is not a flower is refused -- nothing in the game
                // credits one, and the ranking is not the place to start.
                if (world.isAlive(share.player) && !world.has<PlayerTag>(share.player)) continue;
                ranked_.push_back(share);
            }
            // Stable: contributors are stored in first-hit order and the
            // reference's sort is specified stable, so on an exact damage tie
            // the slot at the cut belongs to whoever landed their damage first.
            // The order matters beyond the cut too -- the credit fallback below
            // reads the top of this list.
            std::stable_sort(ranked_.begin(), ranked_.end(), [](const auto& a, const auto& b) {
                return a.damage > b.damage;
            });
            selectLootRecipients(ranked_, lootSlotsForRarity(mobRarity), squads, eligible_);
        }

        const std::vector<DropTables::Entry>& table = tables_.forMob(mobIndex);

        // Marked before a single drop is rolled: a corpse with an empty table
        // must be just as finished as one that paid out.
        world.add<LootAwarded>(corpse);
        if (eligible_.empty()) continue;

        // A drop is ONE roll for the whole mob, so it is keyed to one player:
        // the credited killer, falling back to the biggest damage dealer when
        // the killing blow was nobody's -- a poison tick, a mob finishing a
        // mob, a pet whose owner left. If that player is no longer in the world
        // the mob pays out nothing at all, which is the reference's gate: the
        // roll needs an account to read its multiplier off.
        const Entity credit = world.has<PlayerTag>(killer) ? killer : ranked_.front().player;
        if (!world.has<PlayerTag>(credit)) continue;

        // The drop half of that player's leaderboard reward tier: a chance at a
        // second, independent roll of the whole table. The XP half of the same
        // trade is paid per recipient, by the combat system.
        double dropMultiplier = 1.0;
        if (const PlayerAccount* account = world.tryGet<PlayerAccount>(credit)) {
            dropMultiplier = account->dropMultiplier;
        }

        selected_.clear();
        rollTable(table, mobRarity, rng);
        if (dropMultiplier > 1.0 && rng.chance(dropMultiplier - 1.0)) {
            rollTable(table, mobRarity, rng);
        }

        const int copies = mobRarity == Rarity::Apex ? 10 : 1;
        for (const DropTables::Entry* entry : selected_) {
            // A consumable was rolled like any other row -- it is part of the
            // weighted denominator, which is the only reason it is in the table
            // at all -- but this inventory holds petals, so winning one means
            // the mob left nothing.
            if (entry->kind == DropTables::Kind::Consumable) continue;

            // The mob's tier scale is rolled ONCE per winning row, upstream of
            // the copies: an apex batch shares one base rarity and its ten
            // items differ only by their own upgrade rolls.
            const Rarity base = scaleDropRarity(clampRarity(entry->rarityOffset), mobRarity, rng);
            for (int i = 0; i < copies; ++i) {
                const Rarity rarity = finishDropRarity(base, mobRarity, rng);
                // The Random sentinel resolves per COPY, not per row.
                const std::uint16_t petalIndex = entry->kind == DropTables::Kind::RandomPetal
                                                     ? tables_.randomPetal(rng)
                                                     : entry->petalIndex;
                const Vec2 scatter{rng.range(-50.0, 50.0), rng.range(-50.0, 50.0)};
                // Unresolved on purpose, on both servers: the per-tick pass is
                // what pushes a drop out of the geometry it landed in. Noted so
                // the sweep below tests the scattered position, as the
                // reference's same-step pickup does.
                const Entity dropped =
                    spawnDrop(world, petalIndex, rarity, at + scatter, eligible_, nowMillis);
                if (dropped != NULL_ENTITY) fresh_.push_back(dropped);
            }
        }
    }
}

void LootSystem::rollTable(const std::vector<DropTables::Entry>& table, Rarity mobRarity,
                           Rng& rng) {
    // Common mobs roll every row independently, so they can never beat the
    // full set an unusual mob hands out.
    if (mobRarity == Rarity::Common) {
        for (const DropTables::Entry& entry : table) {
            if (rng.chance(entry.probability)) selected_.push_back(&entry);
        }
        return;
    }
    if (mobRarity == Rarity::Uncommon) {
        for (const DropTables::Entry& entry : table) selected_.push_back(&entry);
        return;
    }

    // Above unusual the probabilities are WEIGHTS and exactly one row wins.
    if (table.empty()) return;
    double total = 0.0;
    for (const auto& entry : table) total += entry.probability;
    if (total <= 0.0) return;
    double roll = rng.unit() * total;
    const DropTables::Entry* chosen = &table.back();
    for (const auto& entry : table) {
        roll -= entry.probability;
        if (roll <= 0.0) { chosen = &entry; break; }
    }
    selected_.push_back(chosen);
}

} // namespace flix
