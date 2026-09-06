#pragma once
// Who earned rights to a mob's rewards.
//
// One rule, read by two systems: the loot system decides who a drop is
// reserved for, and the combat system decides who is paid its XP. They used to
// each rank the damage tally themselves, which was the same rule written
// twice; with squads in the world it would be the same rule written twice and
// only one of them squad-aware.
//
// The rule itself is src/server/shared/lootEligibility.ts, and what it gets
// right is worth restating: the cap is spent in PLAYERS but the ranking is by
// CONTENDER, and a squad is ONE contender. Ranking a squad's members
// separately lets a party of four fill every slot on an ordinary mob; capping
// contenders instead of players lets four squads of four take sixteen.

#include <algorithm>
#include <cstddef>
#include <vector>

#include "server/squads.h"
#include "shared/core/entity.h"
#include "shared/core/types.h"
#include "shared/game/components.h"

namespace flix {

/// How many players may be paid for a mob of this tier.
///
/// Everything up to and including mythic shares the base count; the boss tiers
/// open up because they take a crowd to kill. Apex was not in the table these
/// numbers came from -- it sits above unique, so it inherits unique's count
/// rather than falling back to the base four.
inline int lootSlotsForRarity(Rarity rarity) {
    if (rarity == Rarity::Ultra) return 15;
    if (rarity == Rarity::Super) return 20;
    if (rarity == Rarity::Unique || rarity == Rarity::Apex) return 25;
    return 4;
}

/// Ranks `contributors` and fills `out` with the players who may be paid.
///
/// `contributors` is the corpse's damage tally, already filtered to positive
/// damage, in first-hit order -- every sort below is stable, so an exact
/// damage tie is settled by who hit first, which is the reference's rule.
/// `squads` may be null, which is the ordinary case: no squad on the corpse
/// means every contributor ranks as itself and this is a sort and a truncate.
inline void selectLootRecipients(const std::vector<Bounty::Share>& contributors, int slots,
                                 const SquadEntityIndex* squads, std::vector<Entity>& out) {
    out.clear();
    if (slots <= 0 || contributors.empty()) return;

    const auto damageOf = [&](Entity player) {
        for (const Bounty::Share& share : contributors) {
            if (share.player == player) return share.damage;
        }
        return 0.0;
    };

    // One contender per unsquadded player, plus one per squad that touched the
    // corpse. A squad's score is its members' AVERAGE damage, not their total:
    // a party would otherwise outrank a solo player who did more work than any
    // of them by simply having more members.
    struct Contender {
        std::size_t group = 0;        ///< index into squads->groups, when squadded
        Entity player = NULL_ENTITY;  ///< the member itself, when not
        double score = 0;
        int members = 0;
        bool squadded = false;
    };
    std::vector<Contender> ranked;
    ranked.reserve(contributors.size());

    for (const Bounty::Share& share : contributors) {
        const std::vector<Entity>* squad =
            squads != nullptr ? squads->membersOf(share.player) : nullptr;
        if (squad == nullptr) {
            ranked.push_back({0, share.player, share.damage, 1, false});
            continue;
        }
        const std::size_t group = squads->group.find(share.player)->second;
        auto existing = std::find_if(ranked.begin(), ranked.end(), [&](const Contender& c) {
            return c.squadded && c.group == group;
        });
        if (existing == ranked.end()) {
            ranked.push_back({group, NULL_ENTITY, share.damage, 1, true});
        } else {
            existing->score += share.damage;
            existing->members += 1;
        }
    }
    for (Contender& contender : ranked) {
        if (contender.members > 1) contender.score /= contender.members;
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const Contender& a, const Contender& b) { return a.score > b.score; });

    std::vector<Entity> members;
    for (const Contender& contender : ranked) {
        if (static_cast<int>(out.size()) >= slots) break;

        members.clear();
        if (!contender.squadded) {
            members.push_back(contender.player);
        } else {
            // Within one squad the slots go to the members who earned them: a
            // squadmate who never touched the mob is not paid for standing
            // near one who did.
            for (const Entity member : squads->groups[contender.group]) {
                if (damageOf(member) > 0.0) members.push_back(member);
            }
            std::stable_sort(members.begin(), members.end(), [&](Entity a, Entity b) {
                return damageOf(a) > damageOf(b);
            });
        }

        for (const Entity member : members) {
            if (static_cast<int>(out.size()) >= slots) break;
            if (std::find(out.begin(), out.end(), member) != out.end()) continue;
            out.push_back(member);
        }
    }
}

} // namespace flix
